#ifndef RHY_SUN_H_
#define RHY_SUN_H_

#include "remix.h"

// The sun, replacing SARemix_Real_Sun.lua.
//
// The Lua mod fakes the sun by spawning game objects (model ids 15889 / 15890 /
// 15891) at the player and rotating them with the game clock, then lets Remix
// turn those objects' USD light prims into the sun.  The Remix C API has a
// first-class sun: a "distant" light (direction + angular diameter), which
// needs no game object, no model id and no USD asset.
//
// Called every frame from hkPresent.
void rhySunFrame();

#endif  // RHY_SUN_H_
