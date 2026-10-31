-- Archon Mode v1.0.0 - frontend / multiplayer campaign lobby.
--
-- Native patches applied here (archon_native.dll, loaded by script/_lib/mod/!archon_mode_native_boot.lua):
--   A: CanSetFaction -> true; B: the ready-error text builder ignores the "faction_duplicates" bit;
--   C: the ready-mask producer skips duplicate-faction slots
--      -> two or more players can select the same faction and press Ready;
--   E: resumed-lobby seats: the host seats every returning player on the faction of HIS tuple in the save header while
--      that faction has fewer seated players than saved tuples.
-- (Patch D, partner leave, is applied by the campaign loader; its state is part of the STATUS line.)
--
--   * AUTO-APPLY: A+B+C and E are applied once when this frontend script loads, whenever the DLL is loaded and
--     supported. 0.6.0: no flag file any more (an old archon_mode.flag next to the exe is ignored; deleting it is
--     harmless).
--   * A status label (top-left, topmost) is shown while the MP campaign lobby is visible, e.g.
--     "Archon 1.0.0 (dll 1.0.0): PATCHED A+B+C+E". Clicking it writes a STATUS line to the log.
--   * LOBBY diagnostics: one "LOBBY" line with every slot (name, id hash, local, host, faction, lord, state) plus
--     IsResumed and the ready text, logged only when something changed.
--   * RACE PICKER (resumed lobby): vanilla hides "Select Race" when the save has a single human culture (an Archon save
--     with one shared faction). It is forced visible so every player can move among the saved factions. With patch E
--     the engine already seats each returning player on his own saved faction (0.5.x logs), so no scripted reseating.
--   * SHARED-FACTION MARKERS: on a lord tile picked by several players, the other players' real avatars (copies of
--     their lobby player-list steam_avatar) are placed in a 2-column grid next to the vanilla icon. Local UI only.
-- Everything is logged to archon_mode_log.txt (tag [frontend]). If the DLL did not load this stays inert.

local VERSION    = "1.0.0"  -- pack (Lua) version; the DLL reports its own version in status()
local LOBBY_ID   = "mp_grand_campaign"
local TIMER_NAME = "archon_mode_ui_tick"
local TICK_MS    = 400      -- UI tick (0.6.0: was 1000 ms)
local ID_LABEL   = "archon_mode_label"
local LABEL_X, LABEL_Y, LABEL_W, LABEL_H = 20, 76, 300, 36

-- shared helpers from the boot loader (script/_lib/mod runs before script/frontend/mod, same environment)
local U = archon.util
local str, esc, hash8 = U.str, U.esc, U.hash8

local function log(msg) U.log("frontend", msg) end

local last_once = {}
local function log_change(key, msg)
  if last_once[key] ~= msg then last_once[key] = msg; log(msg) end
end

local function native()
  if type(archon) == "table" and type(archon.native_ok) == "function" and archon.native_ok() then
    return archon.native
  end
  return nil
end

local function status_table()
  local n = native()
  if not n then return nil end
  local ok, st = pcall(n.status)
  if ok and type(st) == "table" then return st end
  return nil
end

local function lobby_visible()
  local lobby = find_uicomponent(core:get_ui_root(), LOBBY_ID)
  if not lobby then return false end
  local ok, vis = pcall(function() return lobby:VisibleFromRoot() end)
  if ok then return vis and true or false end
  return true
end

local function do_status()
  local st = status_table()
  if not st then log("STATUS: native not loaded (" .. tostring(archon and archon.native_status) .. ")"); return end
  log(string.format("STATUS: supported=%s reason=%s version=%s canset=%s readybit=%s producer=%s (cont=%s) "
      .. "patched(canset=%s,ready=%s,producer=%s) leave(supported=%s,patched=%s,send=%s) "
      .. "resume(supported=%s,patched=%s,pick=%s,calls=%s,shared=%s,vanilla=%s,errors=%s,last=[%s])",
    tostring(st.supported), tostring(st.reason), tostring(st.version), tostring(st.canset_va),
    tostring(st.readybit_va), tostring(st.producer_va), tostring(st.producer_cont_va),
    tostring(st.patch_canset), tostring(st.patch_ready), tostring(st.patch_producer),
    tostring(st.leave_supported), tostring(st.patch_leave), tostring(st.leave_send_va),
    tostring(st.resume_supported), tostring(st.patch_resume), tostring(st.resume_pick_va), tostring(st.resume_calls),
    tostring(st.resume_shared), tostring(st.resume_vanilla), tostring(st.resume_errors), tostring(st.resume_last)))
end

