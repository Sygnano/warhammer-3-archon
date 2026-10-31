-- Archon Mode v1.0.0 - native loader + shared helpers (runs in frontend, campaign and battle; never raises).
--
-- Load order (vanilla lib_mod_loader.lua): core:load_mods("/script/_lib/mod/", "/script/frontend/mod/") in the
-- frontend and core:load_mods("/script/_lib/mod/", "/script/campaign/mod/", ...) in the campaign, every chunk with
-- the same global environment (core:get_env()). So this file always runs BEFORE archon_mode_frontend.lua and
-- archon_mode_campaign.lua, and the global table `archon` it fills is visible to both.
--
-- 1. archon.util: the helpers both loaders share (log line format, hash8, clean, esc, str, ctx).
-- 2. the native DLL: it writes the
--    embedded archon_native_1_0_0.dll next to the exe only if absent or different, loads it with package.loadlib and
--    records status(). If anything fails, archon.native stays nil and the pure-Lua layer carries on unchanged.
--    Loading the DLL never patches anything; the frontend script applies the lobby patches A/B/C and E, the campaign
--    loader applies patch D, whenever the DLL is loaded and supported (0.6.0: no flag file any more). Patch G is the
--    exception, applied here: it has to be in place in a battle, and it does nothing unless two players share an army.
-- The '!' makes it sort first.

archon = archon or {}

-- ---------------------------------------------------------------------------------------------------------
-- shared helpers

local U = {}
archon.util = U
U.LOG_FILE = "archon_mode_log.txt"

-- one line "[tag] msg" in archon_mode_log.txt (local time) and in the script log (out)
function U.log(tag, msg)
  local line = "[" .. tostring(tag) .. "] " .. tostring(msg)
  pcall(function()
    local f = io.open(U.LOG_FILE, "a")
    if f then f:write(os.date("%Y-%m-%d %H:%M:%S "), line, "\n"); f:close() end
  end)
  pcall(out, "[archon_mode] " .. line)
end

-- 8-hex hash that stays exact with the game's 32-bit float lua_Number (all intermediates < 2^24).
-- Session ids are compared across machines: the result must never change.
function U.hash8(s)
  s = tostring(s)
  local h1, h2 = 7, 13
  for i = 1, #s do
    local b = string.byte(s, i)
    h1 = (h1 * 31 + b) % 65521
    h2 = (h2 * 37 + b) % 65519
  end
  return string.format("%04x%04x", h1, h2)
end

-- names for log lines / messages: only [%w-_.], truncated
function U.clean(s, maxlen)
  s = string.gsub(tostring(s or "?"), "[^%w%-_%.]", "_")
  return string.sub(s, 1, maxlen or 24)
end

-- free text for log lines: control characters -> space, optional truncation
function U.esc(s, maxlen)
  s = tostring(s)
  if maxlen and #s > maxlen then s = string.sub(s, 1, maxlen) .. "..." end
  return (string.gsub(s, "%c", " "))
end

-- nil / false / "" -> nil, anything else -> its string
function U.str(v)
  if v == nil or v == false then return nil end
  local s = tostring(v)
  if s == "" then return nil end
  return s
end

-- common.get_context_value under pcall: 1 argument = global expression, 3 arguments = (type, id, expression)
function U.ctx(a1, a2, a3)
  if type(common) ~= "table" or type(common.get_context_value) ~= "function" then return nil end
  local ok, v
  if a2 == nil and a3 == nil then ok, v = pcall(common.get_context_value, a1)
  else ok, v = pcall(common.get_context_value, a1, a2, a3) end
  if ok then return v end
  return nil
end

-- ---------------------------------------------------------------------------------------------------------
-- native DLL

local LOG = "archon_native_log.txt"      -- the DLL also appends here
local BIN_PATH    = "/script/archon/native/bin_1_0_0"
local DLL_NAME    = "archon_native_1_0_0.dll"
local OPEN_SYMBOL = "luaopen_archon_native"

-- both logs only ever grow, and nobody clears them on a player's machine
local MAX_LOG_BYTES = 5000000
for _, name in ipairs({ U.LOG_FILE, LOG }) do
  pcall(function()
    local f = io.open(name, "rb")
    if not f then return end
    local size = f:seek("end")
    f:close()
    if size and size > MAX_LOG_BYTES then
      local w = io.open(name, "w")
      if w then w:close() end
    end
  end)
end

local function log(s)
  pcall(function()
    local f = io.open(LOG, "a")
    if f then f:write(os.date("%Y-%m-%d %H:%M:%S "), "[boot 1.0.0] ", tostring(s), "\n"); f:close() end
  end)
  if type(ModLog) == "function" then pcall(ModLog, "[archon_native] " .. tostring(s)) end
end

archon.native, archon.native_status = nil, "not loaded"
function archon.native_ok() return archon.native ~= nil and archon.native_status == "ok" end

local ok, err = pcall(function()
  -- load the embedded blob through the VFS (outside the auto-loaded mod folders)
  local loader = loadfile(BIN_PATH)
  if not loader then loader = loadfile(BIN_PATH .. ".lua") end
  assert(loader, "cannot loadfile " .. BIN_PATH)
  local bin = loader()
  local data = table.concat(bin.chunks)
  assert(#data == bin.size, "embedded DLL truncated (" .. #data .. " vs " .. tostring(bin.size) .. ")")

  -- write next to the exe only if absent or different
  local cur
  local rf = io.open(DLL_NAME, "rb")
  if rf then cur = rf:read("*a"); rf:close() end
  if cur ~= data then
    local wf = assert(io.open(DLL_NAME, "wb"), "cannot open " .. DLL_NAME .. " for write")
    wf:write(data); wf:close()
    local vf = io.open(DLL_NAME, "rb"); local back = vf and vf:read("*a"); if vf then vf:close() end
    assert(back == data, "write-back mismatch (file locked, or blocked by antivirus / Smart App Control?)")
    log("wrote " .. DLL_NAME .. " (" .. #data .. " bytes)")
  else
    log(DLL_NAME .. " already present and identical")
  end

  local open = assert(package.loadlib(".\\" .. DLL_NAME, OPEN_SYMBOL))
  local m = open()                 -- fresh table for this Lua state; the DLL pins itself
  assert(type(m) == "table" and type(m.status) == "function", "DLL did not return the expected table")
  local st = m.status()
  if not st.supported then error("native refused: " .. tostring(st.reason)) end
  archon.native, archon.native_status = m, "ok"
  log(string.format("loaded v%s exe_tested=%s supported=%s canset=%s readybit=%s dupstr=%s lea=%s producer=%s producer_cont=%s "
      .. "leave_supported=%s (%s) leave_send=%s patch_leave=%s resume_supported=%s (%s) resume_pick=%s endturn_supported=%s (%s) gift_supported=%s (%s)",
      tostring(st.version), tostring(st.exe_tested), tostring(st.supported), tostring(st.canset_va),
      tostring(st.readybit_va), tostring(st.dupstring_va), tostring(st.lea_va),
      tostring(st.producer_va), tostring(st.producer_cont_va),
      tostring(st.leave_supported), tostring(st.leave_reason), tostring(st.leave_send_va), tostring(st.patch_leave),
      tostring(st.resume_supported), tostring(st.resume_reason), tostring(st.resume_pick_va),
      tostring(st.endturn_supported), tostring(st.endturn_reason), tostring(st.gift_supported), tostring(st.gift_reason)))
  log("patch G -> " .. tostring(m.patch_gift()))
end)

if not ok then
  archon.native, archon.native_status = nil, tostring(err)
end
log("native_status=" .. tostring(archon.native_status))
