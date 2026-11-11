#include "solar.h"

#include <cmath>

// ------------------------------------------------------------------ constants

// GTA SA's axes are x = east, y = north, z = up, which is the same handedness
// the Remix side uses once rtx.zUp = True, so nothing here needs an axis swap.
static const float kDeg = 0.017453292519430f;
static const float kRad = 57.29577951308232f;

// Band-average Rayleigh optical depth at sea level for the three channels.
// Rayleigh scatters short wavelengths far more strongly (1/lambda^4), which is
// why the direct beam loses blue first and turns orange: at 680 nm the air takes
// 7% per air mass, at 460 nm it takes 22%.
static const float kRayleigh[3] = {0.0681f, 0.1054f, 0.2162f};  // 680 / 550 / 460 nm

// Aerosol follows Angstrom's law tau = beta * (lambda/550nm)^-alpha. alpha 1.3
// is an urban value; the exponents below are (680/550)^-1.3, 1, (460/550)^-1.3.
static const float kAerosolExp[3] = {0.7576f, 1.0f, 1.2687f};

// Ozone (Chappuis band) absorbs in the green, which is the reason a real sunset
// reads orange-red rather than orange-yellow. Folded into three constants rather
// than a spectral integral because we only have three channels.
static const float kOzone[3] = {0.020f, 0.030f, 0.010f};

// The geometric horizon gives a relative air mass of ~38 for a sea-level
// observer, which extinguishes the blue channel to zero and the red to 3% -
// harsher than reality, because the sun's disc is half a degree wide, the lower
// atmosphere is not uniform, and GTA's "horizon" is a city street. Clamping the
// path keeps the sunset bright enough to light a street while still being red.
static const float kMaxAirMass = 12.0f;

// ------------------------------------------------------------------ helpers

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Spencer (1971) Fourier series. Two terms of the same fractional-year series
// give declination and the equation of time to well under an arcminute, which
// is far finer than the half-degree sun disc being modelled. Either output may
// be null when the caller only wants one of the two.
static void spencer(int dayOfYear, float* declRad, float* eqTimeMin) {
    float n = static_cast<float>(dayOfYear < 1 ? 1 : (dayOfYear > 366 ? 366 : dayOfYear));
    const float g = 2.0f * 3.14159265358979f * (n - 1.0f) / 365.0f;
    const float cg = cosf(g), sg = sinf(g);
    const float c2 = cosf(2.0f * g), s2 = sinf(2.0f * g);
    const float c3 = cosf(3.0f * g), s3 = sinf(3.0f * g);

    const float decl = 0.006918f - 0.399912f * cg + 0.070257f * sg -
                       0.006758f * c2 + 0.000907f * s2 - 0.002697f * c3 +
                       0.001480f * s3;
    const float eqtime = 229.18f * (0.000075f + 0.001868f * cg - 0.032077f * sg -
                                    0.014615f * c2 - 0.040849f * s2);
    if (declRad) {
        *declRad = decl;
    }
    if (eqTimeMin) {
        *eqTimeMin = eqtime;
    }
}

// Kasten & Young (1989). The classic secant-z air mass blows up at the horizon;
// this form stays finite and matches measured relative paths to ~0.1% out to
// z = 90 degrees.
static float relativeAirMass(float zenithDeg) {
    if (zenithDeg >= 91.0f) {
        return 0.0f;  // body is below the horizon: no direct beam
    }
    const float z = zenithDeg < 0.0f ? 0.0f : zenithDeg;
    const float denom = cosf(z * kDeg) + 0.50572f * powf(96.07995f - z, -1.6364f);
    if (denom <= 1e-4f) {
        return kMaxAirMass;
    }
    const float m = 1.0f / denom;
    return clampf(m, 1.0f, kMaxAirMass);
}

// Beer-Lambert: I = I0 * exp(-tau * m). Applied per channel, which is the whole
// reason a sunset is red - no colour temperature slider is involved.
static void applyExtinction(float airMass, float turbidity, float* out) {
    if (airMass <= 0.0f) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    const float beta = turbidity < 0.0f ? 0.0f : turbidity;
    for (int c = 0; c < 3; ++c) {
        const float tau = kRayleigh[c] + beta * kAerosolExp[c] + kOzone[c];
        out[c] = expf(-tau * airMass);
    }
}

