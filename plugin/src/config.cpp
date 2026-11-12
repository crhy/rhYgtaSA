#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "log.h"
#include "lamps.h"

// Same module-folder logic as log.cpp: ask for the module that contains a
// function of this file, then take its directory. Falls back to the current
// directory when that fails.
static std::string moduleDir() {
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&moduleDir), &self);
    char module[MAX_PATH];
    module[0] = '\0';
    DWORD n = self ? GetModuleFileNameA(self, module, MAX_PATH) : 0;
    if (n == 0 || n >= MAX_PATH) {
        return "";  // "" means "current directory"
    }
    std::string path(module, n);
    std::string::size_type sep = path.find_last_of("\\/");
    if (sep == std::string::npos) {
        return "";
    }
    return path.substr(0, sep + 1);  // keeps the trailing separator
}

static std::string g_iniPath;

static std::string iniPath() {
    if (!g_iniPath.empty()) {
        return g_iniPath;
    }
    g_iniPath = moduleDir() + "rhyremix.ini";
    return g_iniPath;
}

// Written verbatim when the user has no rhyremix.ini yet.
// Units: the lamp lists are in GTA SA game units and rtx.conf sets
// rtx.sceneScale = 0.01, so 1 game unit = 1 cm in the Remix world.  A street
// lamp sits ~20 units up and its pool should be ~15 units wide, hence radius=15.
// Radiance falls off with the square of that distance, so values around 1 are
// the right order of magnitude; 200-400 blows the whole scene out.
static const char* const kIniDefaults =
    "[streetlights]\r\n"
    "enabled=1\r\n"
    "list=rhyremix_lights.txt\r\n"
    "max_distance=300\r\n"
    "exit_distance=380\r\n"
    "sticky_extra=140\r\n"
    "min_distance=0\r\n"
    "max_lights=560\r\n"
    "on_hour=19\r\n"
    "off_hour=6\r\n"
    "radius=15\r\n"
    "cone_angle=70\r\n"
    "cone_softness=0.4\r\n"
    "sphere_shaping=1\r\n"
    "focus_exponent=0\r\n"
    "radiance_big=2\r\n"
    "radiance_middle=1.2\r\n"
    "radiance_small=0.8\r\n"
    "radiance_scale=1.0\r\n"
    "cold=0.85,0.90,1.00\r\n"
    "warm=1.00,0.80,0.55\r\n"
    "kelt_cold=5000\r\n"
    "kelt_warm=2100\r\n"
    "when=twilight\r\n"
    "dusk_deg=-6\r\n"
    "dawn_deg=-4\r\n"
    "resync_frames=30\r\n"
    "light_hash_base=0\r\n"
    "destroy_delay_ms=500\r\n"
    "max_total_lights=0\r\n"
    "block_gamma=1\r\n"
    "probe=0\r\n"
    "probe_x=2489.927\r\n"
    "probe_y=-1655.956\r\n"
    "probe_z=19.843\r\n"
    "probe_radius=15\r\n"
    "probe_radiance=2\r\n"
    "probe_scale=1\r\n"
    "probe_follow=1\r\n"
    "probe_dz=2\r\n"
    "probe_hash=900001\r\n"
    "probe_shape=1\r\n"
    "probe_cone=0\r\n"
    "probe_sweep=1\r\n"
    "probe_sweep_axis=0\r\n"
    "probe_sweep_steps=4\r\n"
    "probe_step_ms=4000\r\n"
    "probe_refresh_ms=3000\r\n"
    "\r\n"
    "[sun]\r\n"
    "enabled=1\r\n"
    "model=1\r\n"
    "radiance=6\r\n"
    "color=1.00,0.98,0.95\r\n"
    "angular_diameter=0.53\r\n"
    "spin=0\r\n"
    "tilt=0\r\n"
    "roll=0\r\n"
    "fade_pow=1\r\n"
    "min_elevation=0.02\r\n"
    "step_deg=1\r\n"
    "volumetric=0\r\n"
    "hash=7\r\n"
    "latitude=34.05\r\n"
    "longitude=-118.24\r\n"
    "timezone=-8\r\n"
    "dst=1\r\n"
    "day_of_year=186\r\n"
    "track_date=1\r\n"
    "turbidity=0.12\r\n"
    "\r\n"
    "[clock]\r\n"
    "ms_per_game_minute=2000\r\n"
    "jitter_tolerance_min=8\r\n"
    "reanchor_ms=2000\r\n"
    "calendar_stable_reads=30\r\n"
    "\r\n"
    "[moon]\r\n"
    "enabled=1\r\n"
    "radiance=0.10\r\n"
    "color=0.78,0.85,1.00\r\n"
    "angular_diameter=0.52\r\n"
    "phase=0.5\r\n"
    "fade_pow=1\r\n"
    "min_elevation=0\r\n"
    "step_deg=3\r\n"
    "hash=8\r\n"
    "\r\n"
    "[sky]\r\n"
    "apply=1\r\n"
    "day=1.0\r\n"
    "night=0.02\r\n"
    "moon=0.05\r\n"
    "dusk_deg=-10\r\n"
    "dawn_deg=4\r\n"
    "emissive_day=1.0\r\n"
    "emissive_night=1.0\r\n"
    "cloud=0\r\n"
    "turbidity_per_sec=0.05\r\n"
    "cloud_per_sec=0.05\r\n"
    "settings=rhyremix-rtx.conf\r\n"
    "probe=0\r\n"
    "\r\n"
    "[trafficlights]\r\n"
    "enabled=1\r\n"
    "mode=cycle\r\n"
    "list=rhyremix_traffic.txt\r\n"
    "radius=6\r\n"
    "radiance=1.5\r\n"
    "bulb_radius=0\r\n"
    "bulb_radiance=1200\r\n"
    "glow_radius=3\r\n"
    "glow_flux=304\r\n"
    "corona_radius=0.45\r\n"
    "period=60\r\n"
    "max_lights=90\r\n"
    "max_distance=250\r\n"
    "exit_distance=300\r\n"
    "phase_near=150\r\n"
    "night_only=1\r\n"
    "resync_frames=30\r\n"
    "source=game\r\n"
    "head_ttl_ms=2000\r\n"
    "head_forward=0.15\r\n"
    "head_normal=0\r\n"
    "head_grace_ms=20000\r\n"
    "lens_offset=1\r\n"
    "\r\n"
    "[fade]\r\n"
    "in_ms=700\r\n"
    "out_ms=900\r\n"
    "step=0.25\r\n"
    "creates_per_frame=24\r\n"
    "player_grace_ms=8000\r\n"
    "sky_rel_per_sec=3.0\r\n"
    "sky_abs_per_sec=0.25\r\n"
    "sun_rel_per_sec=3.0\r\n"
    "sun_deg_per_sec=12.0\r\n"
    "handle_cadence_ms=2000\r\n"
    "handle_cadence_min_ms=150\r\n"
    "handle_step_deg=0.5\r\n"
    "handle_rel_lum=0.05\r\n"
    "phase_fade_ms=250\r\n"
    "\r\n"
    "[shutdown]\r\n"
    "present_ms=1500\r\n"
    "device_ms=3000\r\n"
    "window_ms=750\r\n"
    "calls_ms=4000\r\n"
    "log_ms=5000\r\n"
    "stop_present_after_ms=0\r\n";

