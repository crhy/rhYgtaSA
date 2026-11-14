#!/usr/bin/env python3
"""n10/n24 offline night-lighting sim: what the shipped profile actually puts on the sand.

    python3 tools/night_ambient.py [--ini config/rhyremix-production.ini]
                                   [--conf config/rtx.conf] [--quiet] [--json] [--ev EV]

n24 (2026-11-14) brought this off the parked `30a1a43` and made it describe TODAY's tree:
it reads n22's `[tonemap] night_ev` instead of only `rtx.tonemap.exposureBias` (the plugin
overwrites that key at runtime, so the old sim was simulating the day anchor at night), it
models the plugin's night ceiling on the ambient (`Scene.apply_sky_cap`, and the ini's
`[sky] ambient_cap_mult` is read by default), it prints the moon light's PUBLISHED radiance
(`Scene.moon_published_radiance`, the same arithmetic as `sun.cpp beamRadiance()`) because
that - not `[moon] radiance` - is what the cap compares against, and it reports the fallback
light against the moon (`fallback_term_no_lights`, `fallback_over_moon`) rather than only
against the lamp pool. `tests/test_night_ambient.py` is the gate on those numbers.

Why this exists
---------------
rhY's 2026-11-12c video (build ba4c2c3, 23:42-23:56, Verona Beach / Santa Maria pier)
measures sand "away from any lamp" at 25-72 % of the sand directly under a lamp
(.strata/notes-n10-night-ambient-realism.md, step 2). In the SCENE that ratio is about
1 : 1000 - the lamps and the moon are not the problem. The problem is everything that
sits between the scene and the image: the runtime's LOCAL tonemapper lifts shadow areas
by 2^rtx.localtonemap.shadows and crushes highlight areas by 2^-rtx.localtonemap.highlights,
which is a 2^6.5 = 90x squeeze of exactly the ratio rhY is looking at.

So a check that only reads the ini cannot answer the question, and a check that only
looks at screenshots cannot say why. This file does both: it computes the scene-linear
radiance of the sand from the shipped ini (one moon distant light + the lamp pool + the
sky probe + the fallback light), then pushes it through the renderer's own tone curve
(the histogram -> noise-aware slope -> prefix-sum -> blur chain of
rtx/pass/tonemap/tonemapping_tone_curve.comp.slang, Remix 1.6.1) and reports the ratio
in the image.

Every number below is quoted to the 1.6.1 source that produced it
(build/tmp/s18/dxvk-remix-remix-1.6.1, the unpacked release tarball; RtxOptions.md ships
inside it). Nothing here is fitted to the video: the video is the thing being predicted.

Light math (the runtime's own, not a guess)
------------------------------------------
sphere light   lightSample radiance = radiance, cone pdf = (1+cos)/(2*pi*sin^2) so
               1/pdf = pi*r^2/d^2 (concept/light/sphere_light.slangh); with a diffuse
               BRDF f_r = albedo/pi:
                   L_out = albedo * radiance * (r^2/d^2) * cos(incidence) * shaping
               and sphereLightCalcPower = 4*pi*r^2 * pi * radiance, positional weight
               = (power/4*pi)/d^2 = pi*r^2*radiance/d^2 - the same number.
distant light  lightSample radiance = radiance/sin^2(halfAngle) with a uniform-cone pdf
               1/(2*pi*(1-cos)) (concept/light/distant_light.slangh); the two factors
               reduce to pi*radiance, so with f_r = albedo/pi:
                   L_out = albedo * radiance * cos(incidence)
sky            composite/geometry_resolver/integrator_indirect all sample
               skyRadiance = rtx.skyBrightness * SkyProbe(...); the probe is rasterized
               from the game's own sky draw calls in the game's colour format
               (rtx.skyForceHDR = False), so its radiance is <= 1.0 per channel.
fallback light rtx_light_manager.cpp:249-330: mode 1 (the runtime default) creates it
               whenever no lights are present, and with rtx.fallbackLightType = 1 it is a
               SPHERE light at mainCamera.getPosition() + fallbackLightPositionOffset with
               rtx.fallbackLightRadius - a light welded to the camera.
"""
import argparse
import json
import math
import re
import sys

# ---------------------------------------------------------------- ini / conf parsing

