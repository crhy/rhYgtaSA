#ifndef RHY_LAMPS_H_
#define RHY_LAMPS_H_

// Physical data for the small artificial light sources: street luminaires,
// traffic signal lenses, shopfront glazing.
//
// ---------------------------------------------------------------------------
// WHY THIS IS NOT A COLOUR TEMPERATURE DIAL
// ---------------------------------------------------------------------------
// Almost every street and signal lamp in San Andreas is a GAS DISCHARGE lamp,
// and a gas discharge lamp does not have a colour temperature in any useful
// sense. Its spectrum is a handful of narrow atomic emission lines. A Planck
// curve at the same correlated colour temperature spreads energy across the
// whole visible band, so it gets the colour wrong in a way you can see:
//
//   high-pressure sodium: the sodium D doublet at 589.0 and 589.6 nm carries
//     most of the flux. It looks yellow-orange. A 2000 K Planck curve - which
//     is its *correlated* temperature - puts a third of its energy below 650 nm
//     in the deep red, and real sodium lamps have almost none there. That is
//     why "just set kelvin=2000" makes sodium look like a dim incandescent.
//   mercury vapour: lines at 404.7, 435.8, 546.1 and 577-579 nm. The 546.1 nm
//     green line is strong, which is why mercury street lighting reads
//     greenish-white with a magenta cast, not the neutral 6500 K white a
//     temperature dial produces.
//
// So the colours here are built from the documented emission lines of each lamp
// type, integrated through the visible spectrum, rather than picked by eye.
//
// ---------------------------------------------------------------------------
// WHY SIZE AND BRIGHTNESS ARE SEPARATE NUMBERS
// ---------------------------------------------------------------------------
// A sphere light's total output is radiance x area. The shipped profile used
// radius = 15 game units, and one GTA SA unit is about one metre, so that is a
// 30 metre glowing ball - which is why the light pools read as shapeless haze
// and why volumetrics turn into a fog wall. The radius was never physical; it
// was standing in for brightness, because shrinking it would have made the lamp
// go dark.
//
// The fix is to stop coupling them: radiance is scaled by (radius_ref/radius)^2
// so the fixture can be the size it actually is without changing how much light
// it puts out. See radianceForFlux().

// One emission line: wavelength in nanometres and relative intensity.
struct LampLine {
    float nm;
    float rel;
};

// Linear sRGB for a single wavelength, using the standard piecewise-linear
// approximation to the visible spectrum (380-780 nm). Not a substitute for the
// CIE colour-matching functions, but it reproduces the qualitative colour of a
// spectral line, which is all the line-spectrum model below needs. Out of range
// returns black. Values are linear, not gamma-encoded, and are NOT normalised:
// the green channel peaks at 1.
void rhyWavelengthToRgb(float nm, float* rgb);

// Linear sRGB of a rectangular spectral band, by integrating the above across
// [nmLo, nmHi]. Used for traffic signal lenses, which are wide coloured filters
// rather than line sources - integrating is what makes a 610-640 nm red lens
// come out red instead of orange.
void rhyBandToRgb(float nmLo, float nmHi, float* rgb);

// Linear sRGB of a set of emission lines weighted by relative intensity,
// normalised so the strongest channel is 1.
void rhyLinesToRgb(const LampLine* lines, int count, float* rgb);

// The real lamp families found in San Andreas street lighting.
enum RhyLampFamily {
    RHY_LAMP_HPS = 0,        // high-pressure sodium: orange, the classic SA street lamp
    RHY_LAMP_MERCURY,        // mercury vapour: greenish cool white
    RHY_LAMP_METAL_HALIDE,   // metal halide: neutral white, newer installations
    RHY_LAMP_LED,            // white LED: blue pump plus phosphor continuum
    RHY_LAMP_TUNGSTEN,       // incandescent: interiors, older signage
    RHY_LAMP_NEON_RED,       // signage / open neon
    RHY_LAMP_FLUORESCENT,    // tri-phosphor tube: the white globe posts n11 found
    RHY_LAMP_COUNT
};

// Colour of a lamp family, linear sRGB normalised so the strongest channel is 1.
void rhyXyToLinearRgb(float x, float y, float* rgb);  // CIE 1931 xy -> linear sRGB, max 1
void rhyLampFamilyRgb(int family, float* rgb);
const char* rhyLampFamilyName(int family);

// Map a family name from rhyremix.ini ("hps", "mercury", ...) to the enum.
// Returns -1 when the name is not recognised.
int rhyLampFamilyFromName(const char* name);

// ---------------------------------------------------------------------------

