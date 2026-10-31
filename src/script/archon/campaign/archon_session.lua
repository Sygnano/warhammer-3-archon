-- Archon Mode v1.0.0 - session: Archon detection, per-machine identity, session roster, partner leaving.
--
-- Detection ("Archon active"): multiplayer campaign AND at least one human faction is controlled by more than
-- one network player. Source: the engine's session player list, read through the context system
--   CampaignRoot.NetworkPlayerList            ("all the human players in the current mp game")
--   CcoCampaignRoot / NetworkPlayerList        (same, 3-argument form)
--   MultiplayerRootContext.PlayerList          ("CcoNetworkPlayers in the current game")
-- per player: Id ("unique mp client id"), Name, IsLocalPlayer, SessionPlayerId, PlayerSlot and
-- CurrentGameCampaignFactionContext (.CQI, .FactionRecordContext.Key). The list is session data that every
-- machine holds in full, so the per-faction counts are the same on every machine. Two independent tests are
-- combined: (a) some faction has more than one player in the list, (b) pigeonhole: more players than human
-- factions in the (synced) model. The result is computed once per machine at the pre-first-tick (same model
-- tick everywhere) and frozen; if the list is unreadable there, it is retried at the first use.
--
-- Identity: archon.local_id() = "m" + 8-hex hash of the local CcoNetworkPlayer.Id; fallback: a random id
-- persisted in archon_mode_machine_id.txt next to the exe. Each machine broadcasts
--   archon_hello|<id>|<expected_machines>|<active 0/1>|<mod version>|<name>
-- once at first tick through CampaignUI.TriggerCampaignScriptEvent; every machine records the roster from
-- UITrigger (same model tick everywhere, so the roster is identical). The roster is diagnostic; the barrier
-- uses expected_machines(), which comes from the session list, not from hellos.
--
-- Partner leaving (v0.4.0): the session list is frozen at the
-- pre-first-tick, and with DLL patch D the shared faction stays FULL_HUMAN when a partner quits, so is_idle_human()
-- no longer lowers expected_machines() and every later barrier would wait for the departed machine. Two signals:
--   * S.refresh(): re-reads the session list at synced model points (WorldStartRound, FactionTurnStart of a human
--     faction, FactionBecomesIdleHuman) and lowers the per-faction counts when the list has shrunk (never raises
--     them: nobody can join a running campaign);
--   * archon_left|<id>|<n>|<ids>: a machine whose local list has shrunk (two identical polls 2 s apart, real-time,
--     local) broadcasts the new player count and the remaining machine ids once; every machine applies them at the
--     same model tick as a cap on expected_machines() (min of all announced values), so a barrier that blocks play
--     is completed too, and an end-turn vote of a departed machine stops counting (0.7.0).
-- Both then re-check the open barriers (archon_barrier B.recheck) and log PLAYER LEFT lines.

local A = archon
local S = {}
A.session = S

local LOG = function(msg) A.log("session", msg) end
local HELLO = "archon_hello|"
local ID_FILE = "archon_mode_machine_id.txt"
local MAX_PLAYERS = 16

-- ---------------------------------------------------------------------------------------------------------
-- small helpers

-- shared helpers (archon.util, boot loader): hash8 must stay bit-identical on every machine
local U = A.util
S.hash8 = U.hash8
local clean = U.clean
local ctx = U.ctx
S.ctx = ctx

local SOURCES = {
  { name = "CampaignRoot.NetworkPlayerList",       get = function(e) return ctx("CampaignRoot.NetworkPlayerList" .. e) end },
  { name = "CcoCampaignRoot|NetworkPlayerList",    get = function(e) return ctx("CcoCampaignRoot", "", "NetworkPlayerList" .. e) end },
  { name = "MultiplayerRootContext.PlayerList",    get = function(e) return ctx("MultiplayerRootContext.PlayerList" .. e) end },
}

local as_string = U.str