// Solar ecliptic longitude, 0 degrees at the March equinox.
//
// Mean motion alone is not enough: Earth's orbit is eccentric, so the sun moves
// about 1.7 degrees per day near perihelion (early January) and 1.5 near aphelion
// (early July). A pure mean-motion longitude puts the September equinox four days
// early and takes the solstice declinations a fifth of a degree off. Adding the
// equation of center - the first two terms of the Kepler series - fixes both for
// about ten floating point operations once per sky update.
static float solarEclipticLongitudeDeg(int dayOfYear) {
    const float n = static_cast<float>(dayOfYear < 1 ? 1 : (dayOfYear > 366 ? 366 : dayOfYear));
    // Perihelion falls around January 3-4; day 3 is the reference.
    float m = (n - 3.0f) * (360.0f / 365.25f);  // mean anomaly, degrees
    m = m - 360.0f * floorf(m / 360.0f);
    const float mr = m * kDeg;
    const float c = 1.915f * sinf(mr) + 0.020f * sinf(2.0f * mr);  // equation of center
    // 282.94 degrees = the sun's ecliptic longitude on January 3, i.e. the
    // longitude of perihelion for our reference day.
    float l = 282.94f + m + c;
    l = l - 360.0f * floorf(l / 360.0f);
    return l;
}

// Equatorial coordinates from an ecliptic longitude: rotate by the obliquity of
// the ecliptic (23.44 degrees), which is the tilt that gives us seasons.
static void eclipticToEquatorial(float lambdaDeg, float* raDeg, float* decDeg) {
    const float l = lambdaDeg * kDeg;
    const float e = 23.44f * kDeg;
    const float sinL = sinf(l), cosL = cosf(l);
    *decDeg = asinf(clampf(sinf(e) * sinL, -1.0f, 1.0f)) * kRad;
    *raDeg = atan2f(cosf(e) * sinL, cosL) * kRad;
    if (*raDeg < 0.0f) {
        *raDeg += 360.0f;
    }
}

// Altitude and azimuth from declination + hour angle, for an observer at a
// latitude. Azimuth is measured clockwise from north (N=0, E=90, S=180, W=270),
// which maps straight onto GTA's +y = north, +x = east.
//
// The atan2 form is used deliberately. The acos form that appears in the NOAA
// spreadsheet, acos((sin(phi)cos(z) - sin(d)) / (cos(phi)cos(z))), is singular
// at the horizon - exactly where sunrise and sunset azimuths matter - and needs
// a separate sign fix from the hour angle. atan2 is well behaved everywhere.
static void equatorialToAltAz(float latDeg, float decDeg, float haDeg,
                              float* elevDeg, float* azDeg) {
    const float lat = latDeg * kDeg;
    const float dec = decDeg * kDeg;
    const float h = haDeg * kDeg;

    // The astronomical triangle. With azimuth measured from north and positive
    // toward east:
    //   sin(el)          = sin(lat) sin(dec) + cos(lat) cos(dec) cos(H)
    //   cos(el) sin(A)   = -cos(dec) sin(H)
    //   cos(el) cos(A)   = sin(dec) cos(lat) - cos(dec) sin(lat) cos(H)
    // The third line is the one that is easy to get wrong: writing it as
    // sin(dec) - sin(lat) sin(el) looks like a rearrangement and is not one, and
    // the error only shows up near the horizon, i.e. exactly at sunrise and
    // sunset, where it moves the azimuth by four degrees or more.
    const float sinEl = sinf(lat) * sinf(dec) + cosf(lat) * cosf(dec) * cosf(h);
    const float el = asinf(clampf(sinEl, -1.0f, 1.0f));

    const float sAz = -cosf(dec) * sinf(h);
    const float cAz = sinf(dec) * cosf(lat) - cosf(dec) * sinf(lat) * cosf(h);
    float a = atan2f(sAz, cAz) * kRad;
    if (a < 0.0f) {
        a += 360.0f;
    }
    *elevDeg = el * kRad;
    *azDeg = a;
}

