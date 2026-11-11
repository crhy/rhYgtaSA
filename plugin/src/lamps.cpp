#include "lamps.h"

#include <cmath>
#include <cstring>

#include <strings.h>  // strcasecmp: the ini names are user-typed

#include "solar.h"    // colorTemperatureToRgb for tungsten, a real thermal source

// ------------------------------------------------------------------ spectrum

// Piecewise-linear wavelength -> linear sRGB. The breakpoints are the standard
// ones for this approximation: the visible spectrum runs 380-780 nm, and the
// ramps between the primary anchors (440 blue, 490 cyan, 510 green, 580 yellow,
// 645 red) are what make a spectral line come out the right colour. The
// end-for-intensity term models the eye's sensitivity falling off towards the
// violet and red limits, which is why a 404 nm line reads dim purple rather than
// full-brightness violet.
void rhyWavelengthToRgb(float nm, float* rgb) {
    float r = 0.0f, g = 0.0f, b = 0.0f;

    if (nm >= 380.0f && nm < 440.0f) {
        r = -(nm - 440.0f) / 60.0f;
        b = 1.0f;
    } else if (nm < 490.0f) {
        g = (nm - 440.0f) / 50.0f;
        b = 1.0f;
    } else if (nm < 510.0f) {
        g = 1.0f;
        b = -(nm - 510.0f) / 20.0f;
    } else if (nm < 580.0f) {
        r = (nm - 510.0f) / 70.0f;
        g = 1.0f;
    } else if (nm < 645.0f) {
        r = 1.0f;
        g = -(nm - 645.0f) / 65.0f;
    } else if (nm <= 780.0f) {
        r = 1.0f;
    } else {
        rgb[0] = rgb[1] = rgb[2] = 0.0f;
        return;
    }
    if (nm < 380.0f) {
        rgb[0] = rgb[1] = rgb[2] = 0.0f;
        return;
    }

    float f = 1.0f;
    if (nm < 420.0f) {
        f = 0.30f + 0.70f * (nm - 380.0f) / 40.0f;
    } else if (nm > 700.0f) {
        f = 0.30f + 0.70f * (780.0f - nm) / 80.0f;
        if (f < 0.0f) {
            f = 0.0f;
        }
    }

    rgb[0] = r * f;
    rgb[1] = g * f;
    rgb[2] = b * f;
}

// Integrate across a band. 5 nm steps: the piecewise function is linear between
// its anchors, so this is exact for any band whose endpoints sit on an anchor
// and well under a percent otherwise, for 80 evaluations.
void rhyBandToRgb(float nmLo, float nmHi, float* rgb) {
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    if (nmHi <= nmLo) {
        return;
    }
    const float step = 5.0f;
    const int n = static_cast<int>((nmHi - nmLo) / step + 0.5f);
    if (n <= 0) {
        float t[3];
        rhyWavelengthToRgb(0.5f * (nmLo + nmHi), t);
        rgb[0] = t[0];
        rgb[1] = t[1];
        rgb[2] = t[2];
        return;
    }
    float acc[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i <= n; ++i) {
        float t[3];
        rhyWavelengthToRgb(nmLo + (nmHi - nmLo) * (static_cast<float>(i) /
                                                   static_cast<float>(n)), t);
        acc[0] += t[0];
        acc[1] += t[1];
        acc[2] += t[2];
    }
    rgb[0] = acc[0] / static_cast<float>(n + 1);
    rgb[1] = acc[1] / static_cast<float>(n + 1);
    rgb[2] = acc[2] / static_cast<float>(n + 1);
}

// Unnormalised accumulation, so a colour can be a mixture of lines and bands
// (a white LED is exactly that: a blue pump line plus a phosphor continuum).
static void accumLines(float* acc, const LampLine* lines, int count, float weight) {
    for (int i = 0; i < count; ++i) {
        float t[3];
        rhyWavelengthToRgb(lines[i].nm, t);
        acc[0] += t[0] * lines[i].rel * weight;
        acc[1] += t[1] * lines[i].rel * weight;
        acc[2] += t[2] * lines[i].rel * weight;
    }
}