INI_NUM = re.compile(r"^\s*([A-Za-z0-9_]+)\s*=\s*(-?[\d.eE+]+)\s*(?:;.*)?$")
INI_TRIPLE = re.compile(r"^\s*([A-Za-z0-9_]+)\s*=\s*([-\d.eE+]+)\s*,\s*([-\d.eE+]+)\s*,\s*([-\d.eE+]+)\s*(?:;.*)?$")
CONF_KEY = re.compile(r"^\s*(rtx\.[A-Za-z0-9_.]+)\s*=\s*(.+?)\s*(?:#.*)?$")


def parse_ini(path):
    """{section: {key: float | (r,g,b)}} - comments and blank lines ignored."""
    out, section = {}, None
    with open(path, "r") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith(";") or not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                out.setdefault(section, {})
                continue
            if section is None:
                continue
            m = INI_TRIPLE.match(line)
            if m:
                out[section][m.group(1)] = tuple(float(x) for x in m.groups()[1:])
                continue
            m = INI_NUM.match(line)
            if m:
                out[section][m.group(1)] = float(m.group(2))
    return out


def parse_conf(path):
    """{rtx.key: value} with True/False/numbers coerced; last write wins (the runtime's rule)."""
    out = {}
    with open(path, "r") as fh:
        for line in fh:
            m = CONF_KEY.match(line)
            if not m:
                continue
            key, raw = m.group(1), m.group(2).strip()
            if raw.lower() in ("true", "false"):
                out[key] = raw.lower() == "true"
            elif "," in raw:
                try:
                    out[key] = tuple(float(p) for p in raw.split(","))
                except ValueError:
                    out[key] = raw
            else:
                try:
                    out[key] = float(raw)
                except ValueError:
                    out[key] = raw
    return out


# ---------------------------------------------------------------- the scene

SAND_ALBEDO = 0.270
# Mean BT.709 luminance of the pack's own sand albedo, measured (not assumed):
# <game>/rtx-remix/mods/gameReadyAssets/texture/sand/Ground054_albedo.dds, 2048^2,
# mean linear RGB (0.337, 0.262, 0.158) -> 0.2126R+0.7152G+0.0722B = 0.270.
SAND_ALBEDO_RGB = (0.337, 0.262, 0.158)


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb(c):
    return 12.92 * c if c <= 0.0031308 else 1.055 * (c ** (1.0 / 2.4)) - 0.055


def bt709(rgb):
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def lunar_phase_factor(phase):
    """solar.cpp lunarPhaseFactor(): the lit fraction squared (Lommel-Seeliger-ish)."""
    p = phase - int(phase)
    if p < 0.0:
        p += 1.0
    lit = 0.5 * (1.0 - math.cos(2.0 * math.pi * p))
    return lit * lit


def aces_film(x):
    """ACESFilm as shipped: ACES.hlsl via rtx/utility/color.slangh (non-legacy branch)."""
    a, b, c, d, e = 0.0245786, 0.0000305556, 0.000090537, 0.00032, 0.00039
    return (x * (a * x + b)) / (x * (c * x + d) + e)


def size_band(lamp_type):
    """streetlights.cpp sizeBand(): types 1-2/7-8 big, 3-4/9-10 middle, 5-6/11-12 small."""
    return ((lamp_type - 1) % 6) // 2 if 1 <= lamp_type <= 12 else 1


FIXTURE_RADIUS = {0: 0.20, 1: 0.15, 2: 0.10}   # lamps.cpp rhyStreetFixture(), metres
FIXTURE_CONE = {0: 125.0, 1: 120.0, 2: 140.0}  # same function, full cone angle


