#!/usr/bin/env python3
"""What an rtx.conf fog-remap block actually does to the air in GTA San Andreas.

    usage: tools/fog-budget.py PROFILE [PROFILE ...]
           tools/fog-budget.py --fog-end 400 800 4000 [--distances 50 100 500 1000 2000]
                               [--color 0.999,0.999,0.999] [--bounds MIN MAX TMIN TMAX]

n17 (docs/TARGET.md §3 T3, "distance must fade") ships six numbers for the atmosphere. They are
not eyeballed: they are a density, and a density has consequences that arithmetic settles before
any frame is rendered. This tool is that arithmetic, so the numbers in docs/FINDINGS.md and the
comment in config/rtx.conf can be re-derived instead of trusted.

The model, all of it read out of the 1.6.1 runtime (build/tmp/n16-dxvk/dxvk-remix-remix-1.6.1,
upstream NVIDIAGameWorks/dxvk-remix, MIT):

  * the game's D3D9 fog is captured per draw call: fogState.end = D3DRS_FOGEND
    (src/d3d9/d3d9_rtx_utils.cpp:227 setFogState);
  * with rtx.volumetrics.enableFogRemap + enableFogMaxDistanceRemap and LINEAR fog, the runtime
    maps that end distance into the volumetric medium's "transmittance measurement distance"
    (src/dxvk/rtx_render/rtx_global_volumetrics.cpp:495-520):

        normalized = (fogEnd - fogRemapMaxDistanceMinMeters) / (Max - Min)     # NOT clamped
        d_meas     = normalized * (TransMax - TransMin) + TransMin

    both bounds in metres, scaled by getMeterToWorldUnitScale() = 100 * rtx.sceneScale
    (rtx_options.h:1473); with our rtx.sceneScale = 0.01 that scale is 1.0, i.e. SA metres.
  * the medium is Beer-Lambert (rtx_global_volumetrics.cpp:542-546 and
    shaders/rtx/utility/brdf.slangh:484):

        sigma = -ln(linear(transmittanceColor)) / d_meas        [1/m]
        T(x)  = exp(-sigma * x)

  * Koschmieder's relation turns sigma into the number a weather report would use:
    visibility V = 3.912 / sigma (the contrast-threshold distance). That is the unit this file
    reports, because "d_meas = 6 m" means nothing and "10 km of air" means something.

Two things this makes visible and that the config comments lean on:

  1. the runtime default (transmittanceColor 0.999 measured at 200 m) is sigma = 1.1e-5 /m =
     344 km of air: no aerial perspective at all, which is what n15 measured as far/near local
     contrast 1.22-3.67;
  2. rtx.volumetrics.enableFogColorRemap replaces the transmittance colour with the game's fog
     colour, and sigma is -ln(colour)/d_meas: a mid-grey fog colour multiplies sigma by ~500 and
     a dark one by ~2000, i.e. the world within 50 m disappears. Never ship it without re-deriving
     d_meas with this tool.
"""
import argparse
import math
import os
import re
import sys

KEYS = {
    "rtx.volumetrics.enableFogRemap": ("False", bool),
    "rtx.volumetrics.enableFogMaxDistanceRemap": ("True", bool),
    "rtx.volumetrics.enableFogColorRemap": ("False", bool),
    "rtx.volumetrics.fogRemapMaxDistanceMinMeters": ("1", float),
    "rtx.volumetrics.fogRemapMaxDistanceMaxMeters": ("40", float),
    "rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMinMeters": ("20", float),
    "rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMaxMeters": ("100", float),
    "rtx.volumetrics.fogRemapColorMultiscatteringScale": ("0.1", float),
    "rtx.volumetrics.transmittanceColor": ("0.999, 0.999, 0.999", str),
    "rtx.volumetrics.transmittanceMeasurementDistanceMeters": ("200", float),
    "rtx.sceneScale": ("1", float),
}
KEYRE = re.compile(r"^\s*(rtx\.[A-Za-z0-9_.]+)\s*=\s*(.*?)\s*(?:#.*)?$")
DEFAULT_FOG_END = (400.0, 800.0, 1500.0, 4000.0)
DEFAULT_DISTANCES = (50.0, 100.0, 300.0, 500.0, 1000.0, 2000.0, 4000.0)


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def parse_profile(path):
    """The keys this profile writes, with the 1.6.1 defaults filled in for the rest."""
    got = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = KEYRE.match(line)
            if m:
                got[m.group(1)] = m.group(2)
    out = {}
    for key, (default, typ) in KEYS.items():
        raw = got.get(key, default)
        if typ is bool:
            out[key] = raw.strip().lower() in ("true", "1", "yes")
        elif typ is float:
            out[key] = float(raw.split(",")[0])
        else:
            out[key] = tuple(float(x) for x in raw.split(","))
    return out


def sigma_per_m(color, d_meas):
    """Beer-Lambert extinction coefficient, 1/m. d_meas is the distance the colour was measured at."""
    lin = [srgb_to_linear(c) for c in color]
    return [-math.log(max(v, 1e-9)) / d_meas for v in lin]


