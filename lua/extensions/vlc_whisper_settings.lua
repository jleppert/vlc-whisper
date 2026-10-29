-- vlc_whisper_settings.lua -- VLC-Whisper Settings GUI (Lua extension).
-- VLC 3.0.23 Lua 5.1 runtime. Validates with `luac -p` (Lua 5.1).
-- Reads/writes plugin config only. Network access for model downloads and subtitle translation is worker-confined;
-- no Lua callback performs HTTP, polling, sleeps, or other blocking translation work.

local dlg = nil
local w_engine = nil
local w_model = nil
local w_language = nil
local w_threads = nil
local w_logging = nil
local w_show_paused = nil
local w_trans_enabled = nil
local w_trans_from = nil
local w_trans_to = nil
local w_trans_mode = nil
local w_trans_test_btn = nil
local w_trans_test_result = nil
local w_status = nil
local w_model_status = nil
local w_download = nil

-- Robust config bridge: VLC 3.0 Lua exposes config as `config` in some builds
-- and `vlc.config` in others. Try both so the extension loads on either.
local function cfg_get(name)
  local ok, val = pcall(function()
    if vlc and vlc.config and vlc.config.get then return vlc.config.get(name) end
    if config and config.get then return config.get(name) end
    return nil
  end)
  if ok then return val end
  return nil
end
local function cfg_set(name, value)
  local ok, result = pcall(function()
    if vlc and vlc.config and vlc.config.set then return vlc.config.set(name, value) ~= false end
    if config and config.set then return config.set(name, value) ~= false end
    return false
  end)
  return ok and result == true
end

local function logging_enabled()
  local value = cfg_get("whisper-logging")
  return value == true or value == 1 or value == "1" or value == "true"
end

local function log_info(message)
  if logging_enabled() then vlc.msg.info(message) end
end

local function log_error(message)
  if logging_enabled() then vlc.msg.err(message) end
end

-- id -> string maps for dropdown get_value() results (Lua 5.1-safe).
local engine_map = { [1] = "auto", [2] = "gpu", [3] = "cpu" }
local engine_labels = { [1] = "auto (default)", [2] = "GPU (Vulkan)", [3] = "CPU only" }
local model_map = {
  [1] = "tiny.en",
  [2] = "tiny",
  [3] = "base.en",
  [4] = "base",
  [5] = "small",
  [6] = "medium",
  [7] = "large",
  [8] = "large-v3-turbo",
}
-- Catalog ids mirror model_map labels (plugin catalog ids are these strings).
local catalog_id_map = {
  [1] = "tiny.en",
  [2] = "tiny",
  [3] = "base.en",
  [4] = "base",
  [5] = "small",
  [6] = "medium",
  [7] = "large",
  [8] = "large-v3-turbo",
}
-- Relative model paths under models/ (selection allowed even if file absent;
-- expected E_MODEL_MISSING disables captions until provisioned -- see README).
local model_path_map = {
  [1] = "models/ggml-tiny.en.bin",
  [2] = "models/ggml-tiny.bin",
  [3] = "models/ggml-base.en.bin",
  [4] = "models/ggml-base.bin",
  [5] = "models/ggml-small.bin",
  [6] = "models/ggml-medium.bin",
  [7] = "models/ggml-large-v3.bin",
  [8] = "models/ggml-large-v3-turbo.bin",
}
-- Reverse lookup: path -> id (for preselection from current model-path).
-- Derived with a numeric loop: the VLC 3.0 scan pass runs this file in a bare
-- Lua state with NO standard libraries (no pairs/ipairs), so top-level code
-- must be library-free. `#` is an operator, not a library call.
local model_path_to_id = {}
for _id = 1, #model_path_map do
  model_path_to_id[model_path_map[_id]] = _id
end

-- Language dropdown: concrete codes ONLY -- no "auto" entry in this dialog.
local language_map = {
  [1] = "en",
  [2] = "ro",
  [3] = "tr",
  [4] = "de",
  [5] = "fr",
  [6] = "es",
}
local language_labels = {
  [1] = "English (en)",
  [2] = "Romanian (ro)",
  [3] = "Turkish (tr)",
  [4] = "German (de)",
  [5] = "French (fr)",
  [6] = "Spanish (es)",
}

local engine_to_id = { ["auto"] = 1, ["gpu"] = 2, ["cpu"] = 3 }
local language_to_id = { ["en"] = 1, ["ro"] = 2, ["tr"] = 3, ["de"] = 4, ["fr"] = 5, ["es"] = 6 }

