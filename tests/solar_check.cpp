// tests/solar_check.cpp - validates plugin/src/solar.cpp against published
// reference values. Compiled natively (not with MinGW) because the solar model
// has no Windows dependency at all; that is deliberate, it means the physical
// part of the lighting can be tested without a game, a GPU or Wine.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -o solar_check tests/solar_check.cpp && ./solar_check
//
// Exit code 0 = every check passed.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../plugin/src/solar.cpp"
// n24 asserts the sky ambient cap going through the REAL rate limiter, not a copy of
// it, so smooth.cpp is compiled into this file too. It is pure arithmetic (no windows.h,
// no Remix), same reason solar.cpp is here at all.
#include "../plugin/src/smooth.cpp"

static int g_fail = 0;
static int g_checks = 0;

// Local copies: solar.cpp's constants are file-static, and the closed-form
// reference values below need the same degree/radian factors.
static const float kDeg_ = 0.017453292519430f;
static const float kRad_ = 57.29577951308232f;

static float clampf_(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Absolute difference between two azimuths, treating 0 and 360 as the same.
static float azDiff(float a, float b) {
    float d = fmodf(fabsf(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}

static void check(const char* what, float got, float want, float tol) {
    ++g_checks;
    const float d = fabsf(got - want);
    if (d > tol) {
        ++g_fail;
        std::printf("FAIL  %-52s got %10.4f  want %10.4f  (tol %.4f)\n", what,
                    got, want, tol);
    } else {
        std::printf("ok    %-52s %10.4f (want %.4f)\n", what, got, want);
    }
}

static void checkTrue(const char* what, bool cond) {
    ++g_checks;
    if (!cond) {
        ++g_fail;
        std::printf("FAIL  %s\n", what);
    } else {
        std::printf("ok    %s\n", what);
    }
}

// Local time (minutes past midnight) at which the sun's elevation crosses
// `target` while rising / setting. Scans at 1-minute steps, then bisects.
static float crossTime(const SkyObserver& ob, float target, bool rising) {
    SkyBody b;
    float prev = -99.0f;
    for (int m = 0; m < 1440; ++m) {
        solarPosition(ob, m / 60, m % 60, &b);
        if (prev < target && b.elevationDeg >= target && rising) {
            return static_cast<float>(m);
        }
        if (prev > target && b.elevationDeg <= target && !rising) {
            return static_cast<float>(m);
        }
        prev = b.elevationDeg;
    }
    return -1.0f;
}

// n24: the moon light's published radiance, the same arithmetic as sun.cpp
// beamRadiance(): base * tint[c] * transmit[c] / zenithT[c]. The extinction constants are
// solar.cpp's own (this file includes it), so the test cannot drift from the plugin, and
// the base/tint are the shipped ini's [moon] radiance and color.
static float moonRadianceLum(const SkyBody& b, float turbidity, float phase) {
    const float tint[3] = {0.78f, 0.85f, 1.00f};
    const float luma[3] = {0.2126f, 0.7152f, 0.0722f};
    const float base = 0.10f * lunarPhaseFactor(phase);
    float lum = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float tau = kRayleigh[c] + kOzone[c] + turbidity * kAerosolExp[c];
        const float zenithT = expf(-tau);
        if (zenithT <= 1e-4f) {
            continue;
        }
        lum += luma[c] * base * tint[c] * b.transmit[c] / zenithT;
    }
    return lum;
}

int main() {
    // ---- declination: Spencer's series vs the textbook extremes ----
    check("declination at June solstice (day 172)", solarDeclinationDeg(172), 23.44f, 0.35f);
    check("declination at December solstice (day 355)", solarDeclinationDeg(355), -23.44f, 0.35f);
    check("declination at March equinox (day 80)", solarDeclinationDeg(80), 0.0f, 0.5f);
    check("declination at September equinox (day 266)", solarDeclinationDeg(266), 0.0f, 0.5f);
    // Earth's eccentricity is what makes these two gaps unequal: the sun takes
    // 8 more days from the June solstice to the September equinox than from the
    // March equinox to the June solstice.
    check("declination, July 15 (USNO 21.35 deg)", solarDeclinationDeg(196), 21.35f, 0.3f);

    // ---- Los Angeles, mid-July. 34.05 N, 118.24 W, PDT = UTC-7 ----
    SkyObserver la;
    la.latitudeDeg = 34.05f;
    la.longitudeDeg = -118.24f;
    la.dayOfYear = 196;      // July 15
    la.timezoneHours = -7.0f;
    la.turbidity = 0.12f;

    SkyBody s;
    solarPosition(la, 13, 0, &s);  // near solar noon
    // Max altitude = 90 - latitude + declination = 90 - 34.05 + 21.3 = 77.25
    check("solar noon altitude, LA July 15", s.elevationDeg, 77.2f, 1.5f);
    checkTrue("solar noon is due south", fabsf(s.azimuthDeg - 180.0f) < 8.0f);
    check("air mass at ~77 deg elevation", s.airMass, 1.03f, 0.06f);

    // The sun is highest around 13:00 PDT in LA in July (12:00 + 1 h DST + ~14
    // min of longitude/equation-of-time offset).
    float noon = -1.0f, best = -90.0f;
    for (int m = 0; m < 1440; ++m) {
        solarPosition(la, m / 60, m % 60, &s);
        if (s.elevationDeg > best) {
            best = s.elevationDeg;
            noon = static_cast<float>(m);
        }
    }
    // Solar noon in Los Angeles in mid-July is about 12:59 PDT: the city sits
    // 1.76 degrees east of the Pacific standard meridian (120 W), so solar noon
    // is ~7 minutes before 12:00 PST, and the equation of time in mid-July
    // pushes it back by roughly 6 minutes.
    check("solar noon clock time, LA July 15 (min past midnight)", noon, 12 * 60 + 59, 12.0f);

    // USNO for Los Angeles, 2025-07-15: sunrise 5:58 am, sunset 8:05 pm PDT.
    // The model's geometric crossing (centre at 0 deg) is a few minutes later
    // than the published limb-plus-refraction time, so the tolerance is wide on
    // purpose; the azimuth check below is the tight one.
    const float rise = crossTime(la, 0.0f, true);
    const float set = crossTime(la, 0.0f, false);
    check("sunrise (min past midnight)", rise, 5 * 60 + 58, 12.0f);
    check("sunset (min past midnight)", set, 20 * 60 + 5, 12.0f);

    // At the geometric horizon, cos(azimuth) = sin(dec)/cos(lat) exactly. That
    // closed form is the reference; getting the azimuth triangle's cosine term
    // wrong moves these by four degrees and nobody notices until a sunset looks
    // sideways.
    const float decJul = solarDeclinationDeg(196);
    const float azWant = acosf(sinf(decJul * kDeg_) / cosf(la.latitudeDeg * kDeg_)) * kRad_;
    solarPosition(la, static_cast<int>(rise / 60), static_cast<int>(rise) % 60, &s);
    check("sunrise azimuth vs cos^-1(sin dec / cos lat)", s.azimuthDeg, azWant, 1.5f);
    solarPosition(la, static_cast<int>(set / 60), static_cast<int>(set) % 60, &s);
    check("sunset azimuth vs 360 - that", s.azimuthDeg, 360.0f - azWant, 1.5f);
    std::printf("      sunrise az %.2f (USNO ~61 deg for the refracted limb)\n", azWant);

    // ---- the physics that the old fade_pow model did not have ----
    solarPosition(la, 12, 0, &s);
    const float noonR = s.transmit[0], noonB = s.transmit[2];
    checkTrue("at noon red survives more than blue", noonR > noonB);

    // Sample the beam at 5 degrees of elevation rather than at a fixed clock
    // time, so the assertion is about the atmosphere and not about the date.
    float lowR = 0, lowG = 0, lowB = 0, lowM = 0;
    for (int m = 0; m < 1440; ++m) {
        solarPosition(la, m / 60, m % 60, &s);
        if (s.elevationDeg > 4.9f && s.elevationDeg < 5.1f && m > 720) {
            lowR = s.transmit[0];
            lowG = s.transmit[1];
            lowB = s.transmit[2];
            lowM = s.airMass;
            break;
        }
    }
    checkTrue("low sun: red transmittance far above blue", lowR > 3.0f * lowB);
    checkTrue("low sun: air mass is large", lowM > 8.0f);
    std::printf("      5 deg elevation: air mass %.2f  transmittance R=%.3f G=%.3f B=%.3f\n",
                lowM, lowR, lowG, lowB);

    // Air mass: Kasten-Young gives 5.6 at 10 degrees elevation.
    SkyBody probe;
    probe = s;
    float elev10 = -1.0f;
    for (int m = 0; m < 1440; ++m) {
        solarPosition(la, m / 60, m % 60, &probe);
        if (probe.elevationDeg > 9.9f && probe.elevationDeg < 10.1f && m < 720) {
            elev10 = probe.airMass;
            break;
        }
    }
    check("Kasten-Young air mass at 10 deg elevation", elev10, 5.6f, 0.3f);

    // ---- the moon ----
    SkyBody sun, moon;
    solarPosition(la, 0, 0, &sun);
    lunarPosition(la, 0, 0, 0.5f, &moon);  // full moon at midnight
    const float dot = sun.toX * moon.toX + sun.toY * moon.toY + sun.toZ * moon.toZ;
    checkTrue("full moon at midnight is opposite the sun", dot < -0.999f);
    checkTrue("full moon at midnight is above the horizon", moon.elevationDeg > 0.0f);
    // A full moon is the anti-sun point, so its altitude is the sun's altitude
    // negated - and in northern summer that is LOW. The sun reaches +77 deg at
    // noon here, but the anti-sun point is only +33 deg, because the ecliptic
    // dips low in the southern sky in July. Same geometry as the Harvest Moon.
    check("full moon altitude = -sun altitude (midnight)", moon.elevationDeg, -sun.elevationDeg, 0.5f);
    check("full moon azimuth = sun azimuth + 180 (midnight)",
          azDiff(moon.azimuthDeg, sun.azimuthDeg + 180.0f), 0.0f, 0.5f);

    lunarPosition(la, 12, 0, 0.5f, &moon);  // full moon at noon
    checkTrue("full moon at noon is below the horizon", moon.elevationDeg < 0.0f);

    lunarPosition(la, 0, 0, 0.0f, &moon);  // new moon at midnight
    checkTrue("new moon at midnight is below the horizon", moon.elevationDeg < 0.0f);

    // A new moon sits on the sun, so at any instant it must match it exactly.
    SkyBody s2;
    solarPosition(la, 12, 0, &s2);
    lunarPosition(la, 12, 0, 0.0f, &moon);
    check("new moon altitude = sun altitude (12:00)", moon.elevationDeg, s2.elevationDeg, 0.3f);
    check("new moon azimuth = sun azimuth (12:00)", moon.azimuthDeg, s2.azimuthDeg, 0.3f);

    lunarPosition(la, 21, 0, 0.25f, &moon);  // first quarter at 21:00
    checkTrue("first quarter moon is up in the evening", moon.elevationDeg > 0.0f);
    // A first quarter moon is 90 degrees from the sun, so it sets about 6 h
    // after it - up all evening, gone by midnight.
    lunarPosition(la, 21, 0, 0.25f, &moon);
    solarPosition(la, 21, 0, &sun);
    float sep = acosf(clampf_(moon.toX * sun.toX + moon.toY * sun.toY +
                              moon.toZ * sun.toZ, -1.0f, 1.0f)) * kRad_;
    check("first quarter is 90 degrees from the sun", sep, 90.0f, 3.0f);

    check("phase factor: full", lunarPhaseFactor(0.5f), 1.0f, 0.01f);
    check("phase factor: new", lunarPhaseFactor(0.0f), 0.0f, 0.01f);
    check("phase factor: quarter (much fainter than half)", lunarPhaseFactor(0.25f), 0.25f, 0.01f);

    // ---- twilight and lamp switching ----
    check("daylight factor at noon", skyDaylightFactor(70.0f, -6.0f, -4.0f), 1.0f, 0.001f);
    check("daylight factor at night", skyDaylightFactor(-20.0f, -6.0f, -4.0f), 0.0f, 0.001f);
    check("daylight factor mid-twilight", skyDaylightFactor(-5.0f, -6.0f, -4.0f), 0.5f, 0.02f);
    checkTrue("lamps on when the sun is below -6", lampsShouldBeOn(-8.0f, -6.0f));
    checkTrue("lamps off when the sun is above -6", !lampsShouldBeOn(-2.0f, -6.0f));
    // The rule is strict and stateless: at the threshold the lamps are off, and
    // the answer depends on the elevation only, never on whether the sun is
    // rising or setting (that is what [streetlights] dawn_deg would need).
    checkTrue("lamps off exactly at the dusk threshold", !lampsShouldBeOn(-6.0f, -6.0f));

    // ---- colour temperature ----
    float rgb[3];
    colorTemperatureToRgb(6500.0f, rgb);
    checkTrue("6500 K is near neutral white", rgb[0] > 0.9f && rgb[1] > 0.9f && rgb[2] > 0.9f);
    colorTemperatureToRgb(2000.0f, rgb);  // high-pressure sodium street lamp
    checkTrue("2000 K is strongly red-dominant", rgb[0] > 0.95f && rgb[2] < 0.45f);
    std::printf("      2000 K -> %.3f, %.3f, %.3f   2700 K ->", rgb[0], rgb[1], rgb[2]);
    colorTemperatureToRgb(2700.0f, rgb);
    std::printf(" %.3f, %.3f, %.3f   4000 K ->", rgb[0], rgb[1], rgb[2]);
    colorTemperatureToRgb(4000.0f, rgb);
    std::printf(" %.3f, %.3f, %.3f\n", rgb[0], rgb[1], rgb[2]);

    // ---- n24: a distant light may never shine up through the ground ----
    // The 2026-11-12 log, 11:48:30: a sun handle with radiance 0.0376 and
    // dir=0.2206,-0.8386,+0.4981 while the sun sat at -31.6 deg. dir z > 0 is light
    // coming up through the ground, and it happens because the radiance ramp-down keeps
    // the handle alive tens of seconds after the body sets.
    float up[3] = {0.2206f, -0.8386f, 0.4981f};
    solarClampLightDir(up);
    check("clamped log vector: z", up[2], 0.0f, 1e-6f);
    check("clamped log vector: still unit length",
          sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]), 1.0f, 1e-5f);
    check("clamped log vector: x", up[0], 0.2544f, 1e-3f);
    check("clamped log vector: y", up[1], -0.9672f, 1e-3f);
    float down[3] = {0.0f, 0.0f, -1.0f};
    solarClampLightDir(down);
    check("a body at the zenith is untouched", down[2], -1.0f, 1e-6f);
    float grazing[3] = {0.7071f, 0.0f, -0.7071f};
    solarClampLightDir(grazing);
    check("a 45 deg body is untouched", grazing[2], -0.7071f, 1e-5f);
    float nadir[3] = {0.0f, 0.0f, 1.0f};
    solarClampLightDir(nadir);
    checkTrue("a body at the nadir becomes horizontal, not left pointing up",
              nadir[2] == 0.0f && nadir[0] == 1.0f);
    // Idempotent, and true for every minute of a real day for both bodies: the sun and
    // the moon spend half the day under the horizon, and that is exactly when the ramp
    // is still carrying a handle.
    float again[3] = {up[0], up[1], up[2]};
    solarClampLightDir(again);
    checkTrue("clamping twice changes nothing", fabsf(again[0] - up[0]) < 1e-6f &&
              fabsf(again[1] - up[1]) < 1e-6f && fabsf(again[2] - up[2]) < 1e-6f);
    int badDir = 0;
    for (int m = 0; m < 1440; ++m) {
        solarPositionF(la, static_cast<float>(m), &s);
        float d[3] = {-s.toX, -s.toY, -s.toZ};
        solarClampLightDir(d);
        if (d[2] > 0.0f) {
            ++badDir;
        }
        lunarPositionF(la, static_cast<float>(m), 0.5f, &moon);
        float md[3] = {-moon.toX, -moon.toY, -moon.toZ};
        solarClampLightDir(md);
        if (md[2] > 0.0f) {
            ++badDir;
        }
    }
    checkTrue("no minute of a whole day publishes dir z > 0 after the clamp", badDir == 0);
    // And the clamp does not invent movement for a body that is up: at 30 degrees the
    // direction it publishes is the geometry's own, to the float.
    int movedWhileUp = 0;
    for (int m = 0; m < 1440; ++m) {
        solarPositionF(la, static_cast<float>(m), &s);
        if (s.elevationDeg < 5.0f) {
            continue;
        }
        float d[3] = {-s.toX, -s.toY, -s.toZ};
        const float before = d[2];
        solarClampLightDir(d);
        if (fabsf(d[2] - before) > 1e-6f) {
            ++movedWhileUp;
        }
    }
    checkTrue("a body above the horizon is never touched by the clamp", movedWhileUp == 0);

    // ---- n24: the night ceiling on the sky ambient ----
    // Day and twilight pass through; once the sun is down the ambient may not exceed
    // capMultiple x the moon light's published radiance. 0.25 is what ships
    // ([sky] ambient_cap_mult); 0 is the switch that turns the rule off.
    check("cap: full day passes through", skyAmbientCap(0.9f, 0.0669f, 1.0f, 31.0f, 0.25f),
          0.9f, 1e-6f);
    check("cap: mid-twilight passes through", skyAmbientCap(0.5f, 0.0669f, 0.5f, 31.0f, 0.25f),
          0.5f, 1e-6f);
    check("cap: just above the night line passes through",
          skyAmbientCap(0.07f, 0.0669f, 0.051f, 31.0f, 0.25f), 0.07f, 1e-6f);
    check("cap: at the night line it binds", skyAmbientCap(0.07f, 0.0669f, 0.05f, 31.0f, 0.25f),
          0.016725f, 1e-5f);
    check("cap: never raises", skyAmbientCap(0.001f, 0.0669f, 0.0f, 31.0f, 0.25f), 0.001f,
          1e-6f);
    check("cap: ambient_cap_mult=0 is the switch off", skyAmbientCap(0.07f, 0.0669f, 0.0f,
                                                                     31.0f, 0.0f),
          0.07f, 1e-6f);
    check("cap: a moon that is not up sets no ceiling",
          skyAmbientCap(0.07f, 0.0669f, 0.0f, -5.0f, 0.25f), 0.0f, 1e-6f);
    // Where the night line actually sits on the shipped twilight band: skyDaylightFactor
    // is a smoothstep over [-10, +4], so daylight = 0.05 at a sun elevation of -8.1 deg.
    // Above it the sun still owns the ambient and the cap must not bite.
    check("the night line is at -8.1 deg of sun elevation",
          skyDaylightFactor(-8.1f, -10.0f, 4.0f), 0.05f, 5e-3f);
    checkTrue("and at -7 deg the cap does not bind",
              skyAmbientCap(0.07f, 0.0669f, skyDaylightFactor(-7.0f, -10.0f, 4.0f), 31.0f,
                            0.25f) == 0.07f);
    // The shipped numbers, computed from this file's own atmosphere rather than quoted:
    // [moon] radiance 0.10, full moon, the ini's moon tint, at the altitude a July full
    // moon actually reaches over Los Angeles (the anti-sun point: 33 deg, not 77).
    lunarPosition(la, 23, 30, 0.5f, &moon);
    const float moonLum = moonRadianceLum(moon, la.turbidity, 0.5f);
    std::printf("      moon elev %+.1f  published radiance luminance %.4f  ->  night "
                "ambient 0.0700 capped to %.4f (%.0f%% of it)\n", moon.elevationDeg, moonLum,
                skyAmbientCap(0.07f, moonLum, 0.0f, moon.elevationDeg, 0.25f),
                100.0f * skyAmbientCap(0.07f, moonLum, 0.0f, moon.elevationDeg, 0.25f) /
                    0.07f);
    checkTrue("the moon's published radiance is the order of magnitude the notes quote",
              moonLum > 0.05f && moonLum < 0.09f);
    checkTrue("the cap binds on the shipped ini's night ambient",
              skyAmbientCap(0.07f, moonLum, 0.0f, moon.elevationDeg, 0.25f) < 0.02f);
    // The ambient follows the moon's altitude instead of the clock: the same ini at three
    // points of the night, each with the radiance that altitude actually publishes.
    float capLow = 0.0f, capHigh = 0.0f;
    for (int hh = 18; hh <= 30; ++hh) {   // 18:00 through 06:00
        lunarPosition(la, hh % 24, 0, 0.5f, &moon);
        if (moon.elevationDeg <= 0.0f) {
            continue;
        }
        const float cap = skyAmbientCap(0.07f, moonRadianceLum(moon, la.turbidity, 0.5f),
                                        0.0f, moon.elevationDeg, 0.25f);
        capLow = capLow > 0.0f && capLow < cap ? capLow : cap;
        capHigh = cap > capHigh ? cap : capHigh;
    }
    std::printf("      the night's ceiling over the night: %.4f .. %.4f\n", capLow, capHigh);
    checkTrue("a higher moon lifts the ceiling", capHigh > capLow * 1.5f);
    checkTrue("and it stays under the ini's 0.0700 target", capHigh < 0.07f);

    // ---- n24: the cap moves a TARGET, and the limiter still owns the published value ----
    // s28's rule: no published lighting value may step. The cap can therefore only ever
    // be a step in the target; rhyRateStep turns it into a ramp. These are the shipped
    // [fade] numbers (sky_rel_per_sec 3.0, sky_abs_per_sec 0.25) and a 60 fps frame.
    SkyBody moonNight;
    lunarPosition(la, 23, 30, 0.5f, &moonNight);   // up, 30.6 deg
    const float nightTarget =
        skyAmbientCap(0.07f, moonRadianceLum(moonNight, la.turbidity, 0.5f), 0.0f,
                      moonNight.elevationDeg, 0.25f);
    const float dt = 1.0f / 60.0f;
    RhyRate sky;
    rhyRateInit(sky, 0.07f, 3.0f, 0.25f, 0.002f);
    float prev = sky.cur, worstAbs = 0.0f;
    int frames = 0;
    bool monotone = true, bounded = true;
    while (fabsf(sky.cur - nightTarget) > 0.002f && frames < 1000) {
        rhyRateStep(sky, nightTarget, dt);
        // smooth.h's own bound: |move| <= cur * (relPerSec^dt - 1) + absPerSec * dt.
        const float allowed = prev * (powf(3.0f, dt) - 1.0f) + 0.25f * dt;
        if (fabsf(sky.cur - prev) > allowed + 1e-6f) {
            bounded = false;
        }
        if (sky.cur > prev) {
            monotone = false;  // going down must never overshoot back up
        }
        worstAbs = fmaxf(worstAbs, fabsf(sky.cur - prev));
        prev = sky.cur;
        ++frames;
    }
    std::printf("      ambient 0.0700 -> %.4f: %d frames (%.2f s), biggest single move "
                "%.5f (%.0f%% of the start)\n", nightTarget, frames, frames * dt, worstAbs,
                100.0f * worstAbs / 0.07f);
    checkTrue("the capped ambient never arrives in one frame", frames > 10);
    checkTrue("every frame stays inside the limiter's own bound", bounded);
    checkTrue("the ramp is monotone", monotone);
    checkTrue("and no frame takes more than a quarter of the ambient away",
              worstAbs <= 0.07f * 0.25f);
    // A clock glitch that puts the sun back up must not step the ambient either: the cap
    // releases, and the same limiter carries it back up.
    const float before = sky.cur;
    rhyRateStep(sky, skyAmbientCap(0.07f, moonRadianceLum(moonNight, la.turbidity, 0.5f),
                                   1.0f, moonNight.elevationDeg, 0.25f), dt);
    checkTrue("releasing the cap in one frame does not step the published value",
              fabsf(sky.cur - before) <= 0.005f && sky.cur < 0.035f);  // 0.07 is a quarter
                                                                       // second away, not
                                                                       // one frame away

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
