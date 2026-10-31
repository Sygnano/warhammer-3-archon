-- Archon Mode v1.0.0 - end-turn consensus inside a shared faction.
--
-- Vanilla readiness is per faction, so one press would end the shared faction's turn for every player on it. With
-- native patch F a press on a shared faction no longer reaches the engine: this machine's button shows the vanilla
-- waiting state (the UI readers of the ready set answer "ready" for the local press) and this module votes:
--   archon_endturn|<machine id>|vote|<turn>     this machine pressed End Turn
--   archon_endturn|<machine id>|cancel|<turn>   this machine pressed again (or a vanilla auto-cancel fired)
--   archon_endturn|<machine id>|reset|<turn>    the real end turn could not happen: everybody starts over
-- Same rules as the barrier: UITriggers are handled at the same model tick on every machine of the faction, and
-- who must vote comes from the session (archon_session), lowered only at synced points. When every connected player
-- of the faction has voted, all of them put the hooks in bypass (cancels and the warning dialog must reach the
-- engine from then on) and the machine that completed the set (when a departure completed it: the smallest voter
-- id) issues one vanilla end turn. Nothing else enters the synced stream.
-- A vote only drives the local UI and that one command, so machines on other factions ignore these messages.
-- The press itself is polled (real-time, local). Without patch F the turn ends on the first press, as in 0.6.0.

local A = archon
local E = {}
A.endturn = E

local LOG = function(msg) A.log("endturn", msg) end
local PREFIX = "archon_endturn|"
local POLL_MS = 100
local WATCH_POLLS = 20       -- 2 s after the issue, then the issuer looks at what became of it
local MAX_TRIES = 5          -- gates refused (battle pending...): retried, then everybody starts over

E.enabled = false
E.key, E.cqi = nil, nil      -- the local faction, when it is shared
E.votes = {}                 -- machine id -> true, current turn
E.sent = false               -- what this machine last told the others
E.committed = false          -- the vote set completed
E.watch = nil                -- issuer only: polls left until the watchdog looks
E.tries, E.issued = 0, false

local N = function() return A.native end

local function count_votes()
  local n, ids = 0, {}
  for id in pairs(E.votes) do n = n + 1; table.insert(ids, id) end
  table.sort(ids)
  return n, ids
end

local function send(kind)
  local msg = PREFIX .. A.local_id() .. "|" .. kind .. "|" .. cm:model():turn_number()
  LOG("SEND " .. msg)
  CampaignUI.TriggerCampaignScriptEvent(E.cqi, msg)
end

-- the spinner's visibility is a context expression over the ready set; it may only refresh on a real readiness
-- change, so it is set here too (UI only). One log line tells whether the vanilla look followed.
local function nudge(pressed)
  local btn = find_uicomponent(core:get_ui_root(), "button_end_turn")
  if not btn then return end
  local spinner = find_uicomponent(btn, "spinner")
  if spinner then pcall(function() spinner:SetVisible(pressed) end) end
  pcall(function()
    cm:real_callback(A.safe("endturn look", function()
      LOG("look: pressed=" .. tostring(pressed) .. " button state=" .. tostring(btn:CurrentState())
        .. " spinner visible=" .. tostring(spinner and spinner:Visible()))
    end), 300)
  end)
end

-- every machine at a synced point, or local at load
local function clear_round(why)
  local n = count_votes()
  if n > 0 or E.committed or E.sent then LOG("round cleared (" .. why .. "), " .. n .. " vote(s) dropped") end
  local was_pressed = E.sent
  E.votes, E.sent, E.committed, E.watch, E.tries, E.issued = {}, false, false, nil, 0, false
  N().endturn_reset()
  if was_pressed then pcall(nudge, false) end
end

local function issue()
  E.tries = E.tries + 1
  local ok, reached = pcall(N().endturn_issue)
  E.issued = E.issued or (ok and reached == true)
  LOG("ISSUE try " .. E.tries .. ": " .. (ok and (reached and "issuer reached" or "refused by the vanilla gates") or ("ERROR " .. tostring(reached))))
  E.watch = WATCH_POLLS
end

-- every machine of the faction, at a synced point. last = the voter whose message arrived, nil after a departure.
function E.evaluate(last, reason)
  if not E.enabled or E.committed then return end
  local members, expected = A.session.faction_machines(E.key)
  if members then
    for id in pairs(E.votes) do
      if not members[id] then E.votes[id] = nil; LOG("vote of " .. id .. " dropped (left the session)") end
    end
  end
  local n, ids = count_votes()
  if n == 0 or n < expected then
    LOG((reason or "vote") .. ": " .. n .. "/" .. expected .. " voted {" .. table.concat(ids, ",") .. "}")
    return
  end
  local issuer = (last and E.votes[last]) and last or ids[1]
  E.committed = true
  N().endturn_bypass(true)
  LOG("COMPLETE " .. n .. "/" .. expected .. " {" .. table.concat(ids, ",") .. "} -> issuer " .. issuer)
  if issuer == A.local_id() then E.watch = 1 end    -- next poll: from the UI side, like a click, not inside this event
