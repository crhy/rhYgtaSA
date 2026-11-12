#!/usr/bin/env python3
"""Validate an RTX Remix water material variant (text USD) against the pack's original.

Checks, in order:
  1. the file is text USD: first line is exactly '#usda 1.0';
  2. braces/brackets/parens are balanced (comments, "strings" and @asset@ paths skipped);
  3. the prim paths are exactly the prim paths of the reference file (so the override still
     targets RootNode/Looks/mat_.../Shader and Remix cannot silently drop it);
  4. every `inputs:<name>` written by the variant is an input that the
     AperturePBR_Translucent.mdl actually declares (this is what stops us inventing
     inputs:normal_strength / inputs:roughness, which do not exist in that mdl).

usage: tools/water_check.py CANDIDATE [REFERENCE]
exits 0 and prints "OK ..." on success, 1 with the reasons otherwise.
"""
import re
import sys

# parameters of `export material AperturePBR_Translucent(...)` in
# <game>/rtx-remix/mods/gameReadyAssets/materials/AperturePBR_Translucent.mdl (Remix 1.6.1)
MDL_INPUTS = {
    "ior_constant", "thin_walled", "thin_wall_thickness",
    "use_diffuse_layer", "transmittance_texture", "transmittance_color",
    "transmittance_measurement_distance",
    "enable_emission", "emissive_color", "emissive_mask_texture", "emissive_intensity",
    "sprite_sheet_fps", "sprite_sheet_cols", "sprite_sheet_rows",
    "preload_textures", "ignore_material",
    "filter_mode", "wrap_mode_u", "wrap_mode_v",
    "normalmap_texture", "encoding",
}

PRIM_RE = re.compile(r'^(\s*)(?:over|def|class)\s+"([^"]+)"')
INPUT_RE = re.compile(r'^\s*[A-Za-z0-9_]+\s+inputs:([A-Za-z0-9_]+)\s*=\s*(.*)$')


def bracket_scan(text):
    """Return the bare bracket stream of `text`, skipping comments/strings/asset paths."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "#":
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith('"""', i):
            i += 3
            while i < n and not text.startswith('"""', i):
                i += 1
            i += 3
        elif c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
        elif c == "@":
            j = text.find("@", i + 1)
            if j < 0:
                raise ValueError("unterminated @asset path@")
            i = j + 1
        else:
            if c in "{}()[]":
                out.append(c)
            i += 1
    return "".join(out)


def balance_errors(stream):
    pairs = {")": "(", "]": "[", "}": "{"}
    stack = []
    for k, c in enumerate(stream):
        if c in "([{":
            stack.append(c)
        elif stack and c in pairs and stack[-1] == pairs[c]:
            stack.pop()
        else:
            return f"unbalanced {c!r} at bracket #{k}"
    return None if not stack else f"unclosed {stack[-1]!r} ({len(stack)} open at EOF)"


def prim_paths(text):
    """Prim paths, tracked by indentation (property blocks close with ')', deeper braces ignored)."""
    stack, paths = [], []
    for ln in text.split("\n"):
        s = ln.strip()
        if not s or s.startswith("#"):
            continue
        m = PRIM_RE.match(ln)
        if m:
            stack.append((len(ln) - len(ln.lstrip()), m.group(2)))
            paths.append("/".join(n for _, n in stack))
        elif s == "}":
            ind = len(ln) - len(ln.lstrip())
            if stack and ind == stack[-1][0]:
                stack.pop()
    return paths, stack


def input_values(text):
    vals = {}
    for ln in text.split("\n"):
        m = INPUT_RE.match(ln)
        if m:
            vals.setdefault(m.group(1), []).append(m.group(2).split(" (")[0].strip())
    return vals


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cand_path = sys.argv[1]
    ref_path = sys.argv[2] if len(sys.argv) > 2 else None
    cand = open(cand_path, encoding="utf-8").read()
    errs = []

    first = cand.split("\n", 1)[0].strip()
    if first != "#usda 1.0":
        errs.append(f"first line is {first!r}, expected '#usda 1.0'")

    try:
        stream = bracket_scan(cand)
    except ValueError as e:
        errs.append(str(e))
        stream = ""
    if stream:
        err = balance_errors(stream)
        if err:
            errs.append(err)

    paths, unclosed = prim_paths(cand)
    if unclosed:
        errs.append(f"{len(unclosed)} prim block(s) never closed: {[p[1] for p in unclosed]}")

    if ref_path:
        ref = open(ref_path, encoding="utf-8").read()
        ref_paths, _ = prim_paths(ref)
        if paths != ref_paths:
            missing = [p for p in ref_paths if p not in paths]
            added = [p for p in paths if p not in ref_paths]
            errs.append(f"prim paths differ from reference; missing={missing} added={added}")

    bad = sorted(set(input_values(cand)) - MDL_INPUTS)
    if bad:
        errs.append(f"inputs not declared by AperturePBR_Translucent.mdl: {bad}")

    if errs:
        print(f"FAIL {cand_path}")
        for e in errs:
            print("  - " + e)
        return 1

    print(f"OK {cand_path}: text USD, braces balanced, {len(paths)} prim paths match reference, "
          f"{len(set(input_values(cand)))} distinct inputs all declared by the mdl")
    if ref_path:
        a, b = input_values(ref), input_values(cand)
        for name in sorted(set(a) | set(b)):
            va, vb = a.get(name, ["<absent>"]), b.get(name, ["<absent>"])
            if va != vb:
                print(f"    {name}: {' / '.join(va)}  ->  {' / '.join(vb)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
