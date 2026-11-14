#ifndef RHY_SOLAR_H_
#define RHY_SOLAR_H_

// Physically-grounded sun/moon geometry and atmospheric extinction.
//
// Why this exists: the first version of sun.cpp turned the sun 360 degrees per
// 1440 game minutes on a fixed plane and faded its radiance with sin(elevation)
// to the power fade_pow. That gets the shadow direction roughly right and
// nothing else: no season, no latitude, no sunrise in the east, and a sunset
// that is simply "dimmer" instead of red. Real sunlight at the horizon is both
// much dimmer AND much redder because it crosses tens of times more air, and
// that is Beer-Lambert extinction, not a power curve.
//
// Everything here is plain arithmetic on the standard astronomical
// approximations (Spencer's Fourier series for declination and the equation of
// time, the NOAA hour-angle construction for altitude/azimuth, Kasten-Young for
// relative air mass). No tables, no system clock, no floating point beyond
// float32 - it runs once per sky update inside a 32-bit game process.

// Observer location. GTA SA's map stands in for coastal Southern California, so
// the defaults are Los Angeles' coordinates; they are ini-configurable because a
// mod user may want the sun higher or lower for looks.
struct SkyObserver {
    float latitudeDeg;    // +north
    float longitudeDeg;   // +east (Los Angeles is -118.24)
    int   dayOfYear;      // 1..366, sets the solar declination (the season)
    float timezoneHours;  // local standard offset from UTC, LA = -8
    float turbidity;      // aerosol extinction beta at 550 nm; 0.08 clean, 0.25 smoggy
};

// One celestial body as seen by that observer.
struct SkyBody {
    // Unit vector from the observer TO the body, in GTA world axes:
    // x = east, y = north, z = up. A light's direction is the NEGATION of this,
    // because a light points where its photons travel.
    float toX, toY, toZ;
    float elevationDeg;   // degrees above the horizon; negative = below it
    float azimuthDeg;     // degrees clockwise from north
    float airMass;        // Kasten-Young relative air mass, 1 at zenith, 0 below horizon
    float transmit[3];    // fraction of the light left after the air, per channel R,G,B
    bool  above;          // elevation > 0
};

// Sun position at local clock time. `minuteOfHour` is unused precision-wise but
// kept so callers pass the real game minute.
void solarPosition(const SkyObserver& ob, int hour, int minute, SkyBody* out);

// The same position at a CONTINUOUS local clock time, in game minutes from
// midnight (0..1440, fractional). s29: the game clock only ticks whole minutes
// (one every ~2000 ms), so the int form above makes the sun a 0.25 degree
// staircase; plugin/src/tod.cpp turns the same bytes into a continuous minute and
// the sky module evaluates the bodies with this form every frame.
void solarPositionF(const SkyObserver& ob, float minuteOfDay, SkyBody* out);

// The moon, modelled as a second body on the SAME diurnal circle, offset in
// hour angle by (phase - 0.5) * 360 degrees. phase 0.5 (full) therefore puts it
// exactly opposite the sun - rising at sunset, highest at midnight - and phase
// 0.25 (first quarter) puts it 6 hours ahead of the sun, i.e. up through the
// afternoon and setting around midnight. That reproduces moonrise/moonset times
// to within the accuracy anyone can see, without a full ecliptic/lunar-orbit
// solution (which would need the moon's 5 degree orbital inclination and an
// 18.6 year node cycle to improve on).
void lunarPosition(const SkyObserver& ob, int hour, int minute, float phase,
                   SkyBody* out);

// Continuous-time form, see solarPositionF.
void lunarPositionF(const SkyObserver& ob, float minuteOfDay, float phase,
                    SkyBody* out);

// Illumination fraction of the lunar disc: 0 at new, 1 at full. This is the
// phase angle integral, not a linear ramp, because a half moon is a quarter as
// bright as a full one, not half as bright.
float lunarPhaseFactor(float phase);

// ---------------------------------------------------------------- n24: one real source

