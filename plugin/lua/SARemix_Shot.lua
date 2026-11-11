-- SARemix_Shot - the in-game half of the screenshot hotkey (F12 by default).
--
-- The game's own PrintScreen path goes through GDI + D3D9 and takes the Remix
-- bridge's server process down (see docs/FINDINGS.md), so this script never
-- touches D3D9. It only drops a marker file into
-- <game>/screenshots/pending/, and scripts/shot-watch.sh - started by
-- launcher/launch-rtx.sh - turns each marker into a grab of the composited
-- window (post-tonemap, which is the image you actually want when judging
-- brightness or whether a lamp casts).
--
-- This is the one Lua script the project keeps: MoonLoader is the only thing
-- that knows the game's key state, and reading it from native code would mean
-- hardcoding a game address. Everything light/sun related stays in rhyremix.asi.

script_name("SARemix_Shot")
script_author("rhY")
script_version("0.1.0")

local work = getWorkingDirectory()
local confPath = work .. "/moonloader/SARemix_Shot.conf"
local pendingDir = work .. "/screenshots/pending"
local fallback = work .. "/scripts/rhyshot.req"   -- used if pending/ is missing

local enabled = 1
local key = 0x7B          -- VK_F12 (see moonloader/SARemix_Shot.conf)
local cooldown = 0.4      -- seconds between two triggers

local function readConf()
  local f = io.open(confPath, "r")
  if not f then return end
  for line in f:lines() do
    local k, v = line:match("^%s*([%w_]+)%s*=%s*([%w_.%-]+)")
    if k == "enabled" then enabled = tonumber(v) or enabled
    elseif k == "key" then key = tonumber(v) or key
    elseif k == "cooldown" then cooldown = tonumber(v) or cooldown
    end
  end
  f:close()
end

local function requestShot(seq)
  local f = io.open(string.format("%s/rhyshot_%d.req", pendingDir, seq), "w")
  if not f then f = io.open(fallback, "w") end
  if not f then return false end
  f:write("shot\n")
  io.close(f)
  return true
end

function main()
  readConf()
  if enabled ~= 1 then
    print("[SARemix_Shot] disabled by " .. confPath)
    return
  end
  if type(isPs2KeyboardKeyJustPressed) ~= "function" then
    print("[SARemix_Shot] this MoonLoader has no key state API; hotkey unavailable")
    return
  end
  print(string.format("[SARemix_Shot] hotkey on, key=%d cooldown=%.2fs", math.floor(key), cooldown))
  local last = -1
  local seq = 0
  local warned = false
  while true do
    wait(0)
    local ok, down = pcall(isPs2KeyboardKeyJustPressed, key)
    if not ok then
      if not warned then
        print("[SARemix_Shot] key query failed: " .. tostring(down))
        warned = true
      end
    elseif down then
      local now = os.clock()
      if now - last >= cooldown then
        last = now
        seq = seq + 1
        if requestShot(seq) then
          print(string.format("[SARemix_Shot] marker %d requested", math.floor(seq)))
        elseif not warned then
          print("[SARemix_Shot] cannot write a marker; is screenshots/pending there?")
          warned = true
        end
      end
    end
  end
end