// Flux-preserving radiance. `radianceAtRef` is whatever was calibrated at
// `radiusRef` game units; the returned value keeps the total emitted power the
// same when the emitting sphere is resized to `radius`.
//
// A sphere's area goes as r^2, so radiance has to go as 1/r^2. Without this,
// making a lamp the size of a real luminaire head (about 0.3 m, not 15 m) would
// cut its output by a factor of about 2500 and the street would go black, which
// is exactly why nobody had fixed the radius before.
float rhyRadianceForFlux(float radianceAtRef, float radius, float radiusRef);

// Real fixture geometry, in GTA SA game units (1 unit ~ 1 metre).
struct RhyFixture {
    float radius;         // emitting sphere radius
    float coneDeg;        // full shaping cone angle, UsdLux convention
    float coneSoftness;   // 0 = hard edge, 1 = fully soft
    const char* what;     // for the log line
};

// Street luminaire by size band (0 = big cobra-head, 1 = middle, 2 = small).
// A cobra-head housing is roughly 0.50 m x 0.25 m; as a sphere that is about
// 0.2 m radius. The cone is wide because a street luminaire is deliberately
// near-Lambertian over the roadway: cutting it to a narrow cone is what makes a
// lamp look like a spotlight instead of a street lamp.
void rhyStreetFixture(int band, RhyFixture* out);

// Traffic signal lens. Real signal lenses are 200 mm (8 in) or 300 mm (12 in)
// diameter; SA uses the small ones. The cone is wide because a signal lens is
// a refractor designed to spread light over a wide viewing angle - that is the
// whole point of the visor-and-lens assembly.
void rhySignalFixture(RhyFixture* out);

// Effective emitting-sphere radius in game units, with the ini's fixture_scale
// applied. These exist so the plugin and the config log cannot disagree about
// how big a light is.
float rhyStreetRadius(int band, float fixtureScale);
float rhySignalRadius(float fixtureScale);

// ---------------------------------------------------------------------------
// n11: the lamp data the GAME carries (2dFx), and the head that has to glow
// ---------------------------------------------------------------------------
// The lamp list v2 (tools/lamp_list.py --format v2) carries, for every light,
// the colour the model's own 2dFx record stores and the family the fitting
// belongs to. These two functions decide how the two combine, and what the
// visible head costs.

// sRGB byte (0..255) -> linear. The 2dFx colour bytes are display values: the
// game's own light code reads them straight off the record, but Remix wants
// linear radiance. Converting is what makes the cross-check below work at all.
float rhySrgbToLinear(float v01);

// The colour of one 2dFx lamp, linear sRGB with the strongest channel at 1.
//
//   a chromatic 2dFx colour wins. Measured, the game's own sodium lamps and the
//     published HPS chromaticity agree to a few percent once the bytes are
//     linearised: lamppost3 records (255,148,52) -> linear (1.00, 0.297, 0.034),
//     rhyLampFamilyRgb(RHY_LAMP_HPS) is (1.00, 0.254, 0.003). Two independent
//     sources - a raster author and a lighting reference - naming the same lamp.
//   an achromatic or missing colour falls back to the family, because a bulb
//     recorded as plain white says nothing about its spectrum: that is exactly
//     the case where "fluorescent" vs "mercury" vs "tungsten" has to be decided
//     by the fitting instead.
void rhyLampTintedColour(int family, const int* rgb255, float* rgb);

// The head of the fitting: the part you can see, as opposed to the luminaire
// light that lights the road. A Remix light is not visible geometry (see the
// n02b finding: "a point light is not visible geometry, it only lights what is
// near it"), so the head gets its OWN small unshaped light at the bulb, the same
// trick n02b shipped for signal lenses: 1200 nits in a 0.10 m sphere makes the
// housing around it read as a lit bulb, which is what a 0.20 m downward cone
// aimed at the road never does.
void rhyLampHeadFixture(int band, RhyFixture* out);

// Radiance of that head light. `nits` is the head's own luminance, applied at
// its own radius: deliberately NOT rhyRadianceForFlux(). That function exists so
// a shrunken STREET luminaire keeps lighting the street; applied to a head it
// turns a bulb into a flood lamp (the same mistake s30 diagnosed for signals).
float rhyLampHeadRadiance(float nits, float radius, float radianceScale);

// The head light is a near-field light: a 0.1 m globe is a couple of pixels at
// 100 m and nothing at 300, so it is created inside a smaller radius than the
// luminaire. Clamped here so an ini cannot ask for a bulb that reaches further
// than the lamp it belongs to (and the exit radius keeps the n04 hysteresis).
void rhyLampHeadRadius(float want, float exitWant, float maxDistance,
                       float* nearOut, float* exitOut);

// Traffic signal lens colours, from the lens passbands rather than from a
// temperature: red glass passes roughly 610-640 nm, the amber lens 585-605 nm,
// and the green lens 515-565 nm. Linear sRGB, strongest channel normalised to 1.
void rhySignalLensRgb(int phase, float* rgb);  // 0 green, 1 amber, 2 red

#endif  // RHY_LAMPS_H_
