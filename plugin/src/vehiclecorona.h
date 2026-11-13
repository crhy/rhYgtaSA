#ifndef RHY_VEHICLECORONA_H
#define RHY_VEHICLECORONA_H

// ---------------------------------------------------------------------------
// n12: the game-facing half of the vehicle lights.
//
// vehiclelights.h/.cpp decide what a car's lamps are; this file reads them out
// of the game's corona store and turns them into Remix lights. It does NOT patch
// any game code: the store at 0xC3E058 is written by the game itself every frame
// and is read here, page-validated, the same way signalhook.cpp validates every
// pointer it follows. A read of committed .bss cannot fault, and a read of a
// vehicle pointer can, so the entity matrix read behind RhyCorona::normal goes
// through the same page cache the signal hook uses.
// ---------------------------------------------------------------------------

#include "vehiclelights.h"

// One-time sanity check of the store's layout, logged once. Called from
// hkPresent next to the other InitOnce calls.
void rhyVehicleLightsInitOnce();

// Read the game's live coronas into `out` (at most outCap). Returns how many
// were written; *outTotal is the count the game itself reports (which may be
// larger than outCap). Reasons a corona is skipped are counted, not logged: the
// once-a-second line prints them.
unsigned rhyVehicleCoronaRead(RhyCorona* out, unsigned outCap, unsigned* outTotal);

// The sprite pointer the game resolved for corona sprite index 4
// ("coronaheadlightline"), i.e. the value the game stores at 0xC3E010. 0 when the
// table is not readable or the entry is null - in which case no corona is
// classified as a headlamp and the module lights only the rear lamps.
uint32_t rhyVehicleHeadTexture();

// One pass, called from hkPresent. Reads the coronas, builds the lamps, and
// keeps the Remix handles in step with them (create / keep / fade out / destroy).
void rhyVehicleCoronaFrame();

// Counters for the once-a-second log line.
unsigned rhyVehicleCoronaCount();     // coronas read on the last pass
unsigned rhyVehicleLampCount();       // lamps the last pass asked for
unsigned rhyVehicleLiveCount();       // handles alive right now
unsigned rhyVehicleVehicles();        // vehicles behind those lamps
unsigned rhyVehicleCreates();         // cumulative creates
unsigned rhyVehicleSkips();           // passes where the create budget was spent

#endif  // RHY_VEHICLECORONA_H
