#!/usr/bin/env python3
"""Print the vtable index of IDirect3DDevice9 / IDirect3DDevice9Ex methods.

Usage: python3 tools/vtidx.py [pattern]
Index 0 is QueryInterface, so the slot we patch with raw pointer writes
matches this numbering exactly.  (MinGW's d3d9.h declares the interfaces
with DECLARE_INTERFACE_IID_, in strict vtable order.)
"""
import re
import sys

HDRS = [
    "/usr/share/mingw-w64/include/d3d9.h",
    "/usr/i686-w64-mingw32/include/d3d9.h",
    "/usr/x86_64-w64-mingw32/include/d3d9.h",
]

pat = sys.argv[1] if len(sys.argv) > 1 else ""
src = None
for h in HDRS:
    try:
        src = open(h, encoding="utf-8", errors="ignore").read()
        break
    except OSError:
        continue
if src is None:
    sys.exit("d3d9.h not found")

RX = re.compile(r"STDMETHOD\((\w+)\)|STDMETHOD_\s*\([^,]+,\s*(\w+)\)")

def iface(name):
    start = src.index("#define INTERFACE " + name + "\n")
    end = src.index("#undef INTERFACE", start)
    return [m.group(1) or m.group(2)
            for m in RX.finditer(src[start:end])]

for name in ("IDirect3DDevice9", "IDirect3DDevice9Ex"):
    names = iface(name)
    print(f"== {name}: {len(names)} entries ==")
    for i, n in enumerate(names):
        if pat and pat.lower() not in n.lower():
            continue
        print(f"{i:3d}  {n}")