local trans_from_map = {
  [1] = "auto",
  [2] = "en",
  [3] = "ro",
  [4] = "es",
  [5] = "fr",
  [6] = "de",
  [7] = "it",
  [8] = "pt",
  [9] = "ru",
  [10] = "uk",
  [11] = "tr",
  [12] = "ja",
  [13] = "ko",
  [14] = "zh",
}
local trans_from_labels = {
  [1] = "Auto detect (auto)",
  [2] = "English (en)",
  [3] = "Romanian (ro)",
  [4] = "Spanish (es)",
  [5] = "French (fr)",
  [6] = "German (de)",
  [7] = "Italian (it)",
  [8] = "Portuguese (pt)",
  [9] = "Russian (ru)",
  [10] = "Ukrainian (uk)",
  [11] = "Turkish (tr)",
  [12] = "Japanese (ja)",
  [13] = "Korean (ko)",
  [14] = "Chinese (zh)",
}
local trans_from_to_id = {}
for _id = 1, #trans_from_map do
  trans_from_to_id[trans_from_map[_id]] = _id
end

local trans_to_map = {
  [1] = "en",
  [2] = "ro",
  [3] = "es",
  [4] = "fr",
  [5] = "de",
  [6] = "it",
  [7] = "pt",
  [8] = "ru",
  [9] = "uk",
  [10] = "tr",
  [11] = "ja",
  [12] = "ko",
  [13] = "zh",
}
local trans_to_labels = {
  [1] = "English (en)",
  [2] = "Romanian (ro)",
  [3] = "Spanish (es)",
  [4] = "French (fr)",
  [5] = "German (de)",
  [6] = "Italian (it)",
  [7] = "Portuguese (pt)",
  [8] = "Russian (ru)",
  [9] = "Ukrainian (uk)",
  [10] = "Turkish (tr)",
  [11] = "Japanese (ja)",
  [12] = "Korean (ko)",
  [13] = "Chinese (zh)",
}
local trans_to_to_id = {}
for _id = 1, #trans_to_map do
  trans_to_to_id[trans_to_map[_id]] = _id
end

local trans_mode_map = { [1] = 1, [2] = 0 }
local trans_mode_labels = {
  [1] = "Show source + translation (dual line)",
  [2] = "Show translation only",
}

local default_model_id = 2
local default_model_path = model_path_map[default_model_id] or "models/ggml-tiny.bin"

local function model_is_english_only(model_id)
  return model_id == 1 or model_id == 3
end

local function env_get(name)
  local ok, value = pcall(function()
    if os and os.getenv then return os.getenv(name) end
    return nil
  end)
  if ok then return value end
  return nil
end

local function join_path(base, suffix)
  if base == nil or base == "" then return nil end
  if base:sub(-1) == "/" or base:sub(-1) == "\\" then return base .. suffix end
  return base .. "/" .. suffix
end

local function config_dir(name)
  local ok, value = pcall(function()
    if vlc and vlc.config and vlc.config[name] then return vlc.config[name]() end
    if config and config[name] then return config[name]() end
    return nil
  end)
  if ok then return value end
  return nil
end

local function file_exists(path)
  if path == nil or path == "" then return false end
  local ok, file = pcall(function()
    if vlc and vlc.io and vlc.io.open then return vlc.io.open(path, "rb") end
    return nil
  end)
  if not ok or file == nil then return false end
  pcall(function() file:close() end)
  return true
end

local function model_filename(model_id)
  local relative = model_path_map[model_id]
  if relative == nil then return nil end
  return relative:match("([^/\\]+)$")
end

local function model_candidate_paths(model_id)
  local filename = model_filename(model_id)
  if filename == nil then return nil, nil end

  local bundled_dir = join_path(config_dir("datadir"), "models")
  local local_appdata = env_get("LOCALAPPDATA")
  if local_appdata == nil or local_appdata == "" then
    local user_profile = env_get("USERPROFILE")
    if user_profile ~= nil and user_profile ~= "" then local_appdata = join_path(user_profile, "AppData/Local") end
  end
  local data_root = local_appdata or env_get("XDG_DATA_HOME")
  if data_root == nil or data_root == "" then
    data_root = join_path(env_get("HOME"), ".local/share")
  end
  local user_dir = join_path(join_path(data_root, "vlc-whisper"), "models")
  return join_path(bundled_dir, filename), join_path(user_dir, filename)
