#ifndef RHY_GAME_H_
#define RHY_GAME_H_

// Player position in GTA SA game units.  CPed* at 0xB7CD98; CMatrix* at
// ped+0x14 with the position at matrix+0x30, otherwise ped+0x4.  Returns false
// before the player object exists (loading screen).
bool rhyPlayerPos(float* out);

// Game clock hour, 0..23 (0xB70153).  The same source the lamp list was built
// from, so "which lamps are on" and "is it night" cannot disagree.
int rhyGameHour();

#endif  // RHY_GAME_H_
