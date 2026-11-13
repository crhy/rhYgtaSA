#include "rtxconf.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "log.h"
#include "remix.h"

static unsigned g_sets = 0;
static unsigned g_unknown = 0;
static unsigned g_badValue = 0;

// Last value actually sent per key. Without this the sky module would push
// rtx.skyBrightness sixty times a second, and the bridge documents the target as
// a settings *layer* - a file - not a per-frame uniform.
static std::map<std::string, std::string> g_sent;

unsigned rhyRtxSets() { return g_sets; }
unsigned rhyRtxUnknown() { return g_unknown; }
unsigned rhyRtxBadValue() { return g_badValue; }

// True for keys that reconfigure a renderer subsystem (quality preset,
// raytracing mode, upscaler, ray reconstruction, NRC training, the volumetric
// subsystem, view distance). Sending those after the first frame is what took
// the bridge down once; they belong in rtx.conf, which is read before any frame
// exists. Defined below next to the list, forward-declared here because
// rhyRtxApplyFile needs it.
static bool rhyRtxKeyIsUnsafe(const char* key);

bool rhyRtxSet(const char* key, const char* value) {
    const remixapi_Interface* api = rhyRemixApi();
    if (!api || !api->SetConfigVariable || !key || !value) {
        return false;
    }

    const remixapi_ErrorCode err = api->SetConfigVariable(key, value);
    if (err == REMIXAPI_ERROR_CODE_SUCCESS) {
        ++g_sets;
        g_sent[key] = value;
        return true;
    }
    // Only the first handful of each kind is logged: an unknown key is a typo in
    // rhyremix-rtx.conf and repeating it every frame buries the real messages.
    static int unknownLogged = 0;
    static int badValueLogged = 0;
    if (err == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS) {
        ++g_badValue;
        if (badValueLogged++ < 6) {
            rhyLog("rtxconf: %s = %s rejected: the key exists but the value does "
                   "not parse or is out of range", key, value);
        }
    } else {
        ++g_unknown;
        if (unknownLogged++ < 6) {
            rhyLog("rtxconf: %s = %s rejected: unknown key for this Remix build",
                   key, value);
        }
    }
    return false;
}

bool rhyRtxSetFloat(const char* key, float value, float minDelta) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(value));

    if (minDelta > 0.0f) {
        std::map<std::string, std::string>::iterator it = g_sent.find(key);
        if (it != g_sent.end()) {
            char* end = nullptr;
            const float prev = std::strtof(it->second.c_str(), &end);
            if (end != it->second.c_str() &&
                fabsf(prev - value) < minDelta) {
                return true;  // unchanged enough to be the same setting
            }
        }
    }
    return rhyRtxSet(key, buf);
}

// Trim ASCII whitespace in place.
static char* trim(char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        ++s;
    }
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n')) {
        *--e = '\0';
    }
    return s;
}

int rhyRtxApplyFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        rhyLog("rtxconf: no settings file at %s (that is fine: the sky module "
               "still drives what it can)", path);
        return 0;
    }
    int applied = 0, rejected = 0;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        char* p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';') {
            continue;
        }
        char* eq = std::strchr(p, '=');
        if (!eq) {
            rhyLog("rtxconf: ignoring '%s' in %s: expected key = value", p, path);
            ++rejected;
            continue;
        }
        *eq = '\0';
        const char* key = trim(p);
        const char* val = trim(eq + 1);
        if (*key == '\0' || *val == '\0') {
            ++rejected;
            continue;
        }
        if (rhyRtxKeyIsUnsafe(key)) {
            rhyLog("rtxconf: refusing to push '%s' at runtime: it reconfigures a "
                   "renderer subsystem, and doing that mid-frame took the bridge "
                   "down once. Put it in rtx.conf instead.", key);
            ++rejected;
            continue;
        }
        if (rhyRtxSet(key, val)) {
            ++applied;
        } else {
            ++rejected;
        }
    }
    std::fclose(f);
    rhyLog("rtxconf: applied %d setting(s) from %s (%d rejected)", applied, path,
           rejected);
    return applied;
}