local function do_patch(origin)
  local n = native()
  if not n then log(origin .. " PATCH ignored: native not loaded"); return end
  local ok, res = pcall(n.patch_lobby)
  log(origin .. " PATCH -> patch_lobby() = " .. tostring(ok) .. " " .. tostring(res))
  -- patch E resolves independently: A/B/C stay applied if E is unavailable
  if type(n.patch_resume) == "function" then
    local ok2, res2 = pcall(n.patch_resume)
    log(origin .. " PATCH -> patch_resume() = " .. tostring(ok2) .. " " .. tostring(res2))
  else
    log(origin .. " patch E skipped: DLL has no patch_resume")
  end
  do_status()
end

local function auto_apply()
  local st = status_table()
  if not st then log("AUTO-APPLY skipped: native not loaded (" .. tostring(archon and archon.native_status) .. ")"); return end
  if not st.supported then log("AUTO-APPLY skipped: native unsupported (" .. tostring(st.reason) .. ")"); return end
  do_patch("AUTO-APPLY at frontend load:")
end

-- --------------------------------------------------------------------------------------------------
-- status label: parented to the UI root, topmost, shown only while the MP campaign lobby is visible

local function label_text()
  local st = status_table()
  if not st then
    -- the boot loader drops a DLL that loaded but could not find the lobby patches: a game update, not a blocked file
    if string.find(tostring(archon and archon.native_status), "native refused") then return "Archon " .. VERSION .. ": UNSUPPORTED" end
    return "Archon " .. VERSION .. ": DLL NOT LOADED"
  end
  if not st.supported then return "Archon " .. VERSION .. ": UNSUPPORTED" end
  if st.patch_canset and st.patch_ready and st.patch_producer then
    local prefix = st.exe_tested == false and "UNTESTED GAME VERSION - Archon " .. VERSION or "Archon " .. VERSION .. " (dll " .. tostring(st.version) .. ")"
    return prefix .. ": PATCHED A+B+C" .. (st.patch_resume and "+E" or "")
  elseif st.patch_canset or st.patch_ready or st.patch_producer then
    return "Archon " .. VERSION .. ": PARTIAL?!"
  end
  return "Archon " .. VERSION .. ": NOT PATCHED"
end

local label_cache = nil

local function create_label(root)
  local l = UIComponent(root:CreateComponent(ID_LABEL, "ui/templates/square_medium_text_button"))
  pcall(function() l:SetCanResizeWidth(true); l:SetCanResizeHeight(true) end)
  l:Resize(LABEL_W, LABEL_H); l:MoveTo(LABEL_X, LABEL_Y); l:SetVisible(true)
  l:SetTooltipText("Archon Mode lobby patch state (click = write STATUS to archon_mode_log.txt)", true)
  pcall(function() l:RegisterTopMost() end)
  label_cache = nil
  return l
end

local function set_label(l, text)
  if label_cache == text then return end
  local ok = pcall(function()
    local t = find_uicomponent(l, "button_txt")
    if t then t:SetStateText(text) else l:SetStateText(text) end
  end)
  if ok then label_cache = text end
end

local function ensure_label(root)
  local c = find_uicomponent(root, ID_LABEL)
  if c then
    c:SetVisible(true)
    local ok, vis = pcall(function() return c:VisibleFromRoot() end)
    if ok and not vis then
      pcall(function() c:Destroy() end)
      c = nil
      log_change("recreate", "label was not visible from root; re-creating")
    end
  end
  if not c then c = create_label(root) end
  return c
end

-- --------------------------------------------------------------------------------------------------
-- lobby model: one read of every slot per tick (context system only)

local L = "FrontendRoot.CampaignLobbyContext."
local MAX_SLOTS = 8

local ctx = U.ctx
local function ctx3(typ, id, expr)
  if id == nil then return nil end
  return U.ctx(typ, id, expr)
end

local function is_true(v) return v == true or v == 1 or v == "true" end

local function pat_escape(s) return (string.gsub(s, "[%^%$%(%)%%%.%[%]%*%+%-%?]", "%%%0")) end

-- per lobby visit caches: the occupant of a slot (name, local, host) is re-read only when the slot's network id
-- changes; IsResumed is no longer read once it was true. Faction, lord and state are read every tick.
local slot_cache = {}           -- slot index -> { nid, name, is_local, is_host }
local resumed_cache = false

local function read_slot(i)
  local b = L .. "PlayerSlotList.At(" .. i .. ")."
  local nid = str(ctx(b .. "NetworkPlayerContext.Id"))
  local c = slot_cache[i]
  if not (c and nid and c.nid == nid) then
    c = { nid = nid, name = str(ctx(b .. "Name")), is_local = is_true(ctx(b .. "IsLocalPlayer")),
          is_host = is_true(ctx(b .. "IsHost")) }
    slot_cache[i] = nid and c or nil
  end
  return {
    index = i, nid = nid, id8 = nid and hash8(nid) or nil,
    name = c.name, is_local = c.is_local, is_host = c.is_host,
    faction = str(ctx(b .. "CurrentFactionContext.Key")),
    lord = str(ctx(b .. "CurrentLordContext.GeneralKey")),
    state = str(ctx(b .. "GetCurrentState")),
  }
