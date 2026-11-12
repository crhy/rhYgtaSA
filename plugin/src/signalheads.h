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

// ---------------------------------------------------------------------------
// s19: the hook's decisions, kept out of the machine code.
//
// signalhook.cpp is a hand-written i386 detour and cannot be unit tested by
// calling it. Everything that is a *decision* - may we patch at all, is a
// pointer safe to follow, does this record get stored or dropped - therefore
// lives here, where tests/signals_check.cpp can run it against fake memory.
// The disassembly these constants come from, with the stack depth of every
// instruction on both sides of the detour, is in
// .strata/notes-s19-signal-hook-hardening.md.
// ---------------------------------------------------------------------------

// The addresses the detour is defined against (GTA SA 1.0 US exe, md5
// 85662cbb5aa3df8251ad7664f7a42929). A typed enum, so they stay compile-time
// constants for the stub's inline asm and link-time constants for the tests.
enum : uint32_t {
    kRhySignalCallSite = 0x49DD4C,  // CTrafficLights::DisplayActualLight's
    kRhySignalResumeAt = 0x49DD51,  // `call 0x6FC580`; resume = the next
    kRhySignalCleanupAt = 0x49DD54, // instruction; cleanup = its add esp,0x54
    kRhySignalCoronaFn = 0x6FC580   // CCoronas::RegisterCorona type thunk
};

// The 21-dword argument block of that call, as the callee reads it (position at
// 6, r/g/b at 2/3/4, alpha at 5, strength at 7, range at 8, corona type at 9,
// radius at 18). kRhySignalArgBytes is what the callee touches: 21 * 4.
enum {
    kRhyArgKey = 0,       // entity + head index: the game's corona store key
    kRhyArgEntity = 1,    // CEntity* (0 at this call site: position is world)
    kRhyArgR = 2,
    kRhyArgG = 3,
    kRhyArgB = 4,
    kRhyArgAlpha = 5,
    kRhyArgPos = 6,       // CVector* - the lens, in world coordinates
    kRhyArgStrength = 7,
    kRhyArgRange = 8,
    kRhyArgType = 9,      // mapped through 0xC3E000 by the thunk
    kRhyArgRadius = 18,
    kRhySignalArgCount = 21,
    kRhySignalArgBytes = kRhySignalArgCount * 4u,
    kRhyEntityMatrix = 0x14,  // CPlaceable::m_matrix
    kRhyMatrixAt = 0x20       // CMatrix::at (the third 4-float column)
};

// --- the kill switch --------------------------------------------------------
// Why the detour may not be installed, for the log line and the fallback.
enum RhySignalPatchReason {
    kRhyPatchOk = 0,
    kRhyPatchSource = 1,    // [trafficlights] source is not "game"
    kRhyPatchCallSite = 2,  // 0x49DD4C is not `call 0x6FC580`
    kRhyPatchCallee = 3,    // 0x6FC580 is not the corona-type thunk
    kRhyPatchCleanup = 4    // the caller's `add esp,0x54` is not there
};

// The first five bytes of 0x6FC580: `movzx edx,BYTE PTR [esp+0x28]` - the
// corona-type lookup the stub's hand-built stack is sized for.
extern const uint8_t kRhyPatchCalleeBytes[5];

// Decide whether the patch may go in. callSite/callee/cleanup point at the
// three byte windows named above (the caller reads them out of game memory).
// Decoding the call's rel32 rather than comparing five fixed bytes means a
// different-but-equivalent `call 0x6FC580` still passes, while a call to any
// other address - which is what another mod's hook leaves behind - does not.
// The cleanup check is the one that catches a caller that cleans a different
// number of arguments: the stub pushes no stack of its own across the tail
// jump, so a different cleanup would unbalance the game's frame.
// calleeTarget (optional) gets the address the call site really calls.
int rhySignalPatchCheck(int sourceIsGame, const uint8_t* callSite,
                        const uint8_t* callee, const uint8_t* cleanup,
                        uint32_t* calleeTarget);

// --- reading game memory ----------------------------------------------------
// Copy len bytes from the 32-bit game address `addr` into `dst` and return 1,
// or return 0 without touching dst when the address is not safe to read.
// signalhook.cpp implements this with rhyRangePlausible() + VirtualQuery();
// tests/signals_check.cpp implements it against a fake image, which is what
// lets every recorder path be tested without a process.
typedef int (*RhyGameReadFn)(void* ctx, uint32_t addr, void* dst, unsigned len);

// --- the recorder's decision path ------------------------------------------
// One collector: the per-frame record array plus the counters the log line
// prints. Plain ints and no locks: the patched call site and the frame function
// both run on the game's render thread. `busy` is the re-entrancy guard.
struct RhySignalCollector {
    RhySignalHead heads[kRhySignalHeadCap];
    unsigned n;          // records held for the next frame
    unsigned calls;      // the patched instruction ran at all
    unsigned records;    // a record was stored
    unsigned dropped;    // the array was full
    unsigned bad;        // a pointer was not readable
    unsigned reentrant;  // the hook fired while the recorder was already in
    unsigned busy;       // re-entrancy guard, owned by rhyCollectHead
};

enum RhyCollectResult {
    kRhyCollectOk = 0,
    kRhyCollectFull = 1,
    kRhyCollectBadArgs = 2,
    kRhyCollectBadPos = 3,
    kRhyCollectReentrant = 4
};

// The whole record path in one place: count the call, refuse re-entrancy, make
// room, read the argument block / the CVector / the entity matrix through
// `read`, and fill the slot. Every counter it reports is updated here, so the
// log line and the tests see exactly what the hook did. `tick` is passed in
// because GetTickCount() is on the Windows side.
//
// This function must never throw and never allocate: it is called from the
// naked stub, whose .eh_frame entry is empty (the unwinder would resume with
// "return address = [esp]", which is argument 0), so an exception that escaped
// here would unwind into garbage. See the s19 notes.
int rhyCollectHead(RhySignalCollector* c, uint32_t argsAddr, uint32_t entity,
                   uint32_t index, uint32_t tick, RhyGameReadFn read, void* ctx);

// Copy the collected heads into out (at most outCap), reset the array, and
// return how many were copied.
unsigned rhyCollectorTake(RhySignalCollector* c, RhySignalHead* out,
                          unsigned outCap);

// --- the page cache ---------------------------------------------------------
// An 8-entry MRU of "is this page readable", so the recorder does not call
// VirtualQuery for every pointer of every head. Kept pure so the tests can
// check hits, misses, eviction and - the point of s19 - invalidation: a page
// cached as readable in frame N and decommitted in frame N+1 must not be
// trusted in frame N+2.
enum { kRhyPageCacheSlots = 8 };
struct RhyPageCache {
    uint32_t key[kRhyPageCacheSlots];
    int ok[kRhyPageCacheSlots];
    unsigned slot;  // round-robin victim
};

void rhyPageCacheInit(RhyPageCache* c);
void rhyPageCacheClear(RhyPageCache* c);   // force re-validation
// 0/1 when the page is cached, -1 on a miss.
int rhyPageCacheFind(const RhyPageCache* c, uint32_t page);
void rhyPageCachePut(RhyPageCache* c, uint32_t page, int ok);

#endif  // RHY_SIGNALHEADS_H_
