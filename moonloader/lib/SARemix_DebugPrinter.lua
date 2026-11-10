-- Stand-in for the SARemix_DebugPrinter module missing from the GTASA-Remix repo: every method is a no-op.
local DebugPrinter = {}
local noop = function() end
function DebugPrinter:new()
    return setmetatable({}, { __index = function() return noop end })
end
return DebugPrinter
