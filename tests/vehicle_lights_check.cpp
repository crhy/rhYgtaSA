// tests/vehicle_lights_check.cpp - validates plugin/src/vehiclelights.cpp (n12).
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -Iplugin/src -o build/tmp/vehc
//       tests/vehicle_lights_check.cpp plugin/src/vehiclelights.cpp
//       plugin/src/lamps.cpp plugin/src/solar.cpp
//   ./build/tmp/vehc
//
// What is under test, and why it is testable at all: the game's vehicle lamps
// reach the renderer as 30 m emissive corona billboards (test-2026-11-12c V5:
// "tail lights throw large red blobs on the sand ... headlights a small weak
// pool"), and the fix is to size our own lights from the game's own light
// dummies. The dummies are a memory layout, so the test builds that layout by
// hand - the same record vehiclecorona.cpp fills from 0xC3E058 - and asserts
// what comes out: which corona became which lamp, where the beam points, how
// wide it is, and how much light it carries.
//
// The numbers are physical, so the assertions are too: a headlamp's peak
// intensity has to come out near a real low beam's, a tail lamp's near a real
// rear position lamp's, and a tail lamp has to stay far below a street
// luminaire. See .strata/notes-n12-vehicle-lights-particles.md for where each
// constant comes from.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../plugin/src/lamps.h"
#include "../plugin/src/vehiclelights.h"

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

static bool near(float a, float b, float tol) {
    return std::fabs(a - b) <= tol;
}

// ---------------------------------------------------------------------------
// fixtures and radiometry
// ---------------------------------------------------------------------------

static void testFixtures() {
    RhyFixture head, tail;
    rhyVehicleHeadFixture(&head);
    rhyVehicleTailFixture(&tail);

    // A headlamp reflector is 120-200 mm across; a tail lens 20-30 mm. Both are
    // an order of magnitude smaller than the 15-unit (30 m) billboard the game
    // uses, which is the whole V5 complaint.
    ok("headlamp radius is a reflector aperture (0.06-0.15 m)",
       head.radius >= 0.06f && head.radius <= 0.15f);
    ok("tail lens radius is a lens (0.02-0.10 m)",
       tail.radius >= 0.02f && tail.radius <= 0.10f);
    ok("the tail lamp is smaller than the headlamp", tail.radius < head.radius);
    // The task asks for a 40-60 degree cone. coneAngleDegrees is the HALF angle
    // (rtx_remix_api.cpp:544 takes its cosine), so 20-30 here is 40-60 there.
    ok("headlamp beam spread is 40-60 degrees",
       head.coneDeg * 2.0f >= 40.0f && head.coneDeg * 2.0f <= 60.0f);
    ok("a rear position lamp is not shaped", tail.coneDeg == 0.0f);
}

static void testRadiometry() {
    ok("flux -> radiance -> flux round-trips",
       near(rhyRadianceToFlux(rhyFluxToRadiance(123.0f, 0.3f), 0.3f), 123.0f,
            1e-3f));
    ok("a zero radius does not divide by zero",
       rhyFluxToRadiance(100.0f, 0.0f) == 0.0f);

    // 2/(1-cos 25deg) = 21.35: the factor a 50-degree cone needs to carry the
    // same peak intensity as the unshaped source, because the runtime's shaping
    // is a 0..1 mask (light_shaping.slangh) and not a flux concentrator.
    ok("cone gain at 25 deg half-angle is ~21.3",
       near(rhyBeamConcentrationGain(25.0f), 21.35f, 0.05f));
    ok("cone gain at 90 deg (a hemisphere) is 2",
       near(rhyBeamConcentrationGain(90.0f), 2.0f, 1e-3f));
    ok("no cone means no gain", rhyBeamConcentrationGain(0.0f) == 1.0f &&
                               rhyBeamConcentrationGain(-5.0f) == 1.0f &&
                               rhyBeamConcentrationGain(180.0f) == 1.0f);
    ok("a narrower cone concentrates more",
       rhyBeamConcentrationGain(15.0f) > rhyBeamConcentrationGain(30.0f));
}