// ------------------------------------------------------------------- probing

// Which keys the runtime will actually take is not what SetConfigVariable's
// return code says. Measured on Remix 1.6.1: the client returns SUCCESS for
// every key in the list below, while the server logs
//
//   warn: [RTX Option]: 'rtx.userBrightness' is not an RtxOption or a key in a
//         registered dynamic namespace.
//
// for twelve of them. So the return code only proves the key parses; the truth
// lives in rtx-remix/logs/remix-dxvk.log, and scripts/check-rtx-probe.sh
// cross-references the two. Anything the server calls "not an RtxOption" is a
// startup-only option: it belongs in rtx.conf, which is read before the first
// frame, and no amount of pushing it at runtime will do anything.
//
// Probing writes real values into a live renderer, so it is off by default and
// the list below is restricted to keys whose test value is the same value the
// shipped rtx.conf already uses. Keys that switch renderer modes, upscalers or
// cache training are NOT here: see the note above kUnsafeKeys.
struct ProbeKey {
    const char* key;
    const char* value;
};

static const ProbeKey kProbeKeys[] = {
    // brightness / tone
    {"rtx.skyBrightness", "1.0"},
    {"rtx.emissiveIntensity", "1.0"},
    {"rtx.autoExposure.enabled", "True"},
    {"rtx.autoExposure.evMinValue", "-4"},
    {"rtx.autoExposure.evMaxValue", "4"},
    {"rtx.autoExposure.autoExposureSpeed", "1.0"},
    {"rtx.autoExposure.averageMode", "1"},
    {"rtx.userBrightness", "50"},
    {"rtx.userBrightnessEVRange", "10"},
    {"rtx.localtonemap.shadows", "1.0"},
    {"rtx.localtonemap.highlights", "1.0"},
    {"rtx.localtonemap.contrast", "0.5"},
    {"rtx.localtonemap.saturation", "0.5"},
    {"rtx.localtonemap.exposure", "1.0"},
    {"rtx.localtonemap.boostLocalContrast", "False"},
    {"rtx.tonemap.finalizeWithACES", "True"},
    // n22: the key the plugin now drives at runtime from the sun's elevation
    // (plugin/src/exposure.h, sun.cpp). Probing it costs one SetConfigVariable and
    // puts its name in the probe log next to the doubled-name trap above, which is
    // how a live run confirms the name the runtime knows is this one.
    {"rtx.tonemap.exposureBias", "-0.5"},
    {"rtx.tonemap.toneCurveMinStops", "-6"},
    {"rtx.tonemap.toneCurveMaxStops", "6"},
    {"rtx.tonemap.colorGradingEnabled", "False"},
    {"rtx.tonemap.enableHighlightRecovery", "True"},
    // light behaviour
    {"rtx.enableDirectLighting", "True"},
    {"rtx.enableSecondaryBounces", "True"},
    {"rtx.enablePrimaryShadows", "True"},
    {"rtx.fallbackLightRadiance", "0"},
    {"rtx.fallbackLightRadius", "3"},
    {"rtx.fallbackLightType", "1"},
    {"rtx.ignoreGameDirectionalLights", "True"},
    {"rtx.uniqueObjectDistance", "1000"},
    // path tracing depth and denoising
    {"rtx.pathMaxBounces", "3"},
    {"rtx.pathMinBounces", "1"},
    {"rtx.usePostFilter", "False"},
    {"rtx.postFilterThreshold", "10"},
    {"rtx.di.enableDenoiserConfidence", "False"},
    {"rtx.di.permutationSamplingNthFrame", "1"},
    {"rtx.restirGI.useSampleStealing", "1"},
    {"rtx.restirGI.useVirtualSample", "False"},
    {"rtx.neeCache.enableModeAfterFirstBounce", "2"},
    // shading realism
    {"rtx.shadowTerminator.enableOffset", "True"},
    {"rtx.shadowTerminator.maxLength", "0.05"},
};

