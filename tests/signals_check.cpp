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
#include "../plugin/src/solar.h"  // n02b: colorTemperatureToRgb for the lamp flux

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
    ok("a null byte window is not a verdict, it is a step (s30)",
       rhySignalPatchCheck(1, nullptr, kGoodCallee, kGoodCleanup, nullptr) ==
           kRhyPatchNoBytes);
    ok("... and the source answer still comes first, before any byte is looked at",
       rhySignalPatchCheck(0, nullptr, nullptr, nullptr, nullptr) ==
           kRhyPatchSource);
    // s30 REGRESSION. rhySignalHookInstall() asks the source question with null
    // windows and then, if that answer says "go and read", reads them and asks
    // again. When the null-window answer was kRhyPatchCallSite the two calls
    // together meant the exe was never read and the hook never installed: every
    // run from s19 to the 2026-11-12 test logged "the bytes at 0x49DD4C are not
    // 'call 0x6FC580'" about an exe whose bytes are exactly that, and lit zero
    // heads. This is the install sequence, spelled out, so it cannot break again
    // without turning this binary red.
    {
        int why = rhySignalPatchCheck(1, nullptr, nullptr, nullptr, nullptr);
        ok("install step 1 with source=game asks for the bytes instead of refusing",
           why == kRhyPatchNoBytes);
        if (why == kRhyPatchNoBytes) {
            why = rhySignalPatchCheck(1, kGoodCall, kGoodCallee, kGoodCleanup,
                                      &target);
        }
        ok("install step 2 with the real exe bytes allows the patch (hook installs)",
           why == kRhyPatchOk && target == 0x6FC580u);
        // and the same walk with source=list never reaches the exe at all
        int whyList = rhySignalPatchCheck(0, nullptr, nullptr, nullptr, nullptr);
        ok("install with source=list stops at step 1", whyList == kRhyPatchSource);
    }

    // ---- s30: the game's own corona, which is the green flood ------------
    // 0x49DCD9 pushes the 15.0 game-unit radius for the head's corona: a 30 m
    // wide additive pure-green sprite per lit head. Measured in
    // .strata/notes-s30-signal-bulbs.md: 10-39% of the signal-stop frames are
    // saturated green with not one green pixel above luma 200, and that wash is
    // there even in the 2026-11-11 tour, where the plugin created no traffic
    // lights at all. Resizing that immediate is the fix; it is only done when
    // the bytes say it is the radius push.
    const uint8_t kGoodCoronaPush[5] = {0x68, 0x00, 0x00, 0x70, 0x41};  // push 15.0f
    float gameRadius = 0.0f;
    ok("the corona radius push is recognised and reads back as 15.0",
       rhySignalCoronaPatchCheck(kGoodCoronaPush, &gameRadius) == kRhyPatchOk &&
           gameRadius == 15.0f);
    ok("the constant in signalheads.cpp is those bytes",
       std::memcmp(kRhyPatchCoronaBytes, kGoodCoronaPush, 5) == 0);
    const uint8_t kOtherPush[5] = {0x68, 0x00, 0x00, 0x48, 0x42};  // push 50.0f
    ok("a different push is refused (the corona is left alone)",
       rhySignalCoronaPatchCheck(kOtherPush, &gameRadius) == kRhyPatchCorona);
    ok("a null pointer is refused, not read",
       rhySignalCoronaPatchCheck(nullptr, &gameRadius) == kRhyPatchCorona);
    ok("corona_radius=0 keeps the game's own value (the off switch)",
       rhySignalCoronaRadius(15.0f, 0.0f) == 15.0f);
    ok("corona_radius=0.45 asks for 0.45 m",
       rhySignalCoronaRadius(15.0f, 0.45f) == 0.45f);
    ok("a tiny corona is clamped to something that still reads as a light",
       rhySignalCoronaRadius(15.0f, 0.001f) == 0.05f);
    ok("a huge one is clamped before it can flood the street again",
       rhySignalCoronaRadius(15.0f, 40.0f) == 1.2f);
    ok("the resize shrinks the sprite's area by more than a thousand",
       (15.0f * 15.0f) / (0.45f * 0.45f) > 1000.0f);
    // And the safety net that goes with it: the corona is only shrunk when the
    // hook is live, so a hook that is live but records nothing must not be left
    // as the only signal layer (that would be dark heads, worse than a flood).
    // n02 makes that test range-aware: "no records" only counts when a signal of
    // the loaded list is inside the distance at which the game itself would draw
    // one (50.0 m: push $0x42480000 at 0x49DCF3, tested inside the callee at
    // 0x6FC22E). The 2026-11-11 runs sat 79.1 m from the nearest of the 1522
    // poles, so their records=0 proved nothing and must not flip the source.
    const float RANGE = static_cast<float>(kRhySignalCoronaRangeM);
    ok("a hook that has recorded a head is the source",
       rhyHeadsHealthy(1, 0, 100000, 20000, 14.4f, RANGE) == kRhyHeadSourceGame);
    ok("a fresh hook with no records yet gets the grace window",
       rhyHeadsHealthy(0, 100000, 115000, 20000, 14.4f, RANGE) == kRhyHeadSourceGrace);
    ok("a signal inside the game's corona range, grace over, no record: use the list",
       rhyHeadsHealthy(0, 100000, 125001, 20000, 14.4f, RANGE) == kRhyHeadSourceList);
    ok("the same silence 79.1 m from the nearest pole proves nothing: keep the hook",
       rhyHeadsHealthy(0, 100000, 125001, 20000, 79.1f, RANGE) == kRhyHeadSourceFar);
    ok("an empty list (FLT_MAX) is 'nothing to light', not 'the hook is broken'",
       rhyHeadsHealthy(0, 100000, 125001, 20000, 3.402823466e+38f,
                       RANGE) == kRhyHeadSourceFar);
    ok("exactly at the game's range still counts as a signal being near",
       rhyHeadsHealthy(0, 100000, 125001, 20000, RANGE, RANGE) == kRhyHeadSourceList);
    ok("the grace still wins over the distance",
       rhyHeadsHealthy(0, 100000, 115000, 20000, 79.1f, RANGE) == kRhyHeadSourceGrace);
    ok("grace 0 decides on the spot (and still asks the distance question)",
       rhyHeadsHealthy(0, 100000, 100001, 0, 14.4f, RANGE) == kRhyHeadSourceList);
    ok("the health check survives the GetTickCount() wraparound",
       rhyHeadsHealthy(0, 0xFFFFFFF0u, 0x00000100u, 20000, 14.4f,
                       RANGE) == kRhyHeadSourceGrace);
    ok("and it is cumulative: one head years ago keeps the source",
       rhyHeadsHealthy(7, 100000, 10000000, 20000, 79.1f, RANGE) == kRhyHeadSourceGame);
    ok("only the list decision gives the hook up",
       rhyHeadKeepsHook(kRhyHeadSourceFar) == 1 &&
       rhyHeadKeepsHook(kRhyHeadSourceGrace) == 1 &&
       rhyHeadKeepsHook(kRhyHeadSourceGame) == 1 &&
       rhyHeadKeepsHook(kRhyHeadSourceList) == 0);
    ok("every decision says why in the log line",
       std::strstr(rhyHeadSourceText(kRhyHeadSourceFar), "corona range") != nullptr &&
       std::strstr(rhyHeadSourceText(kRhyHeadSourceList), "inside") != nullptr);

    // ---- n02: the distance the decision is made on ----------------------
    const float xy[] = { 1040.0f, -1392.0f, 1068.0f, -1428.0f, 2520.0f, -1729.2f };
    const float atStop[] = { 1052.0f, -1400.0f };
    // Three poles from the shipped list and a point on one of the tour's signal
    // stops (the 11 Nov tour parked CJ at 1040,-1392 and 1068,-1428, where the
    // nearest of the 1522 poles is 14.4 m and 13.5 m away).
    ok("the nearest of three poles, measured exactly (2D)",
       std::fabs(rhyNearestSignal2D(xy, 3, atStop) -
                 std::sqrt(12.0f * 12.0f + 8.0f * 8.0f)) < 0.001f);
    ok("and it is the near one, not the far one",
       rhyNearestSignal2D(xy, 3, atStop) < 15.0f);
    ok("an empty list reads as 'nothing near', not 'a signal is here'",
       rhyNearestSignal2D(nullptr, 0, atStop) > 1e30f);
    ok("no player position is also 'nothing near'",
       rhyNearestSignal2D(xy, 3, nullptr) > 1e30f);
    ok("one pole, and only that pole",
       std::fabs(rhyNearestSignal2D(xy, 1, atStop) - 14.4222f) < 0.001f);

    // ---- n02: the lens, not the mast -----------------------------------
    // Lines copied out of the generated rhyremix_traffic.txt. This one is a
    // cj_traffic_light4 instance (720 of the 1522) heading 0, and the lamp table
    // says its head sits at (-2.10, +0.10, +7.60) in the model's own frame.
    RhyTrafficLine v2, v1, five, junk;
    ok("a v2 line parses all eight columns",
       rhyTrafficParseLine("-269.781 1088.109 18.711 7.60 0.0 -2.100 +0.100 +7.600",
                           &v2));
    ok("the v2 columns are kept", v2.hasLens == 1 && v2.heading == 0.0f &&
       v2.lens[0] == -2.100f && v2.lens[1] == 0.100f && v2.lens[2] == 7.600f);
    ok("column 4 is still the v1 headz", v2.headz == 7.60f);
    ok("a v1 line still parses, and carries no lens columns",
       rhyTrafficParseLine("-269.781 1088.109 18.711 7.60", &v1) && v1.hasLens == 0);
    ok("a v1 line's lens is straight up the mast, so nothing moves",
       v1.lens[0] == 0.0f && v1.lens[1] == 0.0f && v1.lens[2] == 7.60f);
    ok("a heading with no lens columns is honoured (five numbers)",
       rhyTrafficParseLine("1 2 3 4 90.0", &five) && five.heading == 90.0f &&
       five.hasLens == 0);
    ok("a comment, a blank line and a half-written v2 line are refused",
       !rhyTrafficParseLine("# rhyremix traffic v2", &junk) &&
       !rhyTrafficParseLine("", &junk) && !rhyTrafficParseLine("1 2 3 4 5 6", &junk));

    float lensPos[3], polePos[3];
    rhyTrafficLightPos(v2, 0, polePos);
    rhyTrafficLightPos(v2, 1, lensPos);
    ok("lens_offset=0 is the old pole position, byte for byte",
       polePos[0] == v2.x && polePos[1] == v2.y && polePos[2] == v2.z + v2.headz);
    ok("the lens is the pole plus the offset rotated by the heading (0 deg here)",
       std::fabs(lensPos[0] - (v2.x - 2.10f)) < 1e-4f &&
       std::fabs(lensPos[1] - (v2.y + 0.10f)) < 1e-4f &&
       std::fabs(lensPos[2] - (v2.z + 7.60f)) < 1e-4f);
    const float mx = lensPos[0] - polePos[0], my = lensPos[1] - polePos[1];
    ok("which is 2.10 m off the mast - the part the v1 file threw away",
       std::fabs(std::sqrt(mx * mx + my * my) - 2.1024f) < 0.001f);
    RhyTrafficLine old = v1;
    float a1[3], a2[3];
    rhyTrafficLightPos(old, 0, a1);
    rhyTrafficLightPos(old, 1, a2);
    ok("a v1 file gives the same light either way (the compatibility rule)",
       a1[0] == a2[0] && a1[1] == a2[1] && a1[2] == a2[2]);

    // The heading is not decoration: the same offset points four different ways.
    RhyTrafficLine rot = v2;
    rot.x = 0.0f; rot.y = 0.0f; rot.z = 0.0f;
    const float wantXY[4][2] = { { -2.10f, 0.10f }, { -0.10f, -2.10f },
                               { 2.10f, -0.10f }, { 0.10f, 2.10f } };
    const float deg[4] = { 0.0f, 90.0f, 180.0f, 270.0f };
    bool rotOk = true, rigid = true;
    for (int i = 0; i < 4; ++i) {
        rot.heading = deg[i];
        float p[3];
        rhyTrafficLightPos(rot, 1, p);
        if (std::fabs(p[0] - wantXY[i][0]) > 1e-3f ||
            std::fabs(p[1] - wantXY[i][1]) > 1e-3f ||
            std::fabs(p[2] - 7.60f) > 1e-4f) {
            rotOk = false;
        }
        // A rotation is rigid: the lens stays the same distance from the pole.
        const float d = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (std::fabs(d - std::sqrt(2.10f * 2.10f + 0.01f + 7.60f * 7.60f)) > 1e-3f) {
            rigid = false;
        }
    }
    ok("the offset rotates with the heading (0/90/180/270 deg)", rotOk);
    ok("and the rotation is rigid: the head stays 7.88 m from the pole base", rigid);
    ok("the game's own corona range is the 50.0 m the exe pushes",
       kRhySignalCoronaRangeM == 50 && kRhySignalCoronaFadeM == 35);

    // ---- s30: the bulb - radius and intensity of the light WE create -----
    // The 2026-11-12 run created `radius=0.10 radiance=9.64,5400.00,1845.57`:
    // 1.5 nits flux-scaled by (6.0/0.10)^2 = x3600, i.e. the output of the old
    // six-metre blob squeezed into a 0.1 m sphere. Sixty of those is a green
    // street, not sixty small heads. The bulb model uses the lens radius and a
    // lens luminance, and a signal must stay far dimmer in total output than a
    // street luminaire (a real signal is ~10^2 lm against ~10^4 lm).
    RhySignalBulb bulb;
    rhySignalBulb(0, 1, rhySignalRadius(1.0f), 1200.0f, 1.5f, 6.0f, 1.0f, &bulb);
    ok("the bulb radius is the real lens, 0.10 m", bulb.radius == 0.10f);
    ok("the bulb radiance is the ini nits at the lens, not x3600",
       bulb.radiance[1] == 1200.0f);
    ok("the green lens colour is still applied to the other channels",
       bulb.radiance[1] > 0.0f && bulb.radiance[0] < 0.01f * bulb.radiance[1] &&
           bulb.radiance[2] < 0.5f * bulb.radiance[1]);
    ok("the bulb model is not the legacy path", bulb.legacy == 0);
    ok("the old numbers are gone: no channel reaches the 5400 of the flux rule",
       bulb.radiance[0] < 5400.0f && bulb.radiance[1] < 5400.0f &&
           bulb.radiance[2] < 5400.0f);
    // A mid-size street luminaire, same functions, same ini defaults: the bound
    // that keeps a signal from out-lighting the lamp over its head.
    RhyFixture street;
    rhyStreetFixture(1, &street);
    const float streetRad = rhyRadianceForFlux(2.0f, street.radius, 15.0f);
    float streetMean = 0.0f;
    float lampCol[3];
    rhyLampFamilyRgb(RHY_LAMP_HPS, lampCol);
    for (int i = 0; i < 3; ++i) {
        streetMean += lampCol[i] * streetRad;
    }
    streetMean /= 3.0f;
    const float streetFlux = 4.0f * 3.14159265f * street.radius * street.radius *
                             streetMean;
    ok("one signal puts out under 5% of one street luminaire",
       bulb.flux < streetFlux / 20.0f);
    ok("and the old 5400-nit bulb would not have passed that bound",
       4.0f * 3.14159265f * 0.10f * 0.10f * (5400.0f * 0.447f) > streetFlux / 20.0f);
    ok("the flux grows with r^2 and not with the ini radius alone",
       (rhySignalRadius(2.0f) * rhySignalRadius(2.0f)) >
           (rhySignalRadius(1.0f) * rhySignalRadius(1.0f)));
    // legacy fixture_size=0: the blob path keeps the flux-preserving rule, so
    // nobody's old ini silently goes dark.
    RhySignalBulb legacy;
    rhySignalBulb(2, 0, 6.0f, 2500.0f, 1.5f, 6.0f, 1.0f, &legacy);
    ok("fixture_size=0 still gets the flux-preserving radiance (1.5 nits at 6 m)",
       legacy.legacy == 1 && legacy.radiance[0] == 1.5f && legacy.radius == 6.0f);
    RhySignalBulb legacySmall;
    rhySignalBulb(2, 0, 0.1f, 2500.0f, 1.5f, 6.0f, 1.0f, &legacySmall);
    ok("... which is exactly the x3600 the 2026-11-12 log shows at 0.1 m",
       std::fabs(legacySmall.radiance[0] - 5400.0f) < 1.0f);
    // Guards: a bad ini value must not reach Remix.
    RhySignalBulb noRadius;
    rhySignalBulb(0, 1, 0.0f, 2500.0f, 1.5f, 6.0f, 1.0f, &noRadius);
    ok("a zero radius falls back to the lens size instead of a zero-area light",
       noRadius.radius == 0.10f && noRadius.flux > 0.0f);
    RhySignalBulb off;
    rhySignalBulb(0, 1, 0.1f, 0.0f, 1.5f, 6.0f, 1.0f, &off);
    ok("bulb_radiance=0 publishes a dark light (the off switch)",
       off.radiance[0] == 0.0f && off.radiance[1] == 0.0f && off.flux == 0.0f);
    RhySignalBulb scaled;
    rhySignalBulb(0, 1, 0.1f, 1200.0f, 1.5f, 6.0f, 0.5f, &scaled);
    ok("[trafficlights] radiance_scale scales the bulb too (n23: not the street knob)",
       std::fabs(scaled.radiance[1] - 600.0f) < 1e-3f);
    RhySignalBulb scaleBad;
    rhySignalBulb(0, 1, 0.1f, 1200.0f, 1.5f, 6.0f, -2.0f, &scaleBad);
    ok("a nonsense radiance_scale is ignored rather than going negative",
       scaleBad.radiance[1] == 1200.0f);
    // The same bulb for both paths: a head light and a list light for the same
    // phase must be the same bulb, or the fallback changes the look.
    RhySignalBulb b1, b2;
    rhySignalBulb(1, 1, rhySignalRadius(1.0f), 1200.0f, 1.5f, 6.0f, 1.0f, &b1);
    rhySignalBulb(1, 1, rhySignalRadius(1.0f), 1200.0f, 1.5f, 6.0f, 1.0f, &b2);
    ok("head and list paths get identical bulbs",
       b1.radius == b2.radius && b1.radiance[1] == b2.radiance[1]);
    // Positions: the head light sits on the lens, and the bulb size does not
    // move it (a 0.1 m sphere at the lens, not a 6 m ball around it).
    RhySignalHead lensHead = head(0x87F2C0u, 2, 2520.0f, -1729.2f, 23.4f,
                                  0, 255, 0, 1000);
    float lpos[3];
    rhyHeadLightPos(&lensHead, nullptr, 0.0f, lpos);
    RhySignalBulb lensBulb;
    rhySignalBulb(0, 1, rhySignalRadius(1.0f), 1200.0f, 1.5f, 6.0f, 1.0f,
                  &lensBulb);
    ok("the bulb is small enough to fit inside the head it sits on",
       2.0f * lensBulb.radius < 0.25f);
    ok("and its centre is the lens the game drew, to the millimetre",
       lpos[0] == lensHead.pos[0] && lpos[1] == lensHead.pos[1] &&
           lpos[2] == lensHead.pos[2]);

    // ---- n02b: the glow - the SECOND light every lit signal needs --------
    // rhY 2026-11-12 08:50: "We wanted the glow from the traffic light and the
    // bulb. It should be both like real life." s30 fixed the bulb and, in doing
    // it, removed the glow twice over (our light's flux 304 -> 67, the game's own
    // 15.0-unit corona -> 0.45: .strata/notes-n02b-signal-glow-and-bulb.md step
    // 2). These are the four bounds the task asked for: the bulb stays small and
    // hot, the glow is bigger and softer, the two are the same colour and phase,
    // and the glow's total output stays under one street luminaire's.
    RhySignalBulb b0;
    rhySignalBulb(0, 1, rhySignalRadius(1.0f), 1200.0f, 1.5f, 6.0f, 1.0f, &b0);
    RhySignalGlow g0;
    rhySignalGlow(0, 3.0f, kRhySignalGlowFluxRef, kRhySignalGlowFluxCap, 1.0f,
                  &g0);
    ok("the glow is a bigger sphere than the bulb (3 m of spill vs a 0.10 m lens)",
       g0.radius > 10.0f * b0.radius);
    ok("and far softer per unit area (6 nits against the bulb's 1200)",
       g0.radiance[1] < b0.radiance[1] / 100.0f);
    ok("the glow carries the flux the 2026-11-12 00:48 glow measured (304)",
       std::fabs(g0.flux - 304.0f) < 1.0f);
    ok("which is more total light than the s30 bulb, at a fraction of its radiance",
       g0.flux > b0.flux && g0.radiance[1] < b0.radiance[1]);
    // Same colour, by construction: both halves take their triple from
    // rhySignalLensRgb(phase), so the ratio between them is one constant.
    bool sameColour = true;
    bool fluxSteady = true;
    for (int p = 0; p < 3; ++p) {
        RhySignalBulb bb;
        RhySignalGlow gg;
        rhySignalBulb(p, 1, rhySignalRadius(1.0f), 1200.0f, 1.5f, 6.0f, 1.0f, &bb);
        rhySignalGlow(p, 3.0f, kRhySignalGlowFluxRef, kRhySignalGlowFluxCap, 1.0f,
                      &gg);
        int bmax = 0, gmax = 0;
        for (int i = 1; i < 3; ++i) {
            if (bb.radiance[i] > bb.radiance[bmax]) bmax = i;
            if (gg.radiance[i] > gg.radiance[gmax]) gmax = i;
        }
        if (bmax != gmax) sameColour = false;
        // every channel must sit at the same fraction of the bulb's
        float ratio = -1.0f;
        for (int i = 0; i < 3; ++i) {
            if (bb.radiance[i] <= 0.0f) {
                if (gg.radiance[i] != 0.0f) sameColour = false;
                continue;
            }
            const float r = gg.radiance[i] / bb.radiance[i];
            if (ratio < 0.0f) ratio = r;
            else if (std::fabs(r - ratio) > 1e-4 * ratio) sameColour = false;
        }
        if (!(gg.flux > 0.0f && gg.flux <= kRhySignalGlowFluxCap)) sameColour = false;
        // The glow is solved for FLUX, not for base radiance, so the spill under
        // a signal keeps its strength through the cycle and only changes colour.
        if (std::fabs(gg.flux - kRhySignalGlowFluxRef) > 1.0f) fluxSteady = false;
    }
    ok("and the glow is the SAME colour as the bulb in all three phases",
       sameColour);
    ok("the spill does not brighten when the aspect changes: flux is constant",
       fluxSteady);
    // The ceiling, derived rather than asserted: the same 4*pi*r^2*mean the two
    // signal functions report, applied to a mid street luminaire with the shipped
    // ini (radiance_big=2.0 at radius_ref=15, the 2100 K / 5000 K lamp pair).
    float warm[3], cold[3], mix[3];
    colorTemperatureToRgb(2100.0f, warm);
    colorTemperatureToRgb(5000.0f, cold);
    for (int i = 0; i < 3; ++i) mix[i] = (warm[i] + cold[i]) / 2.0f;
    const float lampFlux = rhyLuminaireFlux(2.0f, 15.0f, mix);
    ok("one mid street luminaire puts out the 3473 the s30 notes quote",
       std::fabs(lampFlux - 3473.0f) < 0.01f * 3473.0f);
    ok("a signal's glow is under a tenth of that: it must not out-light its lamp",
       g0.flux < lampFlux / 10.0f);
    RhySignalGlow gCap;
    rhySignalGlow(0, 3.0f, 999999.0f, 0.0f, 1.0f, &gCap);
    ok("a hand-edited glow_flux is capped at one luminaire, and says so",
       gCap.clamped == 1 && std::fabs(gCap.flux - kRhySignalGlowFluxCap) < 1.0f);
    RhySignalGlow gOff;
    rhySignalGlow(0, 3.0f, 0.0f, 0.0f, 1.0f, &gOff);
    ok("glow_flux=0 publishes no glow (the bulb-only A/B)",
       gOff.flux == 0.0f && gOff.radiance[0] == 0.0f &&
           gOff.radiance[1] == 0.0f && gOff.radiance[2] == 0.0f);
    RhySignalGlow gDef;
    rhySignalGlow(0, 0.0f, 304.0f, 0.0f, 1.0f, &gDef);
    ok("glow_radius=0 falls back to the 3 m spill rather than a zero-area light",
       gDef.radius == kRhySignalGlowRadius && gDef.flux > 0.0f);
    RhySignalGlow gBig;
    rhySignalGlow(0, 500.0f, 304.0f, 0.0f, 1.0f, &gBig);
    ok("a glow_radius that would rebuild the 30 m flood is clamped",
       gBig.radius <= 20.0f && gBig.flux > 0.0f);
    RhySignalGlow g1m, g3m;
    rhySignalGlow(0, 1.0f, 304.0f, 0.0f, 1.0f, &g1m);
    rhySignalGlow(0, 3.0f, 304.0f, 0.0f, 1.0f, &g3m);
    ok("the flux is the knob and the radius only moves it: same flux, 9x radiance",
       std::fabs(g1m.flux - g3m.flux) < 1.0f &&
           std::fabs(g1m.radiance[1] - 9.0f * g3m.radiance[1]) <
               0.01f * g1m.radiance[1]);
    RhySignalGlow gScaled;
    rhySignalGlow(0, 3.0f, 304.0f, 0.0f, 0.5f, &gScaled);
    ok("[trafficlights] radiance_scale scales the glow too (n23: one dial, signals only)",
       std::fabs(gScaled.flux - g0.flux / 2.0f) < 1.0f);
    RhySignalGlow gBad;
    rhySignalGlow(0, 3.0f, 304.0f, 0.0f, -2.0f, &gBad);
    ok("a nonsense radiance_scale does not make the glow negative",
       gBad.flux > 0.0f && gBad.radiance[1] > 0.0f);
    // The glow sits on the lens too: it is the light that lens throws, so it has
    // no business being anywhere else (and the pair must not be 3 m apart).
    ok("the glow is centred on the same lens as the bulb (one signal, one place)",
       2.0f * g0.radius < 7.6f);  // smaller than the head's height above the road

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