static void normalize(float* rgb) {
    const float m = rgb[0] > rgb[1] ? (rgb[0] > rgb[2] ? rgb[0] : rgb[2])
                                    : (rgb[1] > rgb[2] ? rgb[1] : rgb[2]);
    if (m > 1e-6f) {
        rgb[0] /= m;
        rgb[1] /= m;
        rgb[2] /= m;
    }
}

void rhyLinesToRgb(const LampLine* lines, int count, float* rgb) {
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    accumLines(rgb, lines, count, 1.0f);
    normalize(rgb);
}

// ------------------------------------------------------------------ families

// Emission lines. The wavelengths are the textbook atomic lines; the relative
// intensities are the usual low-pressure ordering, which is what determines the
// perceived colour. Precision on the intensities is not the point - the point is
// that the colour comes from where the lines ARE, and a Planck curve at the same
// correlated temperature puts energy in completely the wrong places.
// CIE 1931 chromaticity -> linear sRGB (IEC 61966-2-1 matrix), out-of-gamut
// channels clamped at zero, brightest channel normalised to 1. Linear is what
// Remix's radiance expects; gamma-encoded display values would look washed out.
void rhyXyToLinearRgb(float x, float y, float* rgb) {
    const float Y = 1.0f, X = x / y, Z = (1.0f - x - y) / y;
    float r = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
    float g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
    float b = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;
    r = r < 0.0f ? 0.0f : r; g = g < 0.0f ? 0.0f : g; b = b < 0.0f ? 0.0f : b;
    const float m = r > g ? (r > b ? r : b) : (g > b ? g : b);
    rgb[0] = m > 0.0f ? r / m : 1.0f;
    rgb[1] = m > 0.0f ? g / m : 1.0f;
    rgb[2] = m > 0.0f ? b / m : 1.0f;
}

void rhyLampFamilyRgb(int family, float* rgb) {
    // Measured chromaticities (CIE 1931 x, y) of each lamp family, as published
    // in lighting references, converted to the linear sRGB primaries Remix
    // renders in. A spectral line model needs proper colour-matching functions
    // AND the real line shapes to land on these: high-pressure sodium in
    // particular is a broad, self-reversed band from ~540 to ~650 nm, not the D
    // doublet, and the earlier line sum came out far too yellow-green.
    switch (family) {
        case RHY_LAMP_HPS:          rhyXyToLinearRgb(0.530f, 0.415f, rgb); return;  // ~2050 K, deep orange
        case RHY_LAMP_MERCURY:      rhyXyToLinearRgb(0.326f, 0.390f, rgb); return;  // clear mercury, green-white
        case RHY_LAMP_METAL_HALIDE: rhyXyToLinearRgb(0.380f, 0.380f, rgb); return;  // ~4000 K, near neutral
        case RHY_LAMP_LED:          rhyXyToLinearRgb(0.346f, 0.359f, rgb); return;  // 5000 K roadway LED
        case RHY_LAMP_TUNGSTEN:     rhyXyToLinearRgb(0.4476f, 0.4074f, rgb); return;  // CIE illuminant A, 2856 K
        case RHY_LAMP_NEON_RED:     rhyXyToLinearRgb(0.660f, 0.330f, rgb); return;
        default: rgb[0] = rgb[1] = rgb[2] = 1.0f; return;
    }
}

const char* rhyLampFamilyName(int family) {
    switch (family) {
        case RHY_LAMP_HPS:          return "hps";
        case RHY_LAMP_MERCURY:      return "mercury";
        case RHY_LAMP_METAL_HALIDE: return "halide";
        case RHY_LAMP_LED:          return "led";
        case RHY_LAMP_TUNGSTEN:     return "tungsten";
        case RHY_LAMP_NEON_RED:     return "neon";
        default:                    return "unknown";
    }
}

