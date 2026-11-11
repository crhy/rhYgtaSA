-- SARemix_Tour - an in-game photo tour for the lighting tests in docs/TESTING.md.
--
-- Runs only when <game>/screenshots/tour.req exists (delete it to never run).
-- For each stop it moves CJ, sets the clock and clear weather, puts a fixed
-- camera on the subject, waits for streaming and the Remix denoiser to settle,
-- then drops the same marker file F12 does, so scripts/shot-watch.sh grabs the
-- composited window. The order and names of the shots go to
-- screenshots/tour-manifest.txt. Player control, camera and HUD are restored
-- at the end, and tour.req is removed so the tour runs once.

script_name("SARemix_Tour")
script_author("rhY")
script_version("0.1.0")

-- getWorkingDirectory() is <game>\moonloader, not the game folder: go one up.
local work = getWorkingDirectory() .. "/.."
local flag = work .. "/screenshots/tour.req"
local pendingDir = work .. "/screenshots/pending"
local manifest = work .. "/screenshots/tour-manifest.txt"

local SETTLE_MS = 12000   -- streaming + denoiser after a teleport / time change
local GAP_MS = 2500       -- between two shots at the same stop

-- name, hour, minute, player xyz, camera xyz, look-at xyz
local stops = {
  {"00-baseline-noon-grove",       13, 0,  2471.7, -1678.0, 13.5,  2440.0, -1672.0, 16.0,  2500.0, -1668.0, 13.5},
  {"01-under-lamp-grove-2100",     21, 0,  2471.7, -1677.0, 13.5,  2465.5, -1682.0, 16.5,  2471.7, -1673.7, 13.5},
  {"02-grove-30m-pools-2100",      21, 0,  2440.0, -1670.0, 13.5,  2438.0, -1671.0, 15.5,  2500.0, -1668.0, 13.5},
  {"03-sodium-lamp-closeup-2100",  21, 0,  2471.7, -1677.0, 13.5,  2467.0, -1679.0, 18.5,  2471.7, -1673.7, 19.8},
  {"04-tungsten-shopfront-2100",   21, 0,  2302.0, -1648.0, 14.8,  2300.0, -1652.0, 15.8,  2309.8, -1643.5, 14.8},
  {"05-burger-shot-marina-2100",   21, 0,   805.0, -1622.0, 13.5,   798.0, -1628.0, 15.5,   811.0, -1616.0, 14.5},
  {"06-signals-close-2100",        21, 0,  1040.0, -1392.0, 13.4,  1036.0, -1394.0, 16.0,  1061.6, -1416.1, 15.0},
  {"07-signals-cross-street-2100", 21, 0,  1068.0, -1428.0, 13.4,  1070.0, -1432.0, 16.0,  1052.8, -1385.5, 15.0},
  {"08a-sunset-facing-east-1905",  19, 5,   370.0, -2040.0,  7.8,   368.0, -2042.0, 10.0,   600.0, -2000.0, 30.0},
  {"08b-sunset-facing-west-1905",  19, 5,   370.0, -2040.0,  7.8,   372.0, -2040.0, 10.0,   100.0, -2060.0, 15.0},
  {"09-grove-dawn-0530",            5, 30, 2440.0, -1670.0, 13.5,  2438.0, -1671.0, 15.5,  2500.0, -1668.0, 13.5},
  {"10-water-santa-maria-noon",    13, 0,   305.0, -1925.0,  6.0,   300.0, -1932.0,  5.0,   200.0, -2060.0,  0.0},
  {"11-water-santa-maria-2100",    21, 0,   305.0, -1925.0,  6.0,   300.0, -1932.0,  5.0,   200.0, -2060.0,  0.0},
}

local function exists(p) local f = io.open(p, "r"); if f then f:close(); return true end; return false end

local function note(line)
  local f = io.open(manifest, "a")
  if f then f:write(os.date("%Y-%m-%d %H:%M:%S") .. "  " .. line .. "\n"); f:close() end
  print("[SARemix_Tour] " .. line)
end

local seq = 0
local function shot(name)
  seq = seq + 1
  local f = io.open(string.format("%s/rhytour_%02d.req", pendingDir, seq), "w")
  if f then f:write(name .. "\n"); f:close(); note(string.format("shot %02d %s", seq, name))
  else note("cannot write marker for " .. name) end
end

-- Ped opcodes (00A0, 00A1, ...) raise an unhandled exception under MoonLoader
-- 0.27 in this RTX/Linux build (see moonloader/lib/sa_ped_safe.lua), so CJ is
-- moved by writing his matrix position directly. GTA SA 1.0 US offsets.
local memory = require "memory"
local PLAYER_PED_PTR = 0xB7CD98   -- CWorld::Players[0].m_pPed

local function teleport(x, y, z)
  local p = memory.getuint32(PLAYER_PED_PTR)
  if p == 0 then error("no player ped") end
  local m = memory.getuint32(p + 0x14)          -- CPlaceable::m_matrix
  if m ~= 0 then
    memory.setfloat(m + 0x30, x); memory.setfloat(m + 0x34, y); memory.setfloat(m + 0x38, z)
  else
    memory.setfloat(p + 0x4, x); memory.setfloat(p + 0x8, y); memory.setfloat(p + 0xC, z)
  end
  memory.setfloat(p + 0x44, 0.0); memory.setfloat(p + 0x48, 0.0); memory.setfloat(p + 0x4C, 0.0)  -- m_vecMoveSpeed
end

local function restore()
  pcall(restoreCameraJumpcut); pcall(setCameraBehindPlayer)
  pcall(displayHud, true); pcall(displayRadar, true)
  pcall(setPlayerControl, PLAYER_HANDLE, true)
end

local function runStop(s)
  local name, h, m = s[1], s[2], s[3]
  forceWeatherNow(1)
  setTimeOfDay(h, m)
  requestCollision(s[4], s[5]); loadScene(s[4], s[5], s[6])
  teleport(s[4], s[5], s[6] + 0.5)
  setFixedCameraPosition(s[7], s[8], s[9], 0.0, 0.0, 0.0)
  pointCameraAtPoint(s[10], s[11], s[12], 2)
  wait(SETTLE_MS)
  setTimeOfDay(h, m)          -- the clock ran during the wait; pin it again
  wait(GAP_MS)
  shot(name)
  wait(GAP_MS)
end

function main()
  if not exists(flag) then return end
  while not isPlayerPlaying(PLAYER_HANDLE) do wait(500) end
  -- Teleporting out of an interior (CJ's house, a shop) crashed opcode 00A1 on
  -- 2026-11-11, so the tour waits until CJ is outdoors.
  local told = false
  while getActiveInterior() ~= 0 do
    if not told then note("waiting: step outside to start the tour"); told = true end
    wait(1000)
  end
  wait(5000)
  note("tour start, " .. #stops .. " stops")
  setPlayerControl(PLAYER_HANDLE, false)
  displayHud(false); displayRadar(false)
  for _, s in ipairs(stops) do
    if not isPlayerPlaying(PLAYER_HANDLE) then note("player not playing; tour stopped"); break end
    local ok, err = pcall(runStop, s)
    if not ok then note("stop " .. s[1] .. " failed: " .. tostring(err)); break end
  end
  restore()                   -- always: control, HUD and camera come back
  os.remove(flag)
  note("tour done")
end
