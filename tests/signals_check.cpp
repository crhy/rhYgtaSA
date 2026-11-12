// tests/signals_check.cpp - validates the pure half of s01: the corona-to-light
// mapping in plugin/src/signalheads.cpp.
//
// Build:
//   g++ -std=c++17 -Iplugin/src tests/signals_check.cpp
//       plugin/src/signalheads.cpp plugin/src/lamps.cpp plugin/src/solar.cpp
//       -o build/tmp/sigc && build/tmp/sigc
//
// Nothing here needs the game. What is being checked is the three things the
// game-facing hook can get wrong and nobody would notice until a screenshot:
// the colour the game chose turning into the right lens, the head keeping its
// identity across frames, and a head that stopped being registered losing its
// light. The game-side facts these values come from (the byte tables at
// 0x8A6214/0x8A6218/0xA9AD94, the argument layout of the RegisterCorona call at
// 0x49DD4C) are recorded with evidence in .strata/notes-s01-signal-heads.md.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../plugin/src/lamps.h"
#include "../plugin/src/signalheads.h"

static int g_fail = 0;
static int g_checks = 0;

static void ok(const char* what, bool cond) {
    ++g_checks;
    if (!cond) {
        ++g_fail;
        std::printf("FAIL  %s\n", what);
    } else {
        std::printf("ok    %s\n", what);
    }
}

// ---------------------------------------------------------------------------
// s19: a fake game memory, so the recorder's decision path can run here.
//
// rhyCollectHead() never dereferences a game address itself: it asks a
// RhyGameReadFn to copy bytes and believes the answer. In the plugin that
// reader is "plausible range + committed page + memcpy"; here it is "is this
// range inside one of these windows". Same contract, so what the test exercises
// is the code that runs on the render thread.
// ---------------------------------------------------------------------------
struct FakeMem {
    uint32_t addr;
    const void* data;
    unsigned len;
};

static int fakeRead(void* ctx, uint32_t addr, void* dst, unsigned len) {
    const FakeMem* m = static_cast<const FakeMem*>(ctx);
    for (; m->data; ++m) {
        if (addr >= m->addr && len <= m->len &&
            addr - m->addr <= m->len - len) {  // no wrap, fully inside
            std::memcpy(dst, static_cast<const char*>(m->data) + (addr - m->addr),
                        len);
            return 1;
        }
    }
    return 0;
}

// A reader that re-enters the collector while it is being called: the only way
// to get a second entry while the first is still running, which is what the
// re-entrancy guard exists for.
static RhySignalCollector* g_reenter = nullptr;
static int g_reenterResult = 0;
static int reentrantRead(void* ctx, uint32_t addr, void* dst, unsigned len) {
    const int r = fakeRead(ctx, addr, dst, len);
    if (g_reenter && addr == 0x00100000u) {
        g_reenterResult = rhyCollectHead(g_reenter, addr, 0x00300000u, 2, 0u,
                                         &fakeRead, ctx);
    }
    return r;
}

// A record as the hook would fill it: entity, head index, world position, and
// the colour the game handed RegisterCorona.
static RhySignalHead head(uint32_t entity, uint32_t index, float x, float y,
                          float z, int r, int g, int b, uint32_t tick) {
    RhySignalHead h;
    h.entity = entity;
    h.index = index;
    h.pos[0] = x;
    h.pos[1] = y;
    h.pos[2] = z;
    h.normal[0] = 1.0f;  // the entity matrix's "at" axis, whatever it points at
    h.normal[1] = h.normal[2] = 0.0f;
    h.rgb[0] = static_cast<uint8_t>(r);
    h.rgb[1] = static_cast<uint8_t>(g);
    h.rgb[2] = static_cast<uint8_t>(b);
    h.alpha = 255;
    h.strength = 1.0f;
    h.tick = tick;
    return h;
}

