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
void rhyFlushDelayed();

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
    kTagMoon = 400000
};

// Number of handles still waiting to be destroyed (for the log line).
unsigned rhyDelayedPending();

#endif  // RHY_DELAYED_H_
