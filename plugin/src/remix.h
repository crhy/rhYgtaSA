#ifndef RHY_REMIX_H_
#define RHY_REMIX_H_

// Called on the first hkPresent (render thread): connects to the Remix bridge.
void rhyRemixInitOnce();

// Called on every hkPresent: draws the test light instance.
void rhyRemixFrame();

#endif  // RHY_REMIX_H_