static std::string iniStringSec(const char* section, const char* key,
                                const char* def) {
    char buf[512];
    buf[0] = '\0';
    GetPrivateProfileStringA(section, key, def, buf, sizeof buf,
                             iniPath().c_str());
    return std::string(buf);
}

static int iniIntSec(const char* section, const char* key, int def) {
    return static_cast<int>(GetPrivateProfileIntA(section, key, def,
                                                  iniPath().c_str()));
}

static unsigned long long iniU64Sec(const char* section, const char* key,
                                    unsigned long long def) {
    std::string s = iniStringSec(section, key, "");
    if (s.empty()) {
        return def;
    }
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    return (end == s.c_str()) ? def : v;
}

static std::string iniString(const char* key, const char* def) {
    return iniStringSec("streetlights", key, def);
}

static int iniInt(const char* key, int def) {
    return iniIntSec("streetlights", key, def);
}

static std::string iniStringSec(const char* section, const char* key,
                                const char* def);

static float iniFloatSec(const char* section, const char* key, float def) {
    std::string s = iniStringSec(section, key, "");
    if (s.empty()) {
        return def;
    }
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    return (end == s.c_str()) ? def : v;
}

static float iniFloat(const char* key, float def) {
    std::string s = iniString(key, "");
    if (s.empty()) {
        return def;
    }
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    return (end == s.c_str()) ? def : v;
}

static unsigned long long iniU64(const char* key,
                                 unsigned long long def) {
    std::string s = iniString(key, "");
    if (s.empty()) {
        return def;
    }
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    return (end == s.c_str()) ? def : v;
}

// "0.85,0.90,1.00" -> {0.85, 0.90, 1.00}
static void iniRgb(const char* key, float* out) {
    std::string s = iniString(key, "");
    const char* p = s.c_str();
    for (int i = 0; i < 3; ++i) {
        if (!*p) {
            break;
        }
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end == p) {
            break;
        }
        out[i] = v;
        p = end;
        if (*p == ',') {
            ++p;
        }
    }
}

static RhyConfig g_cfg;

// Set by loadConfig()/rhyConfigGet(); declared here because loadConfig() needs
// it to decide whether it may write the default ini.
static bool g_cfgLoaded = false;

