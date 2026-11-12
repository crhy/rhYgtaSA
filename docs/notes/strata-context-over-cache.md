---
name: strata-context-over-cache
description: rhY 2026-11-12 - maximum context beats expert-cache hit rate/speed for every model; never shrink context to free VRAM.
metadata:
  type: feedback
---
Keep every model at its highest context. Never propose lowering context (or trading it away) to raise Strata's expert-cache hit
rate or tok/s.

**Why:** rhY: "Higher context should remain the priority from now on for all models." (asked about the cache dropping to ~65-70%
on qwen-iq3_s at 262K; Claude suggested 128K and was refused.)
**How to apply:** speed fixes must keep context at max (e.g. stop other GPU users, KV placement only if context stays the same).
Related: [[strata-context-lesson]], [[watch-strata-speed]], [[strata-self-tuning-plan]].
