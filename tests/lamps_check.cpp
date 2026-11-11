// tests/lamps_check.cpp - validates plugin/src/lamps.cpp.
//
// Build:
//   g++ -std=c++17 -O2 -o lamps_check tests/lamps_check.cpp
//       plugin/src/lamps.cpp plugin/src/solar.cpp
//   ./lamps_check
//
// What is being checked is not "does the number look nice" but "does the model
// reproduce the qualitative facts about real lamps that a colour-temperature
// dial gets wrong".

#include <cmath>
#include <cstdio>

#include "../plugin/src/lamps.h"
#include "../plugin/src/solar.h"

static int g_fail = 0;
static int g_checks = 0;

static void ok(const char* what, bool cond) {
    ++g_checks;
    if (!cond) {
        ++g_fail;
        std::printf("FAIL  %s\n", what);
    } else {
        std::printf("ok    %s\n", what);
    }
}

static void show(const char* what, const float* c) {
    std::printf("      %-26s r=%.3f g=%.3f b=%.3f\n", what, c[0], c[1], c[2]);
}

int main() {
    float c[3];

    // ---- the spectral function itself, at wavelengths whose colour is fact ----
    rhyWavelengthToRgb(650.0f, c);
    ok("650 nm is red-dominant", c[0] > 0.9f && c[1] < 0.2f && c[2] < 0.05f);
    rhyWavelengthToRgb(589.0f, c);  // sodium D line
    ok("589 nm (sodium D) is yellow-orange, no blue",
       c[0] > 0.9f && c[1] > 0.7f && c[2] < 0.05f);
    rhyWavelengthToRgb(546.1f, c);  // mercury green line
    ok("546 nm (mercury green) is green-dominant", c[1] > 0.9f && c[2] < 0.1f);
    rhyWavelengthToRgb(435.8f, c);  // mercury blue line
    ok("435.8 nm is blue-dominant", c[2] > 0.9f && c[0] < 0.2f);
    rhyWavelengthToRgb(404.7f, c);  // mercury violet line
    ok("404.7 nm is violet and dim (eye falls off at the violet end)",
       c[2] > 0.5f && c[0] > 0.2f && c[1] < 0.05f);
    rhyWavelengthToRgb(800.0f, c);
    ok("800 nm (infrared) contributes nothing", c[0] == 0.0f && c[2] == 0.0f);

    // ---- lamp families: measured chromaticity, linear sRGB ----
    float hps[3], planck2000[3];
    rhyLampFamilyRgb(RHY_LAMP_HPS, hps);
    colorTemperatureToRgb(2000.0f, planck2000);
    show("high-pressure sodium", hps);
    show("Planck at 2000 K", planck2000);
    // Real HPS sits at x 0.530 y 0.415, close to a ~2050 K blackbody: deep orange
    // in linear sRGB (1, 0.25, 0). The old line sum gave green > 0.75 (yellow).
    ok("HPS is deep orange: green near 0.25, no blue",
       hps[0] > 0.99f && hps[1] > 0.20f && hps[1] < 0.30f && hps[2] < 0.05f);
    ok("HPS is close to a ~2000 K blackbody (green within 0.12)",
       fabsf(hps[1] - planck2000[1]) < 0.12f);

    float mv[3];
    rhyLampFamilyRgb(RHY_LAMP_MERCURY, mv);
    show("mercury vapour", mv);
    ok("mercury is green-white: green the strongest channel", mv[1] > mv[0] && mv[1] > mv[2]);
    ok("mercury keeps real blue", mv[2] > 0.45f);
    ok("mercury is not neutral white (red clearly lower than green)", mv[0] < mv[1] - 0.15f);

    float mh[3], led[3], w[3];
    rhyLampFamilyRgb(RHY_LAMP_METAL_HALIDE, mh);
    rhyLampFamilyRgb(RHY_LAMP_LED, led);
    show("metal halide", mh);
    show("white LED", led);
    ok("metal halide is warm-neutral, between tungsten and LED", mh[2] > 0.30f && mh[2] < led[2]);
    ok("a 5000 K LED is the coolest white here", led[2] > 0.55f);

    rhyLampFamilyRgb(RHY_LAMP_TUNGSTEN, w);
    show("tungsten 2856 K (illuminant A)", w);
    ok("tungsten is warm and red-dominant", w[0] > 0.99f && w[2] < 0.20f);
    ok("HPS is redder than tungsten (less green)", hps[1] < w[1]);
    float planck2856[3];
    colorTemperatureToRgb(2856.0f, planck2856);
    ok("the Kelvin dial is linear: 2856 K matches illuminant A within 0.08",
       fabsf(planck2856[1] - w[1]) < 0.08f && fabsf(planck2856[2] - w[2]) < 0.08f);

    // ---- traffic signal lenses ----
    float grn[3], amb[3], red[3];
    rhySignalLensRgb(0, grn);
    rhySignalLensRgb(1, amb);
    rhySignalLensRgb(2, red);
    show("signal green lens", grn);
    show("signal amber lens", amb);
    show("signal red lens", red);
    ok("signal green is blue-green: green dominant, real blue, no red",
       grn[1] > 0.99f && grn[2] > 0.25f && grn[0] < 0.05f);
    ok("signal amber is orange: full red, some green, no blue",
       amb[0] > 0.99f && amb[1] > 0.12f && amb[1] < 0.35f && amb[2] < 0.05f);
    ok("signal red is pure red", red[0] > 0.99f && red[1] < 0.05f && red[2] < 0.05f);
    ok("amber and red are distinguishable", amb[1] > red[1] + 0.12f);

    // ---- fixture geometry and flux preservation ----
    RhyFixture st, sig;
    rhyStreetFixture(0, &st);
    rhySignalFixture(&sig);
    std::printf("      street luminaire: radius %.2f game units (~m), cone %.0f deg\n",
                st.radius, st.coneDeg);
    std::printf("      signal lens:      radius %.2f game units (~m), cone %.0f deg\n",
                sig.radius, sig.coneDeg);
    // The shipped values were radius 15 (street) and 6 (signal). One game unit
    // is about one metre, so those were 15 m and 6 m spheres.
    ok("street luminaire is fixture-sized, not vehicle-sized", st.radius < 0.5f);
    ok("signal lens is lens-sized", sig.radius < 0.3f);
    ok("street cone is wide (a luminaire floods the roadway, it does not spot)",
       st.coneDeg > 100.0f);

    const float rRef = 15.0f;
    const float rad = rhyRadianceForFlux(2.0f, st.radius, rRef);
    std::printf("      radiance 2.0 at r=15 -> %.1f at r=%.2f (x%.0f)\n",
                rad, st.radius, rad / 2.0f);
    ok("rescaling keeps total emitted power constant (radiance * r^2 invariant)",
       fabsf(rad * st.radius * st.radius - 2.0f * rRef * rRef) < 1.0f);
    ok("shrinking the fixture raises radiance rather than darkening the lamp",
       rad > 2.0f);
    ok("no change when the radius is already the reference",
       fabsf(rhyRadianceForFlux(2.0f, rRef, rRef) - 2.0f) < 1e-4f);

    // ---- ini name mapping ----
    ok("ini name 'hps' resolves", rhyLampFamilyFromName("hps") == RHY_LAMP_HPS);
    ok("ini name 'MERCURY' resolves case-insensitively",
       rhyLampFamilyFromName("MERCURY") == RHY_LAMP_MERCURY);
    ok("ini alias 'sodium' resolves", rhyLampFamilyFromName("sodium") == RHY_LAMP_HPS);
    ok("a nonsense name is rejected rather than silently defaulting",
       rhyLampFamilyFromName("candle") == -1);

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
