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
    float cone_angle;       // spot shaping, degrees
    float cone_softness;    // spot shaping
    float radiance_big;     // radiance scale per lamp size
    float radiance_middle;
    float radiance_small;
    float cold[3];          // odd lamp types
    float warm[3];          // even lamp types
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
    float sun_fade_pow;       // how fast light dies towards the horizon
    float sun_min_elevation;  // below this altitude the sun is off
    float sun_step_deg;       // rebuild the light when it moved this much
    float sun_volumetric;     // god-ray strength, 0 = off
    unsigned long long sun_hash;
    // The moon is the anti-sun body (the Lua mod's model id 15891).
    int moon_enabled;         // 1 = drive the moon with a Remix distant light
    float moon_radiance;      // radiance when the moon is straight overhead
    float moon_color[3];      // moon tint, multiplied into radiance
    float moon_angular;       // disc size in degrees
    float moon_fade_pow;      // how fast moonlight dies towards the horizon
    float moon_min_elevation; // below this altitude the moon is off
    float moon_step_deg;      // rebuild the light when it moved this much
    unsigned long long moon_hash;

    // --- traffic lights (rhyremix_traffic.txt) ---
    int traffic_enabled;    // 0 = traffic light feature off
    int traffic_mode;       // 0 = off, 1 = cycle, 2 = fixed green
    std::string trafficPath;
    float traffic_radius;   // sphere light radius
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
