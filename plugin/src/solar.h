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

// Illumination fraction of the lunar disc: 0 at new, 1 at full. This is the
// phase angle integral, not a linear ramp, because a half moon is a quarter as
// bright as a full one, not half as bright.
float lunarPhaseFactor(float phase);

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
bool lampsShouldBeOn(float sunElevationDeg, float duskElevationDeg,
                     float dawnElevationDeg);

// Planckian locus -> linear RGB, normalised to max 1. Used for the sun's own
// disc colour when it is high (5772 K) and for the sky ambient. Kim et al. 2002
// piecewise fit, good for 1000..25000 K.
void colorTemperatureToRgb(float kelvin, float* out);

#endif  // RHY_SOLAR_H_