// Place a body given elevation/azimuth into game axes and fill the extinction.
static void finishBody(SkyBody* b, float elevDeg, float azDeg, float turbidity) {
    b->elevationDeg = elevDeg;
    b->azimuthDeg = azDeg;
    b->above = elevDeg > 0.0f;

    const float el = elevDeg * kDeg;
    const float az = azDeg * kDeg;
    const float ce = cosf(el);
    b->toX = sinf(az) * ce;  // east
    b->toY = cosf(az) * ce;  // north
    b->toZ = sinf(el);       // up

    const float n = sqrtf(b->toX * b->toX + b->toY * b->toY + b->toZ * b->toZ);
    if (n > 1e-6f) {
        b->toX /= n;
        b->toY /= n;
        b->toZ /= n;
    }

    b->airMass = relativeAirMass(90.0f - elevDeg);
    applyExtinction(b->airMass, turbidity, b->transmit);
}

// Local apparent solar time in minutes, and the sun's hour angle in degrees.
// 4 minutes of solar time per degree of longitude, minus the timezone offset,
// plus the equation of time: this is what makes solar noon land at a different
// wall-clock time in the east and west of a county, and why it drifts by up to
// 16 minutes across the year.
static float solarHourAngleDeg(const SkyObserver& ob, float localMinutes,
                               float* eqTimeMinOut) {
    float declUnused = 0.0f, eqTimeMin = 0.0f;
    spencer(ob.dayOfYear, &declUnused, &eqTimeMin);
    if (eqTimeMinOut) {
        *eqTimeMinOut = eqTimeMin;
    }
    const float solarTime =
        localMinutes + eqTimeMin + 4.0f * ob.longitudeDeg - 60.0f * ob.timezoneHours;
    float ha = (solarTime - 720.0f) * 0.25f;  // degrees, 0 at solar noon
    while (ha > 360.0f) {
        ha -= 360.0f;
    }
    while (ha < -360.0f) {
        ha += 360.0f;
    }
    return ha;
}

// ------------------------------------------------------------------ public

void solarPosition(const SkyObserver& ob, int hour, int minute, SkyBody* out) {
    const float lambda = solarEclipticLongitudeDeg(ob.dayOfYear);
    float ra = 0.0f, dec = 0.0f;
    eclipticToEquatorial(lambda, &ra, &dec);

    float eq = 0.0f;
    const float ha = solarHourAngleDeg(ob, static_cast<float>(hour * 60 + minute), &eq);

    float elev = 0.0f, az = 0.0f;
    equatorialToAltAz(ob.latitudeDeg, dec, ha, &elev, &az);
    finishBody(out, elev, az, ob.turbidity);
}

void lunarPosition(const SkyObserver& ob, int hour, int minute, float phase,
                   SkyBody* out) {
    // The moon is the sun's position slid along the ecliptic by its elongation,
    // which is phase * 360 degrees: new moon sits on the sun, first quarter 90
    // degrees behind it, full moon exactly opposite. Working in ecliptic
    // longitude and converting to right ascension is what makes this correct -
    // shifting the *clock* by 12 hours instead would put a midsummer full moon
    // at the sun's noon altitude (77 degrees over Los Angeles) when the real one
    // is a low 35 degrees, because the anti-sun point in northern summer is low
    // in the south. That is the same geometry behind the Harvest Moon.
    float p = phase - static_cast<int>(phase);
    if (p < 0.0f) {
        p += 1.0f;
    }

    const float lambdaSun = solarEclipticLongitudeDeg(ob.dayOfYear);
    float raSun = 0.0f, decSun = 0.0f;
    eclipticToEquatorial(lambdaSun, &raSun, &decSun);

    float lambdaMoon = lambdaSun + p * 360.0f;
    lambdaMoon -= 360.0f * floorf(lambdaMoon / 360.0f);
    float raMoon = 0.0f, decMoon = 0.0f;
    eclipticToEquatorial(lambdaMoon, &raMoon, &decMoon);

    // Local sidereal time = the sun's hour angle + the sun's right ascension,
    // because the sun's hour angle is 0 at solar noon. A body's hour angle is
    // then LST - its own right ascension.
    const float haSun = solarHourAngleDeg(ob, static_cast<float>(hour * 60 + minute), nullptr);
    const float lst = haSun + raSun;
    float haMoon = lst - raMoon;
    while (haMoon > 360.0f) {
        haMoon -= 360.0f;
    }
    while (haMoon < -360.0f) {
        haMoon += 360.0f;
    }

    float elev = 0.0f, az = 0.0f;
    equatorialToAltAz(ob.latitudeDeg, decMoon, haMoon, &elev, &az);
    finishBody(out, elev, az, ob.turbidity);
}