class Scene:
    """A flat sand plane under the lamp list, lit by one moon and (optionally) a fallback light."""

    def __init__(self, ini, conf, lamps, sand_z=19.843):
        self.ini, self.conf, self.lamps, self.sand_z = ini, conf, lamps, sand_z
        sky = ini.get("sky", {})
        moon = ini.get("moon", {})
        self.sky_night = sky.get("night", 0.0)
        self.sky_moon = sky.get("moon", 0.0)
        self.moon_radiance = moon.get("radiance", 0.0)
        self.moon_colour = moon.get("color", (1.0, 1.0, 1.0))
        self.moon_elev = 31.0   # the video's value: rhyremix.log 11:50:17 "moon elev=+30.4"
        self.conf_sky = float(conf.get("rtx.skyBrightness", 1.0))
        self.emissive = float(conf.get("rtx.emissiveIntensity", 1.0))
        self.vc_strength = float(conf.get("rtx.vertexColorStrength", 0.6))
        self.ignore_vc = bool(conf.get("rtx.ignoreAllVertexColorBakedLighting", False))
        self.fallback_mode = int(conf.get("rtx.fallbackLightMode", 1))  # runtime default 1
        self.fallback_rad = conf.get("rtx.fallbackLightRadiance", (1.6, 1.8, 2.0))
        if isinstance(self.fallback_rad, float):
            self.fallback_rad = (self.fallback_rad,) * 3
        self.fallback_type = int(conf.get("rtx.fallbackLightType", 0))
        self.fallback_radius = float(conf.get("rtx.fallbackLightRadius", 5.0))
        # n24: the plugin's night cap on the published ambient, as an absolute value.
        # None = no cap. Read from [sky] ambient_cap_mult (the shipped ini carries 0.25);
        # apply_sky_cap() overrides it, and 0 in the ini means the rule is off.
        self.sky_cap = None
        mult = sky.get("ambient_cap_mult", 0.0)
        if mult > 0.0:
            self.sky_cap = mult * self.moon_published_radiance(
                ini.get("sun", {}).get("turbidity", 0.12))

    # ---- the moon as the PLUGIN publishes it (n24). sun.cpp beamRadiance():
    #      radiance[c] = base * tint[c] * transmit[c] / zenithT[c], with base =
    #      [moon] radiance * lunarPhaseFactor(phase) * cloud transmission, transmit from
    #      solar.cpp's Beer-Lambert over the Kasten-Young air mass. The luminance of that
    #      triple is the number solar.cpp skyAmbientCap compares the ambient against.
    KTAU = (0.0681 + 0.020, 0.1054 + 0.030, 0.2162 + 0.010)
    KAER = (0.7576, 1.0, 1.2687)
    LUMA = (0.2126, 0.7152, 0.0722)

    def air_mass(self, elev_deg):
        """solar.cpp relativeAirMass(90 - elev): Kasten-Young 1989, clamped to 12."""
        z = 90.0 - elev_deg
        if z >= 91.0:
            return 0.0
        zz = max(z, 0.0)
        denom = math.cos(math.radians(zz)) + 0.50572 * (96.07995 - zz) ** -1.6364
        if denom <= 1e-4:
            return 12.0
        return min(max(1.0 / denom, 1.0), 12.0)

    def moon_published_radiance(self, turbidity=0.12, phase=None, elev_deg=None):
        if phase is None:
            phase = lunar_phase_factor(self.ini.get("moon", {}).get("phase", 0.5))
        if elev_deg is None:
            elev_deg = self.moon_elev
        base = self.moon_radiance * phase
        m = self.air_mass(elev_deg)
        lum = 0.0
        for c in range(3):
            tau = self.KTAU[c] + turbidity * self.KAER[c]
            zenith_t = math.exp(-tau)
            t = (math.exp(-tau * m) / zenith_t) if zenith_t > 1e-4 else 0.0
            lum += self.LUMA[c] * base * self.moon_colour[c] * t
        return lum

    def apply_sky_cap(self, mult, turbidity=0.12):
        """Model solar.cpp skyAmbientCap(): the published ambient may not exceed
        mult x the moon light's own published radiance. Returns the capped ambient."""
        self.sky_cap = mult * self.moon_published_radiance(turbidity) if mult > 0.0 else None
        return self.sky_term()

    # ---- the lamp pool: what the 7002-lamp list puts on one sand patch
    def lamp_term(self, px, py, pz, radius=300.0):
        """sum of radiance*(r^2/d^2)*cos(incidence)*shaping over the lamp pool."""
        total = 0.0
        for (t, x, y, z) in self.lamps:
            d2 = (x - px) ** 2 + (y - py) ** 2 + (z - pz) ** 2
            if d2 > radius * radius or d2 < 0.25:
                continue
            d = math.sqrt(d2)
            band = size_band(t)
            r = FIXTURE_RADIUS[band]
            rad = self.lamp_radiance(t)
            cosi = max(0.0, (z - pz) / d)
            # UsdLux-style radial shaping over the fixture's cone (coneAngleDegrees is the
            # FULL angle; streetlights.cpp cone_angle/cone_softness, lamps.cpp rhyStreetFixture).
            half = math.radians(FIXTURE_CONE[band] * 0.5)
            ang = math.acos(max(-1.0, min(1.0, cosi)))
            shaping = 1.0 if ang <= half else 0.0
            total += rad * (r * r / d2) * cosi * shaping
        return total

    def lamp_radiance(self, lamp_type):
        """streetlights.cpp createLight(): band radiance * (radius_ref/radius)^2 * scale."""
        ini = self.ini.get("streetlights", {})
        band = size_band(lamp_type)
        base = {0: ini.get("radiance_big", 2.0),
                1: ini.get("radiance_middle", 1.2),
                2: ini.get("radiance_small", 0.8)}[band]
        r = FIXTURE_RADIUS[band]
        rref = ini.get("radius_ref", 15.0)
        if ini.get("fixture_size", 1.0):
            return base * (rref / r) ** 2 * ini.get("radiance_scale", 1.0)
        return base * ini.get("radiance_scale", 1.0)

    # ---- the moon: one distant light, cool, with real shadows (a distant light is
    #      shadowed by the primary ray; rtx.enablePrimaryShadows is on and we never touch it)
    def moon_term(self):
        return self.moon_radiance * math.sin(math.radians(self.moon_elev))

    # ---- the sky probe as an ambient term. rtx.skyBrightness multiplies a probe whose
    #      radiance is <= 1.0 per channel (rasterized in the game's colour format), so
    #      skyBrightness is also the upper bound of what the sky can add. At night the
    #      value that matters is the one sun.cpp publishes (applySkySettings: sky_night +
    #      sky_moon*phase*elev), not the rtx.conf constant - rtx.conf is only what the
    #      renderer uses before the plugin has pushed anything.
    def sky_term(self, sky_probe_radiance=1.0, use_published=True):
        # The cap is a PLUGIN rule (solar.cpp skyAmbientCap, applied to the value
        # sun.cpp pushes). rtx.conf's rtx.skyBrightness is what the renderer uses
        # BEFORE the plugin has pushed anything, so it is reported uncapped on purpose:
        # that is the 1.0-against-a-0.067-moon number n10 measured.
        val = (self.sky_night + self.sky_moon) if use_published else self.conf_sky
        if use_published and self.sky_cap is not None:
            val = min(val, self.sky_cap)
        return val * sky_probe_radiance

    # ---- the fallback light: mode 1 = "only when no lights are present", mode 2 = always.
    #      With type 1 (Sphere) it sits at the camera, so it lights the near ground from
    #      every side with no shadow and it moves when the camera turns.
    def fallback_term(self, lights_present=True, distance=6.0):
        if self.fallback_mode == 0:
            return 0.0
        if self.fallback_mode == 1 and lights_present:
            return 0.0
        lum = bt709(self.fallback_rad)
        if self.fallback_type == 1:      # Sphere at the camera
            r = self.fallback_radius
            return lum * (r * r / (distance * distance))
        return lum * 0.5                # Distant: cos-weighted average over the hemisphere

    # ---- vertex colour: opaque_surface_material_interaction.slangh:703-718 divides the
    #      vertex colour by its own maximum when rtx.vertexColorIsBakedLighting is true
    #      (the runtime default), then mixes it toward white by rtx.vertexColorStrength, so
    #      the multiplier is in [strength, 1] - it can only darken. With
    #      rtx.ignoreAllVertexColorBakedLighting the COLOR0 stream is not even captured
    #      (d3d9_rtx.cpp:295) and the multiplier is exactly 1.
    def vertex_colour_factor(self):
        if self.ignore_vc:
            return 1.0
        return 1.0  # normalized baked lighting: worst case 1.0 (brightest possible)

    def sand_radiance(self, px, py, lights_present=True):
        e = (self.lamp_term(px, py, self.sand_z) + self.moon_term() +
             self.sky_term() + self.fallback_term(lights_present))
        return SAND_ALBEDO * e * self.vertex_colour_factor()


