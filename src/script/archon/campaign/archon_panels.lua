-- Archon Mode v1.0.0 - stale decision panels (UI only, MP-safe).
--
-- With N machines on one faction, every decision panel opens on every machine. The first machine's choice is
-- sent as a lockstep command and resolves the decision everywhere; the other machines keep a stale panel whose
-- buttons do nothing (or, worse, send a second conflicting command). These handlers listen to the SYNCED model
-- event that reports the decision and then fix the local UI:
--   settlement occupation   CharacterPerformsSettlementOccupationDecision -> ClosePanel("settlement_captured")
--   post-battle captives    CharacterPostBattleEnslave/Release/Slaughter, CharacterPostBattleCaptureOption
--                           -> lock the captive option buttons in popup_battle_results (panel stays; its own
--                              continue/dismiss flow is untouched)
--   dilemmas                DilemmaChoiceMadeEvent -> ClosePanel("events") only if the dilemma shown there
--                           reports HasMadeChoice (CcoCampaignEventDilemma); otherwise nothing
--   AI diplomacy offers     PositiveDiplomaticEvent / DiplomaticOfferRejected with recipient = local faction,
--                           proposer = AI, outside our own turn -> ClosePanel("diplomacy_dropdown")
-- Precedent: wh3_campaign_great_bastion.lua:307-308 closes cathay_compass from a model listener.
-- Rules: only when Archon is active; never any model call or command; every handler in pcall; every action
-- logged (with a 400 ms real-timer re-check, so the test logs show whether ClosePanel worked).

local A = archon
local P = {}
A.panels = P

local LOG = function(msg) A.log("panels", msg) end

local function local_faction_key() return cm:get_local_faction_name(true) end

local function visible(uic)
  if not uic then return false end
  local ok, v = pcall(function() return uic:VisibleFromRoot() end)
  if ok then return v and true or false end
  ok, v = pcall(function() return uic:Visible() end)
  return ok and v and true or false
end

local function panel(name) return find_uicomponent(core:get_ui_root(), name) end

local function recheck_later(name, what)
  pcall(function()
    cm:real_callback(A.safe("panel recheck " .. name, function()
      LOG("  recheck " .. what .. ": panel " .. name .. " visible=" .. tostring(visible(panel(name))))
    end), 400)
  end)
end

local function close_panel(name, why)
  local uic = panel(name)
  if not visible(uic) then
    LOG(why .. ": panel " .. name .. " not open here (nothing to close)")
    return false
  end
  local ok, err = pcall(function() CampaignUI.ClosePanel(name) end)
  LOG(why .. ": ClosePanel(" .. name .. ") ok=" .. tostring(ok) .. (ok and "" or (" err=" .. tostring(err)))
    .. " visible_now=" .. tostring(visible(panel(name))))
  recheck_later(name, why)
  return ok
end

-- bounded depth-first walk over a component's descendants
local function walk(uic, depth, max_depth, visit, budget)
  if not uic or depth > max_depth or budget.n <= 0 then return end
  budget.n = budget.n - 1
  if visit(uic, depth) then return true end
  local okc, count = pcall(function() return uic:ChildCount() end)
  if not okc or not count then return end
  for i = 0, count - 1 do
    local okf, child = pcall(function() return UIComponent(uic:Find(i)) end)
    if okf and child then
      if walk(child, depth + 1, max_depth, visit, budget) then return true end
    end
  end
end

local function uic_id(uic)
  local ok, id = pcall(function() return uic:Id() end)
  return ok and tostring(id) or "?"
end

-- ---------------------------------------------------------------------------------------------------------
-- settlement occupation

A.listen("archon_panel_occupation", "CharacterPerformsSettlementOccupationDecision", true, function(context)
  if not A.is_active() then return end
  local fkey = context:character():faction():name()
  if fkey ~= local_faction_key() then return end
  local decision = tostring(context:occupation_decision())
  LOG("CharacterPerformsSettlementOccupationDecision faction=" .. fkey .. " decision=" .. decision
    .. " turn=" .. cm:model():turn_number())
  close_panel("settlement_captured", "occupation decided (" .. decision .. ")")
  local gift = panel("dlc27_settlement_captured_gift_faction_choice")
  if visible(gift) then close_panel("dlc27_settlement_captured_gift_faction_choice", "occupation decided (gift choice)") end
end, true)

-- ---------------------------------------------------------------------------------------------------------
-- post-battle captives: lock the option buttons (never close the results popup)

local function lock_captive_buttons(why)
  local root = panel("popup_battle_results")
  if not visible(root) then LOG(why .. ": popup_battle_results not open here"); return end
  local locked = {}
  walk(root, 0, 8, function(uic)
    local id = uic_id(uic)
    if string.sub(id, 1, 22) == "button_captive_option_" then
      pcall(function() uic:SetState("inactive") end)
      pcall(function() uic:SetDisabled(true) end)
      pcall(function() uic:SetTooltipText("Archon: the captive decision has already been made.", true) end)
      table.insert(locked, id)
    end
    return false
  end, { n = 400 })
  LOG(why .. ": locked captive buttons {" .. table.concat(locked, ",") .. "}")