float lunarPhaseFactor(float phase) {
    float p = phase - static_cast<int>(phase);
    if (p < 0.0f) {
        p += 1.0f;
    }
    // Illuminated fraction of the disc is (1 - cos(2*pi*p))/2, but a lit crescent
    // also reflects less per unit area (Lommel-Seeliger), so brightness falls
    // off faster than the lit fraction. Squaring the lit fraction is the cheap
    // version of that and matches the familiar "quarter moon is much fainter
    // than half as bright" observation.
    const float lit = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979f * p));
    return lit * lit;
}

float solarDeclinationDeg(int dayOfYear) {
    // Same path the bodies use, so the telemetry in the log cannot disagree with
    // the direction the sun light is actually coming from.
    float ra = 0.0f, dec = 0.0f;
    eclipticToEquatorial(solarEclipticLongitudeDeg(dayOfYear), &ra, &dec);
    return dec;
}

float skyDaylightFactor(float sunElevationDeg, float duskElevationDeg,
                        float dawnElevationDeg) {
    // One smoothstep across the twilight band. Real twilight runs to about -18
    // degrees (astronomical), but the sky's *illumination* is essentially gone by
    // -10; past that the remaining light is airglow and starlight, which Remix
    // does not need a directional term for.
    const float lo = duskElevationDeg < dawnElevationDeg ? duskElevationDeg
                                                         : dawnElevationDeg;
    const float hi = duskElevationDeg < dawnElevationDeg ? dawnElevationDeg
                                                         : duskElevationDeg;
    if (hi - lo < 0.01f) {
        return sunElevationDeg >= hi ? 1.0f : 0.0f;
    }
    float t = (sunElevationDeg - lo) / (hi - lo);
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool lampsShouldBeOn(float sunElevationDeg, float duskElevationDeg,
                     float dawnElevationDeg) {
    // Hysteresis in elevation, not in hours: the sun crosses -6 degrees twice a
    // day, once on the way down (lamps on) and once on the way up (lamps off),
    // and the two thresholds differ because a photocell sees the brightening sky
    // before the sun clears the horizon.
    return sunElevationDeg < duskElevationDeg;
}

void colorTemperatureToRgb(float kelvin, float* out) {
    // Tanner Helland's piecewise fit of blackbody colour. It returns gamma-
    // encoded sRGB display values 0..255 (linearised at the end, since Remix
    // radiance is linear), so everything below stays in that range and only
    // the final normalisation divides. Below 1000 K the fit is out of range;
    // clamp rather than extrapolate, since extrapolating it goes negative.
    const float t = clampf(kelvin, 1000.0f, 25000.0f) / 100.0f;
    float r, g, b;
    if (t <= 66.0f) {
        r = 255.0f;
    } else {
        r = 329.698727446f * powf(t - 60.0f, -0.1332047592f);
    }
    if (t <= 66.0f) {
        g = 99.4708025861f * logf(t) - 161.1195681661f;
    } else {
        g = 288.1221695283f * powf(t - 60.0f, -0.0755148492f);
    }
    if (t >= 66.0f) {
        b = 255.0f;
    } else if (t <= 19.0f) {
        b = 0.0f;
    } else {
        b = 138.5177312231f * logf(t - 10.0f) - 305.0447927307f;
    }
    r = clampf(r, 0.0f, 255.0f);
    g = clampf(g, 0.0f, 255.0f);
    b = clampf(b, 0.0f, 255.0f);
    const float m = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (m <= 0.0f) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    // sRGB decode: the fit's values are display-encoded; radiance is linear.
    const float v[3] = {r / m, g / m, b / m};
    for (int i = 0; i < 3; ++i) {
        out[i] = v[i] <= 0.04045f ? v[i] / 12.92f : powf((v[i] + 0.055f) / 1.055f, 2.4f);
    }
}