# ---------------------------------------------------------------- the renderer's tone chain

def tone_curve(luminances, min_stops=-24.0, max_stops=8.0, dynamic_range=15.0,
               max_exposure_increase=5.0, shadow_contrast=0.0, shadow_contrast_end=0.0,
               curve_shift=0.0, samples=256, blur_radius=12):
    """The 1.6.1 global tone curve: histogram -> noise-aware slopes -> prefix sum -> clamp
    -> box blur. Faithful to tonemapping_histogram.comp.slang and
    tonemapping_tone_curve.comp.slang (kToneCurveSampleCount = 256, the +1 saliency per
    bin, the two sections split at middle grey 0.18, the iterative threshold of eq. 17
    with 16 iterations, `myToneCurve = min(myToneCurve, binLogLuminance + maxExposureIncrease)`).
    Returns (bin_log_in, curve_log_out)."""
    hist = [0.0] * samples
    for lum in luminances:
        if not (0.0 < lum < 2.0 ** max_stops):
            continue
        loglum = max(math.log2(lum), min_stops)
        pos = (samples - 1.0) * (loglum - min_stops) / (max_stops - min_stops)
        left = int(pos)
        right = min(left + 1, samples - 1)
        frac = pos - left
        hist[left] += (1.0 - frac)
        hist[right] += frac
    hist = [1.0 + h for h in hist]          # the "+1 saliency" the shader adds

    delta = (max_stops - min_stops) / float(samples)
    crossover = int(samples * (math.log2(0.18) - min_stops) / (max_stops - min_stops))

    def slopes(lo_bin, hi_bin, out_lo, out_hi):
        # computeSlopes(): local histogram for this section, normalized, then eq. (14)/(17)
        h = [hist[i] if lo_bin <= i < hi_bin else 0.0 for i in range(samples)]
        total = max(sum(h), 1e-6)
        h = [x / total for x in h]
        r = out_hi - out_lo
        r_over_delta = r / delta
        rcp = [1.0 / x if x > 0.0 else 0.0 for x in h]
        thresh = 1e-16
        for _ in range(16):
            passed = [1.0 if x >= thresh else 0.0 for x in h]
            len_omega = sum(passed)
            sum_recip = sum(rc * p for rc, p in zip(rcp, passed))
            thresh = (len_omega - r_over_delta) / max(sum_recip, 1e-6)
        passed = [1.0 if x >= thresh else 0.0 for x in h]
        len_omega = sum(passed)
        sum_recip = sum(rc * p for rc, p in zip(rcp, passed))
        return [(1.0 + (rc * (r_over_delta - len_omega) / max(sum_recip, 1e-6))) * p
                if rc else 0.0 for rc, p in zip(rcp, passed)]

    lo_s = slopes(0, crossover, min(-dynamic_range, math.log2(0.17)), math.log2(0.18))
    hi_s = slopes(crossover, samples, math.log2(0.18), 0.0)
    slope = []
    for i in range(samples):
        s = lo_s[i] if i < crossover else hi_s[i]
        if i < crossover:
            s = max(s, 0.0)                 # rtx.tonemap.shadowMinSlope (default 0)
        slope.append(s)

    binlog = [(i / float(samples)) * (max_stops - min_stops) + min_stops
              for i in range(samples)]
    curve, acc = [], 0.0
    for s in slope:
        acc += s
        curve.append(acc)
    sum_slopes = acc
    curve = [(curve[i] - slope[i] - sum_slopes) * delta for i in range(samples)]
    for i in range(samples):
        curve[i] -= shadow_contrast * max(0.0, shadow_contrast_end - binlog[i])
        curve[i] = min(curve[i], binlog[i] + max_exposure_increase)
    # box blur, radius 12, via a summed area table
    sat, acc = [], 0.0
    for c in curve:
        acc += c
        sat.append(acc)
    blurred = []
    for i in range(samples):
        high = min(i + blur_radius, samples - 1)
        low = max(0, i - blur_radius - 1)
        blurred.append((sat[high] - (sat[low] if low >= 0 else 0.0)) /
                       max(float(high - low), 1e-6))
    return binlog, [c + curve_shift for c in blurred]