local function read_players(src)
  local n = tonumber(src.get(".Size"))
  if not n or n < 1 then return nil, n end
  if n > MAX_PLAYERS then n = MAX_PLAYERS end
  local players = {}
  for i = 0, n - 1 do
    local b = ".At(" .. i .. ")."
    local p = {
      index    = i,
      id       = as_string(src.get(b .. "Id")),
      name     = as_string(src.get(b .. "Name")),
      is_local = src.get(b .. "IsLocalPlayer") == true,
      sid      = tonumber(src.get(b .. "SessionPlayerId")),
      slot     = tonumber(src.get(b .. "PlayerSlot")),
      fcqi     = tonumber(src.get(b .. "CurrentGameCampaignFactionContext.CQI")),
      fkey     = as_string(src.get(b .. "CurrentGameCampaignFactionContext.FactionRecordContext.Key")),
    }
    table.insert(players, p)
  end
  return players, n
end

local function model_humans()
  local humans, by_cqi, by_key = {}, {}, {}
  local fl = cm:model():world():faction_list()
  for i = 0, fl:num_items() - 1 do
    local f = fl:item_at(i)
    if f:is_human() then
      local h = { key = f:name(), cqi = f:command_queue_index(), idle = f:is_idle_human() }
      table.insert(humans, h)
      by_cqi[h.cqi] = h
      by_key[h.key] = h
    end
  end
  table.sort(humans, function(a, b) return a.key < b.key end)
  return humans, by_cqi, by_key
end

-- ---------------------------------------------------------------------------------------------------------
-- detection

S.info = nil          -- frozen detection result once the player list was readable (or game is SP)

function S.detect(reason)
  if S.info and S.info.final then return S.info end
  local info = { reason = reason, final = false }
  info.mp = cm:is_multiplayer() and true or false
  local humans, by_cqi, by_key = model_humans()
  info.humans = humans

  local players, src_name
  local sizes = {}
  for _, src in ipairs(SOURCES) do
    local p, n = read_players(src)
    table.insert(sizes, src.name .. "=" .. tostring(n))
    if p and not players then players, src_name = p, src.name end
  end
  info.source_sizes = table.concat(sizes, " ")
  info.source = src_name
  info.players = players or {}
  info.readable = players ~= nil

  -- per-faction player counts (faction key; via cqi when the key is not readable)
  local per_faction, unassigned, max_per = {}, 0, 0
  for _, p in ipairs(info.players) do
    local key = p.fkey
    if (not key or not by_key[key]) and p.fcqi and by_cqi[p.fcqi] then key = by_cqi[p.fcqi].key end
    p.faction = key
    if key then
      per_faction[key] = (per_faction[key] or 0) + 1
      if per_faction[key] > max_per then max_per = per_faction[key] end
    else
      unassigned = unassigned + 1
    end
  end
  info.per_faction, info.unassigned, info.max_per_faction = per_faction, unassigned, max_per
  info.pigeonhole = #info.players > #humans
  info.shared_faction = max_per > 1
  info.active = info.mp and info.readable and (info.shared_faction or info.pigeonhole) or false

  if not info.mp or info.readable then info.final = true end
  S.info = info
  if not info.final then
    S._nonfinal_logs = (S._nonfinal_logs or 0) + 1
    if S._nonfinal_logs > 3 then return info end   -- keep retrying silently
  end

  local hs = {}
  for _, h in ipairs(humans) do
    table.insert(hs, h.key .. "(cqi " .. h.cqi .. (h.idle and ", idle" or "") .. ", players " .. tostring(per_faction[h.key] or 0) .. ")")
  end
  LOG(string.format("DETECT (%s): mp=%s readable=%s source=%s [%s] players=%d humans=%d max_per_faction=%d "
      .. "unassigned=%d pigeonhole=%s -> ARCHON %s%s",
    tostring(reason), tostring(info.mp), tostring(info.readable), tostring(src_name), info.source_sizes,
    #info.players, #humans, max_per, unassigned, tostring(info.pigeonhole),
    info.active and "ACTIVE" or "inactive", info.final and "" or " (not final: player list unreadable, will retry)"))
  LOG("  human factions: " .. table.concat(hs, ", "))
  for _, p in ipairs(info.players) do
    LOG(string.format("  player[%d] id=%s name=%s local=%s session_id=%s slot=%s faction=%s (ctx key=%s cqi=%s)",
      p.index, p.id and S.hash8(p.id) or "nil", clean(p.name, 32), tostring(p.is_local), tostring(p.sid),
      tostring(p.slot), tostring(p.faction), tostring(p.fkey), tostring(p.fcqi)))
  end
  if info.mp and not info.readable then
    LOG("WARNING: session player list unreadable from the context system; Archon stays INACTIVE (vanilla barrier). "
      .. "If two machines share a faction this reproduces the first-turn desync.")
  end
  return info
