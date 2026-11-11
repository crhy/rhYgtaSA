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

    // ---- the reason this module exists: sodium is not a 2000 K blackbody ----
    float hps[3], planck2000[3];
    rhyLampFamilyRgb(RHY_LAMP_HPS, hps);
    colorTemperatureToRgb(2000.0f, planck2000);
    show("high-pressure sodium", hps);
    show("Planck at 2000 K (wrong)", planck2000);
    ok("HPS has almost no blue", hps[2] < 0.10f);
    ok("HPS is yellow-orange: green channel high", hps[1] > 0.75f);
    // The decisive difference. A 2000 K Planck curve puts a lot of energy in the
    // deep red and comparatively little in the green, so it reads as a dim
    // amber. Sodium puts nearly everything at 589 nm, which is yellow. If this
    // assertion ever starts failing the line list has been broken.
    ok("HPS is much greener than a 2000 K Planck curve",
       hps[1] > planck2000[1] + 0.20f);

    float mv[3];
    rhyLampFamilyRgb(RHY_LAMP_MERCURY, mv);
    show("mercury vapour", mv);
    ok("mercury reads cool: blue channel strong", mv[2] > 0.55f);
    ok("mercury reads greenish: the 546 nm line is visible", mv[1] > 0.60f);
    ok("mercury is not neutral white (red channel clearly lower)",
       mv[0] < mv[1] - 0.10f);

    float mh[3], led[3], w[3];
    rhyLampFamilyRgb(RHY_LAMP_METAL_HALIDE, mh);
    rhyLampFamilyRgb(RHY_LAMP_LED, led);
    show("metal halide", mh);
    show("white LED", led);
    ok("metal halide is near neutral (channels within 0.35 of each other)",
       fabsf(mh[0] - mh[1]) < 0.35f && fabsf(mh[1] - mh[2]) < 0.35f);
    ok("white LED still carries a blue pump peak", led[2] > 0.45f);

    rhyLampFamilyRgb(RHY_LAMP_TUNGSTEN, w);
    show("tungsten 2856 K", w);
    ok("tungsten is warm and red-dominant", w[0] > 0.9f && w[2] < 0.50f);

    // ---- traffic signal lenses ----
    float grn[3], amb[3], red[3];
    rhySignalLensRgb(0, grn);
    rhySignalLensRgb(1, amb);
    rhySignalLensRgb(2, red);
    show("signal green lens", grn);
    show("signal amber lens", amb);
    show("signal red lens", red);
    ok("signal green is green-dominant with little red",
       grn[1] > 0.95f && grn[0] < 0.65f);
    ok("signal amber sits between: strong red and strong green, no blue",
       amb[0] > 0.9f && amb[1] > 0.55f && amb[2] < 0.10f);
    ok("signal red is red-dominant and saturated",
       red[0] > 0.95f && red[1] < 0.45f && red[2] < 0.10f);
    ok("the three lenses are mutually distinguishable in green",
       grn[1] > amb[1] + 0.2f && amb[1] > red[1] + 0.2f);
    // A single 620 nm sample would give orange, not red. This is the assertion
    // that justifies integrating a band instead of sampling a wavelength.
    float single620[3];
    rhyWavelengthToRgb(620.0f, single620);
    ok("band integration is what makes the red lens red (a 620 nm sample is more orange)",
       red[1] < single620[1]);

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
