#!/usr/bin/env python3
"""Give UsdLux shaping attributes that are declared without a value an explicit default.
Remix 1.6 may read unset inputs:shaping:focus / focusTint as garbage (NaN) -> GPU device lost.
usage: usd_light_defaults.py <mods_dir> <backup_dir>"""
import os, re, shutil, sys
root, backup = sys.argv[1], sys.argv[2]
rules = [
    (re.compile(r'^(\s*)float inputs:shaping:focus(\s*)$', re.M), r'\1float inputs:shaping:focus = 0\2'),
    (re.compile(r'^(\s*)color3f inputs:shaping:focusTint(\s*)$', re.M), r'\1color3f inputs:shaping:focusTint = (1, 1, 1)\2'),
    (re.compile(r'^\s*asset inputs:shaping:ies:file\s*\r?\n', re.M), ''),
]
total = 0
for dp, _, files in os.walk(root):
    for fn in files:
        if not fn.endswith('.usda'):
            continue
        p = os.path.join(dp, fn)
        with open(p, newline='') as f:
            s = f.read()
        n_all, out = 0, s
        for rx, rep in rules:
            out, n = rx.subn(rep, out)
            n_all += n
        if n_all:
            b = os.path.join(backup, os.path.relpath(p, root))
            os.makedirs(os.path.dirname(b), exist_ok=True)
            if not os.path.exists(b):
                shutil.copy2(p, b)
            with open(p, 'w', newline='') as f:
                f.write(out)
            total += n_all
            print(f'{n_all:5d}  {os.path.relpath(p, root)}')
print('total edits:', total)
