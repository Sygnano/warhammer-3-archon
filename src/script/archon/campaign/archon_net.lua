-- Archon Mode v1.0.0 - single UITrigger dispatcher (+ GC guard).
--
-- Why one dispatcher: runs 3-5 (v0.3.0-0.3.2) crashed BOTH PCs inside the engine accessor behind context:trigger()
-- (Warhammer3.exe 0x14244ea30 -> null deref at +0x4d4bc0) while the listeners' conditions were being evaluated
-- for the first UITrigger processed at the turn-1 unpause. The crash point moved with the number of
-- context:trigger() reads made on that one message (vanilla's reads + ours): 0.3.0 four extra reads, 0.3.1 six,
-- 0.3.2 five. Vanilla alone stays under that limit (0.2.0 never crashed).
--
-- So, when Archon is active, ALL script-event traffic we care about goes through ONE listener that reads the
-- trigger text exactly once, then routes it in Lua:
--   archon_hello|...              roster (archon_session)
--   archon_left|...               partner left: new player count (archon_session, v0.4.0)
--   archon_poac|...               machine-counting barrier (archon_barrier)
--   archon_endturn|...            end-turn votes inside a shared faction (archon_endturn, v0.7.0)
--   progress_on_all_clients_...   vanilla barrier monitor, ported (should not occur while Archon is active)
--   mpq:...                       vanilla progress_on_mp_query answers, ported with the duplicate-answer guard
-- The two vanilla listeners for the last two ("progress_on_all_clients_ui_trigger", "mp_query_listener") are
-- removed, so per message the game now reads the text FEWER times than vanilla does (vanilla: up to 4).
-- When Archon is NOT active nothing is installed and the game stays exactly vanilla.

local A = archon
local N = { routes = {}, installed = false, queue = nil, received = 0 }
A.net = N

local LOG = function(msg) A.log("net", msg) end

-- register a route: fn(trigger_text, faction_cqi) runs in the listener callback (same model tick everywhere)
function N.on(prefix, name, fn)
  table.insert(N.routes, { prefix = prefix, name = name, fn = fn })
end

local function route_for(t)
  for _, r in ipairs(N.routes) do
    if string.sub(t, 1, #r.prefix) == r.prefix then return r end
  end
  return nil
end

local function count_listeners(event, name)
  local n = 0
  local l = core.event_listeners and core.event_listeners[event]
  if l then for i = 1, #l do if l[i].name == name then n = n + 1 end end end
  return n
end

-- GC guard (v0.3.4). Every crash so far hit context:trigger() after heavy Lua allocation inside the same
-- UITrigger dispatch (trace logging in conditions in runs 3-5, a script_error traceback in run 6), while vanilla
-- reads the same text ~13 times per message without trouble. Working hypothesis: a garbage-collection pass during
-- the dispatch frees something the engine's event-text object still relies on. So the collector is paused from
-- our condition until the dispatch has returned, and resumed by a real-time callback (UI side, local only: GC
-- timing never affects the synced model, so this cannot desync).
N.gc = { held = false, holds = 0 }
local function gc_hold()
  if N.gc.held then return end
  local ok = pcall(collectgarbage, "stop")
  if not ok then return end
  N.gc.held = true
  N.gc.holds = N.gc.holds + 1
  if N.gc.holds == 1 then A.log("net", "GC guard engaged for the first time (collector paused during UITrigger dispatch)") end
  local resumed = pcall(function()
    cm:real_callback(function()
      pcall(collectgarbage, "restart")
      N.gc.held = false
    end, 100)
  end)
  if not resumed then pcall(collectgarbage, "restart"); N.gc.held = false end
end

function N.install()
  if N.installed then return end
  N.installed = true
  local before_p = count_listeners("UITrigger", "progress_on_all_clients_ui_trigger")
  local before_q = count_listeners("UITrigger", "mp_query_listener")
  -- take over a vanilla listener only if a route handles its messages (i.e. archon_barrier loaded)
  local function has_route(prefix)
    for _, r in ipairs(N.routes) do if r.prefix == prefix then return true end end
    return false
  end
  if has_route("progress_on_all_clients_") then core:remove_listener("progress_on_all_clients_ui_trigger") end
  if has_route("mpq:") then core:remove_listener("mp_query_listener") end

  -- Deliberately NOT A.listen: no extra wrapping. The condition is the only place the text is read.
  core:add_listener(
    "archon_uitrigger_dispatch",
    "UITrigger",
    function(context)
      gc_hold()
      local ok, t = pcall(function() return context:trigger() end)
      if not ok or type(t) ~= "string" then return false end
      local r = route_for(t)
      if not r then return false end
      local okc, cqi = pcall(function() return context:faction_cqi() end)
      N.queue = N.queue or {}
      table.insert(N.queue, { t = t, cqi = okc and cqi or 0, r = r })
      return true
    end,
    function()
      local q = N.queue
      N.queue = nil
      if not q then return end
      for _, m in ipairs(q) do
        N.received = N.received + 1
        A.log("net", "RECV #" .. N.received .. " " .. m.r.name .. " cqi=" .. tostring(m.cqi) .. " " .. m.t)
        local ok, err = pcall(m.r.fn, m.t, m.cqi)
        if not ok then A.log("ERROR", "net route " .. m.r.name .. ": " .. tostring(err)) end
      end
    end,
    true
  )
  LOG("installed single UITrigger dispatcher (" .. #N.routes .. " routes); removed vanilla listeners "
    .. "progress_on_all_clients_ui_trigger=" .. before_p .. "->" .. count_listeners("UITrigger", "progress_on_all_clients_ui_trigger")
    .. " mp_query_listener=" .. before_q .. "->" .. count_listeners("UITrigger", "mp_query_listener"))
end

-- install at pre-first-tick, only when Archon is active (before any queued command is executed at unpause)
A.on_pre_first_tick("net install", function()
  if A.is_active and A.is_active() then
    N.install()
  else
    LOG("Archon inactive: dispatcher not installed, vanilla UITrigger listeners untouched")
  end
end)
