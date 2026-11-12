#ifndef RHY_TOD_H_
#define RHY_TOD_H_

// s29: the time axis. Everything the sky does is a function of game time, so this
// file owns how game time is turned into a number that is safe to feed a light.
//
// WHY THIS FILE EXISTS
// --------------------
// rhY's #2 complaint (2026-11-12 test, finding 2): "time of day flickers and does
// not progress smoothly". The measurements in .strata/notes-s29-time-of-day-smooth.md
// name three causes, all in the sampling, not in the publishing (s28 already smoothed
// what is published):
//
//   T1 quantisation. The game clock only ever gives WHOLE minutes - one tick every
//      ~2000 ms (measured p50 2015 ms over 320 sample pairs). The sun moves 0.25 deg
//      per game minute, so a whole-minute clock makes the sun a staircase, and the
//      1 Hz geometry pass in sun.cpp is slower than the 2 s tick, so half the passes
//      re-publish a stale value.
//   T2 two calendar sources, alternating. gameDayOfYear() accepted the bytes at
//      0xB70154/0xB70158 whenever they looked like a date. They are not one: the
//      "month" byte walks 25,41,57,...,249,9,25 (+16 per sample) and "day" sits at 3.
//      Every time that counter passes through 1..12 the sanity check passes and the
//      day of year becomes 62/246/337/... instead of the ini's 186: 20 of 409 logged
//      sky samples, and 31 consecutive-sample pairs where the clock moved <=12 minutes
//      but the season changed, swinging the sun's elevation by up to 43.4 deg (median
//      23.7) and straight back on the next sample. That is the flicker: A/B/A/B.
//   T3 a torn read. hour and minute are two separate byte loads. SA rolls the hour
//      when the minute passes 59, so an update landing between the loads reads 6:59 ->
//      7:00 as 6:00: the time walks BACKWARDS 59 minutes for one sample.
//
// On top of that the clock is written by other things: SARemix_Tour.lua calls
// setTimeOfDay() twice per stop and trainers/missions do the same, so 51 of ~400
// consecutive logged samples had the clock go backwards (biggest -1435 game minutes).
//
// THE RULE THIS FILE ENFORCES
// ---------------------------
//   * the published time is CONTINUOUS: whole game minutes plus the milliseconds
//     into the current minute (measured against a self-calibrated ms-per-minute), so
//     the sun is a curve instead of a 0.25 deg staircase;
//   * it never goes BACKWARDS. A backwards raw move inside jitterToleranceMinutes is
//     noise (a script re-pinning the clock, a torn read) and is ignored; a bigger one
//     is a deliberate change of time and is followed only after it has persisted for
//     reanchorMs, which is what turns a tour jump into one re-anchor instead of a
//     oscillation. The only other decrease is the 1440-minute wrap, which bumps the
//     day counter so the absolute minute stays monotone;
//   * a calendar byte stream is only trusted when it is STABLE. A real date does not
//     change between two frames; a counter masquerading as one does.
//
// Pure arithmetic: no windows.h, no Remix, no game pointers, so
// tests/tod_check.cpp can drive a whole simulated 24 h day - weather changes, tour
// clock pins, garbage calendar bytes and a torn read included - and assert the
// per-frame bounds directly.

// ------------------------------------------------------------------ clock

struct RhyTodParams {
    float msPerGameMinute;       // seed for the calibration (SA: ~2000 ms)
    float jitterToleranceMinutes;// backwards moves up to this are noise: hold, do not follow
    unsigned reanchorMs;         // how long a bigger backwards move must persist before we follow it
};

struct RhyTodClock {
    // published
    double minuteOfDay;      // continuous game minutes since midnight, [0, 1440)
    unsigned long long absoluteMinute;  // day*1440 + floor(minuteOfDay): monotone except
                                        // across a deliberate re-anchor (see below)
    // calibration actually in use (measured from the ticks, seeded from the params)
    float msPerGameMinute;
    // diagnostics, all cumulative for the session
    unsigned ticks;          // forward clock ticks accepted
    unsigned tornReads;      // hour/minute pairs repaired (see the header)
    unsigned jitterIgnored;  // backwards moves rejected as noise
    unsigned reanchors;      // deliberate backwards jumps followed
    unsigned wraps;          // midnight crossings
    unsigned badSamples;     // bytes that were not a time at all (hour 24, minute 77)

