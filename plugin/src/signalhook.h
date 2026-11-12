#ifndef RHY_SIGNALHOOK_H_
#define RHY_SIGNALHOOK_H_

#include "signalheads.h"

// ---------------------------------------------------------------------------
// The game-facing half of s01: a 5-byte detour over the one instruction in
// CTrafficLights::DisplayActualLight that registers a signal head's corona.
//
// Everything here is record-only. No Remix call, no allocation, no logging and
// no game call happens inside the hook: it copies the arguments the game was
// about to hand CCoronas::RegisterCorona into a fixed array, and the main loop
// (rhyTrafficFrame) turns that array into lights. The hook and the main loop run
// on the same thread - the game's render thread is the thread that calls
// Present, and DisplayActualLight runs earlier in the same frame - so the array
// needs no lock.
// ---------------------------------------------------------------------------

// Why installing the hook failed, for the log line.
enum RhySignalHookReason {
    kSignalHookOk = 0,
    kSignalHookBytes = 1,    // the bytes at the call site are not what we expect
    kSignalHookProtect = 2,  // VirtualProtect on the code page failed
    kSignalHookRange = 3,    // the stub is too far away for a rel32 jmp
    kSignalHookCallee = 4,   // 0x6FC580 is not the corona thunk the stub assumes
    kSignalHookSource = 5,   // [trafficlights] source is not "game": no patch
    kSignalHookCleanup = 6,  // the caller's `add esp,0x54` is not where it must be
    kSignalHookUnreadable = 7  // the patch site itself could not be read
};

// Install the detour. Safe to call more than once (it is a no-op once it is in).
// The kill switch lives here, not in the caller: it refuses unless
// [trafficlights] source=game AND rhySignalPatchCheck() passes on the bytes it
// reads out of the exe, so no call path can install the hook without both.
// reason (optional) gets an RhySignalHookReason.
bool rhySignalHookInstall(int* reason);

// True once the detour is live. source=game only works when this is true; the
// traffic module falls back to source=list when it is not.
bool rhySignalHookInstalled();

// s30: resize the game's own signal-head corona (it registers one per lit head
// at radius 15.0 = a 30 m additive green flare). iniRadius <= 0 restores the
// game's 15.0. Byte-verified against 0x49DCD9 before anything is written,
// idempotent, and callable on every rhyremix.ini edit, so corona_radius is a
// live A/B knob. Only ever called with the hook's own kill switch satisfied.
bool rhySignalCoronaApply(float iniRadius);

// Human-readable form of an RhySignalHookReason.
const char* rhySignalHookReasonText(int reason);

// Copy the heads recorded since the last call into out (at most outCap of them)
// and reset the collection for the next frame. Returns how many were copied.
unsigned rhySignalHeadsTake(RhySignalHead* out, unsigned outCap);

// Counters for the log line. calls counts every time the patched instruction
// ran, which is what separates "the hook never fires" (calls=0: wrong build, or
// the game never draws a signal) from "it fires and we throw the record away"
// (calls>0, records=0). Then: records taken, records dropped because the array
// was full, records rejected because a pointer was not readable, and records
// refused because the recorder was already running (re-entrancy).
unsigned rhySignalHookCalls();
unsigned rhySignalHookRecords();
unsigned rhySignalHookDropped();
unsigned rhySignalHookBad();
unsigned rhySignalHookReentrant();

#endif  // RHY_SIGNALHOOK_H_
