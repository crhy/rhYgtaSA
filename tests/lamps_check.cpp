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
#include <cstring>
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

    // ---- n11: the 2dFx colour, the family behind it, and the head ----
    // The 2dFx colour bytes are display values (a raster author wrote them); the
    // Remix API wants linear radiance. Without the conversion the game's sodium
    // orange and the published HPS chromaticity do not agree, and the agreement
    // is the cross-check that both sources describe the same lamp.
    ok("sRGB 0 maps to 0 and 1 maps to 1",
       rhySrgbToLinear(0.0f) == 0.0f && fabsf(rhySrgbToLinear(1.0f) - 1.0f) < 1e-6f);
    ok("sRGB 0.5 linearises to the IEC 61966 value 0.2140",
       fabsf(rhySrgbToLinear(0.5f) - 0.2140f) < 2e-3f);
    ok("the lineariser is monotonic", rhySrgbToLinear(0.2f) < rhySrgbToLinear(0.5f) &&
       rhySrgbToLinear(0.5f) < rhySrgbToLinear(0.9f));

    {
        // lamppost3.dff's own 2dFx record, read out of gta3.img by
        // tools/dff_2dfx.py: (255,148,52) at (-1.40,0.22,3.67).
        int rec[3] = {255, 148, 52};
        float c[3];
        rhyLampTintedColour(RHY_LAMP_HPS, rec, c);
        std::printf("    2dFx (255,148,52) -> linear %.3f,%.3f,%.3f\n", c[0], c[1], c[2]);
        ok("a chromatic 2dFx colour wins, normalised to a peak of 1",
           fabsf(c[0] - 1.0f) < 1e-6f && c[1] > 0.25f && c[1] < 0.35f && c[2] < 0.06f);
        float fam[3];
        rhyLampFamilyRgb(RHY_LAMP_HPS, fam);
        ok("the game's sodium colour and the published HPS spectrum agree (the cross-check)",
           fabsf(c[1] - fam[1]) < 0.08f && fabsf(c[2] - fam[2]) < 0.08f);

        // streetlamp1, streetlamp2, lampost_coast and bollardlight all record
        // plain white: bytes that carry no spectrum at all. That is exactly the
        // case the fitting has to decide, and it is why the family exists.
        int white[3] = {255, 255, 255};
        float a[3], fl[3];
        rhyLampTintedColour(RHY_LAMP_FLUORESCENT, white, a);
        rhyLampFamilyRgb(RHY_LAMP_FLUORESCENT, fl);
        ok("white 2dFx bytes fall back to the family's spectrum",
           fabsf(a[0] - fl[0]) < 1e-6f && fabsf(a[1] - fl[1]) < 1e-6f &&
           fabsf(a[2] - fl[2]) < 1e-6f);
        ok("fluorescent is a cool white, not a sodium orange",
           fl[1] > 0.8f && fl[2] > 0.6f);
        float h[3];
        rhyLampTintedColour(RHY_LAMP_HPS, white, h);
        ok("the same white bytes under another family give that family's colour",
           fabsf(h[1] - fam[1]) < 1e-6f);
        int none[3] = {0, 0, 0};
        float z[3], tu[3];
        rhyLampTintedColour(RHY_LAMP_TUNGSTEN, none, z);
        rhyLampFamilyRgb(RHY_LAMP_TUNGSTEN, tu);
        ok("a v1 line with no colour at all also falls back to the family",
           fabsf(z[1] - tu[1]) < 1e-6f);
        // traincross1 records pure red: a saturated colour that is not a lamp
        // spectrum at all, and it must survive as red rather than being averaged
        // towards the family.
        int red[3] = {255, 0, 0};
        float rr[3];
        rhyLampTintedColour(RHY_LAMP_NEON_RED, red, rr);
        ok("a saturated 2dFx colour is kept, not pulled towards the family",
           rr[0] > 0.99f && rr[1] < 0.01f && rr[2] < 0.01f);
    }
    ok("'fluorescent' is a family name the ini can use",
       rhyLampFamilyFromName("fluorescent") == RHY_LAMP_FLUORESCENT &&
       !std::strcmp(rhyLampFamilyName(RHY_LAMP_FLUORESCENT), "fluorescent"));

    {
        // The head: the bulb you can see, as opposed to the housing that lights
        // the road. It has to be small (a bulb) and unshaped (a bulb shines every
        // way, and the downward cone is why a head reads dark from the side).
        RhyFixture big, mid, small, housing;
        rhyLampHeadFixture(0, &big);
        rhyLampHeadFixture(1, &mid);
        rhyLampHeadFixture(2, &small);
        rhyStreetFixture(1, &housing);
        std::printf("    head radii: %.2f / %.2f / %.2f m (housing band 1 is %.2f m)\n",
                    big.radius, mid.radius, small.radius, housing.radius);
        ok("the head grows with the fitting band", big.radius > mid.radius &&
           mid.radius > small.radius);
        ok("a head is a bulb: centimetres, not metres", big.radius > 0.03f && big.radius < 0.30f);
        ok("a head is unshaped (coneDeg 360 is the marker the caller reads)",
           big.coneDeg >= 360.0f);
        ok("the head is not bigger than the housing it sits in",
           big.radius <= housing.radius);
    }
    {
        // The head carries NITS at its own radius. It is deliberately not
        // flux-preserved, and the size of that difference is the s30 lesson: a
        // flux-preserving rescale of a 0.1 m bulb to the 15 unit reference is a
        // flood lamp, not a bulb.
        const float nits = 1200.0f;
        ok("the head radiance is the nits the ini asked for",
           fabsf(rhyLampHeadRadiance(nits, 0.10f, 1.0f) - nits) < 1e-3f);
        ok("a luminance does not change when the bulb size does",
           fabsf(rhyLampHeadRadiance(nits, 0.07f, 1.0f) - nits) < 1e-3f);
        const float fluxed = rhyRadianceForFlux(nits, 0.10f, 15.0f);
        std::printf("    head %.0f nits; the same number flux-preserved to r=15: %.0f "
                    "(x%.0f)\n", nits, fluxed, fluxed / nits);
        ok("flux-preserving a bulb would raise it by more than two orders of magnitude",
           fluxed > 100.0f * nits);
        ok("radiance_scale still scales the head",
           fabsf(rhyLampHeadRadiance(nits, 0.1f, 0.5f) - 0.5f * nits) < 1e-3f);
        ok("head_radiance=0 is off, not a default or a negative",
           rhyLampHeadRadiance(0.0f, 0.1f, 1.0f) == 0.0f);
    }
    {
        float n = 0.0f, e = 0.0f;
        rhyLampHeadRadius(120.0f, 160.0f, 300.0f, &n, &e);
        ok("the shipped head band passes through unchanged", n == 120.0f && e == 160.0f);
        rhyLampHeadRadius(400.0f, 500.0f, 300.0f, &n, &e);
        ok("a bulb can never reach past the lamp it belongs to", n <= 300.0f && e <= 380.0f);
        rhyLampHeadRadius(120.0f, 100.0f, 300.0f, &n, &e);
        ok("an exit radius narrower than the entry radius is widened, not inverted", e > n);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
