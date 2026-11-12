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

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
