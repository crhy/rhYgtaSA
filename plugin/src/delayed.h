#ifndef RHY_DELAYED_H_
#define RHY_DELAYED_H_

#include "remix.h"

// Remix lights are created and destroyed while the Remix server is still
// working through frames that referenced the old handle, so destroying a
// handle the moment we stop using it is a use-after-free risk on the other
// side of the bridge. Queue the handle here instead and let rhyFlushDelayed()
// destroy it destroy_delay_ms later.
//
// tag is the list index (or any id the module picks) the handle belongs to.
// It exists so a module can force the destroy of ONE index to land before it
// creates that index's replacement: a lamp light's LightInfo::hash is derived
// from its list index, so creating first and destroying second would let the
// destroy take the new light with it (that is a lamp that blinks out and stays
// out until the next re-selection).
void rhyDelayDestroy(remixapi_LightHandle handle, int tag);

// Called once per frame from hkPresent, before the light managers run.
//
// n08: `perFrame` bounds how many due handles ONE frame may destroy. There was
// no bound before, and the 2026-11-12 runs show what that costs: `pending_destroy`
// reached 320 in the 00:48 run and 205 in the 10:22 run, every one of them
// destroyed inside a single Present, while creates were bounded by
// [fade] creates_per_frame since s28. 0 = no bound (the pre-n08 behaviour).
void rhyFlushDelayed(unsigned perFrame = 0u);

// Destroy now the pending handles of one tag, before creating that index again.
void rhyFlushDelayedTag(int tag);

// Tag namespaces.  rhyFlushDelayedTag matches on the tag number alone, so two
// modules must never use the same number for different lights: flushing the
// sun's tag would otherwise destroy a street light that is still being drawn.
enum LightTagNamespace {
    kTagStreet = 0,        // + street light list index
    kTagTraffic = 100000,  // + traffic light list index
    kTagProbe = 200000,
    kTagSun = 300000,
    kTagMoon = 400000,
    // s01: one tag per signal HEAD (entity pointer + 2DFX index, folded into a
    // 32-bit tag). Two different heads can land on the same tag; the only
    // consequence is that a flush-before-create retires the other head's handle
    // a few frames early, and that handle was already off the draw list.
    kTagSignalHead = 500000,
    // n12: one per vehicle lamp, from the lamp's own hash. A headlamp is
    // re-created every time the car moves, so the destroy-before-create rule of
    // rhyFlushDelayedTag applies exactly as it does for a lamp light.
    kTagVehicle = 600000
};

// Number of handles still waiting to be destroyed (for the log line).
unsigned rhyDelayedPending();

// n08: how many of those are due RIGHT NOW and were refused by the per-frame
// bound on the last frame. This is the number that says whether the bound is
// actually biting (0 means the queue never had more than perFrame due handles).
unsigned rhyDelayedDue();
unsigned rhyDelayedDeferred();

#endif  // RHY_DELAYED_H_