def apply_curve(binlog, curve, lum):
    """dynamicToneMapper(): sample the curve at log2(lum) and rescale the colour."""
    if lum <= 0.0:
        return 0.0
    loglum = math.log2(lum)
    lo, hi = binlog[0], binlog[0] + (len(binlog) - 1) * (binlog[1] - binlog[0])
    # dynamicToneMapper(): biasedLuminance = (log2(L) - toneCurveMinStops) /
    # (toneCurveMaxStops - toneCurveMinStops), then a 1D texture sample over 256 texels.
    pos = (loglum - lo) / (hi - lo) * (len(binlog) - 1)
    pos = max(0.0, min(len(binlog) - 1.0, pos))
    i = int(pos)
    j = min(i + 1, len(binlog) - 1)
    f = pos - i
    out_log = curve[i] * (1.0 - f) + curve[j] * f
    return math.pow(2.0, out_log)


def local_tonemap_bounds(conf):
    """How much the LOCAL tonemapper (rtx.tonemappingMode = 1, the runtime default) can
    squeeze a shadow/highlight pair. luminance.comp.slang builds three synthetic exposures
    and pushes shadows = pow(2, rtx.localtonemap.shadows), highlights = pow(2, -highlights);
    the weight pass then lets a dark pixel take almost the shadow exposure and a bright pixel
    almost the highlight one, so the pair's ratio can be divided by
    2^(shadows + highlights) before anything else sees it."""
    sh = float(conf.get("rtx.localtonemap.shadows", 2.0))
    hi = float(conf.get("rtx.localtonemap.highlights", 4.0))
    return 2.0 ** (sh + hi)


