#ifndef RHY_STREETLIGHTS_H_
#define RHY_STREETLIGHTS_H_

// Loads the lamp list once (first hkPresent frame) and reads rhyremix.ini.
void rhyStreetlightsInitOnce();

// Called on every hkPresent: re-selects the visible set every 30 frames and
// draws a Remix light instance for every active handle.
void rhyStreetlightsFrame();

#endif  // RHY_STREETLIGHTS_H_