end

local function read_lobby()
  if not resumed_cache then resumed_cache = is_true(ctx(L .. "IsResumed")) end   -- once true, true for the visit
  local lb = {
    resumed = resumed_cache,
    local_ready = ctx(L .. "IsLocalPlayerReady"),
    ready_text = str(ctx(L .. "ReadyStatusText")),
    slots = {},
  }
  local n = tonumber(ctx(L .. "PlayerSlotList.Size")) or 0
  if n > MAX_SLOTS then n = MAX_SLOTS end
  for i = 0, n - 1 do
    local s = read_slot(i)
    table.insert(lb.slots, s)
    if s.is_local and not lb.local_slot then lb.local_slot = s end
  end
  for i = n, MAX_SLOTS - 1 do slot_cache[i] = nil end
  return lb
end

local function diag_log(lb)
  local me = lb.local_slot
  local parts = { string.format("LOBBY resumed=%s local_is_host=%s local_ready=%s slots=%d",
    tostring(lb.resumed), tostring(me and me.is_host), tostring(lb.local_ready), #lb.slots) }
  for _, s in ipairs(lb.slots) do
    table.insert(parts, string.format("slot%d[name=%s id=%s local=%s host=%s faction=%s lord=%s state=%s]",
      s.index, esc(s.name, 40), tostring(s.id8), tostring(s.is_local), tostring(s.is_host),
      tostring(s.faction), tostring(s.lord), tostring(s.state)))
  end
  table.insert(parts, "ready_text=" .. esc(lb.ready_text or "", 300))
  log_change("lobby_diag", table.concat(parts, " | "))
end

-- --------------------------------------------------------------------------------------------------
-- resumed lobby: seat picker. Local UI only.
--   button_select_race ("Select Race") is hidden by the layout when CultureList.Size == 1 (an Archon save with a
--   single human faction). We show it again; its entries call LocalPlayerSlot.SetCulture(culture), which puts the
--   slot on that culture's first saved faction (a free one if any). No engine check refuses a held faction for a
--   slot that already has one (predicate 0x1421a8ce4 refuses only an empty slot).

local RP = nil                  -- race picker state per lobby visit
local RP_HIDDEN_LOG = 8         -- consecutive ticks (~3 s) re-hidden by the layout before that is logged

local function force_race_picker(lobby)
  if not RP then RP = { forced = 0, streak = 0, hidden_logged = false } end
  local btn = find_uicomponent(lobby, "button_select_race")
  if not btn then log_change("rp", "RACE PICKER: button_select_race not found"); return end
  local okv, vis = pcall(function() return btn:Visible() end)
  if okv and vis then RP.streak = 0; return end
  pcall(function() btn:SetVisible(true) end)
  local single = find_uicomponent(lobby, "label_selected_race_when_single_race")
  if single then pcall(function() single:SetVisible(false) end) end
  RP.forced = RP.forced + 1
  RP.streak = RP.streak + 1
  if RP.forced == 1 then
    log("RACE PICKER: 'Select Race' was hidden (saved cultures="
      .. tostring(ctx(L .. "CampaignStartInfoContext.CultureList.Size")) .. ") -> forced visible")
  end
  if RP.streak >= RP_HIDDEN_LOG and not RP.hidden_logged then
    RP.hidden_logged = true
    log("RACE PICKER: the layout hides it again every tick (" .. RP.streak .. " in a row)")
  end
end

-- --------------------------------------------------------------------------------------------------
-- shared-faction avatar markers on the lord tiles (local UI only)

local MARK = "archon_pi_"
local MAX_MARKS = 7             -- 8 players per lobby: up to 7 partners next to the vanilla icon
local MARK_STEP = 24
local MARK_SIZE = 22
-- marker k -> {column, row} in a 2-column grid anchored at the vanilla icon (top right of the tile; the vanilla icon
-- is column 0 row 0). Columns grow to the left, rows downwards: dock offset = (vanilla_x - 24*col, vanilla_y + 24*row).
-- With a ~100x150 tile and 22 px icons the grid spans 46x94 px from the vanilla icon's corner.
local MARK_GRID = { { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 2 }, { 1, 2 }, { 0, 3 }, { 1, 3 } }
local MARK_IMAGE = "ui/skins/default/avatar_offline_small.png"   -- player_icon's own component image
local MARKER_PATH = "ui/archon/marker"                            -- our own layout (src/ui/archon/marker.twui.xml): one 22x22 avatar image
local DUMMY_PATH = "UI/Campaign UI/script_dummy.twui.xml"        -- core.path_to_dummy_component (lib_core.lua)
local SCAN_TICKS = 2            -- re-scan the lobby's player_icons / steam_avatars every 2 ticks while a faction is shared
local VERIFY_TICK = 5           -- ticks (5 x 400 ms = 2 s; the copy is visible meanwhile, this only gates the fallback) before a live route (avatar, copy) must show a real avatar
local leader_cache = {}         -- CcoFrontendFactionLeader context id -> { faction, general }
local markers_present = false
local marker_mode_logged = false
local mark_route = {}           -- container key .. marker name -> { route, ticks, verified, skip } (how it was created)
local marks_at = {}             -- container key -> highest marker index that may exist under it
-- every "player_icon" (lord tiles: {icon, container, ckey, lid}) and "steam_avatar" (player list) under the lobby
local icon_scan = { at = -1000, list = {}, avatars = {} }
local scan_tick = 0

-- The engine exposes cco as a callable TABLE (vanilla scripts just call cco(...)), so callable = function or
-- table/userdata and pcall decides (0.5.4 tested type(cco) == "function" and never created a marker).
local function cco_available()
  local t = type(cco)
  return t == "function" or t == "table" or t == "userdata"
end

-- The context lookup by id returns nil for these tiles, but the id itself starts with the faction key
-- ("wh3_main_cth...", "wh2_main_skv..."). So the faction falls back to the longest lobby faction key that
-- prefixes the id, and the lord to a slot lord key contained in the id (nil = not known, faction only).
-- The two context reads are done once per id (leader_ctx); an unresolved id is re-matched against the lobby's
-- factions on every call (string ops only), since players may pick that faction later.
local leader_ctx = {}           -- id -> { faction, general } from the context system (usually both nil)
local function leader_info(id, by_faction)
  local c = leader_cache[id]
  if c then return c end
  local x = leader_ctx[id]
  if not x then
    x = { faction = str(ctx3("CcoFrontendFactionLeader", id, "FactionRecordContext.Key")),
          general = str(ctx3("CcoFrontendFactionLeader", id, "GeneralKey")) }
    leader_ctx[id] = x
  end
  c = { faction = x.faction, general = x.general, via = "context" }
  if not c.faction then
    c.via = "id prefix"
    for key, group in pairs(by_faction) do
      if string.sub(id, 1, #key) == key and (not c.faction or #key > #c.faction) then
        c.faction = key
        for _, sl in ipairs(group) do
          if sl.lord and string.find(id, pat_escape(sl.lord)) then c.general = sl.lord end
        end
      end
    end
  end
  if c.faction then leader_cache[id] = c end
  return c
end

local function player_cco(s)
  if not cco_available() then return nil end
  local v = ctx(L .. "PlayerSlotList.At(" .. s.index .. ").NetworkPlayerContext")
  local tv = type(v)
  if v ~= nil and tv ~= "string" and tv ~= "number" and tv ~= "boolean" then return v end
  local ids = {}
  if tv == "string" or tv == "number" then table.insert(ids, tostring(v)) end
  if s.nid then table.insert(ids, s.nid) end
  for _, id in ipairs(ids) do
    local ok, o = pcall(cco, "CcoNetworkPlayer", id)
    if ok and o then return o end
  end
  return nil
end

local function shown_nid(uic)
  local ok, cid = pcall(function() return uic:GetContextObjectId("CcoNetworkPlayer") end)
  if not ok or not cid or cid == "" then return nil end
  return str(ctx3("CcoNetworkPlayer", cid, "Id"))
end

-- one-line description of a component: id, visibility, position, size, state, parent, image count/path
local function describe_uic(u)
  local p = {}
  local function g(label, fn) local ok, v = pcall(fn); table.insert(p, label .. "=" .. (ok and tostring(v) or "err")) end
  g("id", function() return u:Id() end)
  g("vis", function() return u:Visible() end)
  g("vroot", function() return u:VisibleFromRoot() end)
  g("pos", function() local x, y = u:Position(); return x .. "," .. y end)
  g("size", function() local w, h = u:Bounds(); return w .. "x" .. h end)
  g("dock", function() local x, y = u:GetDockOffset(); return x .. "," .. y end)
  g("state", function() return u:CurrentState() end)
  g("opacity", function() return u:Opacity() end)
  g("parent", function() return UIComponent(u:Parent()):Id() end)
  g("images", function() return u:NumImages() end)
  g("img0", function() return u:GetImagePath(0) end)
  return "{" .. table.concat(p, " ") .. "}"
end

-- number of images a component carries (0 when the call fails): a marker without an image draws nothing
local function num_images(u)
  local ok, n = pcall(function() return u:NumImages() end)
  return (ok and type(n) == "number") and n or 0
end

-- A real avatar is a runtime texture named "<component id><image index>_setfromimage_64x64_<ptr>" (set by the
-- native NetworkPlayerImage callback), never a file path. So "shows a real avatar" == img0 contains it.
local function img0(u)
  local ok, p = pcall(function() return u:GetImagePath(0) end)
  return (ok and type(p) == "string") and p or ""
end
local function has_real_avatar(u) return string.find(img0(u), "setfromimage") ~= nil end

-- The lobby player list (mp_grand_campaign.twui.xml: .../template_team_entry/player_list/template_player_entry/
-- steam_avatar, 72x72) draws EVERY player's real avatar: NetworkPlayerImage(CcoNetworkPlayer) + a propagator
-- CcoLobbyPlayer.NetworkPlayerContext, and NO PlayerForFactionLeaderContext propagator. Its owner is read from the
-- CcoNetworkPlayer / CcoLobbyPlayer context stored on it (or its entry), else from the entry's dy_player_name text.
local AVATAR_ID = "steam_avatar"
local function avatar_owner(a)
  local nid, name, how = nil, nil, "none"
  local par = nil
  pcall(function() par = UIComponent(a:Parent()) end)
  local ok, cid = pcall(function() return a:GetContextObjectId("CcoNetworkPlayer") end)
  if ok and cid and cid ~= "" then
    nid = str(ctx3("CcoNetworkPlayer", cid, "Id")); name = str(ctx3("CcoNetworkPlayer", cid, "Name")); how = "net"
  end
  if not nid then
    for _, u in ipairs({ a, par }) do
      local okl, lid = pcall(function() return u:GetContextObjectId("CcoLobbyPlayer") end)
      if okl and lid and lid ~= "" then
        nid = str(ctx3("CcoLobbyPlayer", lid, "NetworkPlayerContext.Id"))
        name = name or str(ctx3("CcoLobbyPlayer", lid, "Name"))
        if nid or name then how = "lobby"; break end
      end
    end
  end
  if not nid and not name and par then
    local t = find_uicomponent(par, "dy_player_name")
    if t then pcall(function() name = str(t:GetStateText()) end); how = "text" end
  end
  return nid, name, how
end

-- the partner's steam_avatar in the lobby player list (nil + a one-time log when none matches)
local function find_avatar(w)
  local tried = {}
  for _, a in ipairs(icon_scan.avatars or {}) do
    local nid, name, how = avatar_owner(a)
    if #tried < 8 then table.insert(tried, how .. ":" .. tostring(nid and hash8(nid)) .. "/" .. esc(name or "?", 16)) end
    if (w.nid and nid == w.nid) or (nid == nil and name and w.name and name == w.name) then return a, how end
  end
  log_change("mk_avatar_find", "MARKERS: partner avatar (" .. tostring(w.id8) .. "/" .. esc(w.name or "?", 16)
    .. ") not found among " .. #(icon_scan.avatars or {}) .. " " .. AVATAR_ID .. "(s): " .. table.concat(tried, ", "))
  return nil
end

-- create one marker under btn (the lord_button) by the first route that yields a usable component:
--   "avatar": CopyComponent of the partner's steam_avatar (real avatar), SetContextObject(partner), adopted into btn;
--             verified a few ticks later (the texture may arrive late): img0 must contain "setfromimage"
--   "layout": our own layout ui/archon/marker.twui.xml (one 22x22 generic avatar image; NumImages >= 1)
--   "dummy":  vanilla script_dummy + SetImagePath (works only if the dummy has an image slot)
--   "copy":   CopyComponent(player_icon) + SetContextObject(partner): the copied leader propagator may reset it
-- Routes in `skip` (failed verification earlier for this marker) are not tried again. Failures are logged once.
local ROUTES = { "avatar", "layout", "dummy", "copy" }
local function create_mark(btn, icon, name, w, skip)
  local fails = {}
  local function try(route, make)
    local ok, addr = pcall(make)
    if not ok or not addr then table.insert(fails, route .. ": " .. tostring(addr)); return nil end
    local m = UIComponent(addr)
    if route == "dummy" then pcall(function() m:SetImagePath(MARK_IMAGE, 0, true) end) end
    if route == "avatar" or route == "copy" then
      local o = player_cco(w)
      if o then pcall(function() m:SetContextObject(o) end)
      elseif route == "copy" then
        table.insert(fails, "copy: no CcoNetworkPlayer for the partner (cco=" .. type(cco) .. ")")
        pcall(function() m:Destroy() end)
        return nil
      end
      if route == "avatar" then
        local host = find_uicomponent(m, "label_host")      -- the copied "host" badge must not show on the marker
        if host then pcall(function() host:SetVisible(false) end) end
      end
    elseif num_images(m) < 1 then
      table.insert(fails, route .. ": component has no image")
      pcall(function() m:Destroy() end)
      return nil
    end
    return m
  end
  local makers = {
    avatar = function()
      local src, how = find_avatar(w)
      if not src then error("partner's " .. AVATAR_ID .. " not found") end
      local addr = src:CopyComponent(name)                  -- sibling of the source, inside the player entry ...
      if not addr then error("CopyComponent failed") end
      local c = UIComponent(addr)
      local oka = pcall(function() btn:Adopt(c:Address()) end)   -- ... moved under the lord button
      if not oka then pcall(function() c:Destroy() end); error("Adopt into lord_button failed") end
      log_change("mk_avatar_src", "MARKERS: partner avatar found via " .. tostring(how) .. "; source img0=" .. esc(img0(src), 80))
      return addr
    end,
    layout = function() return btn:CreateComponent(name, MARKER_PATH) end,
    dummy  = function() return btn:CreateComponent(name, DUMMY_PATH) end,
    copy   = function() return icon:CopyComponent(name) end,
  }
  local m, route = nil, nil
  for _, r in ipairs(ROUTES) do
    if not (skip and skip[r]) then
      m = try(r, makers[r])
      if m then route = r; break end
    end
  end
  log_change("mk_create", "MARKERS: marker route = " .. tostring(route)
    .. (#fails > 0 and (" (failed: " .. table.concat(fails, "; ") .. ")") or ""))
  return m, route
end

-- markers 1..#want under one lord_button (container); higher indexes that may still exist are destroyed
local function set_marks(btn, ckey, icon, want)
  local limit = marks_at[ckey]
  if limit then
    if #want > limit then limit = #want end
  else
    -- first visit of this tile in this lobby visit: sweep every index once (stale components of an earlier visit)
    limit = (#want > 0) and MAX_MARKS or 0
  end
  if limit == 0 then return false end
  local any, highest = false, 0
  local ix, iy = -5, 6                       -- player_icon's dock_offset in mp_grand_campaign.twui.xml
  if icon then pcall(function() local x, y = icon:GetDockOffset(); if x and y then ix, iy = x, y end end) end
  for k = 1, limit do
    local name = MARK .. k
    local rkey = ckey .. "/" .. name
    local m = find_uicomponent(btn, name)
    local w = want[k]
    if w and icon then
      -- A COPY of player_icon never shows by itself: in mp_grand_campaign.twui.xml the icon carries
      -- ContextVisibilitySetter(CcoNetworkPlayer.IsValidContext) and a ContextPropagator that re-pushes
      -- PlayerForFactionLeaderContext(this) (ONE player per leader) and clears the player context when empty, so the
      -- copy is hidden or shows the first player again. Hence the partner's real avatar comes from a copy of his
      -- player-list steam_avatar; our plain-image layout (generic avatar) remains the safe fallback.
      local rec = mark_route[rkey]
      if m and not rec then pcall(function() m:Destroy() end); m = nil end   -- stale component from a previous lobby
      if not m then
        local skip = (rec and rec.skip) or {}
        local created, route = create_mark(btn, icon, name, w, skip)
        m = created
        if m then mark_route[rkey] = { route = route, ticks = 0, skip = skip, verified = (route ~= "avatar" and route ~= "copy") } end
        rec = mark_route[rkey]
      end
      if m then
        any = true
        highest = k
        rec.ticks = rec.ticks + 1
        local cell = MARK_GRID[k]
        pcall(function()
          m:SetCanResizeWidth(true); m:SetCanResizeHeight(true)
          m:Resize(MARK_SIZE, MARK_SIZE)
          m:SetDockingPoint(3)               -- top right, like player_icon
          m:SetDockOffset(ix - MARK_STEP * cell[1], iy + MARK_STEP * cell[2])
          m:SetInteractive(true)             -- tooltip on hover ...
          m:SetDisabled(true)                -- ... but clicks still reach the lord button (same as player_icon)
          m:SetVisible(true)
          m:SetTooltipText(w.name or "?", true)
        end)
        -- live routes: the real avatar must be there by VERIFY_TICK (one SetContextObject retry), else the component
        -- is dropped, that route is skipped for this marker and the next tick recreates it by the next route
        if not rec.verified and rec.ticks >= VERIFY_TICK then
          if has_real_avatar(m) then
            rec.verified = true
          elseif not rec.retried then
            rec.retried = true
            local o = player_cco(w)
            if o then pcall(function() m:SetContextObject(o) end) end
            rec.ticks = 0
          else
            log("MARKERS: route " .. tostring(rec.route) .. " shows no real avatar (img0=" .. esc(img0(m), 80) .. ") -> next route")
            rec.skip[rec.route] = true
            pcall(function() m:Destroy() end)
            mark_route[rkey] = { skip = rec.skip }            -- keeps the skip list; no component until next tick
            m = nil
            highest = k                                       -- recreated next tick
          end
        end
        -- one-time geometry dump per created marker, once it is settled
        if m and not rec.geom_logged and (rec.verified or rec.ticks >= VERIFY_TICK) then
          rec.geom_logged = true
          log("MARKERS geometry (" .. tostring(rec.route) .. ", #" .. k .. " cell " .. cell[1] .. "," .. cell[2] .. "): vanilla "
            .. describe_uic(icon) .. " | marker " .. describe_uic(m) .. " | marker nid=" .. tostring(shown_nid(m) and hash8(shown_nid(m)))
            .. " want=" .. tostring(w.id8) .. " name=" .. esc(w.name or "?", 32))
        end
      end
    elseif m then
      pcall(function() m:Destroy() end)
      mark_route[rkey] = nil
    else
      mark_route[rkey] = nil
    end
  end
  marks_at[ckey] = highest
  return any
end

-- every component with this id under root (bounded depth-first search)
local function find_all(root, id, out, depth)
  if depth > 16 or #out >= 48 then return end
  local n = 0
  pcall(function() n = root:ChildCount() end)
  for i = 0, n - 1 do
    local c = nil
    pcall(function() c = UIComponent(root:Find(i)) end)
    if c then
      local cid = nil
      pcall(function() cid = c:Id() end)
      if cid == id then table.insert(out, c) else find_all(c, id, out, depth + 1) end
    end
  end
end

-- nearest ancestor (icon itself included) carrying a CcoFrontendFactionLeader context
local function leader_of(icon)
  local u = icon
  for _ = 1, 8 do
    if not u then return nil end
    local ok, lid = pcall(function() return u:GetContextObjectId("CcoFrontendFactionLeader") end)
    if ok and lid and lid ~= "" then return lid end
    local okp, par = pcall(function() return u:Parent() end)
    u = (okp and par) and UIComponent(par) or nil
  end
  return nil
end

-- the lobby's player_icons (with their lord_button container and leader id, resolved once per scan) and steam_avatars
local function scan_lobby(lobby)
  local found = {}
  find_all(lobby, "player_icon", found, 0)
  local list = {}
  for _, icon in ipairs(found) do
    local okp, par = pcall(function() return icon:Parent() end)
    local container = (okp and par) and UIComponent(par) or nil
    if container then
      local oka, addr = pcall(function() return container:Address() end)
      table.insert(list, { icon = icon, container = container, ckey = tostring(oka and addr or #list + 1),
                           lid = leader_of(icon) })
    end
  end
  icon_scan.list = list
  local avatars = {}
  find_all(lobby, AVATAR_ID, avatars, 0)
  icon_scan.avatars = avatars
end

local function update_markers(lobby, lb)
  local by_faction, shared = {}, false
  for _, s in ipairs(lb.slots) do
    if s.faction then
      by_faction[s.faction] = by_faction[s.faction] or {}
      table.insert(by_faction[s.faction], s)
      if #by_faction[s.faction] >= 2 then shared = true end
    end
  end
  if not shared and not markers_present then return end
  if not lobby then return end
  scan_tick = scan_tick + 1
  if scan_tick - icon_scan.at >= SCAN_TICKS then
    icon_scan.at = scan_tick
    scan_lobby(lobby)
  end
  local any, diag = false, {}
  for _, e in ipairs(icon_scan.list) do
    local icon, lid = e.icon, shared and e.lid or nil
    local want = {}
    local info = lid and leader_info(lid, by_faction) or nil
    local vis = false
    if info then
      -- only the tile whose vanilla icon is shown (= the lord those players picked) gets markers
      local okv, v = pcall(function() return icon:Visible() end)
      vis = not okv or v ~= false
    end
    if #diag < 6 and lid then
      table.insert(diag, "[" .. esc(lid, 64) .. " -> " .. tostring(info and info.faction) .. "/"
        .. tostring(info and info.general) .. " via " .. tostring(info and info.via) .. (vis and " visible" or " hidden") .. "]")
    end
    local group = vis and info and info.faction and by_faction[info.faction]
    if group and #group >= 2 then
      local shown = shown_nid(icon)
      local on_tile = {}
      for _, sl in ipairs(group) do
        if sl.lord == nil or info.general == nil or sl.lord == info.general then table.insert(on_tile, sl) end
      end
      if #on_tile >= 2 then
        local shown_ix = 1
        for ix, sl in ipairs(on_tile) do if shown and sl.nid == shown then shown_ix = ix end end
        for ix, sl in ipairs(on_tile) do
          if ix ~= shown_ix and #want < MAX_MARKS then table.insert(want, sl) end
        end
        if not marker_mode_logged then
          marker_mode_logged = true
          log("MARKERS: tile " .. tostring(info.general) .. " (" .. tostring(info.faction) .. ") has " .. #on_tile
            .. " players; vanilla icon shows id " .. tostring(shown and hash8(shown)) .. "; adding " .. #want
            .. " marker(s); cco=" .. type(cco))
        end
      end
    end
    local made = set_marks(e.container, e.ckey, icon, want)
    if made then any = true end
    -- last resort: every creation route failed -> list all names on the vanilla icon's tooltip
    if not made and #want > 0 then
      local names = {}
      for _, sl in ipairs(by_faction[info.faction] or {}) do table.insert(names, sl.name or "?") end
      pcall(function() icon:SetTooltipText(table.concat(names, "\n"), true) end)
      log_change("mk_fallback", "MARKERS: no marker could be created -> names listed on the vanilla icon's tooltip")
    end
  end
  if shared then
    log_change("markers_scan", "MARKERS scan: " .. #icon_scan.list .. " player_icon(s) found; tiles = "
      .. table.concat(diag, ", "))
  end
  markers_present = any
end

-- --------------------------------------------------------------------------------------------------
-- UI tick

local lobby_shown = false

local function lobby_left()
  RP = nil
  slot_cache = {}
  resumed_cache = false
  markers_present = false
  marker_mode_logged = false
  mark_route = {}
  marks_at = {}
  leader_cache = {}
  leader_ctx = {}
  icon_scan = { at = -1000, list = {}, avatars = {} }
  scan_tick = 0
  last_once["lobby_diag"] = nil
  last_once["rp"] = nil
end

local function ui_tick()
  local ok, err = pcall(function()
    local root = core:get_ui_root()
    if not lobby_visible() then
      -- lobby hidden: nothing runs except this visibility check; the label is hidden once on the transition
      if lobby_shown then
        lobby_shown = false
        lobby_left()
        local c = find_uicomponent(root, ID_LABEL)
        if c then pcall(function() c:SetVisible(false) end) end
      end
      log_change("lobby", "MP campaign lobby not visible -> label hidden")
      return
    end
    lobby_shown = true
    log_change("lobby", "MP campaign lobby visible -> label shown")
    pcall(function() set_label(ensure_label(root), label_text()) end)
    local okl, lb = pcall(read_lobby)
    if not okl or type(lb) ~= "table" then log_change("lobby_err", "LOBBY read error: " .. tostring(lb)); return end
    local ok1, e1 = pcall(diag_log, lb)
    if not ok1 then log_change("diag_err", "LOBBY diag error: " .. tostring(e1)) end
    local lobby = find_uicomponent(root, LOBBY_ID)
    local ok3, e3 = pcall(update_markers, lobby, lb)
    if not ok3 then log_change("mk_err", "MARKERS error: " .. tostring(e3)) end
    if lobby and lb.resumed then
      local ok4, e4 = pcall(force_race_picker, lobby)
      if not ok4 then log_change("rp_err", "RACE PICKER error: " .. tostring(e4)) end
    end
  end)
  if not ok then log_change("tick_err", "UI TICK ERROR: " .. tostring(err)) end
end

-- --------------------------------------------------------------------------------------------------
-- listeners

core:add_listener("archon_mode_fe_clicks", "ComponentLClickUp", true, function(context)
  local ok, err = pcall(function()
    if context.string == ID_LABEL then do_status(); ui_tick() end
  end)
  if not ok then log("CLICK HANDLER ERROR: " .. tostring(err)) end
end, true)

core:add_listener("archon_mode_fe_screens", "FrontendScreenTransition", true, function(context)
  pcall(function() log("FrontendScreenTransition -> " .. tostring(context.string)) end)
end, true)

-- --------------------------------------------------------------------------------------------------
-- load

log("archon_mode_frontend.lua v" .. VERSION .. " loaded; native_ok=" .. tostring(native() ~= nil))
pcall(do_status)
do
  local ok, err = pcall(auto_apply)
  if not ok then log("AUTO-APPLY error: " .. tostring(err)) end
end

do
  local ok, err = pcall(function()
    local tm = core:get_tm()
    if tm and tm.repeat_real_callback then
      tm:repeat_real_callback(ui_tick, TICK_MS, TIMER_NAME)
      log("UI tick registered (repeat_real_callback, " .. TICK_MS .. " ms)")
    else
      log("UI tick NOT registered: timer manager / repeat_real_callback unavailable")
    end
  end)
  if not ok then log("UI tick registration error: " .. tostring(err)) end
end