int main() {
    // ---- the colour the game chose -> which aspect it is -------------------
    // The game's own three aspect colours, straight out of its byte tables.
    ok("the game's green corona maps to the green lens",
       rhySignalPhaseFromGameRgb(0, 255, 0) == 0);
    ok("the game's amber corona maps to the amber lens",
       rhySignalPhaseFromGameRgb(255, 128, 0) == 1);
    ok("the game's red corona maps to the red lens",
       rhySignalPhaseFromGameRgb(255, 0, 0) == 2);

    // The game multiplies all three by a global factor ([0xB7C4E4]*[0x858CA8]),
    // so the same aspect arrives dimmed as well as full. Matching on normalised
    // colour is what keeps the answer the same.
    ok("a half-brightness green corona is still green",
       rhySignalPhaseFromGameRgb(0, 128, 0) == 0);
    ok("a half-brightness amber corona is still amber",
       rhySignalPhaseFromGameRgb(128, 64, 0) == 1);
    ok("a quarter-brightness red corona is still red",
       rhySignalPhaseFromGameRgb(64, 0, 0) == 2);

    // Amber and red differ only in the green channel; a signal that reads the
    // wrong one of these is the classic "orange instead of red" bug.
    ok("amber (255,120,0) is not called red", rhySignalPhaseFromGameRgb(255, 120, 0) == 1);
    ok("red (255,30,0) is not called amber", rhySignalPhaseFromGameRgb(255, 30, 0) == 2);
    ok("a near-black corona is 'unknown', not forced to red",
       rhySignalPhaseFromGameRgb(0, 0, 0) == -1);
    ok("a white corona is 'unknown', not forced to an aspect",
       rhySignalPhaseFromGameRgb(255, 255, 255) == -1);
    ok("a blue corona is 'unknown'", rhySignalPhaseFromGameRgb(0, 0, 255) == -1);
    ok("small noise around green stays green",
       rhySignalPhaseFromGameRgb(12, 240, 8) == 0);

    // ---- and the aspect then picks the measured lens colour ----------------
    // This is the chain the task asks for: the game's choice, our spectrum.
    float lens[3];
    rhySignalLensRgb(rhySignalPhaseFromGameRgb(0, 255, 0), lens);
    ok("green corona -> blue-green lens (green dominant, real blue, no red)",
       lens[1] > 0.99f && lens[2] > 0.25f && lens[0] < 0.05f);
    rhySignalLensRgb(rhySignalPhaseFromGameRgb(255, 128, 0), lens);
    ok("amber corona -> amber lens (full red, some green, no blue)",
       lens[0] > 0.99f && lens[1] > 0.12f && lens[1] < 0.35f && lens[2] < 0.05f);
    rhySignalLensRgb(rhySignalPhaseFromGameRgb(255, 0, 0), lens);
    ok("red corona -> red lens (no green, no blue)",
       lens[0] > 0.99f && lens[1] < 0.05f && lens[2] < 0.05f);

    // ---- identity: one head, one id, every frame ---------------------------
    const uint64_t a = rhyHeadId(0x87F2C0u, 0);
    const uint64_t b = rhyHeadId(0x87F2C0u, 1);
    const uint64_t c = rhyHeadId(0x87F340u, 0);
    ok("the same head keeps the same id", rhyHeadId(0x87F2C0u, 0) == a);
    ok("the two heads of one signal are different lights", a != b);
    ok("two signals are different lights", a != c);
    ok("index 0 and index 8 of one entity differ",
       rhyHeadId(0x87F2C0u, 0) != rhyHeadId(0x87F2C0u, 8));

    // Remix keys lights by hash, so a collision would make one head's light
    // stand in for another's. Entity pointers in SA are 16-byte aligned and
    // clustered in a few heap blocks, which is exactly the input a naive
    // id+phase hash fails on.
    bool hashStable = true;
    bool hashDiffers = true;
    uint64_t seen[3000];
    int n = 0;
    for (uint32_t e = 0x870000u; e < 0x870000u + 40u * 16u; e += 16) {
        for (uint32_t i = 0; i < 3; ++i) {
            const uint64_t id = rhyHeadId(e, i);
            for (int p = 0; p < 3; ++p) {
                const uint64_t h = rhyHeadLightHash(id, p, 0x5248590000000000ull);
                if (h != rhyHeadLightHash(id, p, 0x5248590000000000ull)) {
                    hashStable = false;
                }
                if (p > 0 && h == rhyHeadLightHash(id, p - 1, 0x5248590000000000ull)) {
                    hashDiffers = false;  // a phase change would not recreate
                }
                for (int k = 0; k < n; ++k) {
                    if (seen[k] == h) {
                        hashDiffers = false;
                    }
                }
                if (n < 3000) {
                    seen[n++] = h;
                }
            }
        }
    }
    ok("a head's light hash is stable across calls", hashStable);
    ok("120 clustered heads x 3 phases produce distinct hashes", hashDiffers && n == 360);
    ok("the hash base moves the whole hash space (no clash with the lamp lights)",
       rhyHeadLightHash(a, 0, 0) != rhyHeadLightHash(a, 0, 1));

    // ---- lifetime: a head the game stopped registering ---------------------
    ok("a head seen 1.9 s ago is not stale", rhyHeadStale(10000, 11900, 2000) == 0);
    ok("a head seen 2.1 s ago is stale", rhyHeadStale(10000, 12100, 2000) == 1);
    ok("a head seen this frame is not stale", rhyHeadStale(12100, 12100, 2000) == 0);
    // GetTickCount() wraps after ~49.7 days; unsigned subtraction is the
    // wraparound-safe form, and a signed comparison would go stale for 24 days.
    ok("the expiry survives the GetTickCount() wraparound",
       rhyHeadStale(0xFFFFFFF0u, 0x00000100u, 2000u) == 0);
    ok("and still expires after the wraparound",
       rhyHeadStale(0xFFFFFFF0u, 0x00001100u, 2000u) == 1);

    // ---- position: the corona position IS the lens -------------------------
    const RhySignalHead h = head(0x87F2C0u, 1, -1024.5f, 2240.25f, 30.5f,
                                 255, 0, 0, 1000);
    float out[3];
    rhyHeadLightPos(&h, nullptr, 0.15f, out);
    ok("with no direction known the light sits exactly on the corona",
       out[0] == h.pos[0] && out[1] == h.pos[1] && out[2] == h.pos[2]);
    rhyHeadLightPos(&h, h.normal, 0.0f, out);
    ok("a zero forward distance changes nothing either",
       out[0] == h.pos[0] && out[1] == h.pos[1] && out[2] == h.pos[2]);
    rhyHeadLightPos(&h, h.normal, 0.15f, out);
    ok("a unit normal moves the lens 0.15 m along it",
       std::fabs(out[0] - (h.pos[0] + 0.15f)) < 1e-5f && out[1] == h.pos[1]);
    const float big[3] = {0.0f, 0.0f, 4.0f};
    rhyHeadLightPos(&h, big, 0.15f, out);
    ok("a non-unit normal is normalised, not scaled",
       std::fabs(out[2] - (h.pos[2] + 0.15f)) < 1e-5f);
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    rhyHeadLightPos(&h, zero, 0.15f, out);
    ok("a zero-length normal moves nothing instead of producing NaN",
       out[0] == h.pos[0] && out[2] == h.pos[2]);

    // ---- selection: which heads get a light --------------------------------
    // A mast-arm signal: the pole at x=0, the heads several metres out along the
    // arm. This is the bug s01 fixes - the old list mode lit the pole.
    RhySignalHead recs[8];
    recs[0] = head(0x87F2C0u, 0, 0.0f, 0.0f, 6.0f, 255, 0, 0, 5000);      // pole end
    recs[1] = head(0x87F2C0u, 1, 4.0f, 0.0f, 6.0f, 255, 0, 0, 5000);      // arm head
    recs[2] = head(0x87F2C0u, 2, 8.0f, 0.0f, 6.0f, 0, 255, 0, 5000);      // far head, green
    recs[3] = head(0x87F300u, 0, 400.0f, 0.0f, 6.0f, 255, 0, 0, 5000);    // far away
    recs[4] = head(0x87F310u, 0, 120.0f, 0.0f, 6.0f, 255, 128, 0, 5000);  // mid distance
    const float player[3] = {0.0f, 0.0f, 0.0f};

    RhyHeadWant want[8];
    unsigned n2 = rhyChooseHeads(recs, 5, player, 250.0f, 300.0f, 60, 8,
                                 nullptr, 0, want, 8);
    ok("every head within range is chosen (the far one at 400 m is not)", n2 == 4);
    ok("the nearest head is first", want[0].id == rhyHeadId(0x87F2C0u, 0));
    ok("the mast-arm head is chosen at its own position, not the pole's",
       want[1].id == rhyHeadId(0x87F2C0u, 1));
    ok("each head carries the aspect the game showed it",
       want[0].phase == 2 && want[1].phase == 2 && want[2].phase == 0 &&
           want[3].phase == 1);
    ok("the chosen ids are the ids the records describe",
       want[2].id == rhyHeadId(0x87F2C0u, 2) &&
           want[3].id == rhyHeadId(0x87F310u, 0));

    // Hysteresis: a head between max_distance and exit_distance keeps the light
    // it already has but does not get a new one.
    const uint64_t active[1] = {rhyHeadId(0x87F310u, 0)};
    unsigned n3 = rhyChooseHeads(recs, 5, player, 50.0f, 300.0f, 60, 8, active, 1,
                                 want, 8);
    bool kept = false;
    for (unsigned i = 0; i < n3; ++i) {
        if (want[i].id == rhyHeadId(0x87F310u, 0)) {
            kept = true;
        }
    }
    ok("an active head past max_distance but inside exit_distance keeps its light",
       kept);
    unsigned n4 = rhyChooseHeads(recs, 5, player, 50.0f, 300.0f, 60, 8, nullptr, 0,
                                 want, 8);
    bool gotNew = false;
    for (unsigned i = 0; i < n4; ++i) {
        if (want[i].id == rhyHeadId(0x87F310u, 0)) {
            gotNew = true;
        }
    }
    ok("the same head does not get a NEW light at that distance", !gotNew);

    // The cap drops the farthest, never the ones that already have a handle.
    unsigned n5 = rhyChooseHeads(recs, 5, player, 0.0f, 0.0f, 2, 0, nullptr, 0,
                                 want, 8);
    ok("the cap is honoured", n5 == 2);
    ok("the two nearest heads win the cap",
       want[0].id == rhyHeadId(0x87F2C0u, 0) &&
           want[1].id == rhyHeadId(0x87F2C0u, 1));
    const uint64_t act2[2] = {rhyHeadId(0x87F2C0u, 2), rhyHeadId(0x87F310u, 0)};
    unsigned n6 = rhyChooseHeads(recs, 5, player, 0.0f, 0.0f, 2, 2, act2, 2, want, 8);
    bool stickyKept = false, nearestDropped = true;
    for (unsigned i = 0; i < n6; ++i) {
        if (want[i].id == rhyHeadId(0x87F310u, 0) ||
            want[i].id == rhyHeadId(0x87F2C0u, 2)) {
            stickyKept = true;
        }
        if (want[i].id == rhyHeadId(0x87F2C0u, 0)) {
            nearestDropped = false;
        }
    }
    // Both already-lit heads keep their handle even though the cap is 2 and five
    // heads are in range: dropping them is what makes a signal blink.
    ok("already-active heads survive a cap they alone exceed",
       n6 == 2 && stickyKept && nearestDropped);

    // A garbage record must not take the intersection down with it.
    RhySignalHead bad = head(0x87F2C0u, 3, 0.0f, 0.0f, 6.0f, 255, 0, 0, 5000);
    bad.pos[0] = std::nanf("");
    unsigned n7 = rhyChooseHeads(&bad, 1, player, 0.0f, 0.0f, 60, 0, nullptr, 0,
                                 want, 8);
    ok("a NaN position is dropped rather than sorted as nearest", n7 == 0);

    // An unrecognisable colour is reported as -1 so the caller keeps the colour
    // the head already had instead of flicking it to red.
    RhySignalHead white = head(0x87F2C0u, 4, 1.0f, 0.0f, 6.0f, 255, 255, 255, 5000);
    unsigned n8 = rhyChooseHeads(&white, 1, player, 0.0f, 0.0f, 60, 0, nullptr, 0,
                                 want, 8);
    ok("an unknown corona colour is passed through as -1",
       n8 == 1 && want[0].phase == -1);

    // Two frames of the same intersection: the ids must match, or every frame
    // would look like a phase change and the light would blink.
    RhySignalHead f2[3];
    f2[0] = head(0x87F2C0u, 0, 0.0f, 0.0f, 6.0f, 255, 0, 0, 5033);
    f2[1] = head(0x87F2C0u, 1, 4.0f, 0.0f, 6.0f, 255, 0, 0, 5033);
    f2[2] = head(0x87F2C0u, 2, 8.0f, 0.0f, 6.0f, 0, 255, 0, 5033);
    RhyHeadWant w2[3];
    unsigned n9 = rhyChooseHeads(f2, 3, player, 0.0f, 0.0f, 60, 0, nullptr, 0, w2, 3);
    const bool sameIds = (n9 == 3) &&
                         (w2[0].id == rhyHeadId(0x87F2C0u, 0)) &&
                         (w2[1].id == rhyHeadId(0x87F2C0u, 1)) &&
                         (w2[2].id == rhyHeadId(0x87F2C0u, 2));
    ok("the same heads in the next frame give the same ids (no churn)", sameIds);

    // Pointer sanity, the half that does not need a process. The hook reads game
    // memory on the render thread; the value that crashed the 13:04:44 run was
    // the float -575.2 (0xC48F9A0F) being used as an argument-block pointer.
    ok("a kernel/high address is not readable (0xC48F9A0F = the crash value)",
       !rhyRangePlausible(0xC48F9A0Fu, 84u));
    ok("null and the first page are not readable",
       !rhyRangePlausible(0u, 4u) && !rhyRangePlausible(0x14u, 4u));
    ok("a read that would wrap past the end of the address space is refused",
       !rhyRangePlausible(0x7FFFFFD0u, 0x10000u));
    ok("an absurd length is refused even at a good address",
       !rhyRangePlausible(0x0E153028u, 0x20000u));
    ok("a normal heap range is plausible (the game's corona store)",
       rhyRangePlausible(0x0C3E064u, 84u) && rhyRangePlausible(0x0178DEFB4u, 12u));

    // =====================================================================
    // s19: the hook's decision paths, run against fake memory.
    //
    // The detour itself is machine code (tests/test_signal_hook.py disassembles
    // it); everything below is the part that was moved out of it so it could be
    // tested at all: the kill switch, the recorder's every-path, and the page
    // cache. Each case is a path the render thread can actually take.
    // =====================================================================

    // ---- the kill switch: rhySignalPatchCheck ----------------------------
    // The bytes exactly as the exe has them (verified against gta-sa.exe in
    // tests/test_signal_hook.py; the addresses come from signalheads.h).
    const uint8_t kGoodCall[5] = {0xE8, 0x2F, 0xE8, 0x25, 0x00};  // call 0x6FC580
    const uint8_t kGoodCallee[5] = {0x0F, 0xB6, 0x54, 0x24, 0x28};
    const uint8_t kGoodCleanup[3] = {0x83, 0xC4, 0x54};           // add esp,0x54
    uint32_t target = 0;
    ok("source=game + the real bytes: the patch is allowed",
       rhySignalPatchCheck(1, kGoodCall, kGoodCallee, kGoodCleanup, &target) ==
           kRhyPatchOk);
    ok("the call site's rel32 decodes to the corona thunk",
       target == 0x6FC580u);
    ok("source=list refuses even when every byte is right (the kill switch)",
       rhySignalPatchCheck(0, kGoodCall, kGoodCallee, kGoodCleanup, &target) ==
           kRhyPatchSource);
    // Another mod's detour leaves a jmp here; a different build leaves a
    // different call. Both must be refused, and the reason must say which.
    const uint8_t kJmp[5] = {0xE9, 0x00, 0x00, 0x00, 0x00};
    ok("a jmp at the patch site is refused",
       rhySignalPatchCheck(1, kJmp, kGoodCallee, kGoodCleanup, &target) ==
           kRhyPatchCallSite);
    const uint8_t kOtherCall[5] = {0xE8, 0x00, 0x00, 0x00, 0x00};  // call 0x49DD51
    ok("a call to some other address is refused",
       rhySignalPatchCheck(1, kOtherCall, kGoodCallee, kGoodCleanup, &target) ==
           kRhyPatchCallSite);
    ok("... and the reason line can name the address it found",
       target == 0x49DD51u);
    const uint8_t kBadCallee[5] = {0x90, 0xB6, 0x54, 0x24, 0x28};
    ok("a callee that is not the corona-type thunk is refused",
       rhySignalPatchCheck(1, kGoodCall, kBadCallee, kGoodCleanup, &target) ==
           kRhyPatchCallee);
    // The one the s01 checks missed: the call site can be intact while the
    // caller cleans a different number of arguments (a mod that added or
    // removed one). The stub pushes nothing across its tail jump, so the game's
    // own cleanup is what keeps DisplayActualLight's frame balanced - and it
    // writes its locals at [esp+0x2c..0x34] immediately after.
    const uint8_t kBadCleanup[3] = {0x83, 0xC4, 0x50};  // add esp,0x50
    ok("a caller that cleans 20 arguments instead of 21 is refused",
       rhySignalPatchCheck(1, kGoodCall, kGoodCallee, kBadCleanup, &target) ==
           kRhyPatchCleanup);
    const uint8_t kNoCleanup[3] = {0x85, 0xC0, 0x00};  // test eax,eax
    ok("no cleanup at all is refused",
       rhySignalPatchCheck(1, kGoodCall, kGoodCallee, kNoCleanup, &target) ==
           kRhyPatchCleanup);
    ok("a null byte window is refused rather than read",
       rhySignalPatchCheck(1, nullptr, kGoodCallee, kGoodCleanup, nullptr) ==
           kRhyPatchCallSite);

    // ---- the recorder: fake memory, then every path ----------------------
    // Windows-side rhyReadable() is "plausible range + committed page"; here it
    // is "in this image or not". Same contract, so the decision path under test
    // is the one that runs in the game.
    static uint32_t s_args[21];
    static float s_pos[3] = {2495.25f, -1680.5f, 27.75f};
    static uint32_t s_matrixBits = 0x00400000u;   // entity+0x14 -> a real matrix
    static float s_at[3] = {1.0f, 0.0f, 0.0f};    // matrix+0x20: the "at" axis
    const uint32_t kArgsAddr = 0x00100000u;
    const uint32_t kPosAddr = 0x00200000u;
    const uint32_t kEntity = 0x00300000u;
    const uint32_t kMatrixSlot = kEntity + 0x14;  // kRhyEntityMatrix
    const uint32_t kMatrixAt = 0x00400020u;       // s_matrixBits + 0x20
    s_args[kRhyArgKey] = kEntity + 1;
    s_args[kRhyArgEntity] = 0;                    // this call site passes no entity
    s_args[kRhyArgR] = 0x1FFu;                    // > 0xFF on purpose: masked
    s_args[kRhyArgG] = 128;
    s_args[kRhyArgB] = 0;
    s_args[kRhyArgAlpha] = 0x1FFu;
    s_args[kRhyArgPos] = kPosAddr;
    s_args[kRhyArgStrength] = 0x3F800000u;        // 1.0f
    s_args[kRhyArgRange] = 0x42480000u;           // 50.0f
    s_args[kRhyArgType] = 0;
    s_args[kRhyArgRadius] = 0x41700000u;          // 15.0f
    const FakeMem mem[] = {
        {kArgsAddr, s_args, sizeof s_args},
        {kPosAddr, s_pos, sizeof s_pos},
        {kMatrixSlot, &s_matrixBits, sizeof s_matrixBits},
        {kMatrixAt, s_at, sizeof s_at},
        {0u, nullptr, 0u},
    };

    RhySignalCollector col;
    std::memset(&col, 0, sizeof col);
    int res = rhyCollectHead(&col, kArgsAddr, kEntity, 1, 5000u, &fakeRead,
                             const_cast<FakeMem*>(mem));
    ok("a good corona record is taken", res == kRhyCollectOk && col.n == 1);
    ok("the record carries the entity and head index the call site held",
       col.heads[0].entity == kEntity && col.heads[0].index == 1);
    ok("the record carries the world position from argument 6",
       col.heads[0].pos[0] == s_pos[0] && col.heads[0].pos[1] == s_pos[1] &&
           col.heads[0].pos[2] == s_pos[2]);
    ok("colour and alpha keep only the byte the game stores",
       col.heads[0].rgb[0] == 0xFFu && col.heads[0].rgb[1] == 128u &&
           col.heads[0].rgb[2] == 0u && col.heads[0].alpha == 0xFFu);
    ok("strength is the argument's bit pattern, not a cast",
       col.heads[0].strength == 1.0f);
    ok("the entity matrix's at axis is recorded as the normal",
       col.heads[0].normal[0] == 1.0f && col.heads[0].normal[1] == 0.0f);
    ok("the tick is the one the caller passed (GetTickCount is Windows-side)",
       col.heads[0].tick == 5000u);
    ok("calls and records both counted", col.calls == 1 && col.records == 1);

    // The 13:04:44 crash: argument 6 was the float -575.2 used as a pointer.
    std::memset(&col, 0, sizeof col);
    s_args[kRhyArgPos] = 0xC48F9A0Fu;
    res = rhyCollectHead(&col, kArgsAddr, kEntity, 1, 5000u, &fakeRead,
                         const_cast<FakeMem*>(mem));
    ok("a position pointer that is really a float is refused, not followed",
       res == kRhyCollectBadPos && col.n == 0 && col.bad == 1);
    ok("... and the call is still counted (calls>0, records=0 is diagnosable)",
       col.calls == 1 && col.records == 0);
    s_args[kRhyArgPos] = kPosAddr;

    // A null position pointer, and one below the allocation floor.
    std::memset(&col, 0, sizeof col);
    s_args[kRhyArgPos] = 0u;
    res = rhyCollectHead(&col, kArgsAddr, kEntity, 1, 0u, &fakeRead,
                         const_cast<FakeMem*>(mem));
    ok("a null position pointer is refused", res == kRhyCollectBadPos && col.n == 0);
    s_args[kRhyArgPos] = kPosAddr;

    // The argument block itself unreadable (the 12:04:33 crash read it from the
    // wrong place; the stub's offsets are what the python test guards).
    std::memset(&col, 0, sizeof col);
    res = rhyCollectHead(&col, 0x00F00000u, kEntity, 1, 0u, &fakeRead,
                         const_cast<FakeMem*>(mem));
    ok("an argument block that is not readable is refused before it is read",
       res == kRhyCollectBadArgs && col.n == 0 && col.bad == 1);

    // A matrix we cannot read is "no direction", never "no record": the
    // position and colour are already in hand and correct.
    std::memset(&col, 0, sizeof col);
    const FakeMem noMatrix[] = {
        {kArgsAddr, s_args, sizeof s_args},
        {kPosAddr, s_pos, sizeof s_pos},
        {0u, nullptr, 0u},
    };
    res = rhyCollectHead(&col, kArgsAddr, kEntity, 1, 0u, &fakeRead,
                         const_cast<FakeMem*>(noMatrix));
    ok("an unreadable entity matrix still gives a record",
       res == kRhyCollectOk && col.n == 1);
    ok("... with a zero normal instead of a fault",
       col.heads[0].normal[0] == 0.0f && col.heads[0].normal[1] == 0.0f &&
           col.heads[0].normal[2] == 0.0f);
    // m_matrix == 0 (an entity whose matrix was never set up).
    std::memset(&col, 0, sizeof col);
    const uint32_t zeroMatrix = 0u;
    const FakeMem nullMatrix[] = {
        {kArgsAddr, s_args, sizeof s_args},
        {kPosAddr, s_pos, sizeof s_pos},
        {kMatrixSlot, &zeroMatrix, sizeof zeroMatrix},
        {0u, nullptr, 0u},
    };
    res = rhyCollectHead(&col, kArgsAddr, kEntity, 1, 0u, &fakeRead,
                         const_cast<FakeMem*>(nullMatrix));
    ok("a null m_matrix still gives a record with a zero normal",
       res == kRhyCollectOk && col.n == 1 && col.heads[0].normal[0] == 0.0f);

    // The array cap: 256 records is the whole budget for a frame, and the 257th
    // must be counted and dropped, never written past the array.
    std::memset(&col, 0, sizeof col);
    bool allOk = true;
    for (unsigned i = 0; i < kRhySignalHeadCap; ++i) {
        if (rhyCollectHead(&col, kArgsAddr, kEntity, i, 0u, &fakeRead,
                           const_cast<FakeMem*>(mem)) != kRhyCollectOk) {
            allOk = false;
        }
    }
    res = rhyCollectHead(&col, kArgsAddr, kEntity, 999, 0u, &fakeRead,
                         const_cast<FakeMem*>(mem));
    ok("every head up to the cap is taken", allOk && col.n == kRhySignalHeadCap);
    ok("the head past the cap is dropped, not written",
       res == kRhyCollectFull && col.n == kRhySignalHeadCap && col.dropped == 1);
    ok("and it is still counted as a call", col.calls == kRhySignalHeadCap + 1u);

    // Hand the frame's records to the light code: they are copied out and the
    // array is empty again, so a hook that misfires cannot grow anything.
    RhySignalHead taken[kRhySignalHeadCap];
    unsigned got = rhyCollectorTake(&col, taken, kRhySignalHeadCap);
    ok("take() hands over the whole frame and resets",
       got == kRhySignalHeadCap && col.n == 0 &&
           taken[0].entity == kEntity && taken[7].index == 7);
    ok("take() on an empty collector gives nothing",
       rhyCollectorTake(&col, taken, kRhySignalHeadCap) == 0);
    ok("take() never writes more than the caller's array",
       rhyCollectorTake(&col, taken, 0) == 0);

    // Re-entrancy: the fake reader calls the collector back, which is what a
    // hook firing while the recorder is running would look like. The inner call
    // must be refused: the array index and the page cache are shared state.
    std::memset(&col, 0, sizeof col);
    g_reenter = &col;
    g_reenterResult = kRhyCollectOk;
    const FakeMem reentrant[] = {
        {kArgsAddr, s_args, sizeof s_args},
        {kPosAddr, s_pos, sizeof s_pos},
        {kMatrixSlot, &s_matrixBits, sizeof s_matrixBits},
        {kMatrixAt, s_at, sizeof s_at},
        {0u, nullptr, 0u},
    };
    res = rhyCollectHead(&col, kArgsAddr, kEntity, 1, 0u, &reentrantRead,
                         const_cast<FakeMem*>(reentrant));
    ok("a recorder entered from inside itself is refused",
       g_reenterResult == kRhyCollectReentrant && col.reentrant == 1);
    ok("... while the outer record is still stored correctly",
       res == kRhyCollectOk && col.n == 1 && col.heads[0].index == 1);
    g_reenter = nullptr;

    // ---- the page cache --------------------------------------------------
    RhyPageCache pc;
    rhyPageCacheInit(&pc);
    ok("an empty page cache misses", rhyPageCacheFind(&pc, 0x0049D000u) == -1);
    rhyPageCachePut(&pc, 0x0049D000u, 1);
    ok("a cached readable page hits", rhyPageCacheFind(&pc, 0x0049D000u) == 1);
    rhyPageCachePut(&pc, 0x00C3E000u, 0);
    ok("a cached unreadable page hits as unreadable",
       rhyPageCacheFind(&pc, 0x00C3E000u) == 0);
    ok("a page that was never queried still misses",
       rhyPageCacheFind(&pc, 0x010BF000u) == -1);
    for (unsigned i = 0; i < kRhyPageCacheSlots; ++i) {
        rhyPageCachePut(&pc, 0x10000000u + i * 0x1000u, 1);
    }
    ok("the cache is bounded: the oldest entry is gone after 8 more",
       rhyPageCacheFind(&pc, 0x0049D000u) == -1);
    rhyPageCacheClear(&pc);
    ok("clear() forces re-validation of every page (one frame of staleness)",
       rhyPageCacheFind(&pc, 0x10000000u) == -1);

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
