#!/usr/bin/env python3
"""Syntax-check Lua files with LuaJIT (MoonLoader's Lua). usage: lua_check.py file.lua..."""
import sys, importlib
for m in ("lupa.luajit21", "lupa.luajit20", "lupa.lua51"):
    try:
        L = importlib.import_module(m).LuaRuntime(); break
    except Exception:
        continue
else:
    sys.exit("no LuaJIT/5.1 runtime in lupa")
bad = 0
for p in sys.argv[1:]:
    src = open(p, encoding="latin1").read()
    ok, err = L.eval("function(s, n) local f, e = loadstring(s, n); return f ~= nil, e end")(src, p)
    print(("ok   " if ok else "FAIL ") + p + ("" if ok else ": " + str(err)))
    bad += 0 if ok else 1
sys.exit(1 if bad else 0)