end

function S.is_active()
  local ok, info = pcall(S.detect, "is_active")
  return ok and info and info.active or false
end

-- number of machines that must answer a barrier: session players whose faction is not an idle human
-- (model, synced) plus players without a readable faction. Falls back to vanilla's active human faction count.
function S.expected_machines()
  local info = S.detect("expected")
  if not info.readable then return #cm:get_active_human_factions() end
  local n = info.unassigned
  local world = cm:model():world()
  for key, c in pairs(info.per_faction) do
    local f = world:faction_by_key(key)
    if f and not f:is_null_interface() and f:is_human() and not f:is_idle_human() then n = n + c end
  end
  if S.cap and n > S.cap then n = S.cap end      -- announced by archon_left| (synced, identical everywhere)
  if n < 1 then n = 1 end
  return n
end

local function machine_id(p) return p.id and ("m" .. S.hash8(p.id)) or nil end

-- the machines still on a faction (end-turn consensus, v0.7.0): set of ids and count. The set is nil when a player's
-- id is unreadable; the count still holds. S.remaining is the id list announced by archon_left| (synced).
function S.faction_machines(key)
  local info = S.detect("faction machines")
  local members, n = {}, 0
  for _, p in ipairs(info.players) do
    if p.faction == key then
      local id = machine_id(p)
      if not id then
        members = nil
        n = n + 1
      elseif not S.remaining or S.remaining[id] then
        if members then members[id] = true end
        n = n + 1
      end
    end
  end
  return members, n
end

-- ---------------------------------------------------------------------------------------------------------
-- per-machine identity

local function file_id()
  local f = io.open(ID_FILE, "rb")
  if f then
    local s = f:read("*l"); f:close()
    if s and string.match(s, "^%x%x%x%x%x%x%x%x$") then return s end
  end
  -- no math.random here: it is shared Lua state; build entropy from time, clock and a table address
  local seed = tostring(os.time()) .. ":" .. tostring(os.clock()) .. ":" .. tostring({}) .. ":" .. tostring(cm)
  local id = S.hash8(seed)
  local w = io.open(ID_FILE, "wb")
  if w then w:write(id, "\n"); w:close() end
  return id
end

function S.local_identity()
  if S._identity then return S._identity end
  local raw, name, src
  local info = S.detect("identity")
  for _, p in ipairs(info.players) do
    if p.is_local then raw, name, src = p.id, p.name, "session list (" .. tostring(info.source) .. ")" end
  end
  if not raw then
    raw = as_string(ctx("MultiplayerRootContext.LocalPlayerContext.Id"))
    if raw then src = "MultiplayerRootContext.LocalPlayerContext.Id" end
  end
  name = name or as_string(ctx("MultiplayerRootContext.LocalPlayerContext.Name"))
  local id
  if raw then
    id = "m" .. S.hash8(raw)
  else
    local ok, fid = pcall(file_id)
    id = "f" .. ((ok and fid) or S.hash8(tostring({})))
    src = "local file " .. ID_FILE
  end
  S._identity = { id = id, name = clean(name or "player", 24), source = src }
  LOG("local machine id=" .. id .. " name=" .. S._identity.name .. " source=" .. tostring(src))
  return S._identity
