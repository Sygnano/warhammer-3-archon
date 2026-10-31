-- Archon Mode v1.0.0 - machine-counting MP barrier and mp-query duplicate guard (+ recheck when a partner leaves).
--
-- Vanilla cm:progress_on_all_clients_ui_triggered (lib_campaign_manager.lua ~14454-14577) keys its record on
-- the sender's FACTION and completes at #cm:get_active_human_factions() factions. With two machines on one
-- faction that is 1: the first machine's trigger completes the barrier on every machine, the other machine has
-- no callback yet ("no progression callback ... Expect a desync") and keeps a stale record, so its own later
-- trigger is ignored. The callback (intro-finished chain, narrative missions, payloads) ran on one machine only.
--
-- When Archon is active this override is used instead:
--   * the trigger string is  archon_poac|<machine id>|<trigger name>  (does not match the vanilla monitor's
--     "progress_on_all_clients_" pattern, so the vanilla monitor never sees it);
--   * the record is keyed on machine id; completion needs archon.expected_machines() distinct ids;
--   * a machine sends its trigger only AFTER storing its callback, so when the last expected trigger is handled
--     (a UITrigger, processed at the same model tick on every machine) every machine already holds its
--     callback and runs it right there: same tick, every machine;
--   * if a record completes on a machine that has no callback (should not happen), it is kept as
--     "completed, pending" and consumed by that machine's registration; it is never left stale.
-- When Archon is not active the vanilla function is called unchanged.

local A = archon
local B = {}
A.barrier = B

local LOG = function(msg) A.log("barrier", msg) end
local PREFIX = "archon_poac|"

B.records = {}      -- trigger name -> { ids = {id=true}, count, callback, sent, pending }
B.stats = { called = 0, completed = 0, pending = 0, late = 0 }

local vanilla_progress = campaign_manager.progress_on_all_clients_ui_triggered

local function get_record(name)
  local rec = B.records[name]
  if not rec then
    rec = { ids = {}, count = 0, callback = nil, sent = false, pending = false }
    B.records[name] = rec
  end
  return rec
end

local function ids_str(rec)
  local t = {}
  for id in pairs(rec.ids) do table.insert(t, id) end
  table.sort(t)
  return table.concat(t, ",")
end

local function run_callback(name, cb)
  local ok, err = pcall(cb)
  if not ok then
    LOG("ERROR in progression callback for [" .. name .. "]: " .. tostring(err))
    pcall(script_error, "ERROR: archon barrier callback for [" .. name .. "] failed: " .. tostring(err))
  end
end

-- called from the UITrigger handler (synced) when a record reaches the expected machine count
local function complete(name, rec, expected, last_id)
  if rec.callback then
    B.records[name] = nil
    B.stats.completed = B.stats.completed + 1
    out("# progress_on_all_clients_ui_triggered() [archon] received trigger [" .. rec.count .. "/" .. expected
      .. "] for [" .. name .. "] from machine [" .. last_id .. "], calling progression callback")
    LOG("COMPLETE [" .. name .. "] " .. rec.count .. "/" .. expected .. " machines {" .. ids_str(rec)
      .. "} last=" .. last_id .. " turn=" .. cm:model():turn_number() .. " -> callback")
    run_callback(name, rec.callback)
  else
    rec.pending = true
    B.stats.pending = B.stats.pending + 1
    LOG("WARNING: [" .. name .. "] completed " .. rec.count .. "/" .. expected .. " {" .. ids_str(rec)
      .. "} but this machine has no callback yet; kept as COMPLETED-PENDING (will run at local registration)")
  end
end