end

local function model_availability(model_id)
  local bundled_path, user_path = model_candidate_paths(model_id)
  return {
    bundled = file_exists(bundled_path),
    user = file_exists(user_path),
  }
end

local function model_availability_text(availability)
  if availability.bundled and availability.user then return "Model: available (bundled + downloaded)" end
  if availability.bundled then return "Model: available (bundled)" end
  if availability.user then return "Model: available (downloaded)" end
  return "Model: not installed (download required)"
end

local function clamp_threads(v)
  local n = tonumber(v)
  if n == nil then n = 4 end
  n = math.floor(n)
  if n < 1 then n = 1 end
  if n > 16 then n = 16 end
  return n
end

local function resolve_model_id_from_path(path)
  if path == nil or path == "" then return default_model_id end
  if model_path_to_id[path] ~= nil then return model_path_to_id[path] end
  for _id = 1, #model_path_map do
    local _rel = model_path_map[_id]
    local fname = _rel:match("([^/\\]+)$")
    if fname and path:find(fname, 1, true) then
      return _id
    end
  end
  for _id = 1, #model_map do
    local _label = model_map[_id]
    if _label and path:find(_label, 1, true) then
      return _id
    end
  end
  return default_model_id
end

-- VLC 3.0 selects the first added dropdown value and exposes no selection setter.
local function populate_dropdown(widget, labels, selected_id)
  widget:add_value(labels[selected_id], selected_id)
  for _id = 1, #labels do
    if _id ~= selected_id then widget:add_value(labels[_id], _id) end
  end
end

local function refresh_language_dropdown(_model_id, selected_id)
  if w_language == nil then return end
  w_language:clear()
  populate_dropdown(w_language, language_labels, selected_id or 1)
end

local function refresh_model_status(model_id)
  local availability = model_availability(model_id)
  if w_model_status ~= nil then
    pcall(function() w_model_status:set_text(model_availability_text(availability)) end)
  end
  if w_download ~= nil then
    local caption = (availability.bundled or availability.user) and "Re-download Selected Model"
      or "Download Selected Model"
    pcall(function() w_download:set_text(caption) end)
  end
  return availability
end

-- The original Step 21b Test button performed a synchronous GTX request from this Lua callback. VLC extensions run
-- cooperatively on the UI path, so network I/O here could freeze the settings/UI and bypassed the worker-only privacy
-- boundary. Until a dedicated asynchronous worker test-result IPC is added, this button is intentionally a safe
-- runtime-test guide: it performs no HTTP and never logs user subtitle/test text.
local function on_test_translate()
  local from_id = w_trans_from and w_trans_from:get_value() or 1
  local to_id = w_trans_to and w_trans_to:get_value() or 1
  local from_code = trans_from_map[from_id] or "auto"
  local to_code = trans_to_map[to_id] or "en"

  if w_trans_test_result ~= nil then
    pcall(function()
      w_trans_test_result:set_text(
        "Worker-only test: enable Auto translation, Apply, then play media (" .. from_code .. " -> " .. to_code .. ")."
      )
    end)
  end
  log_info("[VLC-Whisper] worker-only translation test guidance shown (" .. from_code .. " -> " .. to_code .. ")")
end