end

function S.local_id() return S.local_identity().id end

-- ---------------------------------------------------------------------------------------------------------
-- roster (from archon_hello UITriggers; identical on all machines)

S.roster_list, S.roster_by_id = {}, {}

function S.roster()
  local copy = {}
  for i, r in ipairs(S.roster_list) do copy[i] = r end
  return copy
end

function S.send_hello()
  if not cm:is_multiplayer() then LOG("single-player: no hello sent"); return end
  if not S.is_active() then LOG("Archon inactive: no hello sent"); return end
  local lf = cm:get_local_faction(true)
  if not lf then LOG("no local faction: no hello sent"); return end
  local me = S.local_identity()
  local msg = HELLO .. me.id .. "|" .. tostring(S.expected_machines()) .. "|" .. (S.is_active() and "1" or "0") .. "|" .. A.VERSION .. "|" .. me.name
  LOG("SEND " .. msg .. " (faction cqi " .. lf:command_queue_index() .. ")")
  CampaignUI.TriggerCampaignScriptEvent(lf:command_queue_index(), msg)
end

-- Players on different mod versions: a dialogue box on this machine only, so nothing here can desync. It waits for
-- the end of the UITrigger dispatch, which has to stay light (archon_net). Shown once, and the game carries on.
local VERSION_BOX = "archon_version_warning"
local VERSION_BOX_DELAY_MS = 3000

local function warn_version(theirs, name)
  LOG("WARNING: VERSION MISMATCH: this machine runs " .. A.VERSION .. ", " .. name .. " runs " .. theirs)
  if S.version_warned then return end
  S.version_warned = true
  local text = "Archon Mode versions differ.\n\nYou: " .. A.VERSION .. "\n" .. name .. ": " .. theirs
    .. "\n\nBoth players need the same version. Update the mod on both PCs, or this campaign may desync or crash."
  cm:real_callback(A.safe("version warning", function()
    local root = core:get_ui_root()
    local box = UIComponent(root:CreateComponent(VERSION_BOX, "UI/Common UI/dialogue_box"))
    find_uicomponent(root, VERSION_BOX, "DY_text"):SetStateText(text)
    box:PropagatePriority(99)
    box:LockPriority()
    core:add_listener(
      "archon_version_warning_closed",
      "ComponentLClickUp",
      function(context) return context.string == "button_tick" or context.string == "button_cancel" end,
      function()
        local shown = find_uicomponent(root, VERSION_BOX)
        if shown then shown:UnLockPriority(); shown:Destroy() end
      end,
      false
    )
  end), VERSION_BOX_DELAY_MS)
end

