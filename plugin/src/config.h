#ifndef RHY_CONFIG_H_
#define RHY_CONFIG_H_

#include <string>

// Settings read from "rhyremix.ini" next to the .asi (written with these
// defaults when the file does not exist yet).
struct RhyConfig {
    int enabled;            // 0 = streetlights feature off
    std::string listPath;   // resolved against the module folder if relative
    int max_distance;       // metres, 2D: a NEW lamp gets a light inside this
    int min_distance;       // metres, 0 = all
    int max_lights;         // cap on simultaneous Remix lights
    int exit_distance;      // metres: an ALREADY ACTIVE lamp survives to here.
                            // The gap between the two is hysteresis: without it a
                            // lamp at the boundary is created and destroyed on
                            // alternate re-selections, which is a lamp that blinks
                            // while the player walks past it.
    int sticky_extra;       // how far past max_lights the active set may grow before
                            // the farthest lamps are dropped (the other half of the
                            // hysteresis: the set shrinks at max_lights+sticky_extra
                            // and grows back at max_lights, instead of churning at
                            // exactly max_lights)
    int on_hour;            // night starts (game clock hour)
    int off_hour;           // night ends (wraps midnight)
    float radius;           // sphere light radius
    // Physical fixture geometry (see lamps.h). With fixture_size=1 the emitting
    // sphere gets the size the real luminaire housing is, and radiance is
    // rescaled by (radius_ref/radius)^2 so the lamp puts out the same total
    // light. That decoupling is the whole reason the radius could be fixed: the
    // shipped radius of 15 game units is a 30 metre ball, and shrinking it
    // without compensation would have taken the street's light with it.
    int fixture_size;       // 1 = real fixture sizes (default), 0 = use `radius`
    float fixture_scale;    // scale the real size; 1.0 = actual world size
    float radius_ref;       // radius the radiance_* values were calibrated at
    float cone_angle;       // spot shaping, degrees
    float cone_softness;    // spot shaping
    float radiance_big;     // radiance scale per lamp size
    float radiance_middle;
    float radiance_small;
    float cold[3];          // odd lamp types
    float warm[3];          // even lamp types
    // Colour temperature in kelvin per family; 0 = fall back to the cold/warm
    // triples above. Odd lamp types are the cool-white fixtures (mercury vapour /
    // metal halide, 4000-6500 K), even types the orange sodium ones (~2000 K).
    // A kelvin number is something a user can look up and get right; an RGB
    // triple is something they have to guess at.
    float kelt_cold;
    float kelt_warm;
    // Lamp family by name ("hps", "mercury", "halide", "led", "tungsten").
    // When set this wins over kelvin, because a street lamp is a gas discharge
    // and a gas discharge does not have a colour temperature in any useful sense:
    // its spectrum is a few atomic lines. -1 = not set, fall back to kelvin.
    int lamp_cold_family;
    int lamp_warm_family;
    float radiance_scale;   // global multiplier over every radiance we send
    float focus_exponent;   // shaping focus exponent, 0 = no focus
    int sphere_shaping;     // 1 = cone shaping for every lamp, not only spot types
    int resync_frames;      // frames between re-diffing the active light set
    unsigned long long light_hash_base;  // 0 = leave LightInfo::hash at zero
    float destroy_delay_ms; // ms before an unused handle is passed to DestroyLight
    // Hard cap on how many light handles the plugin keeps alive at once, across
    // streetlights + traffic + sun + moon + probe.  The Remix renderer has its own
    // light budget; going over it is a plausible reason for the bridge to hand us
    // a handle and then reject every draw on it, and the modules below each cap
    // themselves without looking at the others.  0 = no cap.
    unsigned max_total_lights;

    // --- probe light: the calibration rig.  It is the only light whose only
    // job is to answer "what radius / radiance / coordinate space actually shows
    // up in the Remix world?", so every one of its inputs is a separate knob.
    int probe;              // 1 = create one extra light
    float probe_x;          // game units, same space as the lamp list
    float probe_y;
    float probe_z;
    float probe_radius;     // sphere radius, in the same units as probe_scale
    float probe_radiance;   // white radiance for the probe light
    float probe_scale;      // 1 = game units, 0.01 = Remix metres (rtx.sceneScale)
    int probe_follow;       // 1 = park it on the player instead of probe_x/y/z
    float probe_dz;         // height above the player, game units
    unsigned long long probe_hash;  // must be non-zero, see probe.cpp
    // Which light type the server accepts is unknown: the distant light (sun/
    // moon) is the only type known to work here.  0 = bare LightInfo with no
    // EXT chain, 1 = sphere (what the lamp lights use), 2 = disk, 3 = cylinder.
    // One run per value, nothing else changed: whichever shows up names the
    // type the bridge actually supports.
    int probe_shape;
    int probe_cone;         // 1 = use the streetlight cone, 0 = unshaped sphere
    int probe_sweep;        // 1 = step radiance or radius by x10 every step
    int probe_sweep_axis;   // 0 = sweep radiance, 1 = sweep radius
    int probe_sweep_steps;  // x10 steps before wrapping back to the base value
    int probe_step_ms;      // how long each sweep step is shown
    int probe_refresh_ms;   // recreate the light this often (self-healing)