local function on_apply()
  local eng_id = w_engine and w_engine:get_value() or 1
  local mod_id = w_model and w_model:get_value() or default_model_id
  local lang_id = w_language and w_language:get_value() or 1
  local thr_text = w_threads and w_threads:get_text() or "4"
  local logging = w_logging and w_logging:get_checked() or false
  local show_paused = w_show_paused == nil or w_show_paused:get_checked()

  local trans_en = w_trans_enabled and w_trans_enabled:get_checked() or false
  local trans_from_id = w_trans_from and w_trans_from:get_value() or 1
  local trans_to_id = w_trans_to and w_trans_to:get_value() or 1
  local trans_mode_id = w_trans_mode and w_trans_mode:get_value() or 1

  local engine = engine_map[eng_id] or "auto"
  local model_label = model_map[mod_id] or "tiny"
  local model_path = model_path_map[mod_id] or default_model_path
  local language = model_is_english_only(mod_id) and "en" or language_map[lang_id] or "en"
  local threads = clamp_threads(thr_text)
  local trans_from = trans_from_map[trans_from_id] or "auto"
  local trans_to = trans_to_map[trans_to_id] or "en"
  local trans_mode = trans_mode_map[trans_mode_id] or 1

  if model_is_english_only(mod_id) then lang_id = 1 end
  pcall(function() refresh_language_dropdown(mod_id, lang_id) end)

  if w_threads ~= nil then
    pcall(function() w_threads:set_text(tostring(threads)) end)
  end

  pcall(function() cfg_set("whisper-backend", engine) end)
  pcall(function() cfg_set("model-path", model_path) end)
  pcall(function() cfg_set("whisper-language", language) end)
  pcall(function() cfg_set("whisper-threads", threads) end)
  pcall(function() cfg_set("whisper-logging", logging) end)
  pcall(function() cfg_set("whisper-show-paused", show_paused) end)
  pcall(function() cfg_set("whisper-translate-enabled", trans_en) end)
  pcall(function() cfg_set("whisper-translate-from", trans_from) end)
  pcall(function() cfg_set("whisper-translate-to", trans_to) end)
  pcall(function() cfg_set("whisper-translate-mode", trans_mode) end)

  if logging then
    vlc.msg.info("[VLC-Whisper] applied whisper-backend=" .. engine)
    vlc.msg.info("[VLC-Whisper] applied model-path=" .. model_path .. " (" .. model_label .. ")")
    vlc.msg.info("[VLC-Whisper] applied whisper-language=" .. language)
    vlc.msg.info("[VLC-Whisper] applied whisper-threads=" .. tostring(threads))
    vlc.msg.info("[VLC-Whisper] applied whisper-logging=true")
    vlc.msg.info("[VLC-Whisper] applied whisper-show-paused=" .. tostring(show_paused))
    vlc.msg.info("[VLC-Whisper] applied whisper-translate-enabled=" .. tostring(trans_en))
    vlc.msg.info("[VLC-Whisper] applied whisper-translate-from=" .. trans_from)
    vlc.msg.info("[VLC-Whisper] applied whisper-translate-to=" .. trans_to)
    vlc.msg.info("[VLC-Whisper] applied whisper-translate-mode=" .. tostring(trans_mode))
  end

  if w_status ~= nil then
    local active = nil
    pcall(function() active = cfg_get("whisper-backend-active") end)
    if active == nil or active == "" then active = "(pending -- start playback)" end
    pcall(function() w_status:set_text("Detected backend: " .. tostring(active)) end)
  end
  refresh_model_status(mod_id)
end

local function on_abort_download()
  pcall(function() cfg_set("whisper-model-download", "abort") end)
  pcall(function() cfg_set("whisper-model-status", "aborting") end)
  log_info("[VLC-Whisper] abort requested")
  pcall(function()
    if w_status ~= nil then
      w_status:set_text("Model download: aborting...")
    end
  end)
end

local function on_download()
  local sel_id = nil
  pcall(function()
    if w_model ~= nil then sel_id = w_model:get_value() end
  end)
  if sel_id == nil then
    local cur_path = nil
    pcall(function() cur_path = cfg_get("model-path") end)
    sel_id = resolve_model_id_from_path(cur_path)
  end
  local catalog_id = catalog_id_map[sel_id] or "tiny"
  local selected_language = 1
  if model_is_english_only(sel_id) then
    selected_language = 1
  else
    pcall(function()
      if w_language ~= nil then selected_language = w_language:get_value() end
    end)
    if selected_language == nil or selected_language < 1 then selected_language = 1 end
  end
  pcall(function() refresh_language_dropdown(sel_id, selected_language) end)
  local availability = refresh_model_status(sel_id)
  if availability.bundled or availability.user then
    log_info("[VLC-Whisper] model already present; download remains available for refresh: " .. tostring(catalog_id))
  end

  local is_playing = false
  pcall(function()
    if vlc and vlc.input and vlc.input.is_playing then
      is_playing = vlc.input.is_playing()
    elseif vlc and vlc.playlist and vlc.playlist.is_playing then
      is_playing = vlc.playlist.is_playing()
    end
  end)

  local control_written = cfg_set("whisper-model-download", catalog_id)
  local control_visible = nil
  pcall(function()
    local pending = cfg_get("whisper-model-download")
    if pending ~= nil then control_visible = tostring(pending) == catalog_id end
  end)
  cfg_set("whisper-model-status", "downloading")
  cfg_set("whisper-model-progress", 0)
  if not control_written or control_visible == false then
    log_error("[VLC-Whisper] download control was not retained by VLC config: " .. tostring(catalog_id))
    if w_status ~= nil then
      pcall(function() w_status:set_text("Model config unavailable; restart VLC and retry") end)
    end
    return
  end
  log_info("[VLC-Whisper] download requested " .. tostring(catalog_id))

  if w_status ~= nil then
    local status = is_playing and "download requested" or "queued (play media to start worker)"
    pcall(function() w_status:set_text("Model " .. tostring(catalog_id) .. ": " .. status) end)
  end
