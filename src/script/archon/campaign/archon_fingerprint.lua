-- Archon Mode v1.0.0 - log-only sync fingerprint probe (desync forensics).
--
-- Only while Archon is active (0.6.0: costs nothing otherwise: the listener conditions return false before
-- touching the event context). At the first tick, at each FactionTurnStart of a human faction and at each WorldStartRound, every machine
-- writes one line to archon_mode_log.txt:
--   FP|<point>|turn=<n>|<human>:t=<treasury>,r=<regions>,c=<characters>,f=<forces>,m=<active missions>,pr=<hash>|...
--      |all=<hash of every faction's treasury>|nf=<faction count>|sv=<cm.saved_values key count>
-- The probe only reads the model: no cm:random_number, no model writes, no commands. Lines with the same point
-- and turn must be identical on all machines; the first differing line brackets a divergence far tighter than
-- the engine's sparse sync check. sv= differs when script state diverged (the H1
-- symptom: one machine ran a chain the other did not).

local A = archon
local F = {}
A.fingerprint = F

local hash8 = A.util.hash8

local function num(x)
  if type(x) ~= "number" then return tostring(x) end
  return string.format("%d", math.floor(x + 0.5))
end

local function try(fn, default)
  local ok, v = pcall(fn)
  if ok then return v end
  return default
end

local function pooled_hash(faction)
  return try(function()
    local list = faction:pooled_resource_manager():resources()
    local parts = {}
    for i = 0, list:num_items() - 1 do
      local r = list:item_at(i)
      table.insert(parts, r:key() .. "=" .. num(r:value()))
    end
    table.sort(parts)
    return #parts .. ":" .. hash8(table.concat(parts, ";"))
  end, "na")
end

local function faction_part(f)
  return f:name() .. ":t=" .. num(try(function() return f:treasury() end, "na"))
    .. ",r=" .. num(try(function() return f:region_list():num_items() end, "na"))
    .. ",c=" .. num(try(function() return f:character_list():num_items() end, "na"))
    .. ",f=" .. num(try(function() return f:military_force_list():num_items() end, "na"))
    .. ",m=" .. num(try(function() return f:active_missions():num_items() end, "na"))
    .. ",pr=" .. pooled_hash(f)
end

function F.line(point)
  local model = cm:model()
  local fl = model:world():faction_list()
  local humans, all = {}, {}
  for i = 0, fl:num_items() - 1 do
    local f = fl:item_at(i)
    table.insert(all, num(try(function() return f:treasury() end, 0)))
    if f:is_human() then table.insert(humans, f) end
  end
  table.sort(humans, function(a, b) return a:name() < b:name() end)
  local hp = {}
  for _, f in ipairs(humans) do table.insert(hp, faction_part(f)) end
  local sv = 0
  for _ in pairs(cm.saved_values or {}) do sv = sv + 1 end
  return "FP|" .. point .. "|turn=" .. model:turn_number() .. "|" .. table.concat(hp, "|")
    .. "|all=" .. hash8(table.concat(all, ",")) .. "|nf=" .. fl:num_items() .. "|sv=" .. sv
end

function F.log(point)
  A.log("fp", F.line(point))
end

local function active() return A.is_active ~= nil and A.is_active() end

A.listen("archon_fp_faction_turn_start", "FactionTurnStart",
  function(context) return active() and context:faction():is_human() end,
  function(context) F.log("FactionTurnStart:" .. context:faction():name()) end, true)

A.listen("archon_fp_world_start_round", "WorldStartRound", active,
  function() F.log("WorldStartRound") end, true)

A.on_first_tick("fingerprint", function() if active() then F.log("FirstTick") end end)
