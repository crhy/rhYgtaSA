#ifndef RHY_TRAFFICLIGHTS_H_
#define RHY_TRAFFICLIGHTS_H_

// Loads the traffic light list once (first hkPresent frame), and installs the
// signal-head corona hook when [trafficlights] source=game (the default).
void rhyTrafficInitOnce();

// Called on every hkPresent: keeps a Remix light alive for every traffic signal
// near the player, coloured by its current phase.
//
// Two sources, one ini key (see config.h):
//   source=game  one light per signal HEAD, at the position and in the colour the
//                game itself uses for that head's corona (signalhook.cpp).
//   source=list  one light per line of rhyremix_traffic.txt, at the pole origin
//                plus a headz guess, coloured by rhyTrafficPhase() below. Also
//                what runs when the hook cannot be installed.
void rhyTrafficFrame();

// Phase of light #index: 0 = green, 1 = amber, 2 = red. Pure function of
// (index, time, mode, period) so it can be unit tested and replayed.
int rhyTrafficPhase(int index, double nowSeconds, int mode, int periodS);

// Parses one "x y z headz" line. Returns false on malformed input.
bool rhyTrafficParseLine(const char* line, float* x, float* y, float* z,
                         float* headz);

#endif  // RHY_TRAFFICLIGHTS_H_
