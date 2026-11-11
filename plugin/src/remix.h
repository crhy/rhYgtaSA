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
remixapi_LightHandle rhyCreateLight(const remixapi_LightInfo* info,
                                    int* reason = nullptr);
void rhyDestroyLight(remixapi_LightHandle handle);
bool rhyDrawLight(remixapi_LightHandle handle);

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