-- hello arrives through the single dispatcher (archon_net): no own UITrigger listener, no extra trigger reads
A.net.on(HELLO, "hello", function(t, cqi)
  local id, exp, act, version, name = string.match(t, "^archon_hello|([^|]*)|([^|]*)|([^|]*)|([%d%.]+)|(.*)$")
  if not id then
    -- a version from before 0.7.4 sends no version field
    id, exp, act, name = string.match(t, "^archon_hello|([^|]*)|([^|]*)|([^|]*)|(.*)$")
    version = "older than 0.7.4"
  end
  if not id then LOG("malformed hello: " .. t); return end
  local fkey = "?"
  local f = cm:model():faction_for_command_queue_index(cqi)
  if f and not f:is_null_interface() then fkey = f:name() end
  local local_exp = S.expected_machines()
  if S.roster_by_id[id] then LOG("hello again from " .. id .. " (ignored)"); return end
  local r = { id = id, name = name, version = version, faction = fkey, expected = tonumber(exp), active = act == "1",
              order = #S.roster_list + 1, turn = cm:model():turn_number() }
  S.roster_by_id[id] = r
  table.insert(S.roster_list, r)
  local me = S.local_identity().id
  LOG(string.format("ROSTER +%s%s name=%s faction=%s their_expected=%s their_active=%s (%d/%d)",
    id, id == me and " (SELF)" or "", name, fkey, exp, act, #S.roster_list, local_exp))
  if tonumber(exp) ~= local_exp or (act == "1") ~= S.is_active() then
    LOG("WARNING: MISMATCH with this machine (expected=" .. local_exp .. ", active=" .. tostring(S.is_active())
      .. "). Machines disagree on the session; barrier counts will not agree.")
  end
  if version ~= A.VERSION then warn_version(version, name) end
  if #S.roster_list == local_exp then
    local ids = {}
    for _, x in ipairs(S.roster_list) do table.insert(ids, x.id .. "=" .. x.name .. "@" .. x.faction) end
    LOG("ROSTER COMPLETE (" .. local_exp .. " machines): " .. table.concat(ids, ", "))
  end
end)

-- ---------------------------------------------------------------------------------------------------------
-- partner leaving (v0.4.0)

local LEFT = "archon_left|"
S.cap = nil              -- lowest player count announced through archon_left| (nil = none)
S.left_events = 0

-- smallest positive .Size over the readable sources (3 context reads, no per-player reads); nil if unreadable
local function current_list_size()
  local best
  for _, src in ipairs(SOURCES) do
    local n = tonumber(src.get(".Size"))
    if n and n >= 1 and (not best or n < best) then best = n end
  end
  return best
end

local function known_players()
  local info = S.info
  local n = info and info.players and #info.players or 0
  if S.cap and S.cap < n then n = S.cap end
  return n
end

local function log_left(what, before_n, after_n, before_exp)
  LOG(string.format("PLAYER LEFT (%s): session players %d -> %d, expected machines %d -> %d, turn %s",
    what, before_n, after_n, before_exp, S.expected_machines(), tostring(cm:model():turn_number())))
  if A.native_leave_line then LOG("  native patch D: " .. A.native_leave_line()) end
  local hs = {}
  for _, h in ipairs((model_humans())) do
    table.insert(hs, h.key .. (h.idle and " (IDLE_HUMAN)" or " (human)"))
  end
  LOG("  human factions now: " .. table.concat(hs, ", "))
end

-- synced points only. Re-reads the list; lowers per-faction counts when it has shrunk. Returns true on change.
function S.refresh(reason)
  local info = S.info
  if not (info and info.final and info.readable and info.mp and info.active) then return false end
  local size = current_list_size()
  if not size or size >= #info.players then return false end
  local players
  for _, src in ipairs(SOURCES) do
    local p, n = read_players(src)
    if p and n == size then players = p; break end
  end
  if not players then return false end
  local _, by_cqi, by_key = model_humans()
  local per_faction, unassigned, max_per = {}, 0, 0
  for _, p in ipairs(players) do
    local key = p.fkey
    if (not key or not by_key[key]) and p.fcqi and by_cqi[p.fcqi] then key = by_cqi[p.fcqi].key end
    p.faction = key
    if key then
      per_faction[key] = (per_faction[key] or 0) + 1
      if per_faction[key] > max_per then max_per = per_faction[key] end
    else
      unassigned = unassigned + 1
    end
  end
  local before_n, before_exp = #info.players, S.expected_machines()
  info.players, info.per_faction, info.unassigned, info.max_per_faction = players, per_faction, unassigned, max_per
  S.left_events = S.left_events + 1
  log_left("session list re-read at " .. tostring(reason), before_n, #players, before_exp)
  for _, p in ipairs(players) do
    LOG(string.format("  remaining player[%d] id=%s name=%s local=%s faction=%s", p.index,
      p.id and S.hash8(p.id) or "nil", clean(p.name, 32), tostring(p.is_local), tostring(p.faction)))
  end
  if A.barrier and A.barrier.recheck then A.barrier.recheck("session list at " .. tostring(reason)) end
  if A.endturn then A.endturn.recheck("session list at " .. tostring(reason)) end
  return true
end

-- route on the single dispatcher (archon_net): only numbers and log lines here, no session list reads
A.net.on(LEFT, "left", function(t, cqi)
  local id, n, ids = string.match(t, "^archon_left|([^|]*)|(%d+)|([^|]*)$")
  n = tonumber(n)
  if not id or not n then LOG("malformed left message: " .. t); return end
  local before_n, before_exp = known_players(), S.expected_machines()
  if n < 1 or n >= before_n then
    LOG("left message from " .. id .. " announces " .. n .. " players (known " .. before_n .. "): no change")
    return
  end
  S.cap = n
  if ids ~= "" then
    S.remaining = {}
    for m in string.gmatch(ids, "[^,]+") do S.remaining[m] = true end
  end
  S.left_events = S.left_events + 1
  log_left("announced by " .. id, before_n, n, before_exp)
  if A.barrier and A.barrier.recheck then A.barrier.recheck("archon_left from " .. id) end
  if A.endturn then A.endturn.recheck("archon_left from " .. id) end
end)

-- local real-time poll (UI side, cannot desync by itself): broadcast a stable drop of the local list size once
S.poll = { last = nil, sent = nil }
local function poll_left()
  local size = current_list_size()
  if not size then S.poll.last = nil; return end
  if size < known_players() and size == S.poll.last and size ~= S.poll.sent then
    local lf = cm:get_local_faction(true)
    if not lf then return end
    S.poll.sent = size
    local ids = {}
    for _, src in ipairs(SOURCES) do
      local p, n = read_players(src)
      if p and n == size then
        for _, x in ipairs(p) do table.insert(ids, machine_id(x)) end
        break
      end
    end
    if #ids ~= size then ids = {} end        -- the empty list means "unknown": votes are then only re-counted
    local msg = LEFT .. S.local_id() .. "|" .. size .. "|" .. table.concat(ids, ",")
    LOG("local session list dropped to " .. size .. " players (known " .. known_players() .. "): SEND " .. msg)
    CampaignUI.TriggerCampaignScriptEvent(lf:command_queue_index(), msg)
  end
  S.poll.last = size
end

A.listen("archon_left_world_start_round", "WorldStartRound", true,
  function() S.refresh("WorldStartRound") end, true)
A.listen("archon_left_faction_turn_start", "FactionTurnStart",
  function(context) return S.info and S.info.active and context:faction():is_human() end,
  function(context) S.refresh("FactionTurnStart:" .. context:faction():name()) end, true)
A.listen("archon_left_faction_idle", "FactionBecomesIdleHuman", true, function(context)
  local name = context:faction():name()
  LOG("FactionBecomesIdleHuman: " .. name .. " (the engine switched it to IDLE_HUMAN; native patch D: "
    .. (A.native_leave_line and A.native_leave_line() or "n/a") .. ")")
  if not S.refresh("FactionBecomesIdleHuman:" .. name) and A.barrier and A.barrier.recheck then
    A.barrier.recheck("FactionBecomesIdleHuman:" .. name)
  end
end, true)

A.on_first_tick("session left poll", function()
  if not (cm:is_multiplayer() and S.is_active()) then return end
  cm:repeat_real_callback(function()
    local ok, err = pcall(poll_left)
    if not ok then LOG("ERROR in left poll: " .. tostring(err)) end
  end, 2000, "archon_left_poll")
  LOG("partner-leave poll started (2 s, local session list size; known players " .. known_players() .. ")")
end)

-- ---------------------------------------------------------------------------------------------------------
-- lifecycle and public API

A.on_pre_first_tick("session detect", function() S.detect("pre_first_tick"); S.local_identity() end)
A.on_first_tick("session hello", function() S.send_hello() end)

A.is_active         = S.is_active
A.local_id          = S.local_id
A.roster            = S.roster
A.expected_machines = S.expected_machines