static void loadConfig() {
    RhyConfig& c = g_cfg;

    std::string path = iniPath();
    FILE* probe = std::fopen(path.c_str(), "rb");
    if (!probe) {
        if (!g_cfgLoaded) {  // only ever write defaults on the very first load
            FILE* w = std::fopen(path.c_str(), "wb");
            if (w) {
                std::fwrite(kIniDefaults, 1, std::strlen(kIniDefaults), w);
                std::fclose(w);
                rhyLog("config: no rhyremix.ini, wrote defaults to %s", path.c_str());
            } else {
                rhyLog("config: no rhyremix.ini and cannot create one, using defaults");
            }
        }
    } else {
        std::fclose(probe);
    }

    c.enabled = iniInt("enabled", 1);
    c.max_distance = iniInt("max_distance", 300);
    c.exit_distance = iniInt("exit_distance", 380);
    // n04: the cap has to cover the radius. Measured over the whole 7002-lamp
    // list, the worst case is 408 lamps within 300 units of a point and 552
    // within 380 (median 149 / 234), so 320+90 was evicting lamps that were
    // still inside max_distance. 560+140 covers the whole 380-unit band (552 at
    // the worst point); the cap may now only
    // trim the band beyond max_distance anyway (rhySelectLights hardRadius), so
    // a too-small cap costs lamps at the edge instead of blinking them.
    c.sticky_extra = iniInt("sticky_extra", 140);
    c.min_distance = iniInt("min_distance", 0);
    c.max_lights = iniInt("max_lights", 560);
    c.on_hour = iniInt("on_hour", 19) % 24;
    c.off_hour = iniInt("off_hour", 6) % 24;
    c.radius = iniFloat("radius", 15.0f);
    c.fixture_size = iniInt("fixture_size", 1);
    c.fixture_scale = iniFloat("fixture_scale", 1.0f);
    if (!(c.fixture_scale > 0.0f)) {
        c.fixture_scale = 1.0f;
    }
    if (c.fixture_scale > 1000.0f) {
        c.fixture_scale = 1000.0f;
    }
    // 15 is the radius the shipped radiance_big/middle/small values were tuned
    // against, so it is the correct default reference: with fixture_size=1 the
    // radiance is multiplied up by exactly the factor the smaller sphere loses.
    c.radius_ref = iniFloat("radius_ref", 15.0f);
    if (!(c.radius_ref > 0.0f)) {
        c.radius_ref = 15.0f;
    }
    c.cone_angle = iniFloat("cone_angle", 70.0f);
    c.cone_softness = iniFloat("cone_softness", 0.4f);
    c.sphere_shaping = iniInt("sphere_shaping", 1);
    c.focus_exponent = iniFloat("focus_exponent", 0.0f);
    c.radiance_big = iniFloat("radiance_big", 2.0f);
    c.radiance_middle = iniFloat("radiance_middle", 1.2f);
    c.radiance_small = iniFloat("radiance_small", 0.8f);
    c.radiance_scale = iniFloat("radiance_scale", 1.0f);
    iniRgb("cold", c.cold);
    iniRgb("warm", c.warm);
    c.kelt_cold = iniFloat("kelt_cold", 5000.0f);
    c.kelt_warm = iniFloat("kelt_warm", 2100.0f);
    // Lamp family by name. Default is the single most common real fixture in
    // San Andreas street lighting: high-pressure sodium on the warm side. The
    // cool side defaults to mercury vapour, which is what the cool-white street
    // lamps of this era actually were. Set lamp_cold= / lamp_warm= to nothing to
    // fall back to the kelvin numbers.
    {
        std::string cw = iniString("lamp_warm", "hps");
        std::string cc = iniString("lamp_cold", "mercury");
        c.lamp_warm_family = cw.empty() ? -1 : rhyLampFamilyFromName(cw.c_str());
        c.lamp_cold_family = cc.empty() ? -1 : rhyLampFamilyFromName(cc.c_str());
        if (!cw.empty() && c.lamp_warm_family < 0) {
            rhyLog("config: lamp_warm=\"%s\" is not a lamp family (hps, mercury, "
                   "halide, led, tungsten, neon); falling back to kelt_warm",
                   cw.c_str());
        }
        if (!cc.empty() && c.lamp_cold_family < 0) {
            rhyLog("config: lamp_cold=\"%s\" is not a lamp family (hps, mercury, "
                   "halide, led, tungsten, neon); falling back to kelt_cold",
                   cc.c_str());
        }
    }
    if (c.kelt_cold < 0.0f) {
        c.kelt_cold = 0.0f;
    }
    if (c.kelt_warm < 0.0f) {
        c.kelt_warm = 0.0f;
    }
    if (c.kelt_cold > 25000.0f) {
        c.kelt_cold = 25000.0f;  // the Planck fit is out of range above this
    }
    if (c.kelt_warm > 25000.0f) {
        c.kelt_warm = 25000.0f;
    }
    c.resync_frames = iniInt("resync_frames", 30);
    c.light_hash_base = iniU64("light_hash_base", 0ull);
    c.destroy_delay_ms = iniFloat("destroy_delay_ms", 500.0f);
    c.max_total_lights = static_cast<unsigned>(iniInt("max_total_lights", 0));
    if (c.max_total_lights > 4096) {
        c.max_total_lights = 4096;  // a typo like 640000 must not disable the cap
    }
    c.block_gamma = iniInt("block_gamma", 1);
    c.probe = iniInt("probe", 0);
    c.probe_x = iniFloat("probe_x", 2489.927f);
    c.probe_y = iniFloat("probe_y", -1655.956f);
    c.probe_z = iniFloat("probe_z", 19.843f);
    c.probe_radius = iniFloat("probe_radius", 15.0f);
    c.probe_radiance = iniFloat("probe_radiance", 2.0f);
    c.probe_scale = iniFloat("probe_scale", 1.0f);
    c.probe_follow = iniInt("probe_follow", 1);
    c.probe_dz = iniFloat("probe_dz", 2.0f);
    c.probe_hash = iniU64("probe_hash", 900001ull);
    c.probe_shape = iniInt("probe_shape", 1);
    c.probe_cone = iniInt("probe_cone", 0);
    c.probe_sweep = iniInt("probe_sweep", 0);
    c.probe_sweep_axis = iniInt("probe_sweep_axis", 0);
    c.probe_sweep_steps = iniInt("probe_sweep_steps", 4);
    c.probe_step_ms = iniInt("probe_step_ms", 4000);
    c.probe_refresh_ms = iniInt("probe_refresh_ms", 3000);

    if (c.min_distance < 0) {
        c.min_distance = 0;
    }
    if (c.max_distance < 0) {
        c.max_distance = 0;  // 0 = no upper bound
    }
    if (c.max_lights < 0) {
        c.max_lights = 0;
    }
    if (c.max_lights > 4096) {
        c.max_lights = 4096;  // the list is 7371 lamps; more than that is a typo
    }
    if (c.exit_distance < c.max_distance) {
        // Hysteresis has to be the WIDER of the two numbers, otherwise a lamp is
        // created inside max_distance and retired the moment it leaves it: the blink.
        c.exit_distance = c.max_distance + 100;
    }
    if (c.sticky_extra < 0) {
        c.sticky_extra = 0;
    }
    if (c.sticky_extra > c.max_lights) {
        c.sticky_extra = c.max_lights;  // doubling the set is not hysteresis
    }
    if (!(c.radius > 0.0f)) {
        c.radius = 15.0f;
    }
    if (c.radius > 200.0f) {
        c.radius = 200.0f;  // a lamp pool wider than the block is a mistake
    }
    if (!(c.cone_angle > 0.0f) || c.cone_angle > 179.0f) {
        c.cone_angle = 60.0f;
    }
    if (!(c.cone_softness >= 0.0f) || c.cone_softness > 1.0f) {
        c.cone_softness = 0.2f;
    }
    if (!(c.radiance_scale > 0.0f)) {
        c.radiance_scale = 1.0f;
    }
    if (c.radiance_big < 0.0f) {
        c.radiance_big = 0.0f;
    }
    if (c.radiance_middle < 0.0f) {
        c.radiance_middle = 0.0f;
    }
    if (c.radiance_small < 0.0f) {
        c.radiance_small = 0.0f;
    }
    if (!(c.destroy_delay_ms >= 0.0f)) {
        c.destroy_delay_ms = 500.0f;
    }
    if (c.destroy_delay_ms > 5000.0f) {
        c.destroy_delay_ms = 5000.0f;
    }
    if (!(c.probe_radius > 0.0f)) {
        c.probe_radius = 15.0f;
    }
    if (!(c.probe_radiance > 0.0f)) {
        c.probe_radiance = 2.0f;
    }
    // The header puts no constraint on LightInfo::hash, but every light that
    // has worked here carries a non-zero one and the probe was the only create
    // with hash=0, so an anonymous probe hash is never allowed by default.
    // Set probe_hash=0 to test that claim on its own.
    if (c.probe_hash == 0ull) {
        c.probe_hash = 900001ull;
    }
    if (c.probe_shape < 0 || c.probe_shape > 3) {
        c.probe_shape = 1;  // sphere
    }
    if (!(c.probe_scale > 0.0f)) {
        c.probe_scale = 1.0f;
    }
    if (c.probe_sweep_steps < 1) {
        c.probe_sweep_steps = 1;
    }
    if (c.probe_sweep_steps > 9) {
        c.probe_sweep_steps = 9;  // x10^9 is already past any sane exposure
    }
    if (c.probe_sweep_axis < 0 || c.probe_sweep_axis > 1) {
        c.probe_sweep_axis = 0;
    }
    if (c.probe_step_ms < 500) {
        c.probe_step_ms = 500;
    }
    if (c.probe_refresh_ms < 500) {
        c.probe_refresh_ms = 500;
    }
    if (!(c.focus_exponent >= 0.0f)) {
        c.focus_exponent = 0.0f;
    }
    if (c.resync_frames < 1) {
        c.resync_frames = 1;
    }
    if (c.focus_exponent > 16.0f) {
        c.focus_exponent = 16.0f;
    }

    // --- sun ---
    c.sun_enabled = iniIntSec("sun", "enabled", 1);
    c.sun_radiance = iniFloatSec("sun", "radiance", 1.0f);
    {
        std::string s = iniStringSec("sun", "color", "");
        const char* p = s.c_str();
        for (int i = 0; i < 3; ++i) {
            if (!*p) {
                break;
            }
            char* end = nullptr;
            float v = std::strtof(p, &end);
            if (end == p) {
                break;
            }
            c.sun_color[i] = v;
            p = end;
            if (*p == ',') {
                ++p;
            }
        }
    }
    c.sun_angular = iniFloatSec("sun", "angular_diameter", 3.0f);
    c.sun_spin = iniFloatSec("sun", "spin", 0.0f);
    c.sun_tilt = iniFloatSec("sun", "tilt", 0.0f);
    c.sun_roll = iniFloatSec("sun", "roll", 0.0f);
    c.sun_fade_pow = iniFloatSec("sun", "fade_pow", 0.5f);
    c.sun_min_elevation = iniFloatSec("sun", "min_elevation", 0.0f);
    c.sun_step_deg = iniFloatSec("sun", "step_deg", 0.5f);
    c.sun_volumetric = iniFloatSec("sun", "volumetric", 0.0f);
    c.sun_hash = iniU64Sec("sun", "hash", 7ull);
    c.sun_model = iniIntSec("sun", "model", 1);

    // Observer location. The defaults are Los Angeles, because that is the city
    // GTA SA's map is a stand-in for; getting the latitude wrong is the single
    // biggest error in where the sun ends up, so it is a knob, not a constant.
    c.sky_latitude = iniFloatSec("sun", "latitude", 34.05f);
    c.sky_longitude = iniFloatSec("sun", "longitude", -118.24f);
    c.sky_timezone = iniFloatSec("sun", "timezone", -8.0f);
    c.sky_day_of_year = iniIntSec("sun", "day_of_year", 186);
    c.sky_track_date = iniIntSec("sun", "track_date", 1);
    c.sky_dst = iniIntSec("sun", "dst", 1);
    c.sky_turbidity = iniFloatSec("sun", "turbidity", 0.12f);
    c.sun_force_hour = iniIntSec("sun", "force_hour", -1);
    c.sun_force_minute = iniIntSec("sun", "force_minute", -1);

    // --- the game clock (s29). See plugin/src/tod.h.
    c.clock_ms_per_game_minute = iniFloatSec("clock", "ms_per_game_minute", 2000.0f);
    c.clock_jitter_min = iniFloatSec("clock", "jitter_tolerance_min", 8.0f);
    c.clock_reanchor_ms = iniIntSec("clock", "reanchor_ms", 2000);
    c.clock_calendar_stable_reads = iniIntSec("clock", "calendar_stable_reads", 30);
    if (c.sun_force_hour > 23) {
        c.sun_force_hour = 23;
    }
    if (c.sun_force_minute > 59) {
        c.sun_force_minute = 59;
    }
    if (c.sky_latitude < -89.0f) {
        c.sky_latitude = -89.0f;
    }
    if (c.sky_latitude > 89.0f) {
        c.sky_latitude = 89.0f;
    }
    if (c.sky_longitude < -180.0f) {
        c.sky_longitude = -180.0f;
    }
    if (c.sky_longitude > 180.0f) {
        c.sky_longitude = 180.0f;
    }
    if (c.sky_day_of_year < 1) {
        c.sky_day_of_year = 1;
    }
    if (c.sky_day_of_year > 366) {
        c.sky_day_of_year = 366;
    }
    if (!(c.sky_turbidity >= 0.0f)) {
        c.sky_turbidity = 0.0f;
    }
    if (c.sky_turbidity > 1.0f) {
        c.sky_turbidity = 1.0f;  // beyond this the sun simply vanishes
    }
    // A game minute shorter than a fifth of a second or longer than a minute of real
    // time is a typo, not a clock speed; the calibration in tod.cpp refuses those too.
    if (!(c.clock_ms_per_game_minute >= 200.0f)) {
        c.clock_ms_per_game_minute = 2000.0f;
    }
    if (c.clock_ms_per_game_minute > 60000.0f) {
        c.clock_ms_per_game_minute = 60000.0f;
    }
    if (!(c.clock_jitter_min >= 0.0f)) {
        c.clock_jitter_min = 0.0f;
    }
    if (c.clock_jitter_min > 720.0f) {
        c.clock_jitter_min = 720.0f;  // more than this is not jitter, it is a second clock
    }
    if (c.clock_reanchor_ms < 0) {
        c.clock_reanchor_ms = 0;  // 0 = follow a deliberate backwards set at once
    }
    if (c.clock_reanchor_ms > 60000) {
        c.clock_reanchor_ms = 60000;
    }
    if (c.clock_calendar_stable_reads < 0) {
        c.clock_calendar_stable_reads = 0;  // 0 = never trust the in-game date
    }
    if (c.clock_calendar_stable_reads > 600) {
        c.clock_calendar_stable_reads = 600;
    }

    // --- sky layer: the renderer's own ambient, driven from the sun ---
    c.sky_apply = iniIntSec("sky", "apply", 1);
    c.sky_day = iniFloatSec("sky", "day", 1.0f);
    c.sky_night = iniFloatSec("sky", "night", 0.02f);
    c.sky_moon = iniFloatSec("sky", "moon", 0.05f);
    c.sky_dusk_deg = iniFloatSec("sky", "dusk_deg", -10.0f);
    c.sky_dawn_deg = iniFloatSec("sky", "dawn_deg", 4.0f);
    c.sky_emissive_day = iniFloatSec("sky", "emissive_day", 1.0f);
    c.sky_emissive_night = iniFloatSec("sky", "emissive_night", 1.0f);
    c.sky_cloud = iniFloatSec("sky", "cloud", 0.0f);
    c.weather_turbidity_per_sec = iniFloatSec("sky", "turbidity_per_sec", 0.05f);
    c.weather_cloud_per_sec = iniFloatSec("sky", "cloud_per_sec", 0.05f);
    c.sky_settings = iniStringSec("sky", "settings", "rhyremix-rtx.conf");
    c.sky_probe = iniIntSec("sky", "probe", 0);
    if (c.sky_day < 0.0f) {
        c.sky_day = 0.0f;
    }
    if (c.sky_day > 100.0f) {
        c.sky_day = 100.0f;
    }
    if (c.sky_night < 0.0f) {
        c.sky_night = 0.0f;
    }
    if (c.sky_night > c.sky_day) {
        c.sky_night = c.sky_day;  // a brighter night than day is a typo
    }
    if (c.sky_moon < 0.0f) {
        c.sky_moon = 0.0f;
    }
    if (!(c.sky_dusk_deg < c.sky_dawn_deg)) {
        // Twilight has to run upward. Reversed, the daylight factor is stuck at
        // one end of its range and the whole scene is either day or night.
        c.sky_dusk_deg = -10.0f;
        c.sky_dawn_deg = 4.0f;
    }
    if (c.sky_emissive_day < 0.0f) {
        c.sky_emissive_day = 0.0f;
    }
    if (c.sky_emissive_night < 0.0f) {
        c.sky_emissive_night = 0.0f;  // 0 = do not touch rtx.conf's value
    }
    if (!(c.sky_cloud >= 0.0f)) {  // also catches NaN
        c.sky_cloud = 0.0f;
    }
    if (c.sky_cloud > 1.0f) {
        c.sky_cloud = 1.0f;
    }
    // A weather rate of 0 freezes the atmosphere; the follower would then never reach
    // the value the ini asks for, which is a worse surprise than a fast front.
    if (!(c.weather_turbidity_per_sec > 0.0f)) {
        c.weather_turbidity_per_sec = 0.001f;
    }
    if (c.weather_turbidity_per_sec > 10.0f) {
        c.weather_turbidity_per_sec = 10.0f;
    }
    if (!(c.weather_cloud_per_sec > 0.0f)) {
        c.weather_cloud_per_sec = 0.001f;
    }
    if (c.weather_cloud_per_sec > 10.0f) {
        c.weather_cloud_per_sec = 10.0f;
    }

    // --- lamp switching by twilight rather than by wall clock ---
    std::string lampWhen = iniStringSec("streetlights", "when", "twilight");
    c.lamp_mode = 1;
    if (lampWhen == "hours" || lampWhen == "clock" || lampWhen == "0") {
        c.lamp_mode = 0;
    }
    c.lamp_dusk_deg = iniFloatSec("streetlights", "dusk_deg", -6.0f);
    c.lamp_dawn_deg = iniFloatSec("streetlights", "dawn_deg", -4.0f);
    if (!(c.lamp_dusk_deg < c.lamp_dawn_deg)) {
        c.lamp_dusk_deg = -6.0f;
        c.lamp_dawn_deg = -4.0f;
    }

    // The moon is read from its own [moon] section; every key falls back to
    // the same-named sun key's default, not to the sun's value.
    c.moon_enabled = iniIntSec("moon", "enabled", 1);
    c.moon_radiance = iniFloatSec("moon", "radiance", 0.12f);
    {
        std::string s2 = iniStringSec("moon", "color", "");
        const char* p2 = s2.c_str();
        for (int i = 0; i < 3; ++i) {
            if (!*p2) {
                break;
            }
            char* end = nullptr;
            float v = std::strtof(p2, &end);
            if (end == p2) {
                break;
            }
            c.moon_color[i] = v;
            p2 = end;
            if (*p2 == ',') {
                ++p2;
            }
        }
    }
    c.moon_angular = iniFloatSec("moon", "angular_diameter", 1.5f);
    c.moon_fade_pow = iniFloatSec("moon", "fade_pow", 1.0f);
    c.moon_min_elevation = iniFloatSec("moon", "min_elevation", 0.0f);
    c.moon_step_deg = iniFloatSec("moon", "step_deg", 3.0f);
    c.moon_hash = iniU64Sec("moon", "hash", 8ull);
    c.moon_phase = iniFloatSec("moon", "phase", 0.5f);
    c.moon_phase = c.moon_phase - static_cast<int>(c.moon_phase);
    if (c.moon_phase < 0.0f) {
        c.moon_phase += 1.0f;
    }

    if (!(c.sun_radiance >= 0.0f)) {
        c.sun_radiance = 1.0f;
    }
    if (c.sun_radiance > 100.0f) {
        c.sun_radiance = 100.0f;
    }
    if (!(c.sun_angular > 0.0f)) {
        c.sun_angular = 3.0f;
    }
    if (c.sun_angular > 60.0f) {
        c.sun_angular = 60.0f;
    }
    if (!(c.sun_fade_pow > 0.0f)) {
        c.sun_fade_pow = 0.5f;
    }
    if (c.sun_fade_pow > 8.0f) {
        c.sun_fade_pow = 8.0f;
    }
    if (!(c.sun_min_elevation >= 0.0f)) {
        c.sun_min_elevation = 0.0f;
    }
    if (c.sun_min_elevation > 1.0f) {
        c.sun_min_elevation = 1.0f;
    }
    if (!(c.sun_step_deg > 0.0f)) {
        c.sun_step_deg = 0.5f;
    }
    if (c.sun_step_deg > 30.0f) {
        c.sun_step_deg = 30.0f;
    }
    if (!(c.moon_radiance >= 0.0f)) {
        c.moon_radiance = 0.12f;
    }
    if (c.moon_radiance > 100.0f) {
        c.moon_radiance = 100.0f;
    }
    if (!(c.moon_angular > 0.0f)) {
        c.moon_angular = 1.5f;
    }
    if (c.moon_angular > 60.0f) {
        c.moon_angular = 60.0f;
    }
    if (!(c.moon_fade_pow > 0.0f)) {
        c.moon_fade_pow = 1.0f;
    }
    if (c.moon_fade_pow > 8.0f) {
        c.moon_fade_pow = 8.0f;
    }
    if (!(c.moon_min_elevation >= 0.0f)) {
        c.moon_min_elevation = 0.0f;
    }
    if (c.moon_min_elevation > 1.0f) {
        c.moon_min_elevation = 1.0f;
    }
    if (!(c.moon_step_deg > 0.0f)) {
        c.moon_step_deg = 3.0f;
    }
    if (c.moon_step_deg > 30.0f) {
        c.moon_step_deg = 30.0f;
    }

    // --- traffic lights ---
    c.traffic_enabled = iniIntSec("trafficlights", "enabled", 1);
    std::string mode = iniStringSec("trafficlights", "mode", "cycle");
    c.traffic_mode = 1;  // cycle by default
    if (mode == "off" || mode == "0") {
        c.traffic_mode = 0;
    } else if (mode == "fixed" || mode == "green" || mode == "2") {
        c.traffic_mode = 2;
    }
    c.traffic_period = iniIntSec("trafficlights", "period", 60);
    if (c.traffic_period < 5) {
        c.traffic_period = 5;
    }
    c.traffic_radius = iniFloatSec("trafficlights", "radius", 6.0f);
    c.traffic_radius_ref = iniFloatSec("trafficlights", "radius_ref", 6.0f);
    if (!(c.traffic_radius_ref > 0.0f)) {
        c.traffic_radius_ref = 6.0f;
    }
    c.traffic_radiance = iniFloatSec("trafficlights", "radiance", 1.5f);
    // s30 bulb model. bulb_radius=0 means "the real lens from lamps.h"
    // (rhySignalRadius, 0.10 m for a 200 mm lens, times fixture_scale).
    c.traffic_bulb_radius = iniFloatSec("trafficlights", "bulb_radius", 0.0f);
    if (c.traffic_bulb_radius < 0.0f) {
        c.traffic_bulb_radius = 0.0f;
    }
    if (c.traffic_bulb_radius > 5.0f) {
        c.traffic_bulb_radius = 5.0f;  // not a lens any more
    }
    c.traffic_bulb_radiance = iniFloatSec("trafficlights", "bulb_radiance",
                                          1200.0f);
    if (!(c.traffic_bulb_radiance > 0.0f)) {
        c.traffic_bulb_radiance = 0.0f;  // 0 = no signal lights at all
    }
    if (c.traffic_bulb_radiance > 100000.0f) {
        c.traffic_bulb_radiance = 100000.0f;
    }
    c.traffic_corona_radius = iniFloatSec("trafficlights", "corona_radius",
                                          0.45f);
    if (c.traffic_corona_radius < 0.0f) {
        c.traffic_corona_radius = 0.0f;  // 0 = leave the game's 15 m flare alone
    }
    // n02b: the glow, the other half of rhY's "both like real life". Defaults
    // are the measured 2026-11-12 00:48 look, not a guess: 304 flux units spread
    // over a 3 m sphere at the head. The ceiling is one street luminaire
    // (kRhySignalGlowFluxCap, 3473: rhyLuminaireFlux() reproduces it from the
    // shipped radiance_big/radius_ref/kelvin pair), because a signal that
    // out-lights the lamp over its head is the same defect in a new colour.
    c.traffic_glow_radius = iniFloatSec("trafficlights", "glow_radius",
                                        3.0f);
    if (c.traffic_glow_radius < 0.0f) {
        c.traffic_glow_radius = 0.0f;
    }
    if (c.traffic_glow_radius > 20.0f) {
        c.traffic_glow_radius = 20.0f;  // beyond this it is the old flood again
    }
    c.traffic_glow_flux = iniFloatSec("trafficlights", "glow_flux",
                                      304.0f);
    if (c.traffic_glow_flux < 0.0f) {
        c.traffic_glow_flux = 0.0f;  // 0 = no glow light, bulb only
    }
    if (c.traffic_glow_flux > 3473.0f) {
        c.traffic_glow_flux = 3473.0f;  // one street luminaire, see above
    }
    // n04: 90+120 covers the worst case measured over the 1522-entry list (85
    // signals within 250 units, 150 within 300), so the cap never evicts a signal
    // that is in front of the player.
    c.traffic_max_lights = iniIntSec("trafficlights", "max_lights", 90);
    c.traffic_max_distance = iniIntSec("trafficlights", "max_distance", 250);
    c.traffic_exit_distance = iniIntSec("trafficlights", "exit_distance", 300);
    c.traffic_phase_near = iniIntSec("trafficlights", "phase_near", 150);
    c.traffic_night_only = iniIntSec("trafficlights", "night_only", 1);
    c.traffic_resync_frames =
        iniIntSec("trafficlights", "resync_frames", 30);
    // "game" is the default: it is the only source that knows where the heads
    // actually are and what colour the game is showing. "list" stays available
    // for builds of the game where the hook cannot be installed.
    std::string src = iniStringSec("trafficlights", "source", "game");
    c.traffic_source = (src == "list" || src == "0") ? 0 : 1;
    c.traffic_head_ttl_ms = iniIntSec("trafficlights", "head_ttl_ms", 2000);
    if (c.traffic_head_ttl_ms < 200) {
        c.traffic_head_ttl_ms = 200;  // shorter than this and a single dropped
                                      // frame of coronas blanks the intersection
    }
    if (c.traffic_head_ttl_ms > 60000) {
        c.traffic_head_ttl_ms = 60000;
    }
    c.traffic_head_forward = iniFloatSec("trafficlights", "head_forward", 0.15f);
    c.traffic_head_normal = iniIntSec("trafficlights", "head_normal", 0);
    if (c.traffic_head_forward < 0.0f) {
        c.traffic_head_forward = 0.0f;
    }
    if (c.traffic_head_forward > 2.0f) {
        c.traffic_head_forward = 2.0f;  // further than this is not a lens offset
    }
    // n02: the head-source grace. The default matches the 20 s s30 used; the
    // fallback it guards is range-aware (rhyHeadsHealthy), so this is only how
    // long the module waits before it is allowed to conclude anything.
    c.traffic_head_grace_ms = iniIntSec("trafficlights", "head_grace_ms", 20000);
    if (c.traffic_head_grace_ms < 0) {
        c.traffic_head_grace_ms = 0;  // 0 = decide on the first frame
    }
    if (c.traffic_head_grace_ms > 600000) {
        c.traffic_head_grace_ms = 600000;  // 10 minutes: effectively never
    }
    c.traffic_lens_offset = iniIntSec("trafficlights", "lens_offset", 1);
    if (c.traffic_max_distance < 0) {
        c.traffic_max_distance = 0;  // 0 = no upper bound
    }
    if (!(c.traffic_radius > 0.0f)) {
        c.traffic_radius = 6.0f;
    }
    if (c.traffic_radius > 200.0f) {
        c.traffic_radius = 200.0f;
    }
    if (!(c.traffic_radiance > 0.0f)) {
        c.traffic_radiance = 1.5f;
    }
    if (c.traffic_resync_frames < 1) {
        c.traffic_resync_frames = 1;
    }
    if (c.traffic_max_lights < 0) {
        c.traffic_max_lights = 0;
    }
    if (c.traffic_exit_distance < c.traffic_max_distance) {
        c.traffic_exit_distance = c.traffic_max_distance + 50;
    }
    if (c.traffic_phase_near < 0) {
        c.traffic_phase_near = 0;  // 0 = never recreate on a phase change
    }

    // --- s28: the [fade] section. Everything here is a floor, not a preference:
    // a ramp shorter than half a second, a step bigger than half the light, or no
    // per-frame create budget is the instant world-wide lighting change rhY asked
    // about (finding 1), so an out-of-range value is corrected and logged.
    c.fade_in_ms = iniIntSec("fade", "in_ms", 700);
    c.fade_out_ms = iniIntSec("fade", "out_ms", 900);
    if (c.fade_in_ms < 500) {
        c.fade_in_ms = 500;
    }
    if (c.fade_in_ms > 30000) {
        c.fade_in_ms = 30000;
    }
    if (c.fade_out_ms < 500) {
        c.fade_out_ms = 500;
    }
    if (c.fade_out_ms > 30000) {
        c.fade_out_ms = 30000;
    }
    c.fade_step = iniFloatSec("fade", "step", 0.25f);
    if (!(c.fade_step >= 0.05f)) {
        c.fade_step = 0.05f;  // below this a fade costs too many creates per light
    }
    if (c.fade_step > 0.5f) {
        c.fade_step = 0.5f;   // above this one step is a visible jump
    }
    c.creates_per_frame = iniIntSec("fade", "creates_per_frame", 24);
    if (c.creates_per_frame < 0) {
        c.creates_per_frame = 0;  // 0 = no per-frame limit (not recommended)
    }
    if (c.creates_per_frame > 512) {
        c.creates_per_frame = 512;
    }
    c.player_grace_ms = iniIntSec("fade", "player_grace_ms", 8000);
    if (c.player_grace_ms < 0) {
        c.player_grace_ms = 0;
    }
    if (c.player_grace_ms > 60000) {
        c.player_grace_ms = 60000;
    }
    c.sky_rel_per_sec = iniFloatSec("fade", "sky_rel_per_sec", 3.0f);
    if (!(c.sky_rel_per_sec >= 1.05f)) {
        c.sky_rel_per_sec = 1.05f;
    }
    if (c.sky_rel_per_sec > 1000.0f) {
        c.sky_rel_per_sec = 1000.0f;
    }
    c.sky_abs_per_sec = iniFloatSec("fade", "sky_abs_per_sec", 0.25f);
    if (!(c.sky_abs_per_sec >= 0.0f)) {
        c.sky_abs_per_sec = 0.0f;
    }
    c.sun_rel_per_sec = iniFloatSec("fade", "sun_rel_per_sec", 3.0f);
    if (!(c.sun_rel_per_sec >= 1.05f)) {
        c.sun_rel_per_sec = 1.05f;
    }
    if (c.sun_rel_per_sec > 1000.0f) {
        c.sun_rel_per_sec = 1000.0f;
    }
    c.sun_deg_per_sec = iniFloatSec("fade", "sun_deg_per_sec", 12.0f);
    if (!(c.sun_deg_per_sec >= 0.05f)) {
        c.sun_deg_per_sec = 0.05f;
    }
    if (c.sun_deg_per_sec > 90.0f) {
        c.sun_deg_per_sec = 90.0f;
    }
    c.phase_fade_ms = iniIntSec("fade", "phase_fade_ms", 250);
    if (c.phase_fade_ms < 0) {
        c.phase_fade_ms = 0;
    }
    if (c.phase_fade_ms > 5000) {
        c.phase_fade_ms = 5000;
    }
    // n01 (s29 T7): the distant-light handle cadence. handle_cadence_ms is the
    // fixed period; the limiter-driven shortening can go no lower than
    // handle_cadence_min_ms, which is the bridge-churn floor (two bodies at the
    // floor is 13.3 creates/s, against the 2.43/s of lamp traffic the 2026-11-12
    // log measured - and the per-frame create budget bounds it again on top).
    c.handle_cadence_ms = iniIntSec("fade", "handle_cadence_ms", 2000);
    if (c.handle_cadence_ms < 100) {
        c.handle_cadence_ms = 100;
    }
    if (c.handle_cadence_ms > 30000) {
        c.handle_cadence_ms = 30000;
    }
    c.handle_cadence_min_ms = iniIntSec("fade", "handle_cadence_min_ms", 150);
    if (c.handle_cadence_min_ms < 50) {
        c.handle_cadence_min_ms = 50;
    }
    if (c.handle_cadence_min_ms > c.handle_cadence_ms) {
        c.handle_cadence_min_ms = c.handle_cadence_ms;  // a floor above the period is a typo
    }
    c.handle_step_deg = iniFloatSec("fade", "handle_step_deg", 0.5f);
    if (c.handle_step_deg < 0.0f) {
        c.handle_step_deg = 0.0f;  // 0 = use the body's own [sun]/[moon] step_deg
    }
    if (c.handle_step_deg > 30.0f) {
        c.handle_step_deg = 30.0f;
    }
    c.handle_rel_lum = iniFloatSec("fade", "handle_rel_lum", 0.05f);
    if (!(c.handle_rel_lum >= 0.005f)) {
        c.handle_rel_lum = 0.005f;
    }
    if (c.handle_rel_lum > 1.0f) {
        c.handle_rel_lum = 1.0f;
    }

    // --- n09: the [shutdown] section. These are persistence windows, not preferences: a signal has
    // to hold this long before the plugin stops calling the Remix bridge (plugin/src/shutdown.h).
    // Too small and an alt-tab takes the world's lighting out for the session; too big and the
    // 2026-11-12 exit - 302 lights drawn per frame into a bridge whose server had already exited -
    // comes back. 0 means "one frame is enough", which is what the tests use.
    c.shutdown_present_ms = iniIntSec("shutdown", "present_ms", 1500);
    c.shutdown_device_ms = iniIntSec("shutdown", "device_ms", 3000);
    c.shutdown_window_ms = iniIntSec("shutdown", "window_ms", 750);
    c.shutdown_calls_ms = iniIntSec("shutdown", "calls_ms", 4000);
    c.shutdown_log_ms = iniIntSec("shutdown", "log_ms", 5000);
    c.stop_present_after_ms = iniIntSec("shutdown", "stop_present_after_ms", 0);
    if (c.shutdown_present_ms < 0) {
        c.shutdown_present_ms = 0;
    }
    if (c.shutdown_device_ms < 0) {
        c.shutdown_device_ms = 0;
    }
    if (c.shutdown_window_ms < 0) {
        c.shutdown_window_ms = 0;
    }
    if (c.shutdown_calls_ms < 0) {
        c.shutdown_calls_ms = 0;
    }
    if (c.shutdown_log_ms < 500) {
        c.shutdown_log_ms = 500;  // a shutdown line every half second is a new log spam bug
    }
    if (c.shutdown_log_ms > 60000) {
        c.shutdown_log_ms = 60000;
    }
    if (c.stop_present_after_ms < 0) {
        c.stop_present_after_ms = 0;
    }

    std::string list = iniString("list", "rhyremix_lights.txt");
    if (!list.empty() && (list.find(':') != std::string::npos ||
                          list[0] == '\\' || list[0] == '/')) {
        c.listPath = list;  // absolute (drive-letter or rooted)
    } else {
        c.listPath = moduleDir() + list;
    }

    std::string tlist = iniStringSec("trafficlights", "list", "rhyremix_traffic.txt");
    if (!tlist.empty() && (tlist.find(':') != std::string::npos ||
                           tlist[0] == '\\' || tlist[0] == '/')) {
        c.trafficPath = tlist;  // absolute (drive-letter or rooted)
    } else {
        c.trafficPath = moduleDir() + tlist;
    }

    // Same resolution rule as the lamp lists: a bare name means "next to the
    // .asi", because the game's working directory is the game root, not scripts/.
    if (!c.sky_settings.empty() &&
        c.sky_settings.find(':') == std::string::npos &&
        c.sky_settings[0] != '\\' && c.sky_settings[0] != '/') {
        c.sky_settings = moduleDir() + c.sky_settings;
    }

    // Three lines, not one: a single 70-argument format string drifts by one
    // argument every time a knob is added, and the log then prints the wrong
    // values while looking plausible.  Each group below is small enough to check
    // by eye, and the compiler's -Wformat has something to catch it on.
    rhyLog("config: ini=%s enabled=%d list=%s dist=%d..%d exit=%d max=%d+%d "
           "hours=%d..%d radius=%.2f cone=%.1f/%.2f shaping=%d focus=%.2f "
           "radiance=%.2f/%.2f/%.2f scale=%.2f cold=%.2f,%.2f,%.2f "
           "warm=%.2f,%.2f,%.2f resync=%d hashbase=%llu delay=%.0f "
           "gamma_block=%d budget=%u",
           path.c_str(), c.enabled, c.listPath.c_str(), c.min_distance,
           c.max_distance, c.exit_distance, c.max_lights, c.sticky_extra,
           c.on_hour, c.off_hour, c.radius, c.cone_angle, c.cone_softness,
           c.sphere_shaping, c.focus_exponent, c.radiance_big,
           c.radiance_middle, c.radiance_small, c.radiance_scale,
           c.cold[0], c.cold[1], c.cold[2], c.warm[0], c.warm[1], c.warm[2],
           c.resync_frames, c.light_hash_base, c.destroy_delay_ms,
           c.block_gamma, c.max_total_lights);
    rhyLog("config: lamp source cool=%s warm=%s (kelvin fallback %.0f/%.0f) -> "
           "rgb %.2f,%.2f,%.2f / %.2f,%.2f,%.2f",
           c.lamp_cold_family >= 0 ? rhyLampFamilyName(c.lamp_cold_family) : "kelvin",
           c.lamp_warm_family >= 0 ? rhyLampFamilyName(c.lamp_warm_family) : "kelvin",
           c.kelt_cold, c.kelt_warm, c.cold[0], c.cold[1], c.cold[2],
           c.warm[0], c.warm[1], c.warm[2]);
    {
        // Say what the emitting spheres will actually be. The shipped radius of
        // 15 game units is a 30 metre ball, and the log is the only place a
        // reader finds out which one is in effect this run.
        const float streetR = c.fixture_size ? rhyStreetRadius(0, c.fixture_scale)
                                             : c.radius;
        const float signalR = c.fixture_size ? rhySignalRadius(c.fixture_scale)
                                             : c.traffic_radius;
        const float k = (c.fixture_size && streetR > 0.0f)
                            ? c.radius_ref / streetR : 1.0f;
        rhyLog("config: fixture geometry %s (street big radius %.2f -> %.2f game "
               "units, signal %.2f -> %.2f), radiance x%.0f to hold flux, "
               "fixture_size=%d scale=%.2f ref=%.2f",
           c.fixture_size ? "real luminaire sizes" : "ini radius (legacy blob)",
           c.radius, streetR, c.traffic_radius, signalR, k * k,
           c.fixture_size, c.fixture_scale, c.radius_ref);
    }
    rhyLog("config: traffic=%d/%d source=%s list=%s period=%d radiance=%.2f radius=%.2f "
           "max=%d dist=%d..%d phase_near=%d night_only=%d resync=%d "
           "head_ttl=%dms head_normal=%d head_forward=%.2f head_grace=%dms "
           "lens_offset=%d | bulb radius=%.3f "
           "radiance=%.0f nits (fixture_size=%d) corona_radius=%.2f (%s) | "
           "glow radius=%.2f flux=%.0f (cap %.0f = one street luminaire)",
           c.traffic_enabled, c.traffic_mode,
           c.traffic_source ? "game" : "list", c.trafficPath.c_str(),
           c.traffic_period, c.traffic_radiance, c.traffic_radius,
           c.traffic_max_lights, c.traffic_max_distance,
           c.traffic_exit_distance, c.traffic_phase_near, c.traffic_night_only,
           c.traffic_resync_frames, c.traffic_head_ttl_ms, c.traffic_head_normal,
           c.traffic_head_forward, c.traffic_head_grace_ms, c.traffic_lens_offset,
           c.traffic_bulb_radius > 0.0f
               ? c.traffic_bulb_radius
               : rhySignalRadius(c.fixture_scale),
           c.traffic_bulb_radiance, c.fixture_size, c.traffic_corona_radius,
           c.traffic_corona_radius > 0.0f ? "resizes the game's 15.0 flare"
                                          : "the game's own 15.0 flare",
           c.traffic_glow_radius, c.traffic_glow_flux, 3473.0f);
    rhyLog("config: sun=%d radiance=%.2f col=%.2f,%.2f,%.2f ang=%.2f spin=%.1f "
           "tilt=%.1f roll=%.1f fade=%.2f minel=%.2f step=%.2f vol=%.2f hash=%llu "
           "| moon=%d radiance=%.3f col=%.2f,%.2f,%.2f ang=%.2f fade=%.2f "
           "step=%.2f hash=%llu",
           c.sun_enabled, c.sun_radiance, c.sun_color[0], c.sun_color[1],
           c.sun_color[2], c.sun_angular, c.sun_spin, c.sun_tilt, c.sun_roll,
           c.sun_fade_pow, c.sun_min_elevation, c.sun_step_deg,
           c.sun_volumetric, c.sun_hash, c.moon_enabled, c.moon_radiance,
           c.moon_color[0], c.moon_color[1], c.moon_color[2], c.moon_angular,
           c.moon_fade_pow, c.moon_step_deg, c.moon_hash);
    rhyLog("config: probe=%d pos=%.1f,%.1f,%.1f follow=%d dz=%.1f scale=%.4f "
           "radius=%.2f radiance=%.2f hash=%llu shape=%d cone=%d sweep=%d/%d x10^0..%d "
           "step=%dms refresh=%dms",
           c.probe, c.probe_x, c.probe_y, c.probe_z, c.probe_follow, c.probe_dz,
           c.probe_scale, c.probe_radius, c.probe_radiance, c.probe_hash,
           c.probe_shape,
           c.probe_cone, c.probe_sweep, c.probe_sweep_axis, c.probe_sweep_steps,
           c.probe_step_ms, c.probe_refresh_ms);
    rhyLog("config: sun_model=%d lat=%.2f lon=%.2f tz=%+.1f doy=%d track_date=%d "
           "turbidity=%.3f force=%d:%02d | moon_phase=%.2f | lamps=%s dusk=%.1f dawn=%.1f",
           c.sun_model, c.sky_latitude, c.sky_longitude, c.sky_timezone,
           c.sky_day_of_year, c.sky_track_date, c.sky_turbidity,
           c.sun_force_hour, c.sun_force_minute, c.moon_phase,
           c.lamp_mode ? "twilight" : "hours", c.lamp_dusk_deg, c.lamp_dawn_deg);
    rhyLog("config: sky_apply=%d day=%.3f night=%.3f moon=%.3f twilight=%.1f..%.1f "
           "emissive=%.2f(day)/%.2f(night) cloud=%.2f weather=%.3f/%.3f per s "
           "settings=%s probe=%d",
           c.sky_apply, c.sky_day, c.sky_night, c.sky_moon, c.sky_dusk_deg,
           c.sky_dawn_deg, c.sky_emissive_day, c.sky_emissive_night, c.sky_cloud,
           c.weather_turbidity_per_sec, c.weather_cloud_per_sec,
           c.sky_settings.c_str(), c.sky_probe);
    // s29: the time axis, on one line, because "why does the time of day flicker?"
    // is answered by these four numbers plus the doy the sky module is using.
    rhyLog("config: clock seed=%.0f ms/game-min jitter<=%.0f min reanchor=%dms "
           "calendar_stable_reads=%d",
           c.clock_ms_per_game_minute, c.clock_jitter_min, c.clock_reanchor_ms,
           c.clock_calendar_stable_reads);
    // s28: the anti-pop knobs, on one line, because "why did the street take a
    // second to come on?" and "why did it still pop?" are both answered here.
    rhyLog("config: fade in=%dms out=%dms step=%.2f creates/frame=%d "
           "player_grace=%dms sky=%.2fx+%.2f per s sun=%.2fx %.1f deg/s "
           "phase_fade=%dms handle=%dms(min %dms, <%.2f deg, <%.1f%% lum)",
           c.fade_in_ms, c.fade_out_ms, c.fade_step, c.creates_per_frame,
           c.player_grace_ms, c.sky_rel_per_sec, c.sky_abs_per_sec,
           c.sun_rel_per_sec, c.sun_deg_per_sec, c.phase_fade_ms,
           c.handle_cadence_ms, c.handle_cadence_min_ms, c.handle_step_deg,
           100.0f * c.handle_rel_lum);
    // n09: the shutdown windows, on one line, because "did the plugin notice the exit at all?" is
    // the question the 2026-11-12 log could not answer.
    rhyLog("config: shutdown present<=%dms device<=%dms window<=%dms calls<=%dms log=%dms "
           "stop_present_after=%dms (%s)",
           c.shutdown_present_ms, c.shutdown_device_ms, c.shutdown_window_ms,
           c.shutdown_calls_ms, c.shutdown_log_ms, c.stop_present_after_ms,
           c.stop_present_after_ms ? "Present is dropped once the bridge has been gone that long"
                                   : "Present is always forwarded to the bridge client");
}

