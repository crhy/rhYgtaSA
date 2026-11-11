#ifndef RHY_TRAFFICLIGHTS_H_
#define RHY_TRAFFICLIGHTS_H_

// Loads the traffic light list once (first hkPresent frame).
void rhyTrafficInitOnce();

// Called on every hkPresent: keeps a Remix light alive for every traffic light
// near the player, coloured by its current phase.
void rhyTrafficFrame();

// Phase of light #index: 0 = green, 1 = amber, 2 = red. Pure function of
// (index, time, mode, period) so it can be unit tested and replayed.
int rhyTrafficPhase(int index, double nowSeconds, int mode, int periodS);

// Parses one "x y z headz" line. Returns false on malformed input.
bool rhyTrafficParseLine(const char* line, float* x, float* y, float* z,
                         float* headz);

#endif  // RHY_TRAFFICLIGHTS_H_
