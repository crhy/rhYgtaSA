#!/usr/bin/env python3
"""Remove the GTASA-Remix asset pack's own street-lamp lights.

rhyremix.asi lights every street lamp itself (rhyremix_lights.txt: real fixture
sizes, sodium/mercury spectra, twilight switching). The pack's
SARemix/SARemix_StreetLights.usda also puts SphereLights into the same 12 lamp
models (Spot_Light_* with Light_Focus + Light_Normal, Sphere_Light_* with one),
so every lamp was lit twice. This drops every `def SphereLight` block from that
file and keeps everything else: the lamp meshes, the emissive glass materials
and the `env_light` visibility overrides.

Idempotent: a file with no SphereLights left is not touched. The first run keeps
the original next to it as <file>.orig-with-lights.

Usage: strip_pack_lamp_lights.py /path/to/GTASA-RTX [--restore]
"""
import re
import shutil
import sys
from pathlib import Path

REL = "rtx-remix/mods/gameReadyAssets/SARemix/SARemix_StreetLights.usda"
HEAD = re.compile(r'^\s*def SphereLight "[^"]*"')


def strip(text: str) -> tuple[str, int]:
    """Drop each `def SphereLight "..." (...) { ... }` block, braces balanced."""
    lines = text.splitlines(keepends=True)
    out, i, removed = [], 0, 0
    while i < len(lines):
        if not HEAD.match(lines[i]):
            out.append(lines[i])
            i += 1
            continue
        depth, opened = 0, False
        while i < len(lines):
            depth += lines[i].count("{") - lines[i].count("}")
            opened = opened or "{" in lines[i]
            i += 1
            if opened and depth <= 0:
                break
        removed += 1
    return "".join(out), removed


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    f = Path(sys.argv[1]) / REL
    orig = f.with_name(f.name + ".orig-with-lights")
    if not f.is_file():
        print(f"strip_pack_lamp_lights: no {f}", file=sys.stderr)
        return 1
    if "--restore" in sys.argv[2:]:
        if orig.is_file():
            shutil.copy2(orig, f)
            print(f"strip_pack_lamp_lights: restored {f.name} from {orig.name}")
        return 0
    text = f.read_text()
    new, n = strip(text)
    if n == 0:
        print("strip_pack_lamp_lights: pack lamp lights already removed")
        return 0
    if new.count("{") != new.count("}"):
        print("strip_pack_lamp_lights: unbalanced braces after stripping; file left alone", file=sys.stderr)
        return 1
    if not orig.exists():
        shutil.copy2(f, orig)
    f.write_text(new)
    print(f"strip_pack_lamp_lights: removed {n} pack SphereLights from {f.name} (rhyremix lights the lamps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
