#ifndef RHY_RTXCONF_H_
#define RHY_RTXCONF_H_

// Runtime control of the RTX Remix renderer's own settings.
//
// The Remix C API carries SetConfigVariable in remixapi_Interface (remix_c.h
// line ~710): "Writes an RtxOption value to the user layer. Keys in a registered
// dynamic option namespace are validated immediately and applied at the next
// frame's option update. Returns INVALID_ARGUMENTS for an invalid key or value,
// GENERAL_FAILURE for an unknown key."
//
// That matters more than it looks. rtx.conf is read once at startup, so before
// this existed every brightness decision had to be a constant: the sky ambient,
// the emissive gain and the exposure could not follow the sun, and a night scene
// was lit exactly like noon. Now they can. It is also a key validator - a wrong
// key returns GENERAL_FAILURE, so rtx.conf settings can be tested in one run
// instead of guessed at.
//
// Two consequences to keep in mind:
//   * Values land in the USER layer, which outranks rtx.conf and persists
//     between runs. Everything we send must therefore be idempotent, and a key
//     we stop sending keeps the last value we sent: `tools/rtxprofile.sh
//     restore` puts rtx.conf back, NOT this layer. (n22 found this comment
//     promising a `rhyremix --reset-rtx` command that does not exist anywhere in
//     the tree - do not write it into a doc as an undo. The layer is the
//     runtime's own file; the Remix UI's controls write the same layer, and no
//     user.conf exists in the game tree as of 2026-11-13, so where it lands is a
//     question for a live run.)
//   * Writing the same value every frame is wasted work (and may rewrite the
//     user settings file), so values are cached and only re-sent when they move
//     by more than a threshold.

// Push one key. Returns true when the bridge accepted it.
bool rhyRtxSet(const char* key, const char* value);

// Same, for a float formatted with %g. `minDelta` is the smallest change worth
// re-sending; pass 0 to always send.
bool rhyRtxSetFloat(const char* key, float value, float minDelta);

// Push every "key = value" line from a file. '#' starts a comment. Returns the
// number accepted; unknown or malformed keys are logged, not fatal.
int rhyRtxApplyFile(const char* path);

// Send each key once and report which the runtime knows. Run with
// rhyremix.ini [rtx] probe=1 to get ground truth about which rtx.* names exist in
// this Remix build, instead of trusting a list copied from the docs.
void rhyRtxProbeOnce();

// Counters for the once-a-second log line.
unsigned rhyRtxSets();     // successful SetConfigVariable calls
unsigned rhyRtxUnknown();  // calls the bridge rejected as an unknown key
unsigned rhyRtxBadValue(); // calls the bridge rejected as a bad value

#endif  // RHY_RTXCONF_H_
