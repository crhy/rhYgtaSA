#include "game.h"

#include <cstdint>

bool rhyPlayerPos(float* out) {
    uintptr_t ped = *reinterpret_cast<uintptr_t*>(0xB7CD98);
    if (!ped) {
        return false;
    }
    uintptr_t m = *reinterpret_cast<uintptr_t*>(ped + 0x14);
    const float* p = m ? reinterpret_cast<const float*>(m + 0x30)
                       : reinterpret_cast<const float*>(ped + 0x4);
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    return true;
}

int rhyGameHour() {
    return static_cast<int>(*reinterpret_cast<const uint8_t*>(0xB70153));
}
