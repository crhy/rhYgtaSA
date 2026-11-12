#ifndef RHY_SHUTDOWN_H_
#define RHY_SHUTDOWN_H_

// n09 (2026-11-12 exit hang): when is the Remix bridge gone, and what does "gone" mean?
//
// The run: rhY quit from the menu, NvRemixBridge.exe exited, the window disappeared, and
// gta-sa.exe then sat there for two minutes with its main thread in state R at 66 % CPU while
// rhyremix.log kept printing "remix: live=383 …" until 10:39:14.652 and then printed nothing at
// all - no shutdown line, because the plugin had no DLL_PROCESS_DETACH, no atexit and no idea that
// the process was ending. 302 street lights and 4 signal lights were still being DRAWN, per frame,
// into a bridge client whose server had already exited, and the 0x49DD4C detour was still running
// while the game tore its models down. Full timeline in .strata/notes-n09-exit-hang.md.
//
// So this file answers one question - is the bridge still worth calling? - and it does it in plain
// C++ with the game reads pulled out as arguments, so tests/shutdown_check.cpp can run every path
// (a one-frame Present hiccup, an alt-tab, a Quit, a bridge that dies first, a GetTickCount wrap)
// without a game process.
//
// The rules, and why each one is there:
//
//   * A signal must be on CONTINUOUSLY for its own window. A single failed Present, or a single
//     TestCooperativeLevel() that says D3DERR_DEVICELOST, is what alt-tab and a resolution change
//     look like; turning the world's lights off for the session over one of those is a worse bug
//     than the one being fixed. A gap in a signal resets that signal's own timer.
//   * Our own failing CreateLight/DrawLightInstance counters are NEVER enough on their own. One
//     light the Remix server refuses makes those counters climb for a whole session (that is what
//     g_apiBroken in streetlights.cpp already handles); they only declare the bridge gone when they
//     keep failing while some other signal is on.
//   * Death LATCHES. remix.cpp's rhyRemixInitOnce() already refuses to retry a failed init, the
//     bridge client cannot be re-connected from here, and re-creating 300 lights on a half-dead
//     bridge is worse than dark. Nothing un-declares it except a new process.
//   * DLL_PROCESS_DETACH is instant: the process is ending, no window applies.
//   * A window of 0 means "one frame is enough" (the ini knob's escape hatch, and what the tests
//     use to check the ordering of the rules).

// The signals. A bitmask: several can be on at once, and the log line prints all of them.
enum RhyShutdownSignalBit {
    kRhyShutPresent = 1u << 0,  // the original Present we forward to keeps failing
    kRhyShutDevice  = 1u << 1,  // TestCooperativeLevel keeps saying the device is gone
    kRhyShutWindow  = 1u << 2,  // the window the device was created with is no longer a window
    kRhyShutCalls   = 1u << 3,  // our own Create/Draw calls keep failing (needs company)
    kRhyShutDetach  = 1u << 4   // DLL_PROCESS_DETACH: the process is ending
};
const unsigned kRhyShutCount = 5;  // number of bits above

// How long each signal has to hold before it ends the session, in ms. 0 = one frame.
struct RhyShutdownLimits {
    unsigned presentMs;
    unsigned deviceMs;
    unsigned windowMs;
    unsigned callsMs;
};

// What one frame observed. main.cpp fills this from the device and from our own counters; the
// numbers themselves never reach this file, so the rules can be tested without a game.
struct RhyShutdownSignals {
    bool presentFailed;
    bool deviceLost;
    bool windowGone;
    bool callsFailing;
};

struct RhyShutdownTrack {
    unsigned started;       // bit set once firstSeen[i] holds a tick
    unsigned firstSeen[kRhyShutCount];  // tick the signal started holding (wrap-safe)
    unsigned on;            // signals on right now
    unsigned seen;          // every signal seen at any point (for the post-mortem line)
    unsigned deadTick;      // tick the bridge was declared gone; 0 = still live
    unsigned deadReasons;   // the bits that ended it
    unsigned frames;            // updates seen, before and after
    unsigned suspectFrames;     // updates with at least one signal on, before death
    unsigned framesAfterDead;   // frames the game presented after we stopped calling the bridge
};

void rhyShutdownTrackInit(RhyShutdownTrack& t);

// Feed one frame. Returns true exactly once - the update that declared the bridge gone - which is
// the moment main.cpp latches remix.cpp, unpatches the signal-head detour and logs.
bool rhyShutdownUpdate(RhyShutdownTrack& t, const RhyShutdownSignals& s,
                       const RhyShutdownLimits& lim, unsigned nowMs);

// DLL_PROCESS_DETACH. Returns true when this call is the one that ended the session.
bool rhyShutdownDetach(RhyShutdownTrack& t, unsigned nowMs);

bool rhyShutdownDead(const RhyShutdownTrack& t);

// True while at least one signal is on and the session has not been declared over: the point where
// the log gets its first "shutdown: suspect" line.
bool rhyShutdownSuspect(const RhyShutdownTrack& t);

// Wraparound-safe ms between two GetTickCount() values; 0 when `thenMs` is 0 (never happened) or
// the clock went backwards past the wrap.
unsigned rhyShutdownElapsed(unsigned nowMs, unsigned thenMs);

// The earliest tick at which any signal started holding (0 = nothing has ever been seen). The log
// line uses it to say when the exit actually began, which is the number the 2026-11-12 post-mortem
// had to reconstruct by hand from three logs.
unsigned rhyShutdownFirstSignal(const RhyShutdownTrack& t);

// "window+present" into buf, for the log lines. Returns buf.
const char* rhyShutdownReasonText(unsigned mask, char* buf, unsigned cap);

#endif  // RHY_SHUTDOWN_H_
