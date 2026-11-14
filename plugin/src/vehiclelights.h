#ifndef RHY_VEHICLELIGHTS_H
#define RHY_VEHICLELIGHTS_H

// ---------------------------------------------------------------------------
// n12: the car's own lights, as physical light sources.
//
// .strata/test-2026-11-12c.md (V5): "tail lights throw large red blobs on the
// sand (too wide/strong for 2 small bulbs); headlights a small weak pool". Both
// halves of that sentence are about SIZE, and both come from the same cause: the
// only light sources in the scene that belong to a car are the game's own corona
// sprites, and a corona sprite is a 15 game-unit (30 m) billboard. The runtime
// renders those sprites as emissive geometry (rtx_instance_manager.cpp:1132 -
// an emissive-blended draw call gets its material's emissive forced to
// rtx.emissiveBlendOverrideEmissiveIntensity) and, with
// rtx.enableSeparateUnorderedApproximations (default true), that emissive
// geometry also lights the surroundings. So a tail bulb - 4 cm of red glass -
// reaches the renderer as a 30 m red ball.
//
// The fix is the same one s30/n02b applied to traffic signals: keep the game's
// sprite (it is the thing you SEE) and add our own light (the thing that LITS),
// sized to what the real lamp is. See lamps.h for why size and brightness have
// to be separate numbers, and signalheads.h for the bulb+glow pair that came out
// of that decision for signals.
//
// ---------------------------------------------------------------------------
// WHERE THE POSITIONS COME FROM: the game's own light dummies
// ---------------------------------------------------------------------------
// CVehicle does not keep a list of lamp positions we could read; the game
// computes them every frame and hands them to CCoronas::RegisterCorona. That
// store is the source of truth, and it is readable without patching anything:
//
//   0xC3E038  dword  the number of live coronas (0..64)
//   0xC3E058  entry 0, stride 0x3C, 64 entries
//       +0x00 float x,y,z      world position (0x6FC39F: lea 0xc3e058(%ecx),%eax)
//       +0x0C uint32 key       the store key: the entity, plus the head index
//                              (0x6FC369; the vehicle sites push esi and esi+7)
//       +0x10 uint32 texture   the corona sprite POINTER, mapped from the sprite
//                              index by the thunk at 0x6FC580
//                              (`movzbl 0x28(%esp),%edx; mov 0xc3e000(,%edx,4),%eax`)
//       +0x14 float strength   (0x6FC3B7, argument 7)
//       +0x1C float range      (0x6FC3CB, argument 8)
//       +0x28 float radius     (0x6FC415, argument 18)
//       +0x2C uchar r,g,b,a    (0x6FC378..0x6FC396, arguments 2..5)
//   0xC3E000  the 11-entry sprite table the thunk indexes. The init loop at
//             0x6FAA8B loads the names from 0x8D4950 (stride 0x1A):
//               0 coronastar  1 coronastar  2 coronamoon  3 coronareflect
//               4 coronaheadlightline  9 coronaringb
//
// Which sites are the car's: the six RegisterCorona calls in the vehicle render
// path - 0x6E0D01, 0x6E0DF7, 0x6E219F, 0x6E2297, 0x6E253E, 0x6E2633 - all read
// the model pointer at vehicle+0x384 and the class at vehicle+0x594, push sprite
// index 4 ("coronaheadlightline") or 1 ("coronastar"), a bluish-white colour
// (0xa0,0xa0,0x8c or 0x96,0xc3,0xc3), radius 15.0 and the vehicle itself as the
// link argument. The red rear lamps come from the model's 2dFx entries instead
// (the loop at 0x53B5A8 walks 40-byte C2dFx entries, pushes sprite index 2
// "coronamoon", strength 35.0, radius 15.0 and the entry's own colour), which is
// why the rear lamps are classified by colour and the front ones by sprite.
//
// Everything below is pure arithmetic on that record: no windows.h, no Remix, no
// game pointers dereferenced. tests/vehicle_lights_check.cpp runs it. The
// game-memory side is plugin/src/vehiclecorona.cpp.
// ---------------------------------------------------------------------------

#include <cstdint>

#include "lamps.h"   // RhyFixture

