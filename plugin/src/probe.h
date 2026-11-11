#ifndef RHY_PROBE_H_
#define RHY_PROBE_H_

#include "remix.h"

// One extra light at a fixed position, used to calibrate radius / radiance:
// it is the only light that ignores night hours, distance and the lamp lists.
// Called every frame from hkPresent.
void rhyProbeFrame();

#endif  // RHY_PROBE_H_