// Deliberately never sent. On 2026-11-11 a run that pushed the whole candidate
// list, including these, produced a white first frame and then a C++ exception
// in the bridge server (0xe06d7363) about seventy seconds later, having never
// reached gameplay - where the previous build had run for eight minutes. The
// common factor is that these reconfigure the renderer while it is rendering:
// a quality preset, a raytracing mode, the upscaler, ray reconstruction, the
// neural radiance cache's training pass, the view-distance mode. Whatever the
// exact one is, they are startup options and rtx.conf is where they belong.
//
//   rtx.graphicsPreset, rtx.raytraceModePreset, rtx.qualityDLSS,
//   rtx.dlss.dlssPreset, rtx.enableRayReconstruction, rtx.enableCulling,
//   rtx.russianRouletteMode, rtx.enableVolumetricLighting,
//   rtx.volumetrics.*, rtx.neuralRadianceCache.*, rtx.shadowTerminator.enable,
//   rtx.viewDistance.distanceMode, rtx.skyProbeSide, rtx.skyForceHDR,
//   rtx.ignoreLights (a list: an empty value means "ignore nothing")
static const char* const kUnsafeKeys[] = {
    "rtx.graphicsPreset", "rtx.raytraceModePreset", "rtx.qualityDLSS",
    "rtx.dlss.dlssPreset", "rtx.enableRayReconstruction", "rtx.enableCulling",
    "rtx.russianRouletteMode", "rtx.enableVolumetricLighting",
    "rtx.neuralRadianceCache.trainCache",
    "rtx.neuralRadianceCache.includeDirectLighting",
    "rtx.neuralRadianceCache.enableFog", "rtx.shadowTerminator.enable",
    "rtx.viewDistance.distanceMode", "rtx.skyProbeSide", "rtx.skyForceHDR",
    "rtx.ignoreLights", "rtx.volumetrics.transmittanceMeasurementDistance",
    "rtx.volumetrics.froxelGridResolutionScale",
    "rtx.volumetrics.initialRISSampleCount", "rtx.volumetrics.enableFogRemap",
};

static bool rhyRtxKeyIsUnsafe(const char* key) {
    for (size_t i = 0; i < sizeof kUnsafeKeys / sizeof kUnsafeKeys[0]; ++i) {
        if (std::strcmp(kUnsafeKeys[i], key) == 0) {
            return true;
        }
    }
    // Anything under a namespace that switches a whole subsystem is treated the
    // same way, so a key we have never seen is not assumed safe.
    return std::strstr(key, "volumetrics.") != nullptr ||
           std::strstr(key, "neuralRadianceCache.") != nullptr ||
           std::strstr(key, "dlss.") != nullptr;
}

void rhyRtxProbeOnce() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    if (!rhyRemixApi()) {
        rhyLog("rtxconf: probe skipped, the Remix API is not up");
        return;
    }
    int ok = 0, bad = 0;
    rhyLog("rtxconf: probing %u candidate rtx.* keys. NOTE: the client accepts "
           "every key, so this only proves the name parses - cross-reference "
           "rtx-remix/logs/remix-dxvk.log for \"is not an RtxOption\" with "
           "scripts/check-rtx-probe.sh", 
           static_cast<unsigned>(sizeof kProbeKeys / sizeof kProbeKeys[0]));
    for (size_t i = 0; i < sizeof kProbeKeys / sizeof kProbeKeys[0]; ++i) {
        // Probe keys bypass the cache so each one is really sent.
        const bool accepted = rhyRtxSet(kProbeKeys[i].key, kProbeKeys[i].value);
        rhyLog("rtxconf probe: %-52s = %-12s %s", kProbeKeys[i].key,
               kProbeKeys[i].value[0] ? kProbeKeys[i].value : "(empty)",
               accepted ? "sent (client ok)" : "REFUSED by client");
        accepted ? ++ok : ++bad;
    }
    rhyLog("rtxconf: probe done, %d sent / %d refused. Keys the server then "
           "logged as 'not an RtxOption' are startup-only: put them in rtx.conf", ok, bad);
}