// ---------------------------------------------------------------------------
// classification
// ---------------------------------------------------------------------------

static RhyCorona mk(float x, float y, float z, uint32_t key, uint32_t tex,
                    int r, int g, int b, float radius = 15.0f) {
    RhyCorona c;
    std::memset(&c, 0, sizeof c);
    c.pos[0] = x; c.pos[1] = y; c.pos[2] = z;
    c.key = key;
    c.texture = tex;
    c.strength = 35.0f;
    c.range = 50.0f;
    c.radius = radius;
    c.rgba[0] = static_cast<uint8_t>(r);
    c.rgba[1] = static_cast<uint8_t>(g);
    c.rgba[2] = static_cast<uint8_t>(b);
    c.rgba[3] = 255;
    return c;
}

static void testClassify() {
    const uint32_t headTex = 0x71A240;  // whatever the game resolved slot 4 to
    // The vehicle sites push (0xa0,0xa0,0x8c) and (0x96,0xc3,0xc3) with sprite 4.
    const RhyCorona head = mk(0, 0, 0, 0x1000, headTex, 0xa0, 0xa0, 0x8c);
    ok("the headlightline sprite is a headlamp",
       rhyVehicleClassifyCorona(head, headTex) == kRhyVLampHead);
    // The 2dFx rear lamps carry the model's red.
    const RhyCorona tail = mk(0, 0, 0, 0x1004, 0x71A200, 200, 20, 20);
    ok("a red 2dFx corona is a tail lamp",
       rhyVehicleClassifyCorona(tail, headTex) == kRhyVLampTail);
    // The headlamp colour must not read as a tail lamp when the sprite table
    // could not be resolved: that would put red spheres at the bumpers.
    const RhyCorona headNoTex = mk(0, 0, 0, 0x1000, headTex, 0xa0, 0xa0, 0x8c);
    ok("a white headlamp is not a tail lamp",
       rhyVehicleClassifyCorona(headNoTex, 0) == kRhyVLampNone);
    // An amber indicator (255,150,40) is not a lamp that should light the road.
    const RhyCorona amber = mk(0, 0, 0, 0x1008, 0x71A200, 255, 150, 40);
    ok("an amber indicator is not a tail lamp",
       rhyVehicleClassifyCorona(amber, headTex) == kRhyVLampNone);
    // A white street corona (the same sprite index 1 the vehicles use).
    const RhyCorona white = mk(0, 0, 0, 0x100c, 0x71A200, 255, 255, 255);
    ok("a white corona is neither", rhyVehicleClassifyCorona(white, headTex) ==
                                        kRhyVLampNone);
    ok("with no resolved sprite, nothing is a headlamp",
       rhyVehicleClassifyCorona(head, 0) != kRhyVLampHead);

    // Plausibility: a torn entry must not poison the distance maths.
    RhyCorona nan = mk(0, 0, 0, 0x1010, headTex, 200, 20, 20);
    nan.pos[1] = nanf("");
    ok("a NaN position is rejected", !rhyCoronaPlausible(nan));
    RhyCorona neg = mk(0, 0, 0, 0x1014, headTex, 200, 20, 20, -1.0f);
    ok("a negative radius is rejected", !rhyCoronaPlausible(neg));
    ok("a normal record is accepted", rhyCoronaPlausible(tail));

    // Brake: the game writes the 2dFx colour per light state, so a brighter red
    // is the only brake signal the store carries.
    const RhyCorona dim = mk(0, 0, 0, 0x1018, 0x71A200, 160, 16, 16);
    const RhyCorona hot = mk(0, 0, 0, 0x101c, 0x71A200, 250, 24, 24);
    ok("a bright red tail lamp is a brake lamp", rhyVehicleIsBrake(hot, 220));
    ok("a dim one is a position lamp", !rhyVehicleIsBrake(dim, 220));
    ok("the rule can be switched off", !rhyVehicleIsBrake(hot, 0));
}

