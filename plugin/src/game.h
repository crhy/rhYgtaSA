#ifndef RHY_GAME_H_
#define RHY_GAME_H_

// Player position in GTA SA game units.  CPed* at 0xB7CD98; CMatrix* at
// ped+0x14 with the position at matrix+0x30, otherwise ped+0x4.  Returns false
// before the player object exists (loading screen).
bool rhyPlayerPos(float* out);

// s28: the same position, but without the cliff.
//
// The player ped pointer is 0 in a dozen perfectly ordinary game states - while
// the ped is being deleted and re-created (wasted/busted, an interior or a cut
// scene loading, a script warp, and per the 2026-11-12 test report, while CJ is
// riding on top of a moving vehicle). Every lamp in the world used to be
// destroyed in the frame the pointer went to 0 and rebuilt when it came back:
// that is finding 3 ("the whole world went dark on a cab roof, then came back")
// and one of the biggest contributors to finding 1 (measured: live handles
// 381 -> 1 at 00:51:26 and a 13 s stretch at live=1 from 01:09:20).
//
// So: report where the player is, and when the pointer is transiently 0 report
// the last position we had, for graceMs. The caller uses the state to decide
// when to start fading the lamp layers out (kRhyPlayerGone) - fading, never an
// instant destroy.
//
// nowMs is a GetTickCount() value from the caller (game.cpp stays free of
// windows.h); graceMs is the grace window in milliseconds.
enum RhyPlayerState {
    kRhyPlayerOk = 0,     // the ped is there; out is its position
    kRhyPlayerGrace = 1,  // ped pointer is 0, out is the last known position
    kRhyPlayerGone = 2    // ped pointer has been 0 longer than the grace window
};

RhyPlayerState rhyPlayerPosGrace(float* out, unsigned graceMs, unsigned nowMs);

// The grace-window bookkeeping on its own, with the game-memory read pulled out as
// an argument, so tests/smooth_check.cpp can drive it without a game process. The
// rules that matter and are asserted there: a position is held for the whole grace
// window, the window is measured with wraparound-safe tick arithmetic (GetTickCount
// rolls over at ~49.7 days), a disappearance is counted once and not per frame, and
// a player that was never seen has nothing to hold on to.
struct RhyPlayerTrack {
    float last[3];
    bool haveLast;
    bool lost;            // currently inside a disappearance
    unsigned lastGoodTick;
    unsigned lostEvents;
};

void rhyPlayerTrackInit(RhyPlayerTrack& t);

// `read` is whether the ped pointer gave a position; `pos` is that position when it
// did. `held` receives the position the caller should use (the fresh one, or the
// last known one during the grace window).
RhyPlayerState rhyPlayerTrackUpdate(RhyPlayerTrack& t, bool read, const float* pos,
                                    unsigned graceMs, unsigned nowMs, float* held);

// Diagnostics for the once-a-second log line: how many times the ped pointer
// went to 0 this session, and how long it has been 0 right now (ms).
unsigned rhyPlayerLostEvents();
unsigned rhyPlayerLostMs(unsigned nowMs);

// Game clock hour, 0..23 (0xB70153).  The same source the lamp list was built
// from, so "which lamps are on" and "is it night" cannot disagree.
int rhyGameHour();

#endif  // RHY_GAME_H_