def visibility_km(sig):
    """Koschmieder: the distance at which a black object's contrast falls to 2%."""
    return 3.912 / sig / 1000.0


def remap_d_meas(fog_end, mn, mx, tmin, tmax):
    """rtx_global_volumetrics.cpp:508-520, including the missing clamp (normalized is NOT clamped)."""
    if mx == mn:
        return 0.0, tmin
    n = (fog_end - mn) / (mx - mn)
    return n, n * (tmax - tmin) + tmin


def report(path, fog_ends, distances):
    p = parse_profile(path)
    scale = 100.0 * p["rtx.sceneScale"]
    color = p["rtx.volumetrics.transmittanceColor"]
    mn = p["rtx.volumetrics.fogRemapMaxDistanceMinMeters"] * scale
    mx = p["rtx.volumetrics.fogRemapMaxDistanceMaxMeters"] * scale
    tmin = p["rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMinMeters"] * scale
    tmax = p["rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMaxMeters"] * scale
    print("== %s" % path)
    print("   enableFogRemap=%s  maxDistanceRemap=%s  colorRemap=%s  multiscatter=%.3f"
          % (p["rtx.volumetrics.enableFogRemap"],
             p["rtx.volumetrics.enableFogMaxDistanceRemap"],
             p["rtx.volumetrics.enableFogColorRemap"],
             p["rtx.volumetrics.fogRemapColorMultiscatteringScale"]))
    print("   transmittanceColor=%s  input bounds=[%g, %g] m  output bounds=[%g, %g] m"
          % (",".join("%g" % c for c in color), mn, mx, tmin, tmax))
    if not p["rtx.volumetrics.enableFogRemap"] or not p["rtx.volumetrics.enableFogMaxDistanceRemap"]:
        d = p["rtx.volumetrics.transmittanceMeasurementDistanceMeters"] * scale
        s = sigma_per_m(color, d)[0]
        print("   remap OFF: sigma = %.3e /m, visibility %.1f km (the medium is whatever the two"
              " transmittance keys say, the game's fog is ignored)" % (s, visibility_km(s)))
        return
    print("   game fog end (m)   normalized   d_meas (m)   sigma (1/m)   visibility   "
          + "  ".join("T@%gm" % d for d in distances))
    for fe in fog_ends:
        n, d = remap_d_meas(fe, mn, mx, tmin, tmax)
        if d <= 0:
            print("   %12g   %+9.3f   %10.2f  NEGATIVE d_meas: the medium is undefined" % (fe, n, d))
            continue
        s = sigma_per_m(color, d)[0]
        ts = "  ".join("%9.3f" % math.exp(-s * x) for x in distances)
        flag = "  <- fog end outside the input bounds: the runtime does not clamp" \
            if not (mn <= fe <= mx) else ""
        print("   %12g   %+9.3f   %10.2f   %10.3e   %8.1f km %s%s"
              % (fe, n, d, s, visibility_km(s), ts, flag))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("profiles", nargs="*", help="rtx.conf-style files to read the block from")
    ap.add_argument("--fog-end", nargs="+", type=float, default=list(DEFAULT_FOG_END),
                    help="the game's D3DRS_FOGEND values to test (metres)")
    ap.add_argument("--distances", nargs="+", type=float, default=list(DEFAULT_DISTANCES))
    ap.add_argument("--color", default=None, help="override transmittanceColor, e.g. 0.6,0.6,0.6")
    ap.add_argument("--bounds", nargs=4, type=float, default=None,
                    metavar=("MIN", "MAX", "TMIN", "TMAX"), help="override the four remap bounds")
    a = ap.parse_args(argv)
    if not a.profiles and not (a.color and a.bounds):
        a.profiles = ["config/rtx.conf", "config/rtx.conf.perf", "config/rtx.conf.nopops"]
    for path in a.profiles:
        if a.color or a.bounds:
            tmp = "build/tmp/fog-budget-synthetic.conf"
            os.makedirs("build/tmp", exist_ok=True)
            mn, mx, tmin, tmax = a.bounds or (1, 40, 20, 100)
            with open(tmp, "w", encoding="utf-8") as f:
                f.write("rtx.volumetrics.enableFogRemap = True\n")
                f.write("rtx.volumetrics.enableFogMaxDistanceRemap = True\n")
                f.write("rtx.volumetrics.fogRemapMaxDistanceMinMeters = %g\n" % mn)
                f.write("rtx.volumetrics.fogRemapMaxDistanceMaxMeters = %g\n" % mx)
                f.write("rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMinMeters = %g\n" % tmin)
                f.write("rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMaxMeters = %g\n" % tmax)
                f.write("rtx.sceneScale = 0.01\n")
                if a.color:
                    f.write("rtx.volumetrics.transmittanceColor = %s\n" % a.color)
            path = tmp
        report(path, a.fog_end, a.distances)
    return 0


if __name__ == "__main__":
    sys.exit(main())