// ---------------------------------------------------------------------------
// pairing and the lamps that come out
// ---------------------------------------------------------------------------

static RhyVehicleTuning mkTuning() {
    RhyVehicleTuning t;
    std::memset(&t, 0, sizeof t);
    t.headTexture = 0x71A240;
    t.maxDistance = 90.0f;
    t.headFlux = 950.0f;
    t.tailFlux = 20.0f;
    t.brakeFlux = 100.0f;
    t.headRadius = 0.0f;   // use the fixtures
    t.tailRadius = 0.0f;
    t.headConeDeg = 0.0f;
    t.headConeSoftness = -1.0f;
    t.brakeRedMin = 220;
    t.maxLamps = 24;
    t.maxSpread = 6.0f;
    t.radianceScale = 1.0f;
    return t;
}

// One car along -Y: front at y=-10, rear at y=-6. Keys entity+0 (the vehicle
// sites push esi) and entity+7 (0x6E219A pushes esi+7) - the grouping has to
// join them even though the index is not 0..3.
static unsigned oneCar(RhyCorona* cor, uint32_t base, float yFront, float yRear) {
    cor[0] = mk(-0.6f, yFront, 0.6f, base, 0x71A240, 0xa0, 0xa0, 0x8c);
    cor[1] = mk(0.6f, yFront, 0.6f, base, 0x71A240, 0xa0, 0xa0, 0x8c);
    cor[2] = mk(-0.6f, yRear, 0.6f, base + 7, 0x71A200, 200, 20, 20);
    cor[3] = mk(0.6f, yRear, 0.6f, base + 7, 0x71A200, 200, 20, 20);
    return 4;
}

static void testOneCar() {
    RhyCorona cor[8];
    const unsigned n = oneCar(cor, 0x1000, -10.0f, -6.0f);
    RhyVehicleTuning t = mkTuning();
    RhyVehicleLamp out[16];
    int vehicles = 0;
    const int got = rhyVehicleBuildLamps(cor, n, nullptr, t, out, 16, &vehicles);
    ok("one car gives 4 lamps", got == 4);
    ok("one car counts as one vehicle", vehicles == 1);

    int heads = 0, tails = 0;
    for (int i = 0; i < got; i++) {
        if (out[i].kind == kRhyVLampHead) {
            heads++;
        } else if (out[i].kind == kRhyVLampTail) {
            tails++;
        }
    }
    ok("2 headlamps and 2 tail lamps", heads == 2 && tails == 2);

    // The beam axis comes from the car's own dummies: front minus rear.
    bool aimOk = false;
    for (int i = 0; i < got; i++) {
        if (out[i].kind != kRhyVLampHead) {
            continue;
        }
        aimOk = near(out[i].dir[0], 0.0f, 1e-3f) &&
                near(out[i].dir[1], -1.0f, 1e-3f) &&
                near(out[i].dir[2], 0.0f, 1e-3f);
    }
    ok("the beam points from the tail dummies to the head dummies", aimOk);

    // Radii and cone.
    bool sizeOk = false, coneOk = false;
    for (int i = 0; i < got; i++) {
        if (out[i].kind == kRhyVLampHead) {
            sizeOk = near(out[i].radius, 0.09f, 1e-4f);
            coneOk = out[i].coneDeg * 2.0f >= 40.0f && out[i].coneDeg * 2.0f <= 60.0f;
        }
    }
    ok("headlamp radius comes from the fixture", sizeOk);
    ok("headlamp cone is 40-60 degrees", coneOk);

    // Radiometry. Peak intensity of a sphere light is radiance * pi * r^2, and
    // with the cone gain applied that is flux*gain/4. For the shipped 950 flux
    // and a 25-degree half-angle that is ~5000 - against a street luminaire's
    // 3473/4 = 868, i.e. the headlamp is the brighter object in its beam, which
    // is what a 20 000-30 000 cd low beam against a ~3 000 cd cobra head means.
    float headPeak = 0.0f, tailPeak = 0.0f, tailR = 0, tailG = 0, tailB = 0;
    for (int i = 0; i < got; i++) {
        float mean = 0.0f;
        for (int k = 0; k < 3; k++) {
            mean = std::max(mean, out[i].radiance[k]);
        }
        const float peak = mean * 3.14159265f * out[i].radius * out[i].radius;
        if (out[i].kind == kRhyVLampHead) {
            headPeak = peak;
        } else {
            tailPeak = peak;
            tailR = out[i].radiance[0];
            tailG = out[i].radiance[1];
            tailB = out[i].radiance[2];
        }
    }
    ok("headlamp peak intensity is ~5000 (a real low beam, not a billboard)",
       headPeak > 4000.0f && headPeak < 6500.0f);
    ok("the headlamp out-lights a street luminaire inside its beam",
       headPeak > 868.0f);
    ok("a tail lamp is far dimmer than a headlamp", tailPeak * 50.0f < headPeak);
    ok("a tail lamp is red light, not white light with a tint",
       tailR > 4.0f * tailG && tailR > 4.0f * tailB);

    // And the tail lamp must stay a lamp, not a flood: 20 flux units over a
    // 0.05 m lens is ~25 cd of peak intensity, the range of a real rear
    // position lamp, and it is 1/170 of the street luminaire.
    ok("tail lamp flux is the size of a rear position lamp",
       rhyRadianceToFlux(tailR, 0.05f) > 5.0f &&
       rhyRadianceToFlux(tailR, 0.05f) < 200.0f);
}