-- v0.4.0: called (synced: turn events, FactionBecomesIdleHuman, archon_left| message) after expected_machines()
-- dropped because a partner left. Completes every open record that the remaining machines have now satisfied, in
-- sorted name order so every machine runs the callbacks in the same order.
function B.recheck(reason)
  if not A.is_active() then return end
  local expected = A.expected_machines()
  local names, open = {}, 0
  for name, rec in pairs(B.records) do
    open = open + 1
    if not rec.pending and rec.count > 0 and rec.count >= expected then table.insert(names, name) end
  end
  table.sort(names)
  LOG("RECHECK (" .. tostring(reason) .. "): expected machines now " .. expected .. ", open records " .. open
    .. ", satisfied " .. #names .. (#names > 0 and (" [" .. table.concat(names, ", ") .. "]") or ""))
  for _, name in ipairs(names) do
    local rec = B.records[name]
    if rec and not rec.pending then complete(name, rec, expected, "recheck") end
  end
end

local function archon_progress(self, trigger_name, callback)
  if not validate.is_string(trigger_name) then return false end
  if not validate.is_function(callback) then return false end
  if not self:is_multiplayer() then callback(); return end
  local local_faction = cm:get_local_faction(true)
  if not local_faction then return vanilla_progress(self, trigger_name, callback) end

  B.stats.called = B.stats.called + 1
  local rec = get_record(trigger_name)

  if rec.pending then
    -- the barrier already completed everywhere before this machine registered: run now, clear the record,
    -- do not send (nobody waits for it; sending would open a round that never completes)
    B.records[trigger_name] = nil
    B.stats.late = B.stats.late + 1
    LOG("WARNING: late registration for [" .. trigger_name .. "]: running the pending callback NOW "
      .. "(not at the completion tick; investigate if this appears)")
    run_callback(trigger_name, callback)
    return
  end

  rec.callback = callback
  if rec.sent then
    LOG("re-registration of [" .. trigger_name .. "] before completion: callback replaced, trigger not re-sent")
    return
  end
  rec.sent = true

  local id = A.local_id()
  local msg = PREFIX .. id .. "|" .. trigger_name
  out("")
  out("# progress_on_all_clients_ui_triggered() [archon] called with trigger name [" .. trigger_name
    .. "] on machine [" .. id .. "] local faction [" .. local_faction:name() .. "]")
  out("")
  LOG("REGISTER+SEND [" .. trigger_name .. "] machine=" .. id .. " (already received " .. rec.count .. "/"
    .. A.expected_machines() .. ")")
  CampaignUI.TriggerCampaignScriptEvent(local_faction:command_queue_index(), msg)
end

function B.progress_on_all_clients_ui_triggered(self, trigger_name, callback)
  local active = false
  local ok, err = pcall(function() active = A.is_active() end)
  if not ok then LOG("ERROR deciding Archon state: " .. tostring(err)) end
  if not active then return vanilla_progress(self, trigger_name, callback) end
  return archon_progress(self, trigger_name, callback)
end

-- install on the instance (shadows the class method for every cm:progress_on_all_clients_ui_triggered call)
if type(vanilla_progress) == "function" then
  cm.progress_on_all_clients_ui_triggered = B.progress_on_all_clients_ui_triggered
  LOG("installed machine-counting override of cm:progress_on_all_clients_ui_triggered (used only when Archon is active)")
else
  LOG("ERROR: vanilla campaign_manager.progress_on_all_clients_ui_triggered not found; override NOT installed")
end


-- ---------------------------------------------------------------------------------------------------------
-- Routes on the single UITrigger dispatcher (archon_net). Installed only while Archon is active; the dispatcher
-- reads the trigger text once per message and calls these with (text, faction_cqi).

-- archon-format barrier triggers: archon_poac|<machine id>|<trigger name>
A.net.on(PREFIX, "barrier", function(t, cqi)
  local rest = string.sub(t, #PREFIX + 1)
  local id, name = string.match(rest, "^([^|]+)|(.+)$")
  if not id then LOG("malformed trigger: " .. t); return end
  local rec = get_record(name)
  if rec.ids[id] then
    LOG("duplicate trigger for [" .. name .. "] from " .. id .. " ignored")
    return
  end
  rec.ids[id] = true
  rec.count = rec.count + 1
  local roster_known = A.session and A.session.roster_by_id and A.session.roster_by_id[id]
  local expected = A.expected_machines()
  if rec.count >= expected then
    complete(name, rec, expected, id)
  else
    LOG("RECV [" .. name .. "] from " .. id .. (roster_known and "" or " (not in roster yet)") .. " "
      .. rec.count .. "/" .. expected .. " local_callback=" .. tostring(rec.callback ~= nil))
    out("# progress_on_all_clients_ui_triggered() [archon] received trigger [" .. rec.count .. "/" .. expected
      .. "] for [" .. name .. "] from machine [" .. id .. "]")
  end
end)

-- vanilla-format barrier triggers: faithful port of start_progress_on_all_clients_ui_triggered_monitor()
-- (lib_campaign_manager.lua ~14454-14504), since the dispatcher replaces that listener while Archon is active.
-- With every machine running the override none should arrive; if one does it is logged and handled as vanilla.
local VANILLA_POAC = "progress_on_all_clients_"
A.net.on(VANILLA_POAC, "vanilla barrier", function(t, cqi)
  LOG("WARNING: vanilla-format barrier trigger while Archon is active: " .. t .. " (cqi " .. tostring(cqi) .. ")")
  local triggers = cm.progress_on_all_clients_ui_triggers
  local trigger_name = string.sub(t, #VANILLA_POAC + 1)
  if not triggers[trigger_name] then triggers[trigger_name] = {} end
  local faction = cm:model():faction_for_command_queue_index(cqi)
  if faction:is_null_interface() then return end
  local faction_name = faction:name()
  local trigger_record = triggers[trigger_name]
  if trigger_record[faction_name] then return end
  trigger_record[faction_name] = true
  local count = (trigger_record.count or 0) + 1
  trigger_record.count = count
  local total = #cm:get_active_human_factions()
  if count >= total then
    local callback = trigger_record.callback
    if not callback then
      script_error("ERROR: progress_on_all_clients_ui_triggered() has received ui trigger [" .. count .. "/" .. total
        .. "] for trigger [" .. trigger_name .. "] from faction [" .. faction_name .. "] but no progression callback is set up - how can this be? Expect a desync")
      return
    end
    triggers[trigger_name] = nil
    out("# progress_on_all_clients_ui_triggered() has received ui trigger [" .. count .. "/" .. total .. "] for trigger ["
      .. trigger_name .. "] from faction [" .. faction_name .. "], calling progression callback")
    callback()
  else
    out("# progress_on_all_clients_ui_triggered() has received ui trigger [" .. count .. "/" .. total .. "] for trigger ["
      .. trigger_name .. "] from faction [" .. faction_name .. "]")
  end
end)

-- progress_on_mp_query answers: vanilla mp_query_listener logic (lib_campaign_manager.lua ~14600-14640) plus the
-- duplicate-answer guard. The query runs "on the machine whose local faction matches"; with two machines on that
-- faction BOTH answer. Every machine takes the first answer (same command order everywhere), the second one is
-- logged instead of raising script_error "no record ... How can this be?".
local vanilla_mp_queries_answered = {}
A.net.on("mpq:", "mp query", function(query_str_with_result, cqi)
  local result_separator = string.find(query_str_with_result, "::")  -- vanilla form; plain-mode find misbehaved in-game (run 6)
  if not result_separator then
    script_error("WARNING: mp query event received but could not find result separator? Query string is " .. tostring(query_str_with_result))
    return
  end
  local result = string.sub(query_str_with_result, result_separator + 2)
  local query_str = string.sub(query_str_with_result, 1, result_separator - 1)
  local callback = cm.mp_queries[query_str]
  if not callback then
    if vanilla_mp_queries_answered[query_str] then
      A.log("mpquery", "second answer for [" .. query_str .. "] = " .. tostring(result) .. " ignored (first was "
        .. tostring(vanilla_mp_queries_answered[query_str]) .. "; machines sharing a faction both answer)")
      return
    end
    script_error("WARNING: mp query event received but no record for it could be found. How can this be? Query string is " .. tostring(query_str_with_result))
    return
  end
  cm.mp_queries[query_str] = nil
  vanilla_mp_queries_answered[query_str] = result
  A.log("mpquery", "answer for [" .. query_str .. "] = " .. tostring(result))
  if result == "true" then callback(true)
  elseif result == "false" then callback(false)
  else
    local n = tonumber(result)
    if n then callback(n) else callback(result) end
  end
end)


A.on_first_tick("barrier status", function()
  LOG("first tick: Archon " .. (A.is_active() and "ACTIVE" or "inactive") .. ", expected machines="
    .. tostring(A.expected_machines()) .. ", local id=" .. A.local_id()
    .. ", override installed=" .. tostring(cm.progress_on_all_clients_ui_triggered == B.progress_on_all_clients_ui_triggered))
end)