// Timestamp of the ini the loaded values came from.  A FILETIME is two 32-bit
// halves; comparing them is enough to notice an edit without reading the file.
static ULARGE_INTEGER g_cfgMtime;
static bool g_cfgMtimeKnown = false;
static DWORD g_cfgCheckTick = 0;
static unsigned g_revision = 0;  // bumped on every (re)load of rhyremix.ini

unsigned rhyConfigRevision() { return g_revision; }

// True when the ini's last-modified time differs from what was parsed.
static bool iniChanged() {
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExA(iniPath().c_str(), GetFileExInfoStandard, &attr)) {
        return false;  // gone, or not readable: keep the values we have
    }
    ULARGE_INTEGER t;
    t.HighPart = attr.ftLastWriteTime.dwHighDateTime;
    t.LowPart = attr.ftLastWriteTime.dwLowDateTime;
    if (g_cfgMtimeKnown && g_cfgMtime.QuadPart == t.QuadPart) {
        return false;
    }
    g_cfgMtime = t;
    g_cfgMtimeKnown = true;
    return g_cfgLoaded;
}

const RhyConfig* rhyConfigGet() {
    if (!g_cfgLoaded) {
        g_cfgLoaded = true;
        loadConfig();
        iniChanged();  // remember the timestamp of what was just parsed
        ++g_revision;
    }
    return &g_cfg;
}

// Called every frame from hkPresent; re-parses the ini at most once a second.
// This is what makes the brightness knobs tunable without restarting the game,
// which matters because a Remix launch costs minutes and the tuning loop is
// dozens of iterations.
void rhyConfigReloadIfChanged() {
    if (!g_cfgLoaded) {
        return;  // first load happens through rhyConfigGet()
    }
    const DWORD now = GetTickCount();
    if (static_cast<DWORD>(now - g_cfgCheckTick) < 1000) {
        return;
    }
    g_cfgCheckTick = now;
    if (!iniChanged()) {
        return;
    }
    rhyLog("config: rhyremix.ini changed, re-reading it (lamp lists are NOT "
           "re-read, those need a restart)");
    loadConfig();
    ++g_revision;  // modules that gave up on the Remix API retry now
}