end

function E.recheck(reason)
  if E.enabled then E.evaluate(nil, reason) end
end

local function warning_open()
  local uic = find_uicomponent(core:get_ui_root(), "end_turn_warning")
  if not uic then return false end
  local ok, v = pcall(function() return uic:Visible() end)
  return ok and v and true or false
end

-- issuer only: the turn did not end within 2 s of the issue
local function watchdog()
  E.watch = E.watch - 1
  if E.watch > 0 then return end
  if N().endturn_real_ready() then
    E.watch = nil
    LOG("faction is ready in the engine, waiting for the other human factions")
  elseif warning_open() then
    E.watch = WATCH_POLLS
  elseif not E.issued and E.tries < MAX_TRIES then
    issue()
  else
    E.watch = nil
    LOG("no end turn after " .. E.tries .. " tries (issued=" .. tostring(E.issued) .. "): asking everybody to start over")
    send("reset")
  end
end

-- local, real-time
local function poll()
  local pressed = N().endturn_pressed()
  if pressed ~= E.sent then
    E.sent = pressed
    send(pressed and "vote" or "cancel")
    pcall(nudge, pressed)
  end
  if E.watch then watchdog() end
end

A.net.on(PREFIX, "endturn", function(t, cqi)
  if not E.enabled or cqi ~= E.cqi then return end
  local id, kind, turn = string.match(t, "^archon_endturn|([^|]*)|([^|]*)|(%d+)$")
  if not id then LOG("malformed message: " .. t); return end
  if tonumber(turn) ~= cm:model():turn_number() then LOG("stale " .. kind .. " of " .. id .. " ignored"); return end
  if kind == "vote" then
    E.votes[id] = true
    E.evaluate(id)
  elseif kind == "cancel" then
    E.votes[id] = nil
    if E.committed then
      E.committed, E.watch, E.tries, E.issued = false, nil, 0, false
      N().endturn_bypass(false)
      LOG("CANCEL by " .. id .. " after completion: back to voting")
      if id == A.local_id() then pcall(N().endturn_cancel) end   -- crossed the issue: no-op unless the faction is ready
    else
      E.evaluate(nil, "cancel by " .. id)
    end
  elseif kind == "reset" then
    clear_round("reset by " .. id)
  end
end)

local function round_over(context) return E.enabled and context:faction():name() == E.key end
A.listen("archon_endturn_about", "FactionAboutToEndTurn", round_over, function() clear_round("FactionAboutToEndTurn") end, true)
A.listen("archon_endturn_start", "FactionTurnStart", round_over, function() clear_round("FactionTurnStart") end, true)

-- the DLL outlives the campaign: nothing may carry over from an earlier session, shared or not
if A.native_ok() and type(A.native.endturn_reset) == "function" then
  A.native.endturn_reset()
  A.native.endturn_set_shared(false)
end

A.on_first_tick("endturn setup", function()
  if not (cm:is_multiplayer() and A.is_active()) then return end
  local lf = cm:get_local_faction(true)
  if not lf then return end
  local key = lf:name()
  if (A.session.info.per_faction[key] or 0) < 2 then
    LOG("local faction " .. key .. " is not shared: vanilla end turn")
    return
  end
  local st = A.native_status_table()
  if not st or not st.endturn_supported then
    LOG("patch F unavailable (" .. tostring(st and st.endturn_reason or A.native_status) .. "): the first press ends the shared turn")
    return
  end
  local res = A.native.patch_endturn()
  LOG("patch F -> patch_endturn() = " .. tostring(res) .. "; issuer=" .. tostring(st.endturn_issuer_va) .. " cancel=" .. tostring(st.endturn_cancel_va))
  if string.sub(tostring(res), 1, 2) ~= "ok" then return end
  E.key, E.cqi, E.enabled = key, lf:command_queue_index(), true
  A.native.endturn_set_shared(true)
  cm:repeat_real_callback(function()
    local ok, err = pcall(poll)
    if not ok then LOG("ERROR in poll: " .. tostring(err)) end
  end, POLL_MS, "archon_endturn_poll")
  LOG("consensus on for " .. key .. " (" .. select(2, A.session.faction_machines(key)) .. " machines), poll " .. POLL_MS .. " ms")
end)