end

local function captive_event(event_name)
  A.listen("archon_panel_" .. event_name, event_name, true, function(context)
    if not A.is_active() then return end
    local fkey = context:character():faction():name()
    if fkey ~= local_faction_key() then return end
    LOG(event_name .. " character faction=" .. fkey .. " turn=" .. cm:model():turn_number())
    lock_captive_buttons(event_name)
  end, true)
end
captive_event("CharacterPostBattleEnslave")
captive_event("CharacterPostBattleRelease")
captive_event("CharacterPostBattleSlaughter")

A.listen("archon_panel_capture_option", "CharacterPostBattleCaptureOption", true, function(context)
  if not A.is_active() then return end
  local lf = local_faction_key()
  local involved = false
  pcall(function() involved = lf and cm:pending_battle_cache_faction_is_involved(lf) or false end)
  if not involved then return end
  local rec = tostring(context:get_record_key())
  local outcome = tostring(context:get_outcome_key())
  LOG("CharacterPostBattleCaptureOption record=" .. rec .. " outcome=" .. outcome .. " local_involved=true")
  lock_captive_buttons("CharacterPostBattleCaptureOption " .. outcome)
end, true)

-- ---------------------------------------------------------------------------------------------------------
-- dilemmas: close the events panel only if the dilemma it shows has already been answered

local function find_dilemma_context(root)
  local found
  walk(root, 0, 4, function(uic)
    local ok, cco = pcall(function() return uic:GetContextObject("CcoCampaignEventDilemma") end)
    if ok and cco then found = { uic = uic, cco = cco }; return true end
    return false
  end, { n = 300 })
  return found
end

local function check_dilemma_panel(dilemma_key, why)
  local root = panel("events")
  if not visible(root) then LOG(why .. ": events panel not open here"); return end
  local d = find_dilemma_context(root)
  if not d then
    local ids = {}
    walk(root, 0, 2, function(uic, depth) table.insert(ids, depth .. ":" .. uic_id(uic)); return false end, { n = 40 })
    LOG(why .. ": events panel open but no CcoCampaignEventDilemma context found; not closing. tree={" .. table.concat(ids, " ") .. "}")
    return
  end
  local okh, has = pcall(function() return d.cco:Call("HasMadeChoice") end)
  local okk, key = pcall(function() return d.cco:Call("DilemmaContext.Key") end)
  local okc, choice = pcall(function() return d.cco:Call("ChoiceKey") end)
  LOG(why .. ": events panel shows dilemma key=" .. tostring(okk and key) .. " has_made_choice=" .. tostring(okh and has)
    .. " choice=" .. tostring(okc and choice) .. " (component " .. uic_id(d.uic) .. ")")
  if okh and has == true then
    close_panel("events", why .. " (stale dilemma " .. tostring(key) .. ")")
  end
end

A.listen("archon_panel_dilemma", "DilemmaChoiceMadeEvent", true, function(context)
  if not A.is_active() then return end
  local fkey = context:faction():name()
  local dilemma = tostring(context:dilemma())
  local choice = tostring(context:choice_key())
  LOG("DilemmaChoiceMadeEvent faction=" .. fkey .. " dilemma=" .. dilemma .. " choice=" .. choice
    .. " turn=" .. cm:model():turn_number())
  if fkey ~= local_faction_key() then return end
  local why = "dilemma " .. dilemma .. " decided (" .. choice .. ")"
  check_dilemma_panel(dilemma, why)
  -- the panel may refresh one frame later on the non-deciding machine: check once more
  pcall(function()
    cm:real_callback(A.safe("dilemma recheck", function() check_dilemma_panel(dilemma, why .. " [recheck]") end), 300)
  end)
end, true)

-- ---------------------------------------------------------------------------------------------------------
-- AI diplomacy offers to the shared faction

local function diplomacy_event(event_name)
  A.listen("archon_panel_" .. event_name, event_name, true, function(context)
    if not A.is_active() then return end
    local proposer, recipient = context:proposer(), context:recipient()
    local pk, rk = proposer:name(), recipient:name()
    local lf = local_faction_key()
    if rk ~= lf and pk ~= lf then return end
    local our_turn = false
    pcall(function() our_turn = cm:is_factions_turn_by_key(lf) end)
    LOG(event_name .. " proposer=" .. pk .. (proposer:is_human() and "(human)" or "(AI)") .. " recipient=" .. rk
      .. " our_turn=" .. tostring(our_turn) .. " turn=" .. cm:model():turn_number())
    if not A.is_active() then return end
    if rk ~= lf or proposer:is_human() or our_turn then return end
    close_panel("diplomacy_dropdown", event_name .. " AI offer from " .. pk .. " resolved")
  end, true)
end
diplomacy_event("PositiveDiplomaticEvent")
diplomacy_event("DiplomaticOfferRejected")

LOG("stale-panel handlers registered (occupation, captives, dilemmas, AI diplomacy offers)")
