-- sa_ped_safe: memory-based replacements for ped opcodes that crash under MoonLoader 0.27 in the RTX/Linux build
-- (00A0 getCharCoordinates, 00DF isCharInAnyCar, 03C0 storeCarCharIsInNoSave). GTA SA 1.0 US offsets.
local memory = require 'memory'
local M = {}

local PLAYER_PED_PTR = 0xB7CD98 -- CWorld::Players[0].m_pPed

local function pedPtr(ped)
    local ok, p = pcall(getCharPointer, ped)
    if ok and p and p ~= 0 then return p end
    return memory.getuint32(PLAYER_PED_PTR)
end

function M.coords(ped)
    local p = pedPtr(ped)
    if p == 0 then return 0.0, 0.0, 0.0 end
    local m = memory.getuint32(p + 0x14) -- CPlaceable::m_matrix
    if m ~= 0 then
        return memory.getfloat(m + 0x30), memory.getfloat(m + 0x34), memory.getfloat(m + 0x38)
    end
    return memory.getfloat(p + 0x4), memory.getfloat(p + 0x8), memory.getfloat(p + 0xC)
end

function M.inAnyCar(ped)
    local p = pedPtr(ped)
    if p == 0 then return false end
    return memory.getuint32(p + 0x58C) ~= 0 and bit.band(memory.getuint32(p + 0x46C), 0x100) ~= 0
end

function M.carOf(ped)
    local p = pedPtr(ped)
    if p == 0 then return nil end
    local v = memory.getuint32(p + 0x58C) -- CPed::m_pVehicle
    if v == 0 then return nil end
    return getVehiclePointerHandle(v)
end

function M.install()
    getCharCoordinates = M.coords
    isCharInAnyCar = M.inAnyCar
    storeCarCharIsInNoSave = M.carOf
    print("sa_ped_safe active: ped opcodes replaced with memory reads")
end

return M