# ------------------------------------------------------- the chain, as one callable path

def tone_curve_for(conf, luminances):
    """tone_curve() with the shipped profile's own keys. One place so the report and the
    test cannot drift apart."""
    return tone_curve(
        luminances,
        min_stops=float(conf.get("rtx.tonemap.toneCurveMinStops", -24.0)),
        max_stops=float(conf.get("rtx.tonemap.toneCurveMaxStops", 8.0)),
        dynamic_range=float(conf.get("rtx.tonemap.dynamicRange", 15.0)),
        max_exposure_increase=float(conf.get("rtx.tonemap.maxExposureIncrease", 5.0)),
        shadow_contrast=float(conf.get("rtx.tonemap.shadowContrast", 0.0)),
        shadow_contrast_end=float(conf.get("rtx.tonemap.shadowContrastEnd", 0.0)),
        curve_shift=float(conf.get("rtx.tonemap.curveShift", 0.0)))


def display_luminance(binlog, curve, conf, lum, exposure=1.0):
    """scene-linear -> what the panel shows. exposureBias is exp2'd by the runtime
    (dispatchApplyToneMapping: exposureFactor = exp2(exposureBias + userEVBias)); with
    rtx.autoExposure.enabled = False nothing else multiplies the frame."""
    out = apply_curve(binlog, curve, lum * exposure)
    if bool(conf.get("rtx.tonemap.finalizeWithACES", False)):
        out = aces_film(out)
    return linear_to_srgb(max(0.0, min(1.0, out)))


def night_ev(ini, conf):
    """The EV a night frame actually gets on today's tree (n24 review of this sim).

    rtx.conf's rtx.tonemap.exposureBias is the STARTUP value and, since n22, the
    full-daylight value: with [tonemap] apply=1 the plugin overwrites that same option at
    runtime with [tonemap] night_ev once the sun is down (plugin/src/exposure.cpp, pushed
    through rhyRtxSetFloat at 10 Hz), and SetConfigVariable writes the user layer, which
    outranks rtx.conf. So a night sim that reads only rtx.conf is simulating the day's
    anchor. n22 ships day_ev -0.5 / night_ev +0.5, i.e. +1.0 EV between the two ends.
    """
    tm = ini.get("tonemap", {})
    ev = float(conf.get("rtx.tonemap.exposureBias", 0.0))
    if int(tm.get("apply", 0)) == 1 and "night_ev" in tm:
        ev = float(tm["night_ev"])
    return ev


