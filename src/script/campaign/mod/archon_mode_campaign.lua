-- Archon Mode v1.0.0 - campaign loader ("archon core").
--
-- Loaded by the vanilla mod loader on NewSession (campaign only), i.e. before the first tick and therefore
-- before any vanilla script can call cm:progress_on_all_clients_ui_triggered(). It defines the logger (archon.util
-- from the boot loader script/_lib/mod/!archon_mode_native_boot.lua, which the vanilla mod loader always runs
-- first) and the error guard, then loads the core modules from /script/archon/campaign/ (outside the auto-loaded
-- mod folders) in a fixed order:
--   archon_net          single UITrigger dispatcher (+ GC guard), installed only when Archon is active
--   archon_session      Archon detection, per-machine id, session roster (archon_hello), partner leaving
--   archon_barrier      machine-counting progress_on_all_clients_ui_triggered + mp-query duplicate guard
--   archon_endturn      end-turn consensus inside a shared faction (votes + native patch F)
--   archon_panels       closes / locks stale decision panels on the synced model event (UI only)
--   archon_fingerprint  log-only sync fingerprint at FactionTurnStart (humans) and WorldStartRound (Archon only)
-- Native patch D (partner leave) is applied here at load whenever the DLL is
-- loaded and supported (0.6.0: no flag file any more): the session authority then no longer switches a faction to
-- IDLE_HUMAN while another connected player still controls it.
-- Every handler runs inside pcall. Nothing here writes to the model except through the vanilla callbacks that
-- the barrier runs (exactly as vanilla would, but on every machine at the same tick).

archon = archon or {}
local A = archon
A.VERSION = "1.0.0"

local MODULE_DIR = "/script/archon/campaign/"
local MODULES = { "archon_net", "archon_session", "archon_barrier", "archon_endturn", "archon_panels", "archon_fingerprint" }

-- ---------------------------------------------------------------------------------------------------------
-- logging: "[tag] msg" in archon_mode_log.txt next to the exe (local time) + the script log via out()

A.log = A.util.log

-- wrap a function so that any Lua error is logged and swallowed (returns nil on error)
local function pack(...) return { n = select("#", ...), ... } end

function A.safe(name, fn)
  return function(...)
    local res = pack(pcall(fn, ...))
    if not res[1] then
      A.log("ERROR", tostring(name) .. ": " .. tostring(res[2]))
      return nil
    end
    return unpack(res, 2, res.n)
  end
end

-- call immediately under pcall
function A.try(name, fn, ...)
  return A.safe(name, fn)(...)
end

-- core:add_listener with the callback (and a function condition) wrapped in pcall.
-- Never read context:trigger() in a condition or callback registered here: extra reads of one UITrigger message
-- crashed the engine (see archon_net.lua); only the single dispatcher reads it, exactly once.
function A.listen(name, event, condition, callback, persistent)
  local cond = condition
  if type(condition) == "function" then
    cond = function(context)
      local ok, r = pcall(condition, context)
      if not ok then A.log("ERROR", name .. " condition: " .. tostring(r)); return false end
      return r
    end
  end
  core:add_listener(name, event, cond, A.safe(name, callback), persistent ~= false)
end

-- first-tick hooks registered by modules: A.on_pre_first_tick(fn), A.on_first_tick(fn)
A._pre_first_tick, A._first_tick = {}, {}
function A.on_pre_first_tick(name, fn) table.insert(A._pre_first_tick, { name = name, fn = fn }) end
function A.on_first_tick(name, fn) table.insert(A._first_tick, { name = name, fn = fn }) end

-- ---------------------------------------------------------------------------------------------------------
-- native patch D (partner leave). Applied in every campaign session (it is behaviour-preserving when no faction
-- is shared: the DLL only suppresses the switch while another connected player still controls the faction).

function A.native_status_table()
  if not (A.native_ok and A.native_ok()) then return nil end
  local ok, st = pcall(A.native.status)
  if ok and type(st) == "table" then return st end
  return nil
end

-- one line describing patch D's state and its last decision (used at load and when a partner leaves)
function A.native_leave_line()
  local st = A.native_status_table()
  if not st then return "native not loaded (" .. tostring(A.native_status) .. ")" end
  return string.format("dll %s patch_leave=%s (supported=%s, %s) calls=%s kept_human=%s vanilla=%s errors=%s last=[%s]",
    tostring(st.version), tostring(st.patch_leave), tostring(st.leave_supported), tostring(st.leave_reason),
    tostring(st.leave_calls), tostring(st.leave_kept_human), tostring(st.leave_vanilla), tostring(st.leave_errors),
    tostring(st.leave_last))
end

A.try("native patch D", function()
  local st = A.native_status_table()
  if not st then A.log("native", "patch D skipped: native not loaded (" .. tostring(A.native_status) .. ")"); return end
  if not st.leave_supported then A.log("native", "patch D skipped: unresolved (" .. tostring(st.leave_reason) .. ")"); return end
  if type(A.native.patch_leave) ~= "function" then A.log("native", "patch D skipped: DLL has no patch_leave"); return end
  local ok, res = pcall(A.native.patch_leave)
  A.log("native", "patch D (partner leave) at campaign load -> patch_leave() = " .. tostring(ok) .. " " .. tostring(res)
    .. "; sender=" .. tostring(st.leave_send_va) .. " executor=" .. tostring(st.leave_handler_va)
    .. " set_idle=" .. tostring(st.leave_setidle_va))
end)

-- ---------------------------------------------------------------------------------------------------------
-- module loading (same VFS loadfile mechanism as the native boot loader)

local function load_module(name)
  local path = MODULE_DIR .. name
  local chunk, err = loadfile(path)
  if not chunk then
    local err2
    chunk, err2 = loadfile(path .. ".lua")
    if not chunk then
      A.log("load", "FAILED to load " .. path .. ": " .. tostring(err) .. " / " .. tostring(err2))
      return false
    end
  end
  pcall(setfenv, chunk, core:get_env())
  local ok, e = pcall(chunk)
  if not ok then A.log("load", "FAILED to run " .. path .. ": " .. tostring(e)); return false end
  A.log("load", "module " .. name .. " loaded")
  return true
end

A.log("load", "================ Archon Mode " .. A.VERSION .. " campaign layer loading (campaign "
  .. tostring(CampaignName) .. ", native_status=" .. tostring(A.native_status) .. ") ================")
A.modules_ok = {}
for _, name in ipairs(MODULES) do
  A.modules_ok[name] = A.try("load " .. name, load_module, name) == true
end

-- pre-first-tick: runs on every machine at the same model tick, before the mp/sp first-tick callback lists
A.try("register pre_first_tick", function()
  cm:add_pre_first_tick_callback(function(context)
    for _, h in ipairs(A._pre_first_tick) do A.try("pre_first_tick " .. h.name, h.fn, context) end
  end)
end)

-- mod entry point: called by core:execute_mods() inside the first-tick callback list
function archon_mode_campaign(context)
  for _, h in ipairs(A._first_tick) do A.try("first_tick " .. h.name, h.fn, context) end
end
