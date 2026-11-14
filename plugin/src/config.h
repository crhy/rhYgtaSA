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
    // --- n11: the lamp HEAD (the bulb you can see), a second light layer ------
    // rhY's V2 (2026-11-12c, Verona Beach): "boardwalk/beach lamps are dark: no
    // lit bulb, no pool of light under them". The pool is the luminaire light
    // below; the bulb is this. A Remix light is not visible geometry, so the head
    // gets its own small unshaped nits-based light at the bulb - the same pair
    // n02b shipped for signal lenses (bulb 1200 nits at 0.10 m + glow).
    // It is a near-field light: a 0.1 m globe is nothing at 300 m, so it is
    // created inside a smaller radius than the luminaire and capped separately.
    int head_glow;            // 0 = off (the A/B switch back to one light per lamp)
    int head_distance;        // metres 2D: a bulb gets a light inside this
    int head_exit_distance;   // metres 2D: an existing bulb light survives to here
    int head_max_lights;      // cap on live bulb lights, nearest first
    float head_radiance;      // the head's luminance in NITS at its own radius
    float head_scale;         // scale the head fixture size; 1.0 = real size
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
    float radiance_scale;   // global multiplier over every STREET lamp's radiance
                            // (n23: it no longer reaches the signals or the vehicles -
                            // see traffic_radiance_scale / veh_radiance_scale)
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

    // --- the game clock, s29; [clock] section. See plugin/src/tod.h for what each
    // one protects against and .strata/notes-s29-time-of-day-smooth.md for the
    // measurements that picked the numbers.
    float clock_ms_per_game_minute;   // seed for the tick calibration (measured p50 2015)
    float clock_jitter_min;           // backwards clock moves up to this are noise: hold
    int   clock_reanchor_ms;          // ... a bigger one is followed after this long
    int   clock_calendar_stable_reads; // consecutive identical (month,day) reads (one per
                                      // second, from the sky module) before the in-game
                                      // date is trusted; 0 = always use sky_day_of_year

    // --- the renderer's own ambient layer; [sky] section ---
    int   sky_apply;          // 1 = push rtx.* settings to match the sky
    float sky_day;            // rtx.skyBrightness in full daylight
    float sky_night;          // rtx.skyBrightness at night, before moonlight
    float sky_moon;           // extra skyBrightness at full moon, moon at zenith
    // n24: the night ceiling on that ambient, as a multiple of the moon light's own
    // published radiance. 0.25 ships; 0 switches the rule off (the pre-n24 behaviour,
    // where [sky] night+moon was the whole story). See solar.h skyAmbientCap().
    float sky_ambient_cap_mult;
    float sky_dusk_deg;       // sun elevation where twilight starts (downward)
    float sky_dawn_deg;       // ... and where daylight is back (upward)
    float sky_emissive_day;   // rtx.emissiveIntensity by day
    float sky_emissive_night; // ... at night; 0 = leave rtx.conf's value alone
    // Cloud cover, 0 = clear, 1 = overcast. There is no verified offset for the
    // game's CWeather, so this is an ini knob (s29); it is what the weather
    // follower smooths, and a real weather read would be plugged in at the same
    // place. 0 leaves the shipped look exactly as it was.
    float sky_cloud;
    // How fast the atmosphere is allowed to change: per second, for turbidity and
    // cloud cover. A weather front is minutes wide; a value that moves faster than
    // this is a step wearing a weather name.
    float weather_turbidity_per_sec;
    float weather_cloud_per_sec;
    std::string sky_settings; // extra "key = value" file to push ("" = none)
    int   sky_probe;          // 1 = probe which rtx.* keys this build accepts

    // --- n22: the exposure anchor, driven by the sun ([tonemap] section) ------
    // n16 put rtx.tonemappingMode = 0 (Global) and rtx.tonemap.exposureBias = -0.5
    // into all three profiles. In Global mode the exposure of the whole frame is
    // 2^(exposureBias + userEVBias) (rtx_tone_mapping.cpp:234) - one number, for
    // the whole day, because rtx.conf is a file. Sized for noon (p50 147 -> ~110)
    // that anchor takes a quarter of the light off a 21:00 frame already at p50
    // 7-17 with 34-52 % black (build/tmp/n16/good-before.json). These keys drive
    // the same option from the sun's elevation instead, at runtime, through
    // plugin/src/exposure.h. day_ev defaults to the profiles' -0.5, so at full
    // daylight the plugin publishes what rtx.conf already says: the noon grade is
    // untouched by this, only the night end moves.
    int   tonemap_apply;        // 0 = never write rtx.tonemap.exposureBias
    float tonemap_day_ev;       // the anchor at full daylight
    float tonemap_night_ev;     // ... at night
    float tonemap_dusk_deg;     // sun elevation where the ramp starts, going down
    float tonemap_dawn_deg;     // ... and where daylight is back, going up
    float tonemap_max_ev_per_sec;  // slew bound; clamped to 0.005..0.25 EV/s
    float tonemap_min_ev;       // hard clamp on the published value (UI span -4..4)
    float tonemap_max_ev;       // ... and its ceiling

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
    float traffic_radius;   // sphere light radius (legacy fixture_size=0 path)
    float traffic_radius_ref;  // radius traffic_radiance was calibrated at
    float traffic_radiance; // radiance scale for the lamp colour
    // s30: with fixture_size=1 (the default) a signal is a BULB: `bulb_radius`
    // is the lens radius in metres and `bulb_radiance` is the lens luminance in
    // nits at that radius, with no flux-preserving (ref/r)^2 - see
    // rhySignalBulb() in signalheads.h. 5400 nits over a 0.1 m sphere is the
    // output of a street luminaire in a marble, and sixty of them near a player
    // is a green street rather than sixty small bright heads.
    float traffic_bulb_radius;   // metres (game units)
    float traffic_bulb_radiance; // nits at the lens
    // s30: the radius we leave the game's OWN signal-head corona (it registers
    // one per lit head at radius 15.0 = a 30 m green flare). 0 = do not touch
    // the game's code beyond the hook itself.
    float traffic_corona_radius;
    // n02b: the SECOND light of every lit signal - the glow. rhY 2026-11-12
    // 08:50: "We wanted the glow from the traffic light and the bulb. It should
    // be both like real life." s30 took the glow away twice over (our light's
    // flux 304 -> 67, the game's own 15 m corona -> 0.45) to get the bulb, and
    // nothing was put back, so a lit signal stopped colouring the pole, the wall,
    // the road and the peds under it. glow_radius is the soft sphere around the
    // head in metres, glow_flux is its total output in the same units the log
    // prints (4*pi*r^2*mean radiance): 304 is what the 2026-11-12 00:48 glow
    // measured, and config.cpp clamps it to one street luminaire's flux (3473).
    // glow_flux=0 = no glow light at all, the A/B switch.
    float traffic_glow_radius;
    float traffic_glow_flux;
    // n23 (finding C): the SIGNAL layer's own multiplier. Until n23 the bulb and
    // the glow were multiplied by [streetlights] radiance_scale
    // (trafficlights.cpp:411,423), so "knock the street lamps down" and "keep the
    // glow" (test-2026-11-12b C) could not both be said in one config: taking the
    // lamps to 0.4 silently took 60 % of the signal glow with them. 1.0 = the
    // signals live by bulb_radiance and glow_flux alone.
    float traffic_radiance_scale;
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
    // n02: how long the hook may be live without recording a head before the
    // module considers falling back to the list - and the fallback only happens
    // when a signal of the loaded list is actually inside the game's own corona
    // range (50.0 m: push $0x42480000 at 0x49DCF3, tested inside the callee at
    // 0x6FC22E). A session that starts 79.1 m from the nearest pole - the default
    // spawn - has no such signal, so it keeps the hook instead of spending the
    // whole run on the mast. 0 = decide immediately, no grace.
    int traffic_head_grace_ms;
    // n02: 1 = put a list-mode light on the LENS (rotate the file's model-local
    // head offset by the instance heading and add it), 0 = the old pole position
    // x,y,z+headz. Only a v2 rhyremix_traffic.txt carries those columns; a v1
    // file gives the same position either way.
    int traffic_lens_offset;

    // --- s28: lighting must never change instantly ([fade] section) ---------
    // Remix bakes a light's radiance at CreateLight time, so a fade is a
    // sequence of creates. These knobs set how long a ramp takes, how big one
    // step may be, and how many creates one frame may spend paying for it.
    // See plugin/src/smooth.h.
    int   fade_in_ms;         // a lamp ramps up over this long (clamped >= 500)
    int   fade_out_ms;        // ... and down (clamped >= 500)
    float fade_step;          // largest gain change one handle may carry (0.05..0.5)
    int   creates_per_frame;  // per-frame CreateLight budget (0 = unlimited)
    // n08: and the same bound for the other half of a light's life. s28 bounded
    // creates; the 2026-11-12 runs then showed `pending_destroy` reaching 320 (the
    // 00:48 run) and 205 (the 10:22 run), all of them destroyed inside one Present.
    int   destroys_per_frame;  // per-frame DestroyLight budget (0 = unlimited)
    int   player_grace_ms;    // hold the last player position this long when the
                              // ped pointer reads 0, then FADE the lamp layers out
                              // instead of destroying them in one frame
    float sky_rel_per_sec;    // max multiplicative rate for rtx.skyBrightness /
                              // rtx.emissiveIntensity (3.0 = at most 3x per second)
    float sky_abs_per_sec;    // ... plus this absolute allowance per second, so the
                              // 0.02 night floor can still move
    float sun_rel_per_sec;    // max multiplicative rate for the sun/moon radiance
    float sun_deg_per_sec;    // max angular rate for the sun/moon direction
    // n01 (s29 note T7): the sun/moon distant-light handle used to be rebuilt when
    // the body had moved more than [sun]/[moon] step_deg or brightened by 5% - a
    // threshold, so the step that triggers the rebuild is as big as the threshold
    // and the interval between rebuilds follows the body's rate. These knobs put a
    // clock in its place: rebuild every handle_cadence_ms, shortened only as far as
    // needed that no rebuild carries more than handle_step_deg of direction or
    // handle_rel_lum of luminance, and never below handle_cadence_min_ms (the
    // bridge-churn floor). See rhyCadence* in plugin/src/smooth.h.
    int   handle_cadence_ms;  // fixed rebuild period for a distant-light handle
    int   handle_cadence_min_ms;  // ... and the floor it may be shortened to
    float handle_step_deg;    // direction budget per rebuild; 0 = the body's step_deg
    float handle_rel_lum;     // luminance budget per rebuild (0.05 = 5%)
    int   phase_fade_ms;      // signal colour cross-fade; 0 = instant, as a real
                              // signal is. The light's appearance and disappearance
                              // always use fade_in_ms/fade_out_ms.

    // --- n09: shutdown ([shutdown] section). The 2026-11-12 exit hang: rhY quit from the menu, the
    // Remix bridge server and the window went away, and the plugin went on drawing 302 lights per
    // frame into a bridge client whose server no longer existed, then logged nothing about it.
    // plugin/src/shutdown.h holds the rules; these are how long each signal has to hold before the
    // bridge is declared gone (0 = one frame is enough), how often the "shutdown: ..." line repeats,
    // and the A/B switch that stops forwarding Present once the bridge has been gone that long.
    int   shutdown_present_ms;  // Present must keep failing this long
    int   shutdown_device_ms;   // ... TestCooperativeLevel keep saying the device is gone
    int   shutdown_window_ms;   // ... the device's window stay destroyed (Quit does this)
    int   shutdown_calls_ms;    // ... our own Create/Draw calls keep failing, WITH another signal
                                // on: on its own that is one bad light, not a dead bridge
    int   shutdown_log_ms;      // period of the "shutdown: still running ..." line
    int   stop_present_after_ms; // 0 = always forward Present (default). >0 = once the bridge has
                                // been gone this long, stop calling the bridge client's Present and
                                // return D3D_OK: the experiment that says whether the exit spin is
                                // inside that call or inside the game's own device-reset loop.

    // --- n12: the car's own lights ([vehiclelights] section) ---------------
    // The game's only vehicle light sources are its corona sprites: a 15 game-unit
    // (30 m) billboard per bulb, which the runtime renders as emissive geometry and
    // (with rtx.enableSeparateUnorderedApproximations) lets light the surroundings.
    // That is test-2026-11-12c V5: "tail lights throw large red blobs on the sand
    // (too wide/strong for 2 small bulbs); headlights a small weak pool". The module
    // reads the game's corona store (read-only) and adds our own lights sized to the
    // real lamps. plugin/src/vehiclelights.h holds the rules and the game layout.
    int   veh_enabled;        // 0 = the module does nothing
    int   veh_max_lights;     // cap on live vehicle lamps (nearest vehicles first)
    int   veh_max_distance;   // metres; a vehicle further than this gets no light
    float veh_head_flux;      // headlamp, in the plugin's flux units (see signalheads.h)
    float veh_tail_flux;      // rear position lamp
    float veh_brake_flux;     // rear brake lamp (>= tail)
    float veh_head_radius;    // metres; 0 = rhyVehicleHeadFixture()
    float veh_tail_radius;    // metres; 0 = rhyVehicleTailFixture()
    float veh_head_cone_deg;  // HALF angle of the beam; 0 = the fixture's 25 (50 deg)
    float veh_head_cone_softness;  // -1 = the fixture's
    int   veh_brake_red_min;  // observed tail-lamp red that means "braking" (0 = off)
    float veh_move_step;      // metres a lamp may drift before its handle is rebuilt
    int   veh_cadence_ms;     // floor on the rebuild period of one lamp's handle
    float veh_max_spread;     // metres: the lamps of one vehicle must fit in this box
    // n23: the VEHICLE layer's own multiplier, for the same reason as the signals.
    // vehiclecorona.cpp used to hand the street-lamp knob to rhyVehicleBuildLamps,
    // so the finding-C take-down would have knocked the headlamps n12 sized from a
    // measured ~5000 peak intensity down to 40 % of it. 1.0 = n12's numbers stand.
    float veh_radiance_scale;
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