def frame_luminances(sc, p_under, p_away, grid=41):
    """A frame's worth of luminances for the histogram: the sand along the transect between
    the two metric points, plus the sky band. The video's split is roughly half ground, 30 %
    sky, the rest lit geometry; the sky is carried at its ceiling (skyBrightness x probe 1.0)
    so the histogram is a worst case for how much the curve is asked to lift."""
    (ux, uy), (ax, ay) = p_under, p_away
    out = []
    for i in range(grid):
        for j in range(grid):
            x = ux + (ax - ux) * i / float(grid - 1)
            y = uy + (ay - uy) * j / float(grid - 1)
            out.append(sc.sand_radiance(x, y))
    return out + [sc.sky_term()] * (len(out) // 2)


# ---------------------------------------------------------------- the report

def pier_lamps():
    """A lamp layout with the density the real list has at Verona Beach. The 2026-11-12
    rhyremix_lights.txt puts 228 lamps within 300 units of the probe (2489.9, -1656.0),
    median head height 28.1 against sand at 19.8. This reproduces the shape of what the
    video shows: a pier row at 25 unit spacing, a motel post cluster, and OPEN SAND that
    is 100+ units from any of them - which is where 'away from any lamp' is measured."""
    lamps = []
    for i in range(20):                                   # the pier rail row
        lamps.append((4, 2300.0 + 25.0 * i, -1650.0, 28.1))
    for i in range(3):                                    # the motel double-globe posts
        for j in range(2):
            lamps.append((9, 2400.0 + 30.0 * i, -1750.0 - 30.0 * j, 26.0))
    return lamps


def metric_points(lamps, open_sand=(2560.0, -1500.0)):
    """(under a lamp, away from every lamp) in world XY. 'under' is the sand patch directly
    below the first lamp; 'away' is a patch whose distance to the nearest lamp head is
    printed by the caller (it must be far - that is the whole point of the metric)."""
    t, x, y, z = lamps[0]
    return (x, y), open_sand


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ini", default="config/rhyremix-production.ini")
    ap.add_argument("--conf", default="config/rtx.conf")
    ap.add_argument("--grid", type=int, default=41)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--ev", type=float, default=None,
                    help="override the EV the frame gets; default is the profile's own "
                         "([tonemap] night_ev when the plugin drives it, else "
                         "rtx.tonemap.exposureBias)")
    a = ap.parse_args()

    ini, conf = parse_ini(a.ini), parse_conf(a.conf)
    lamps = pier_lamps()
    sc = Scene(ini, conf, lamps)

    (ux, uy), (ax, ay) = metric_points(lamps)
    under = sc.sand_radiance(ux, uy)
    away = sc.sand_radiance(ax, ay)
    near2 = min(((x - ax) ** 2 + (y - ay) ** 2) for (_t, x, y, _z) in lamps)
    frame = frame_luminances(sc, (ux, uy), (ax, ay), a.grid)
    binlog, curve = tone_curve_for(conf, frame)
    ev = a.ev if a.ev is not None else night_ev(ini, conf)
    exposure = 2.0 ** ev

    def shown(v):
        return display_luminance(binlog, curve, conf, v, exposure)

    res = {
        "nearest_lamp_to_away_units": math.sqrt(near2),
        "moon_term": sc.moon_term(),
        "sky_term_published": sc.sky_term(),
        "sky_term_if_conf_only": sc.sky_term(use_published=False),
        "fallback_term": sc.fallback_term(),
        "fallback_term_no_lights": sc.fallback_term(lights_present=False),
        "fallback_over_moon": (sc.fallback_term(lights_present=False) / sc.moon_term()
                               if sc.moon_term() else 0.0),
        "sky_over_moon": sc.sky_term() / sc.moon_term() if sc.moon_term() else 0.0,
        "away_linear": away,
        "under_linear": under,
        "linear_ratio": away / under if under else 0.0,
        "away_display": shown(away),
        "under_display": shown(under),
        "sky_display": shown(sc.sky_term()),
        "display_ratio": shown(away) / shown(under) if shown(under) else 0.0,
        "exposure_ev": ev,
        "tonemapping_mode": int(conf.get("rtx.tonemappingMode", 1)),
        "local_squeeze": local_tonemap_bounds(conf),
        "display_ratio_if_local": min(1.0, (away / under if under else 0.0) *
                                      local_tonemap_bounds(conf)),
    }
    if a.json:
        print(json.dumps(res, indent=1))
    elif not a.quiet:
        for k, v in res.items():
            print(f"{k:16s} {v:.6g}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