// One corona as read out of the game's store (see the layout above).
struct RhyCorona {
    float pos[3];        // +0x00, world, game units (1 unit ~ 1 metre)
    uint32_t key;        // +0x0C, entity (+ head index)
    uint32_t texture;    // +0x10, sprite pointer
    float strength;      // +0x14
    float range;         // +0x1C
    float radius;        // +0x28
    uint8_t rgba[4];     // +0x2C
    float normal[3];     // filled by the reader: the entity matrix's "at" axis
    bool hasNormal;      // (matrix+0x20) was readable
};

// What a corona is, as far as this module cares.
enum RhyVehicleLampKind {
    kRhyVLampNone = 0,
    kRhyVLampHead = 1,   // a headlamp: the "coronaheadlightline" sprite
    kRhyVLampTail = 2    // a rear lamp: a red 2dFx corona
};

// The real lamp geometry, in the same RhyFixture form lamps.h uses for street
// luminaires and signal lenses.
//
// Headlamp: the reflector aperture of a car headlamp is 120-200 mm across, so a
// 0.09 m emitting sphere is the reflector, not the filament. The cone is the
// BEAM spread: a low beam is 40-60 degrees wide where it leaves the car (that is
// what the task asks for and what ECE R112's grid roughly describes), and
// LightInfoLightShaping::coneAngleDegrees is the HALF angle - the runtime does
// cos(DegToRad(coneAngleDegrees)) at rtx_remix_api.cpp:544 and the street lamps'
// cone_angle=120 is a 240-degree flood, which is only sensible for a luminaire
// pointing at the road. So 25 here is a 50-degree beam.
void rhyVehicleHeadFixture(RhyFixture* out);

// Tail lamp: a tail bulb is 5-10 mm of red glass behind a 20-30 mm lens. 0.05 m
// is the lens. No cone: a tail lamp is deliberately diffuse, ECE R7 makes the
// horizontal intensity distribution of a rear position lamp close to a
// cosine-squared lobe, and the physical reason is that it must be readable from
// any angle. The "short falloff" the task asks for is not a cone, it is the
// inverse-square law applied to a lamp with 60-80 lumens instead of a 30 m
// emissive billboard.
void rhyVehicleTailFixture(RhyFixture* out);

// ---------------------------------------------------------------------------
// Flux, radiance, and the one thing the runtime's cone does NOT do
// ---------------------------------------------------------------------------
// The plugin's flux convention (signalheads.h rhyLuminaireFlux): a sphere of
// radius r and mean radiance L carries flux = 4*pi*r^2*L, and one street
// luminaire measures 3473 of those units with the shipped ini. That is the unit
// every flux number below is in, and it is what makes "a headlight is 1/15 of a
// street lamp" a checkable statement instead of a taste.
float rhyFluxToRadiance(float flux, float radius);
float rhyRadianceToFlux(float radiance, float radius);

// Light shaping in this runtime is a MASK, not a flux concentrator:
// light_shaping.slangh's lightShapingCalcRadialFalloff returns a 0..1 factor
// multiplied into the light's contribution, and nothing renormalizes it. A
// sphere light cut to a 50-degree cone therefore emits 50/360ths of what the
// same radiance would emit unshaped - the opposite of a real headlamp, which
// puts the SAME lumens into that cone and gets a beam you can see 30 m down the
// road. The correction is the geometric concentration of the cone:
//
//     gain = 4*pi / (2*pi*(1 - cos(theta))) = 2 / (1 - cos(theta))
//
// with theta the half-angle. 25 degrees -> 21.1. Pass coneHalfDeg <= 0 or >= 180
// for 1.0 (no shaping, no gain).
float rhyBeamConcentrationGain(float coneHalfDeg);

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

// Is this corona record worth looking at at all? NaN/Inf positions and absurd
// radii come out of a half-written store entry, and a NaN position poisons every
// distance computed from it.
bool rhyCoronaPlausible(const RhyCorona& c);

// Head, tail, or neither.
//
// headTexture is the sprite POINTER the game resolved for index 4
// ("coronaheadlightline") - read from the game's own table at 0xC3E000+4*4, so
// this is an identity test against the game's data, not a guess. 0 = the reader
// could not resolve it, in which case no corona is called a headlamp.
//
// tail: the 2dFx rear lamps carry the model's own red. The test is hue, not
// index, because the front and rear lamps of a vehicle share sprite index 1
// ("coronastar") in some models and the same sprite is used by street effects.
// A rear lamp's observed colour is red-dominant by a wide margin (the game's own
// 2dFx red is around (200,20,20)); the thresholds below are that margin, and
// they deliberately do NOT match the bluish-white headlamp colour
// (0xa0,0xa0,0x8c / 0x96,0xc3,0xc3) the vehicle sites push.
int rhyVehicleClassifyCorona(const RhyCorona& c, uint32_t headTexture);

