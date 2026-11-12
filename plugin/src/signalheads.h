#ifndef RHY_SIGNALHEADS_H_
#define RHY_SIGNALHEADS_H_

#include <cstdint>

// ---------------------------------------------------------------------------
// The pure half of "light the signal heads where the game draws its coronas".
//
// Nothing in this header touches the game, Windows or the Remix API: it is the
// decision logic (which head, which colour, which hash, is it stale), and it is
// unit tested natively in tests/signals_check.cpp. The half that actually reads
// the game lives in signalhook.cpp and only fills RhySignalHead records.
// ---------------------------------------------------------------------------

// One signal-head corona the game registered this frame, copied out of
// CTrafficLights::DisplayActualLight's own RegisterCorona call (see the s01
// notes for the argument layout: position at argument 7, r/g/b at 3/4/5).
struct RhySignalHead {
    uint32_t entity;   // CEntity* as a 32-bit value (the plugin is 32-bit)
    uint32_t index;    // which 2DFX entry of the model this head is
    float pos[3];      // world position of the lens, game units (Z-up, ~metres)
    float normal[3];   // the entity matrix's "at" axis (matrix+0x20). NOT the lens
                       // normal - the 2DFX entry has no direction at all - it is
                       // recorded only so head_forward/head_normal can be A/B'd.
    uint8_t rgb[3];    // the colour the game chose for this head's corona
    uint8_t alpha;     // the game's corona alpha (255 for signal heads)
    float strength;    // the game's corona strength, kept for the log line
    uint32_t tick;     // GetTickCount() when it was recorded
};

// Fixed cap on what one frame can record. The game's own corona store is 64
// entries, and only the currently-lit head of each visible signal is registered,
// so a few dozen records a frame is the normal case; the cap exists so a hook
// that misfires cannot grow anything.
enum { kRhySignalHeadCap = 256 };

// A stable id for one head: the entity pointer plus the 2DFX index. Stable
// across frames (same head, same id, so the light is not recreated) and across
// a game reload only by luck - a freed entity pointer can be reused by another
// signal, which the stale-head expiry cleans up.
uint64_t rhyHeadId(uint32_t entity, uint32_t index);

// The game's traffic-light corona colours, indexed by the head's colour index
// (the exe reads them from byte tables at 0x8A6214 red, 0x8A6218 green and
// 0xA9AD94 blue): 0 = green (0,255,0), 1 = amber (255,128,0), 2 = red (255,0,0).
// Returned as 0/1/2 - the same numbering rhySignalLensRgb() uses.
// Returns -1 when the triple is not recognisable as any of the three.
int rhySignalPhaseFromGameRgb(int r, int g, int b);

// True when a head has not been registered for ttlMs. GetTickCount() wraps at
// ~49.7 days, so the subtraction is done in unsigned arithmetic, which is the
// wraparound-safe form.
int rhyHeadStale(uint32_t lastSeenTick, uint32_t nowTick, uint32_t ttlMs);

// Where to put the Remix light for this head.
//
// The corona position IS the lens position, so the default is "unchanged": the
// 2DFX entry carries only a position offset (0x59C890 rotates e+0x00/0x04/0x08
// through the entity matrix and adds the translation - there is no direction
// vector in the entry to derive a lens normal from), so there is nothing
// camera-independent to move the light along. Pass a normal (the entity matrix's
// "at" axis, which the hook also records) plus a non-zero forward distance to
// test the "just in front of the lens" variant in game.
void rhyHeadLightPos(const RhySignalHead* h, const float* normal,
                     float forward, float* out);

// Remix keys lights by hash, so a head's hash has to be stable per (head,
// phase) and different between them. Mixed rather than added: entity pointers
// are 16-byte aligned and clustered in a couple of heap blocks, so id+k would
// collide constantly.
uint64_t rhyHeadLightHash(uint64_t id, int phase, uint64_t base);

// One head chosen to have a light this pass.
struct RhyHeadWant {
    uint64_t id;
    int src;     // index into the recorded-heads array
    int phase;   // 0 green, 1 amber, 2 red, -1 = colour not recognised
    float d2;    // squared 2D distance from the player, for the log line
};

// Cheap sanity check for a pointer read out of game memory: not null, not a
// kernel address, no arithmetic wrap, and not an absurd length. The Windows side
// then asks the page table whether the range is really committed (see
// signalhook.cpp); this half is here so it can be tested without a process.
bool rhyRangePlausible(uint32_t addr, unsigned len);

// Nearest-first selection, same two-radius hysteresis as the lamp lists: a head
// that already has a light survives out to exitDist, a head that does not must
// be inside maxDist, and the active set may grow to cap+stickyExtra before the
// farthest ones are dropped. Without that, a head at the boundary is destroyed
// and recreated on alternate passes, which is a signal that blinks.
// Returns how many entries were written to out.
unsigned rhyChooseHeads(const RhySignalHead* heads, unsigned nHeads,
                        const float* playerPos, float maxDist, float exitDist,
                        unsigned cap, unsigned stickyExtra,
                        const uint64_t* active, unsigned nActive,
                        RhyHeadWant* out, unsigned outCap);

#endif  // RHY_SIGNALHEADS_H_