end

local function build_dialog()
  dlg = vlc.dialog("VLC-Whisper Settings")

  local cur_backend = nil
  local cur_model_path = nil
  local cur_language = nil
  local cur_threads = nil
  local cur_logging = nil
  local cur_show_paused = nil
  local cur_active = nil
  local cur_trans_enabled = nil
  local cur_trans_from = nil
  local cur_trans_to = nil
  local cur_trans_mode = nil
  pcall(function() cur_backend = cfg_get("whisper-backend") end)
  pcall(function() cur_model_path = cfg_get("model-path") end)
  pcall(function() cur_language = cfg_get("whisper-language") end)
  pcall(function() cur_threads = cfg_get("whisper-threads") end)
  pcall(function() cur_logging = cfg_get("whisper-logging") end)
  pcall(function() cur_show_paused = cfg_get("whisper-show-paused") end)
  pcall(function() cur_active = cfg_get("whisper-backend-active") end)
  pcall(function() cur_trans_enabled = cfg_get("whisper-translate-enabled") end)
  pcall(function() cur_trans_from = cfg_get("whisper-translate-from") end)
  pcall(function() cur_trans_to = cfg_get("whisper-translate-to") end)
  pcall(function() cur_trans_mode = cfg_get("whisper-translate-mode") end)

  if cur_backend == nil or cur_backend == "" then cur_backend = "auto" end
  if cur_model_path == nil or cur_model_path == "" then cur_model_path = default_model_path end
  if cur_language == nil or cur_language == "" then cur_language = "en" end
  if cur_threads == nil or cur_threads == "" then cur_threads = "4" end
  cur_logging = cur_logging == true or cur_logging == 1 or cur_logging == "1" or cur_logging == "true"
  cur_show_paused = cur_show_paused == nil or cur_show_paused == true or cur_show_paused == 1 or
    cur_show_paused == "1" or cur_show_paused == "true"
  cur_threads = tostring(cur_threads)
  if cur_active == nil or cur_active == "" then cur_active = "(pending -- start playback)" end

  cur_trans_enabled =
    cur_trans_enabled == true or cur_trans_enabled == 1 or cur_trans_enabled == "1" or cur_trans_enabled == "true"
  if cur_trans_from == nil or cur_trans_from == "" then cur_trans_from = "auto" end
  if cur_trans_to == nil or cur_trans_to == "" then cur_trans_to = "en" end
  local sel_trans_from = trans_from_to_id[cur_trans_from] or 1
  local sel_trans_to = trans_to_to_id[cur_trans_to] or 1
  local sel_trans_mode = (cur_trans_mode == 0 or cur_trans_mode == "0") and 2 or 1

  local sel_engine = engine_to_id[cur_backend] or 1
  local sel_model = resolve_model_id_from_path(cur_model_path)
  local sel_language = language_to_id[cur_language] or 1
  if model_is_english_only(sel_model) then sel_language = 1 end

  dlg:add_label("Engine:", 1, 1, 1, 1)
  w_engine = dlg:add_dropdown(2, 1, 3, 1)
  populate_dropdown(w_engine, engine_labels, sel_engine)

  dlg:add_label("Model:", 1, 2, 1, 1)
  w_model = dlg:add_dropdown(2, 2, 3, 1)
  local model_labels = {}
  for _id = 1, #model_path_map do
    local label = model_map[_id] or "model"
    if _id == 2 or _id == 4 then
      label = label .. " (multilingual)"
    end
    if _id == default_model_id then
      label = label .. " (bundled default)"
    end
    model_labels[_id] = label
  end
  populate_dropdown(w_model, model_labels, sel_model)

  dlg:add_label("Language:", 1, 3, 1, 1)
  w_language = dlg:add_dropdown(2, 3, 3, 1)
  refresh_language_dropdown(sel_model, sel_language)

  dlg:add_label("Threads (CPU engine):", 1, 4, 1, 1)
  w_threads = dlg:add_text_input(cur_threads, 2, 4, 3, 1)

  w_logging = dlg:add_check_box("Enable diagnostic logging", cur_logging, 1, 5, 4, 1)

  w_show_paused =
    dlg:add_check_box("Show subtitles while paused (local files only)", cur_show_paused, 1, 6, 4, 1)

  w_trans_enabled = dlg:add_check_box("Auto translation (real-time subtitles)", cur_trans_enabled, 1, 7, 4, 1)

  dlg:add_label("Source (from):", 1, 8, 1, 1)
  w_trans_from = dlg:add_dropdown(2, 8, 3, 1)
  populate_dropdown(w_trans_from, trans_from_labels, sel_trans_from)

  dlg:add_label("Translation (to):", 1, 9, 1, 1)
  w_trans_to = dlg:add_dropdown(2, 9, 3, 1)
  populate_dropdown(w_trans_to, trans_to_labels, sel_trans_to)

  dlg:add_label("Screen placement:", 1, 10, 1, 1)
  w_trans_mode = dlg:add_dropdown(2, 10, 3, 1)
  populate_dropdown(w_trans_mode, trans_mode_labels, sel_trans_mode)

  dlg:add_label("Translation test:", 1, 11, 1, 1)
  w_trans_test_btn = dlg:add_button("How to test", on_test_translate, 2, 11, 3, 1)

  w_trans_test_result =
    dlg:add_label("Worker runtime performs translation; this dialog never makes HTTP requests.", 1, 12, 4, 1)

  dlg:add_button("Apply", on_apply, 1, 13, 2, 1)
  w_download = dlg:add_button("Download Selected Model", on_download, 3, 13, 2, 1)
  refresh_model_status(sel_model)

  w_status = dlg:add_label("Detected backend: " .. tostring(cur_active), 1, 14, 4, 1)

  w_model_status = dlg:add_label("Model availability: checking...", 1, 15, 4, 1)
  refresh_model_status(sel_model)

  dlg:add_label(".en models force English; enabling translation sends finalized subtitle text to Google.", 1, 16, 4, 1)
