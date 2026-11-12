#ifndef RHY_REMIX_H_
#define RHY_REMIX_H_

#include "remix_c.h"

// Called on the first hkPresent (render thread): connects to the Remix bridge
// through d3d9.dll's remixapi_InitializeLibrary.
void rhyRemixInitOnce();

// Non-null once the API is live; the interface to call CreateLight /
// DestroyLight / DrawLightInstance through.
const remixapi_Interface* rhyRemixApi();

// Wrapped light calls.  Every create / draw / destroy goes through these so the
// plugin can answer, in one number, how many lights the Remix runtime is holding
// on to right now.  That number is the whole "the log says 60 lights are active
// but nothing is visible" question: the bridge client hands back a handle and
// reports success even when the Remix server never took the light, and the only
// trace of that is the draw failing later.
//
// reason (optional out-param) says WHY a create returned null, because the two
// reasons need opposite reactions from a module:
//   0 = created
//   1 = our own max_total_lights cap refused it (nothing is wrong with the light;
//       disabling the module for the session is wrong, and so is logging it as a
//       bridge failure)
//   2 = the bridge refused it (this is the only case that should disable a module)
//   3 = this frame's CreateLight budget is spent (s28). Nothing is wrong with the
//       light: the module keeps its current handle and retries next frame. This
//       is what lets a 320-light fade be 320 creates spread over frames instead
//       of one burst that changes the whole world's lighting in one frame.
remixapi_LightHandle rhyCreateLight(const remixapi_LightInfo* info,
                                    int* reason = nullptr);
void rhyDestroyLight(remixapi_LightHandle handle);
bool rhyDrawLight(remixapi_LightHandle handle);

// s28: the per-frame CreateLight budget. Called from hkPresent before the light
// modules run. A fade is a sequence of creates, so spreading them is the only
// way to fade a whole layer without a burst; 0 = no per-frame limit.
void rhyBeginFrame(unsigned perFrameCreates);
unsigned rhyFrameCreates();   // creates actually made since the last rhyBeginFrame
unsigned rhyCreateDeferred(); // creates refused by the frame budget (reason 3)

// True when this frame still has CreateLight credit. Modules check this BEFORE
// moving a light's published gain, so a step that cannot be paid for this frame
// leaves the handle exactly as it was: the fade is late, never bigger.
bool rhyCreateBudgetLeft();

// Spend one unit of this frame's budget WITHOUT creating: destroying a handle is
// also a bridge call and also changes how lit the world looks. Without this, a
// fade that finishes retires every light in the same frame - tests/smooth_check.cpp
// measured a 42-of-204 aggregate drop in one frame that way.
void rhySpendCreateSlot();

// True when rhyCreateLight() would accept a create right now (frame budget and the
// global cap both have room). A module that is RE-CREATING a light has to destroy
// the old handle before the new one exists, because LightInfo::hash is fixed per
// light - so it checks this first. Destroying a working light and then failing to
// make its replacement is a light that goes out, which is the opposite of the point.
bool rhyCanCreateLight();

// Diagnostics, logged once a second from hkPresent.
unsigned rhyLiveLights();     // handles we created and have not destroyed
unsigned rhyCreateFailures(); // CreateLight calls the bridge rejected
unsigned rhyDrawFailures();   // DrawLightInstance calls the bridge rejected

// Cap on how many handles the plugin keeps alive at once, across every module
// (rhyremix.ini max_total_lights, 0 = no cap).  It exists as an experiment knob,
// NOT as a default: with a cap set, the modules starve each other in proportion
// to nothing but the order they run in, and a starved create used to read as a
// bridge failure, which switched the whole module off mid-session.  The bridge
// has never refused a light here (create_fail=0, draw_fail=0 in rhyremix.log and
// no 'Remix API call failed' in bridge64.log), so the per-module caps
// (max_lights, traffic_max_lights) are the real limits and the default is no cap.
void rhySetLightBudget(unsigned cap);
unsigned rhyBudgetRefusals();  // creates refused by the cap, not by the bridge

#endif  // RHY_REMIX_H_