// Clamp a distant light's direction so it can never point upward.
//
// A distant light's direction is the direction its photons travel, i.e. the NEGATION
// of the vector to the body: a body 30 degrees up gives dir z = -0.50 (down, right),
// a body 30 degrees BELOW the horizon gives dir z = +0.50 - a light shining up from
// under the ground. Nothing in the renderer refuses that: it lights the undersides of
// everything with nothing above them.
//
// Measured in the 2026-11-12 log (rhyremix.log 11:48:30): a sun handle was published
// with radiance 0.0376 and dir=0.2206,-0.8386,+0.4981 while the sun sat at -31.6 deg.
// The radiance ramp-down in smooth.cpp keeps a handle alive for tens of seconds after
// the body sets, and a glitched clock read can put the body under the horizon while
// the published radiance is still coming down. The ramp is a feature (rule: lighting
// must never change instantly), so what has to go is the direction: a below-horizon
// body grazes from the horizon while it dims, it never lights from underneath.
//
// dir is modified in place and renormalized. A direction that was exactly vertical (a
// body at the nadir) has no horizontal part left to keep; it becomes due east - as
// arbitrary as it is unreachable in practice, and deterministic.
void solarClampLightDir(float dir[3]);

// Ceiling on the sky's ambient term at night.
//
// rtx.skyBrightness multiplies the sky probe, whose radiance is <= 1.0 per channel
// because it is rasterized in the game's colour format (rtx.skyForceHDR = False), so
// skyBrightness is also the most the sky can put on the ground. rhY's inversion
// ("the sky is black, the ground is bright - light from no source") is that ceiling
// sitting above the one real source there is at night: measured today, the published
// ambient is [sky] night 0.02 + moon 0.05 = 0.0700 against a moon light publishing
// 0.0669 at 31 degrees elevation, i.e. an ambient ceiling 1.05x the moon's own
// radiance and 1.36x what the moon actually delivers on level sand (0.0515), and 57.6 %
// of the radiance on open sand comes from "sky".
//
// Once the sun is properly down (daylight <= 0.05 of the twilight ramp) the ambient may
// not exceed capMultiple x the moon light's published radiance. Day and twilight pass
// through untouched: there the sun owns the ambient, and clamping against a moon that
// is a dim disc in a bright sky would only darken dusk. capMultiple <= 0 switches the
// rule off ([sky] ambient_cap_mult; 0.25 is what ships, see docs/FINDINGS.md).
//
// The cap is a CEILING, not a prediction: the probe is usually darker than 1.0, so the
// real ambient is usually below the cap. What the cap guarantees is that it can never
// exceed a fifth of the moon's radiance, and that it follows the moon's altitude
// (0.0068 at 10 deg, 0.0167 at 31 deg, 0.0204 at 60 deg) instead of the clock.
//
// A moon that is not up is not a source: with moonElevationDeg <= 0 the cap goes to 0
// and the caller's floor (0.001, for the denoiser) is all that is left. That is the
// point of the rule, and it is why the moonless, lampless night is dark. The same
// happens with [moon] enabled=0 - the caller passes the moon light's own radiance, so
// turning the moon off also takes away the thing the ambient is allowed to be a
// fraction of. That is the rule, not an accident: with no moon and no lamps there is
// nothing in the sky to be an ambient.
float skyAmbientCap(float skyAmbient, float moonRadianceLum, float daylight,
                    float moonElevationDeg, float capMultiple);

// Solar declination in degrees for a day of year (sanity/telemetry only).
float solarDeclinationDeg(int dayOfYear);

// 0 at night, 1 at day, smooth through twilight. `sunElevationDeg` is the
// body's own elevation; civil twilight is -6 degrees and nautical -12, so the
// default band -6..+4 covers the real window in which street lamps are already
// on while the sky is still bright.
float skyDaylightFactor(float sunElevationDeg, float duskElevationDeg,
                        float dawnElevationDeg);

// True when a street lamp would be alight: the sun is below `duskElevationDeg`.
// Real photocells switch around civil twilight (sun about -4 to -6 degrees),
// not at a wall-clock hour, which is why "on_hour=19" is wrong in both
// directions across a year.
// One threshold on purpose: the test is stateless, so it cannot honour the
// higher *rising* threshold that `[streetlights] dawn_deg` describes (that needs
// the previous lamps-on state). The ini key is still parsed, sanity-checked and
// logged, but no switching rule reads it - see the note in config.h.
bool lampsShouldBeOn(float sunElevationDeg, float duskElevationDeg);

// Planckian locus -> linear RGB, normalised to max 1. Used for the sun's own
// disc colour when it is high (5772 K) and for the sky ambient. Kim et al. 2002
// piecewise fit, good for 1000..25000 K.
void colorTemperatureToRgb(float kelvin, float* out);

#endif  // RHY_SOLAR_H_
