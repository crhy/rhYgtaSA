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

-- s31 (finding 7): stop 12 needs a car in frame and SA does not park one there, so the tour
-- spawns it with MoonLoader's createCar (a CAR opcode, 00E0). The opcodes that crash under
-- MoonLoader 0.27 in this RTX/Linux build are the PED ones - 00A0/00A1/00DF/03C0, see
-- moonloader/lib/sa_ped_safe.lua - so no ped is ever created or read here, and stop 13 works by
-- standing where the game puts pedestrians instead. Every spawn call is wrapped in pcall and the
-- car is deleted after the shot: a stop whose car fails to spawn still takes its picture and says
-- so in the manifest.
local CAR_SADLER = 543      -- data/vehicles.ide: "543, sadler, ..." - the model parked in front of
                            -- the Grove Street house in the opening of the game (docs/MIRRORS.md:
                            -- 179 of 209 stock cars carry the xvehicleenv128 strip the mirror samples)

-- name, hour, minute, player xyz, camera xyz, look-at xyz [, {car={model,x,y,z,heading}, settle=ms}]
local stops = {
  {"00-baseline-noon-grove",       13, 0,  2471.7, -1678.0, 13.5,  2440.0, -1672.0, 16.0,  2500.0, -1668.0, 13.5},
  {"01-under-lamp-grove-2100",     21, 0,  2471.7, -1677.0, 13.5,  2465.5, -1682.0, 16.5,  2471.7, -1673.7, 13.5},
  {"02-grove-30m-pools-2100",      21, 0,  2440.0, -1670.0, 13.5,  2438.0, -1671.0, 15.5,  2500.0, -1668.0, 13.5},
  {"03-sodium-lamp-closeup-2100",  21, 0,  2471.7, -1677.0, 13.5,  2467.0, -1679.0, 18.5,  2471.7, -1673.7, 19.8},
  {"04-tungsten-shopfront-2100",   21, 0,  2302.0, -1648.0, 14.8,  2300.0, -1652.0, 15.8,  2309.8, -1643.5, 14.8},
  -- s31: the old camera (798,-1628,15.5) was 7 m east of the origin of burggrnd1_law,
  -- (790.94,-1628.32,12.62) in law_stream3.ipl - i.e. inside the Burger Shot building. Its west
  -- kerb is the lamppost1 pair (780.72,-1626.70 / -1610.52) in law_stream2.ipl, and every
  -- cj_traffic_light4 in the block sits at z 12.50-12.54 (law_stream2/3.ipl) with the road
  -- pieces lawroads_law14/18 at 12.46/13.52, so the walking surface here is z ~12.5. CJ now
  -- stands on that kerb and the camera is 3 m west of it in the empty carriageway, 15.7 m from
  -- the building front and clear of it (nearest building origin 15.7 m, no wall within 12 m).
  {"05-burger-shot-marina-2100",   21, 0,   779.0, -1623.5, 12.5,   776.0, -1623.5, 14.0,   795.0, -1628.3, 15.0},
  {"06-signals-close-2100",        21, 0,  1040.0, -1392.0, 13.4,  1036.0, -1394.0, 16.0,  1061.6, -1416.1, 15.0},
  {"07-signals-cross-street-2100", 21, 0,  1068.0, -1428.0, 13.4,  1070.0, -1432.0, 16.0,  1052.8, -1385.5, 15.0},
  {"08a-sunset-facing-east-1905",  19, 5,   370.0, -2040.0,  7.8,   368.0, -2042.0, 10.0,   600.0, -2000.0, 30.0},
  {"08b-sunset-facing-west-1905",  19, 5,   370.0, -2040.0,  7.8,   372.0, -2040.0, 10.0,   100.0, -2060.0, 15.0},
  {"09-grove-dawn-0530",            5, 30, 2440.0, -1670.0, 13.5,  2438.0, -1671.0, 15.5,  2500.0, -1668.0, 13.5},
  -- s31: (305,-1925,6) was out on the wet sand and dropped CJ under water (shot 11 was black).
  -- The Santa Maria pier is solid and provable from the data: parkbench1 runs along it at
  -- (378.46, y -1874.6..-2083.3, z 7.21), with streetlamp1 at x 364.6/374.8 (z 9.50, i.e. 2.3 m
  -- above the deck - a lamp origin is always above its ground: lamppost3 reads 16.13 over the
  -- 12.91 pavement at Grove Street), a cj_hippo_bin at (387.1,-1831.4,7.6), the chillidogcart
  -- at (388.9,-2071.7,7.9) and the pier sections pier02..pier04b_law2 under it (all law2_stream3.ipl).
  -- The sunset stops already stand on that deck at (370,-2040,7.8) and came out well. CJ now stands
  -- at the bench (378.46,-1963.88, z 7.21) and the camera looks due west, 1.6 deg down, over sand
  -- and sea: nothing at all is in that cone out to 120 m, and the San Pedro lighthouse
  -- (sanpedlithus_law2, (154.2,-1950.2)) marks the coast straight ahead.
  {"10-water-santa-maria-noon",    13, 0,   378.5, -1963.9,  7.3,   376.0, -1963.9,  8.4,   150.0, -1963.9,  2.0},
  {"11-water-santa-maria-2100",    21, 0,   378.5, -1963.9,  7.3,   376.0, -1963.9,  8.4,   150.0, -1963.9,  2.0},
  -- Three subjects the pack's own material work targets and the list above had no shot for
  -- (added in s15). Positions come from the game's IPL data (see .strata/notes-s15-test-session.md):
  -- the road in front of CJ's house is lae2_roads89 at (2489.3, -1668.5), Pershing Square is
  -- the plaza of pershingsq1/pershingpool_lan at (1479.6, -1631..-1693) with its parkbench1
  -- ring at z 13.5, and the Vinewood sign (VineSign1_cunte, txd group "mulhouslahills") stands
  -- at (1413.4, -804.7, 83.4) with a sm_bevhiltree/pinetree grove at z 90 just north of it.
  -- Rear-view mirror at Grove Street. SA has no mirrors, it has paint: the glass is triangles
  -- inside the body atomic, textured with a pre-baked strip (docs/MIRRORS.md) - under Remix a
  -- smooth metallic material on that hash reflects the road that is really behind CJ. CJ does
  -- not have to be in a car: the camera sits a few metres in front of the driver's door mirror and looks back
  -- along the flank. NOTE: SA ships no car-generator IPL sections (the LA text IPLs only have
  -- inst/cull/path/grge/enex/pick/jump/tcyc/auzo/mult and every bnry IPL in gta3.img is instance-only), so the
  -- parked cars of a street come from the compiled main script and cannot be read out of the data files -
  -- which is why this stop brings its own car (s31, below).
  -- s31: the road in front of the house is lae2_roads89 at (2489.30,-1668.50,12.30)
  -- (lae2_stream0.ipl); the street runs east-west between the front-yard fences dyn_f_r_wood_1b
  -- at y ~-1688 and the north pavement, whose fire_hydrants sit at z 12.91/12.92. A sadler is
  -- spawned in the parking lane at (2491.0,-1677.0) facing east (heading 90), so its LEFT side -
  -- the driver's door and its mirror, SA is LHD - faces the camera at (2495.5,-1674.5,14.0),
  -- 3 m in front of the mirror and looking back along the flank; CJ stands by that door.
  {"12-car-mirror-grove-1300",     13, 0,  2491.0, -1674.5, 12.5,  2495.5, -1674.5, 14.0,  2487.0, -1676.5, 13.3,
   { car = { CAR_SADLER, 2491.0, -1677.0, 12.6, 90 } }},
  -- Pedestrian close-up. Pershing Square at noon is the one downtown plaza whose bench ring
  -- and palm walkways (18 parkbench1 + veg_palmkb1 in lan_stream0.ipl) keep peds crossing in
  -- front of a low camera; the fountain at (1479.6, -1639.6) is the backdrop.
  -- s31: shot 15 had no peds in it. Two changes, both from the data. (a) The camera no longer
  -- looks across the empty middle of the plaza: CJ stands on the north-south walkway and the
  -- camera looks along it, so the frame is the bench-lined path peds walk - parkbench1 at
  -- (1479.47,-1626.02 / 1472.90,-1651.51 / 1486.41,-1651.39, z 13.45), the pool/fountain
  -- pershingpool_lan at 14 m, streetlamp1 at (1465.89,-1629.98)/(1494.41,-1629.98) and a bus
  -- stop bussign1 at (1466.48,-1598.09) on the street north of it: 20 instances in the cone, all
  -- of them pedestrian furniture. (b) Peds are generated around the player over tens of seconds,
  -- not in the 12 s the other stops wait, so this stop settles for 25 s. No ped is ever created:
  -- create_char is one of the opcodes that crash in this build.
  {"13-ped-crowd-pershing-noon",   12, 0,  1479.6, -1650.0, 13.5,  1479.0, -1653.5, 14.4,  1479.5, -1626.0, 13.6,
   { settle = 25000 }},
  -- Vegetation. The s03 hang was in the vegetation streaming, and the hills are where the
  -- game packs it: Mulholland climbs the Vinewood hillside here (the streetlamp1 line from
  -- (1313,-836,61) to (1381,-833,72)) and the grove north of the sign is five sm_bevhiltree
  -- plus pinetree01/05 at z 90-93. Noon, clear weather, so the leaves are lit from the front.
  -- s31: that spot put the grove 70-80 m away and no grass in the frame, so the shot showed only
  -- hillside cards. The search over every LA instance (build/tmp/s31-check.py, same IPL index as
  -- tools/lamp_list.py) scores (1300,-920) in lawn_stream3.ipl as the densest green spot in the
  -- hills: 20 tree instances and 8 lawn/grass instances within 45 m, nearest building 23 m.
  -- Ground z there is 41.9 (veg_treeb1 origins) to 42.4 (fire_hydrant at (1266.09,-906.70));
  -- the camera looks 40 deg east of north over the lawn (foodmartlawn (1315.37,-887.47,41.70),
  -- lawngrndasas (1323.66,-884.63,36.25)) into the sm_bevhiltree / veg_treeb1 line at 8-35 m.
  {"14-vegetation-vinewood-noon",  12, 0,  1298.0,  -914.0, 41.5,  1296.0,  -916.0, 42.5,  1330.0,  -888.0, 41.0},
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

-- Spawn (and later delete) the parked car a stop needs. pcall everywhere: if MoonLoader cannot
-- create the model the stop still runs, it just says so in the manifest.
local function spawnCar(spec)
  local ok, car = pcall(function()
    local c = createCar(spec[1], spec[2], spec[3], spec[4])
    if c and c ~= 0 then
      pcall(setCarHeading, c, spec[5])
      pcall(setCarCoordinates, c, spec[2], spec[3], spec[4])   -- snap it to the kerb we asked for
    end
    return c
  end)
  if not ok then note("spawn car failed: " .. tostring(car)); return nil end
  if not car or car == 0 then note("spawn car returned no handle"); return nil end
  return car
end

local function dropCar(car)
  if car then pcall(deleteCar, car) end
end

local function restore()
  pcall(restoreCameraJumpcut); pcall(setCameraBehindPlayer)
  pcall(displayHud, true); pcall(displayRadar, true)
  pcall(setPlayerControl, PLAYER_HANDLE, true)
end

local function runStop(s)
  local name, h, m = s[1], s[2], s[3]
  local opt = s[13] or {}
  forceWeatherNow(1)
  setTimeOfDay(h, m)
  requestCollision(s[4], s[5]); loadScene(s[4], s[5], s[6])
  -- 1 m of headroom, never 0.5: the ground z of a stop comes from the game's own instances
  -- (road pieces, benches, hydrants), and CJ falling the last metre is harmless while CJ
  -- starting below the surface is not (that is how the 2026-11-12 water stop went under).
  teleport(s[4], s[5], s[6] + 1.0)
  setFixedCameraPosition(s[7], s[8], s[9], 0.0, 0.0, 0.0)
  pointCameraAtPoint(s[10], s[11], s[12], 2)
  local car = opt.car and spawnCar(opt.car) or nil   -- before the wait: the model needs time to load
  wait(opt.settle or SETTLE_MS)
  setTimeOfDay(h, m)          -- the clock ran during the wait; pin it again
  wait(GAP_MS)
  shot(name)
  wait(GAP_MS)
  dropCar(car)
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