    // --- Remix bridge crash guards ---
    int block_gamma;        // 1 = swallow SetGammaRamp instead of forwarding it

    // --- sun (replaces SARemix_Real_Sun.lua) ---
    int sun_enabled;          // 1 = drive the sun with a Remix distant light
    float sun_radiance;       // radiance when the sun is straight overhead
    float sun_color[3];       // sun tint, multiplied into radiance
    float sun_angular;        // disc size in degrees: shadow softness
    float sun_spin;           // shift the orbit in time (degrees, 15/hour)
    float sun_tilt;           // lean the orbit plane (like the Lua rot.x)
    float sun_roll;           // lean it the other way (like the Lua rot.z)
    float sun_fade_pow;       // how fast light dies towards the horizon (legacy model)
    float sun_min_elevation;  // below this altitude the sun is off (legacy model)
    float sun_step_deg;       // rebuild the light when it moved this much
    float sun_volumetric;     // god-ray strength, 0 = off
    unsigned long long sun_hash;
    // 1 = real solar geometry (solar.cpp): latitude, season, hour angle, and
    // Beer-Lambert extinction for brightness and colour. 0 = the original
    // fixed-plane 360-degrees-per-day orbit, kept so the two can be A/B'd in
    // one session and so spin/tilt/roll still mean something.
    int sun_model;

    // --- observer location and calendar; [sun] section ---
    float sky_latitude;       // +north. GTA SA's map stands in for LA: 34.05
    float sky_longitude;      // +east. Los Angeles is -118.24
    float sky_timezone;       // local standard offset from UTC, LA = -8
    int   sky_day_of_year;    // 1..366, the season; used when track_date=0
    int   sky_track_date;     // 1 = read the in-game calendar when it looks valid
    int   sky_dst;            // 1 = US daylight saving (+1 h, ~Mar 11 - Nov 4)
    float sky_turbidity;      // aerosol extinction beta at 550 nm (0.08 clean, 0.25 smog)
    // Pin the game clock for lighting A/B. GTA SA advances one in-game minute
    // every 1.6 real seconds, so walking from noon to sunset by waiting takes
    // about 20 minutes of sitting in front of the game - long enough that nobody
    // does it, which is how "the sunset looks wrong" survives for weeks. -1 =
    // leave the clock alone. Writing the hour/minute bytes is ordinary game
    // state, the same thing the trainer opcode does.
    int   sun_force_hour;
    int   sun_force_minute;

    // --- the renderer's own ambient layer; [sky] section ---
    int   sky_apply;          // 1 = push rtx.* settings to match the sky
    float sky_day;            // rtx.skyBrightness in full daylight
    float sky_night;          // rtx.skyBrightness at night, before moonlight
    float sky_moon;           // extra skyBrightness at full moon, moon at zenith
    float sky_dusk_deg;       // sun elevation where twilight starts (downward)
    float sky_dawn_deg;       // ... and where daylight is back (upward)
    float sky_emissive_day;   // rtx.emissiveIntensity by day
    float sky_emissive_night; // ... at night; 0 = leave rtx.conf's value alone
    std::string sky_settings; // extra "key = value" file to push ("" = none)
    int   sky_probe;          // 1 = probe which rtx.* keys this build accepts

    // --- lamp switching ---
    // 0 = the old on_hour/off_hour window. 1 = solar twilight, which is what a
    // real photocell does and is the reason "on_hour=19" is wrong in summer.
    int   lamp_mode;
    float lamp_dusk_deg;      // lamps come on when the sun drops below this
    float lamp_dawn_deg;      // parsed, sanity-checked and logged, but no rule
                              // reads it: the higher rising threshold needs the
                              // previous lamps-on state, and lampsShouldBeOn()
                              // in solar.h is stateless (s13 removed the dead
                              // argument that used to be passed to it)