int rhyLampFamilyFromName(const char* name) {
    if (!name) {
        return -1;
    }
    for (int f = 0; f < RHY_LAMP_COUNT; ++f) {
        if (strcasecmp(name, rhyLampFamilyName(f)) == 0) {
            return f;
        }
    }
    // Common aliases, because the point of the name is that a user can type the
    // word a spark catalogue uses.
    if (!strcasecmp(name, "sodium") || !strcasecmp(name, "hps_na")) return RHY_LAMP_HPS;
    if (!strcasecmp(name, "mv") || !strcasecmp(name, "mercury_vapor")) return RHY_LAMP_MERCURY;
    if (!strcasecmp(name, "mh") || !strcasecmp(name, "metal_halide")) return RHY_LAMP_METAL_HALIDE;
    if (!strcasecmp(name, "incandescent") || !strcasecmp(name, "filament")) return RHY_LAMP_TUNGSTEN;
    return -1;
}

// ------------------------------------------------------------------ physics

float rhyRadianceForFlux(float radianceAtRef, float radius, float radiusRef) {
    if (radius <= 0.0f || radiusRef <= 0.0f) {
        return radianceAtRef;
    }
    const float k = radiusRef / radius;
    return radianceAtRef * k * k;
}

void rhyStreetFixture(int band, RhyFixture* out) {
    // One GTA SA unit is about one metre: measured from the traffic signal data,
    // where a signal head sits 7.6 units above its road plane, and a real signal
    // head hangs about 6 m up. So the numbers below are metres.
    //
    // A cobra-head roadway luminaire housing is roughly 0.50 m long by 0.25 m
    // wide by 0.10 m deep. Representing that as a sphere gives about 0.2 m
    // radius. The shipped profile used 15 - seventy-five times too big, which is
    // the difference between a lamp and a glowing weather balloon.
    out->what = "street";
    out->radius = 0.20f;
    out->coneDeg = 125.0f;
    out->coneSoftness = 0.45f;
    if (band == 1) {
        out->radius = 0.15f;
        out->coneDeg = 120.0f;
    } else if (band >= 2) {
        // Pedestrian and residential post-top luminaires: smaller head, and a
        // wider, softer distribution because they light a footway, not a road.
        out->radius = 0.10f;
        out->coneDeg = 140.0f;
        out->coneSoftness = 0.60f;
    }
}

void rhySignalFixture(RhyFixture* out) {
    // A traffic signal module lens is 200 mm (8 in) or 300 mm (12 in) diameter;
    // the ones in San Andreas are the small type, so 0.1 m radius. The shipped
    // value was 6 game units = 6 m, which is the size of a bus.
    //
    // The cone stays wide on purpose. A signal lens is a refractor: the visor
    // and the lens together are designed to spread the beam over a wide viewing
    // angle so the signal is readable from the side of the intersection. A
    // narrow cone is the opposite of what the hardware does.
    out->what = "signal";
    out->radius = 0.10f;
    out->coneDeg = 150.0f;
    out->coneSoftness = 0.35f;
}

float rhyStreetRadius(int band, float fixtureScale) {
    RhyFixture f;
    rhyStreetFixture(band, &f);
    return f.radius * (fixtureScale > 0.0f ? fixtureScale : 1.0f);
}

float rhySignalRadius(float fixtureScale) {
    RhyFixture f;
    rhySignalFixture(&f);
    return f.radius * (fixtureScale > 0.0f ? fixtureScale : 1.0f);
}

void rhySignalLensRgb(int phase, float* rgb) {
    // Signal colours are specified as chromaticity regions (ITE VTCSH / CIE
    // S 004); these are typical LED signal heads near the middle of each region.
    // The green (x 0.17, y 0.70) is a blue-green outside the sRGB gamut;
    // clamping would make it pure green and lose the blue that makes a signal
    // green read as one, so it is moved toward white along its own hue to the
    // gamut edge (x 0.262, y 0.461), which keeps the blue-green.
    switch (phase) {
        case 0:  rhyXyToLinearRgb(0.2618f, 0.4614f, rgb); break;  // green
        case 1:  rhyXyToLinearRgb(0.575f, 0.420f, rgb); break;  // amber
        default: rhyXyToLinearRgb(0.690f, 0.305f, rgb); break;  // red
    }
}