    // internals
    bool seeded;
    int day;                       // whole days since the first sample
    double anchorMinute;           // unwrapped whole minute the interpolation starts from
    unsigned long long anchorTick; // tick (ms) of that minute
    double dayBase;                // multiple of 1440 mapping a raw minute into unwrapped space
    unsigned long long backSince;  // tick of the first sample of a pending re-anchor
    double backMinute;             // the raw value that is behind us
    int lastMinuteField;           // the raw minute byte of the previous sample
};

void rhyTodInit(RhyTodClock& c, const RhyTodParams& p);

// Change the calibration seed without changing the time: rhyremix.ini can be edited
// while the game runs, and a new ms_per_game_minute must not teleport the sun.
void rhyTodReseed(RhyTodClock& c, const RhyTodParams& p);

// Feed one sample of the raw clock. rawHour 0..23, rawMinute 0..59, tickMs a
// monotonic millisecond clock (GetTickCount() in the plugin, a fake in the test).
// Out-of-range bytes are clamped, not trusted.
void rhyTodUpdate(RhyTodClock& c, int rawHour, int rawMinute,
                  unsigned long long tickMs, const RhyTodParams& p);

// ------------------------------------------------------------- calendar

// The in-game date, accepted only when it behaves like a date. rhyCalendarUpdate()
// returns a day of year (1..366) or 0 when the bytes are not trustworthy, which is
// the caller's signal to use its ini value. neededStable is how many consecutive
// identical (month, day) reads are required; the plugin calls this once a second and
// uses 30, i.e. 30 seconds of a still date. A real SA date holds for a whole game
// day (~48 real minutes at 2000 ms per game minute), while the counter measured in
// the 2026-11-12 log walked +16 every 10 seconds: the window separates them by a
// factor of ~100, and the day-step check below catches anything that slips through.
struct RhyCalendar {
    int month, day;     // the last read pair
    int stable;         // how many consecutive samples gave exactly that pair
    int doy;            // the last accepted day of year (0 = none yet)
    unsigned accepted, rejected;
};

void rhyCalendarInit(RhyCalendar& c);
int rhyCalendarUpdate(RhyCalendar& c, int month, int day, int neededStable);

// ---------------------------------------------------------------- weather

// The only weather inputs this plugin has are ini values (there is no verified
// offset for the game's CWeather, and rtx.ignoreGameDirectionalLights = True means
// the game's own timecycle/weather blend never reaches the renderer anyway). So the
// job here is the BLEND: a weather change must ramp, never step, and the same
// follower is where a real weather read would be plugged in later.
//
// cloud is 0 = clear, 1 = overcast. It attenuates the DIRECT beam only (an
// overcast sun casts no shadows), which is why the shipped default of 0 leaves the
// look byte-identical.
struct RhyWeather {
    float turbidity;   // the smoothed value actually used by the extinction model
    float cloud;       // the smoothed cloud cover actually used
    bool seeded;
};

void rhyWeatherInit(RhyWeather& w, float turbidity, float cloud);

// maxTurbidityPerSec / maxCloudPerSec are the weather's own speed limits: the
// atmosphere does not change in one frame, and neither may the sun's colour.
void rhyWeatherUpdate(RhyWeather& w, float targetTurbidity, float targetCloud,
                      float dtSec, float maxTurbidityPerSec, float maxCloudPerSec);

// Fraction of the direct beam left after the cloud layer. 1 = clear.
float rhyCloudTransmission(float cloud);

// ... and the gain the ambient gets from the same cloud, because the light an
// overcast takes off the beam is scattered into the whole sky rather than lost.
// Exactly 1.0 at cloud = 0, so the shipped default changes nothing.
float rhyCloudAmbientGain(float cloud);

#endif  // RHY_TOD_H_