end

function descriptor()
  return {
    title = "VLC-Whisper Settings",
    version = "0.1.0",
    author = "vlc-whisper",
    url = "https://github.com/rzv04/vlc-whisper",
    shortdesc = "VLC-Whisper Settings",
    description = "Settings GUI for VLC-Whisper (Lua extension). "
      .. "Engine/Model/Language dropdowns, Threads, and Real-Time Translation controls. "
      .. "Apply writes whisper-backend, model-path, whisper-language, whisper-threads, and translation config; "
      .. "plugin polls config and syncs worker mid-play without playback interruption.",
    capabilities = { "menu" },
  }
end

function activate()
  log_info("[VLC-Whisper] extension activate -- building dialog")
  if dlg ~= nil then
    pcall(function() dlg:hide() end)
    dlg = nil
  end
  w_engine = nil
  w_model = nil
  w_language = nil
  w_threads = nil
  w_logging = nil
  w_show_paused = nil
  w_trans_enabled = nil
  w_trans_from = nil
  w_trans_to = nil
  w_trans_mode = nil
  w_trans_test_btn = nil
  w_trans_test_result = nil
  w_status = nil
  w_model_status = nil
  w_download = nil
  build_dialog()
  dlg:show()
  log_info("[VLC-Whisper] dialog shown with translation controls")
  return true
end

function deactivate()
  log_info("[VLC-Whisper] extension deactivate")
  if dlg ~= nil then
    pcall(function() dlg:hide() end)
    dlg = nil
  end
  w_engine = nil
  w_model = nil
  w_language = nil
  w_threads = nil
  w_logging = nil
  w_show_paused = nil
  w_trans_enabled = nil
  w_trans_from = nil
  w_trans_to = nil
  w_trans_mode = nil
  w_trans_test_btn = nil
  w_trans_test_result = nil
  w_status = nil
  w_model_status = nil
  w_download = nil
end

function close()
  log_info("[VLC-Whisper] extension close (user closed dialog)")
  pcall(function() vlc.deactivate() end)
end

function menu()
  return { "VLC-Whisper Settings", "Download selected model", "Abort model download" }
end

function trigger_menu(id)
  log_info("[VLC-Whisper] trigger_menu id=" .. tostring(id))
  if id == 2 then
    on_download()
    return
  end
  if id == 3 then
    on_abort_download()
    return
  end
  if dlg == nil then
    activate()
  else
    pcall(function() dlg:show() end)
  end
end

function meta_changed()
end

function input_changed()
end

function playing_changed()
end