static void testBrakeAndDistance() {
    RhyCorona cor[8];
    oneCar(cor, 0x2000, -10.0f, -6.0f);
    cor[2].rgba[0] = 250;  // the brake filament
    cor[3].rgba[0] = 250;
    RhyVehicleTuning t = mkTuning();
    RhyVehicleLamp out[16];
    int vehicles = 0;
    const int got = rhyVehicleBuildLamps(cor, 4, nullptr, t, out, 16, &vehicles);
    float brakePeak = 0.0f;
    for (int i = 0; i < got; i++) {
        if (out[i].kind == kRhyVLampTail) {
            brakePeak = std::max(brakePeak, out[i].radiance[0]);
        }
    }
    RhyVehicleLamp out2[16];
    oneCar(cor, 0x2000, -10.0f, -6.0f);  // back to the dim red
    const int got2 = rhyVehicleBuildLamps(cor, 4, nullptr, t, out2, 16, &vehicles);
    float tailPeak = 0.0f;
    for (int i = 0; i < got2; i++) {
        if (out2[i].kind == kRhyVLampTail) {
            tailPeak = std::max(tailPeak, out2[i].radiance[0]);
        }
    }
    ok("braking makes the rear lamps brighter", brakePeak > 3.0f * tailPeak);

    // Distance gate: the same car 200 m away gets nothing.
    const float player[3] = {0, 0, 0};
    RhyCorona far[8];
    oneCar(far, 0x3000, -210.0f, -206.0f);
    const int gotFar = rhyVehicleBuildLamps(far, 4, player, t, out, 16, &vehicles);
    ok("a car 200 m away gets no light", gotFar == 0 && vehicles == 0);

    // Nearest-first capping: three cars, room for four lamps -> the two nearest
    // cars are the ones covered.
    RhyCorona many[12];
    oneCar(many + 0, 0x4000, -10.0f, -6.0f);
    oneCar(many + 4, 0x5000, -30.0f, -26.0f);
    oneCar(many + 8, 0x6000, -80.0f, -76.0f);
    const int gotCap = rhyVehicleBuildLamps(many, 12, player, t, out, 4, &vehicles);
    float farthest = 0.0f;
    for (int i = 0; i < gotCap; i++) {
        const float d = std::sqrt(out[i].pos[1] * out[i].pos[1]);
        farthest = std::max(farthest, d);
    }
    ok("the cap trims the FAR cars, never the near ones",
       gotCap == 4 && farthest < 40.0f);
}

