#ifndef RHY_SUN_H_
#define RHY_SUN_H_

#include "remix.h"

// The sun, the moon and the sky layer, replacing SARemix_Real_Sun.lua.
//
// The Lua mod fakes the sun by spawning game objects (model ids 15889 / 15890 /
// 15891) at the player and rotating them with the game clock, then lets Remix
// turn those objects' USD light prims into the sun.  The Remix C API has a
// first-class sun: a "distant" light (direction + angular diameter), which
// needs no game object, no model id and no USD asset.
//
// This module does three things, not one:
//   1. geometry - where the sun and moon actually are, from the game clock and
//      the observer's latitude (solar.cpp, validated in tests/solar_check.cpp)
//   2. radiance - how bright and what colour, from Beer-Lambert extinction, so
//      a low sun is dim AND red without anyone tuning a colour by hand
//   3. the renderer's own ambient - rtx.skyBrightness / emissiveIntensity, which
//      have to follow the sun or night is lit like noon
//
// Called every frame from hkPresent; the work inside runs about once a second.
void rhySunFrame();

// Sky state, for the lamp modules. They must not decide "is it night?" from a
// different clock than the one that moved the sun, or street lamps come on at a
// different moment than the shadows change.
float rhySunElevationDeg();   // degrees, negative = below the horizon
float rhyDaylightFactor();    // 0 = night, 1 = day, smooth through twilight
bool rhyLampsOn();            // the twilight answer for street/traffic lamps

#endif  // RHY_SUN_H_