// True when the observed red of a rear lamp is the BRAKE filament rather than the
// position filament. The game's 2dFx path pushes the entry's colour per state
// (0x53B710 reads the three bytes at 0x8CCFF0..0x8CCFF2 that the vehicle code
// writes for the current light state), so a brighter red is the only brake
// signal the corona store carries. redMin <= 0 disables the rule.
bool rhyVehicleIsBrake(const RhyCorona& c, int redMin);

// ---------------------------------------------------------------------------
// Pairing: coronas -> lamps of one vehicle
// ---------------------------------------------------------------------------

struct RhyVehicleTuning {
    uint32_t headTexture;   // resolved sprite pointer, 0 = unknown
    float maxDistance;      // metres from the player: beyond this, no light
    float headFlux;         // flux units (see rhyLuminaireFlux)
    float tailFlux;         // ... rear position lamp
    float brakeFlux;        // ... rear brake lamp (>= tailFlux)
    float headRadius;       // metres; <= 0 -> rhyVehicleHeadFixture()
    float tailRadius;       // metres; <= 0 -> rhyVehicleTailFixture()
    float headConeDeg;      // HALF angle; <= 0 -> the fixture's
    float headConeSoftness; // 0..1
    float radianceScale;    // the [vehiclelights] radiance_scale (n23: not the
                            // street-lamp one - the two layers are tuned apart)
    int brakeRedMin;        // observed tail red for the brake rule (0 = off)
    int maxLamps;           // cap on lamps this pass (nearest vehicle first)
    float maxSpread;        // metres: the lamps of one vehicle must fit in a box
                            // this wide, or the "vehicle" is really two objects
};

// One lamp this pass: everything vehiclecorona.cpp needs to fill a
// remixapi_LightInfo. `hash` is stable for one lamp of one vehicle, which is
// what makes the diff (create / keep / destroy) possible at all.
struct RhyVehicleLamp {
    int kind;              // RhyVehicleLampKind
    uint32_t entity;       // the vehicle this lamp belongs to (key & ~3)
    int slot;              // which lamp of that vehicle, 0-based
    float pos[3];
    float dir[3];          // headlamps only: the beam direction, unit length
    float radius;
    float coneDeg;         // HALF angle, 0 = unshaped
    float coneSoftness;
    float radiance[3];     // linear, already scaled
    float flux;            // what the radiance/radius pair carries, for the log
    uint64_t hash;
};

// The lamps of every vehicle in `cor`, nearest-first, at most `cap`.
// `player` is the player position in game units (may be null: no distance gate).
// Returns how many lamps were written, and *outVehicles how many vehicles had
// at least one lamp.
//
// The rules, and why:
//   * Group by the store key: keys within 16 bytes of each other are the same
//     entity, because the game's key is the entity pointer plus the head index
//     (esi at 0x6E0CFF, esi+7 at 0x6E219A). `key & ~3` would split one car in
//     two, since the index reaches 7.
//   * A group is a vehicle only if its lamps fit inside maxSpread - the corona
//     store is shared with street 2dFx, and a red sign on a pole 3 m from a
//     headlamp corona would otherwise pair with it.
//   * The beam direction is head_centroid - tail_centroid. That is the vehicle's
//     own axis, from its own light dummies, which is what the task asks for; the
//     entity matrix's "at" column is the fallback when only one end of the car
//     is registered (the game skips the far end's coronas by its own distance
//     and facing tests).
//   * A headlamp with no direction at all is dropped rather than aimed at
//     nothing: an unshaped white sphere at a bumper is the "small weak pool"
//     complaint wearing a different hat.
int rhyVehicleBuildLamps(const RhyCorona* cor, unsigned nCoronas,
                         const float* player, const RhyVehicleTuning& t,
                         RhyVehicleLamp* out, int cap, int* outVehicles);

// A stable handle id for one lamp of one vehicle. Not the LightInfo hash space
// the lamp lists use (rhyremix.ini light_hash_base): vehicle lamps are transient
// by nature and must not collide with a street lamp's id.
uint64_t rhyVehicleLampHash(uint32_t entity, int kind, int slot);

#endif  // RHY_VEHICLELIGHTS_H