static void testGrouping() {
    RhyVehicleTuning t = mkTuning();
    RhyVehicleLamp out[16];
    int vehicles = 0;

    // A red shopfront 20 m from a car's headlamps shares nothing with the car:
    // the spread test is what stops "the car has a tail lamp 20 m behind it".
    RhyCorona mix[8];
    oneCar(mix, 0x7000, -10.0f, -6.0f);
    mix[2] = mk(-0.6f, -30.0f, 0.6f, 0x7004, 0x71A200, 200, 20, 20);
    mix[3] = mk(0.6f, -30.0f, 0.6f, 0x7004, 0x71A200, 200, 20, 20);
    const int got = rhyVehicleBuildLamps(mix, 4, nullptr, t, out, 16, &vehicles);
    bool anyTailFar = false;
    for (int i = 0; i < got; i++) {
        if (out[i].kind == kRhyVLampTail && out[i].pos[1] < -20.0f) {
            anyTailFar = true;
        }
    }
    // The whole group is refused, not just the far half: a "car" whose rear
    // dummies sit 20 m behind its front ones is not a car, and lighting it would
    // put a red sphere in the middle of nowhere.
    ok("lamps more than maxSpread apart are not one vehicle",
       got == 0 && !anyTailFar);

    // A headlamp with no rear dummies and no entity matrix has no axis, and an
    // un-aimed cone light is worse than no light.
    RhyCorona onlyHead[4];
    onlyHead[0] = mk(-0.6f, -10.0f, 0.6f, 0x8000, 0x71A240, 0xa0, 0xa0, 0x8c);
    onlyHead[1] = mk(0.6f, -10.0f, 0.6f, 0x8000, 0x71A240, 0xa0, 0xa0, 0x8c);
    const int noAxis = rhyVehicleBuildLamps(onlyHead, 2, nullptr, t, out, 16,
                                            &vehicles);
    ok("a headlamp with no axis is dropped, not aimed at nothing", noAxis == 0);

    // ... but the entity matrix's own axis is enough.
    onlyHead[0].hasNormal = true;
    onlyHead[0].normal[0] = 0.0f;
    onlyHead[0].normal[1] = -1.0f;
    onlyHead[0].normal[2] = 0.0f;
    const int withAxis = rhyVehicleBuildLamps(onlyHead, 2, nullptr, t, out, 16,
                                              &vehicles);
    bool dirOk = withAxis == 2;
    for (int i = 0; i < withAxis && dirOk; i++) {
        dirOk = near(out[i].dir[1], -1.0f, 1e-3f);
    }
    ok("the entity matrix axis aims the beam when the car shows one end only",
       dirOk);

    // Hashes: stable per lamp, different between lamps.
    ok("the same lamp hashes the same",
       rhyVehicleLampHash(0x1234, kRhyVLampHead, 0) ==
           rhyVehicleLampHash(0x1234, kRhyVLampHead, 0));
    ok("different lamps hash differently",
       rhyVehicleLampHash(0x1234, kRhyVLampHead, 0) !=
           rhyVehicleLampHash(0x1234, kRhyVLampHead, 1) &&
       rhyVehicleLampHash(0x1234, kRhyVLampHead, 0) !=
           rhyVehicleLampHash(0x1238, kRhyVLampHead, 0) &&
       rhyVehicleLampHash(0x1234, kRhyVLampHead, 0) !=
           rhyVehicleLampHash(0x1234, kRhyVLampTail, 0));
}

int main() {
    testFixtures();
    testRadiometry();
    testClassify();
    testOneCar();
    testBrakeAndDistance();
    testGrouping();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