    // The moon is the anti-sun body (the Lua mod's model id 15891).
    int moon_enabled;         // 1 = drive the moon with a Remix distant light
    float moon_radiance;      // radiance of a FULL moon straight overhead
    float moon_color[3];      // moon tint, multiplied into radiance
    float moon_angular;       // disc size in degrees
    float moon_fade_pow;      // how fast moonlight dies towards the horizon (legacy)
    float moon_min_elevation; // below this altitude the moon is off (legacy)
    float moon_step_deg;      // rebuild the light when it moved this much
    unsigned long long moon_hash;
    // 0 = new, 0.25 = first quarter, 0.5 = full, 0.75 = last quarter. The old
    // code had no phase: the moon was permanently full, which is one reason the
    // night read as "too bright and flat".
    float moon_phase;

    // --- traffic lights (rhyremix_traffic.txt) ---
    int traffic_enabled;    // 0 = traffic light feature off
    int traffic_mode;       // 0 = off, 1 = cycle, 2 = fixed green
    std::string trafficPath;
    float traffic_radius;   // sphere light radius
    float traffic_radius_ref;  // radius traffic_radiance was calibrated at
    float traffic_radiance; // radiance scale for the lamp colour
    int traffic_period;     // seconds per full red/amber/green cycle
    int traffic_max_lights; // cap on simultaneous traffic light Remix lights
    int traffic_max_distance; // metres, 2D (new lights)
    int traffic_exit_distance; // metres, 2D (already active lights survive to here)
    int traffic_phase_near;   // metres: only inside this radius does a phase change
                            // recreate the light. Radiance is baked at CreateLight,
                            // so recreating every light on every phase change is a
                            // traffic light that visibly blinks across the whole
                            // district; nobody can read the colour of a light 300 m
                            // away, so far lights just keep the colour they had.
    int traffic_night_only;   // 1 = only light up in the night window
    int traffic_resync_frames;
    // Where the heads come from. 1 = "game": hook the corona the game registers
    // for each signal head and light exactly that head, in exactly the colour the
    // game chose (default). 0 = "list": one light per line of
    // rhyremix_traffic.txt, at the pole origin plus a headz guess, coloured by a
    // deterministic cycle - what shipped before s01, kept because it needs no
    // code patch and is the fallback when the hook cannot be installed.
    int traffic_source;
    int traffic_head_ttl_ms;  // a head with no corona for this long loses its light
    float traffic_head_forward;  // metres to move the light along head_normal
    int traffic_head_normal;     // 1 = use the entity matrix's "at" axis; 0 = no offset

    // --- s28: lighting must never change instantly ([fade] section) ---------
    // Remix bakes a light's radiance at CreateLight time, so a fade is a
    // sequence of creates. These knobs set how long a ramp takes, how big one
    // step may be, and how many creates one frame may spend paying for it.
    // See plugin/src/smooth.h.
    int   fade_in_ms;         // a lamp ramps up over this long (clamped >= 500)
    int   fade_out_ms;        // ... and down (clamped >= 500)
    float fade_step;          // largest gain change one handle may carry (0.05..0.5)
    int   creates_per_frame;  // per-frame CreateLight budget (0 = unlimited)
    int   player_grace_ms;    // hold the last player position this long when the
                              // ped pointer reads 0, then FADE the lamp layers out
                              // instead of destroying them in one frame
    float sky_rel_per_sec;    // max multiplicative rate for rtx.skyBrightness /
                              // rtx.emissiveIntensity (3.0 = at most 3x per second)
    float sky_abs_per_sec;    // ... plus this absolute allowance per second, so the
                              // 0.02 night floor can still move
    float sun_rel_per_sec;    // max multiplicative rate for the sun/moon radiance
    float sun_deg_per_sec;    // max angular rate for the sun/moon direction
    int   phase_fade_ms;      // signal colour cross-fade; 0 = instant, as a real
                              // signal is. The light's appearance and disappearance
                              // always use fade_in_ms/fade_out_ms.
};

// Loads once on first call; returns the same pointer every time.
const RhyConfig* rhyConfigGet();

// Re-reads rhyremix.ini when its timestamp changed, so numeric knobs can be
// tuned while the game runs instead of restarting it. Cheap: one stat per call,
// and the file is only parsed when it actually changed. The lamp lists are NOT
// re-read - those still need a restart.
void rhyConfigReloadIfChanged();

// Bumped every time rhyremix.ini is re-read.  Modules that gave up on the Remix
// API (a rejected CreateLight switches that module off for the session) watch
// this, so editing the ini is enough to retry without restarting the game.
unsigned rhyConfigRevision();

#endif  // RHY_CONFIG_H_
