# VLC-Whisper Settings GUI — Lua Extension (Step 19c)

Wired Lua extension dialog for VLC 3.0.23 that reads/writes the plugin config namespace
and submits worker commands through the plugin’s ~2 s config bridge. Lua never waits, sleeps, polls download state,
or performs network I/O. Translation is handled by the worker's bounded async engine (21b) via TRANSLATE_CTRL.
## Files

- `lua/extensions/vlc_whisper_settings.lua` — the extension (descriptor `VLC-Whisper Settings`, dialog with Engine/Model/Language/Threads (CPU engine), diagnostic-logging checkbox, and Detected-backend status label).

## Config keys (plugin registers; Lua writes via `vlc.config.set`)

- `whisper-backend` — `"auto"` (default) | `"gpu"` | `"cpu"`
- `model-path` — relative path under `models/`, e.g. `models/ggml-tiny.bin` (bundled `tiny` is the default; an explicit user selection remains authoritative and may be absent until downloaded)
- `whisper-language` — `"en"` (default) | `ro` | `tr` | `de` | `fr` | `es` — **no `auto`** entry in this dialog; automatic language selection is a later UI step
- `whisper-threads` — CPU-engine thread count, integer `1..16`, default `4` (clamped on Apply)
- `whisper-logging` — diagnostic logging enabled (`true`) or disabled (`false`, default); applies to plugin and worker diagnostics
- `whisper-backend-active` — read-only mirror written by plugin when `STATUS` v1.3 `resolved_backend` drains (`gpu`/`cpu`); Lua reads it for the status label

Language/dropdown wiring: `Apply` does `tonumber` + clamp `1..16` for threads, then `vlc.config.set` ×5 (model-path
set to `models/<chosen>.bin` relative path) and logs `[VLC-Whisper] applied …` lines. Plugin sender-loop polls every
~2 s; any diff vs last-applied snapshot → `vw_plugin_respawn_worker()` (brief caption gap, then resumes on a new
epoch). All four currently apply via respawn (live per-call for language/threads is a future optimization).

VLC 3.0's Lua dropdown API has no selection setter: it selects the first value added. The extension therefore adds
the persisted engine, model, and language choice first, followed by the remaining choices; the order may change after
each Apply, but `get_value()` still returns the stable numeric choice ID.

English-only models (`tiny.en` and `base.en`) keep the full language list visible because VLC 3.0 has no dropdown-change
callback. Apply always writes `en` for those models, even if another language was selected. The same pinned API has no
button enabled/disabled method;
the Download button changes to **Re-download Selected Model** when a file is already present and its callback remains
safe to invoke.

## Model download behavior

The existing single dialog and menu expose **Download selected model** and **Abort model download**. On dialog open and
again before a download request, Lua performs bounded existence checks for the selected filename in the bundled
`<VLC>\models` directory and the worker's per-user model directory (`%LOCALAPPDATA%\vlc-whisper\models` on
Windows, `$XDG_DATA_HOME/vlc-whisper/models` or `$HOME/.local/share/vlc-whisper/models` on Linux). It does not hash
large files on VLC's UI thread. The worker remains the authority for SHA-256 verification of downloaded `.part` files.
Each Lua
callback only writes `whisper-model-download`, `whisper-model-status`, and `whisper-model-progress` config values,
updates the current label once, and returns. There is no `os.clock`, `dlg:update`, timer, sleep, polling loop, or
network call in Lua.

The plugin forwards the command after media creates the worker, including when the selected model initially prevents
caption `START`. The worker downloads on its own thread; the plugin sender renders progress on a dedicated SPU
overlay, so playback and playback pause do not affect the transfer. Abort, worker disconnect, and worker shutdown
remove the partial file and clear the overlay. A verified model is installed in the per-user model directory and
the plugin respawns the worker on that model.

For Windows troubleshooting, the final model path is `%LOCALAPPDATA%\vlc-whisper\models\ggml-<catalog-id>.bin`, not
the install-time `models\` directory. The worker writes transfer, SHA-256, and atomic-rename diagnostics to
`%TEMP%\vlc-whisper-worker.log`; VLC Messages shows the plugin-side `PLUGIN_MODEL_CTRL`, `PLUGIN_MODEL_PROGRESS`,
`PLUGIN_MODEL_PATH`, and `PLUGIN_MODEL_ACTIVATE` events. The first `idle:<model>` progress event is only an initial
snapshot and must not be interpreted as download completion or cancellation. On the next worker launch, a relative
selected path is matched by filename against the per-user directory. The Windows uninstaller removes that app-owned
directory, including incomplete `.part` files.

## Windows manual test (verbatim)

1. Install plugin + worker either by running the installer **or** by manual copy:

   ```cmd
   REM manual copy (developer workflow)
   copy build\windows-x64-release\plugin\libvlc_whisper_plugin.dll "C:\Program Files\VideoLAN\VLC\plugins\audio_filter\libvlc_whisper_plugin.dll"
   copy build\windows-x64-release\worker\vlc-whisper-worker.exe "C:\Program Files\VideoLAN\VLC\vlc-whisper-worker.exe"
   copy lua\extensions\vlc_whisper_settings.lua "C:\Program Files\VideoLAN\VLC\lua\extensions\vlc_whisper_settings.lua"
   REM with installer the lua file is already bundled
   "C:\Program Files\VideoLAN\VLC\vlc-cache-gen.exe" "C:\Program Files\VideoLAN\VLC\plugins"
   ```

   Verify:

   ```cmd
   dir "C:\Program Files\VideoLAN\VLC\lua\extensions\vlc_whisper_settings.lua"
   ```

2. Open `Tools > Messages`, set **Verbosity** to `2` (Debug) — or launch with `vlc.exe -vvv --audio-filter=vlc_whisper`.

3. Play any media. Open `View > VLC-Whisper Settings` (on some skins `Tools > Extensions > VLC-Whisper Settings`). The dialog puts current engine, model, and language values first via `vlc.config.get`; the **Model availability** row reports bundled, downloaded, or missing; the **Detected backend** label initially shows `(pending — start playback)` and switches to `gpu` or `cpu` after the first session `STARTED` / `STATUS` `resolved_backend`.

4. Change **Language** `en` → `ro`, click **Apply**. Expected within ~2 s (next sender-loop poll):

   - Messages shows `[VLC-Whisper] applied whisper-language=ro` (plus the other three `applied …` lines).
   - Plugin log `PLUGIN_RESPAWN` and worker restart (`STARTED` with new `session_id`).
   - Captions continue after the brief gap.

   With the bundled multilingual `tiny` model, `ro` is a valid transcription language. The dialog still has no
   `auto` entry.

5. Select `tiny.en` or `base.en`. The full Language dropdown remains visible, but Apply writes `whisper-language=en` even if a non-English value was selected.

6. Change **Engine** to `gpu` on a machine without Vulkan, click **Apply**. After restart the **Detected backend** label reads `cpu` (worker resolved `VW_HAVE_VULKAN` → `cpu`; `STATUS` `resolved_backend` mirrored into `whisper-backend-active`).

7. Close and re-open the dialog — it re-reads `whisper-backend`, `model-path`, `whisper-language`, `whisper-threads`, and `whisper-logging`, showing the last applied values.

## Linux manual test

```sh
mkdir -p ~/.local/share/vlc/lua/extensions
cp lua/extensions/vlc_whisper_settings.lua ~/.local/share/vlc/lua/extensions/vlc_whisper_settings.lua
vlc -vvv --audio-filter=vlc_whisper
# View > VLC-Whisper Settings, change Language en→ro, Apply — expect PLUGIN_RESPAWN + worker restart within ~2 s
```

## Model dropdown label → path

| Label | Relative path |
|-------|---------------|
| tiny.en | `models/ggml-tiny.en.bin` |
| tiny (multilingual, bundled default) | `models/ggml-tiny.bin` |
| base.en | `models/ggml-base.en.bin` |
| base (multilingual) | `models/ggml-base.bin` |
| small | `models/ggml-small.bin` |
| medium | `models/ggml-medium.bin` |
| large | `models/ggml-large-v3.bin` |
| large-v3-turbo | `models/ggml-large-v3-turbo.bin` |

Selection allowed even if file absent; `E_MODEL_MISSING` disables captions until the selected model is downloaded.

## What NOT to expect

- No `auto` language option (automatic language selection is a later UI step).
- No second settings dialog; model download actions remain in this single Lua extension.
- Translation is available via worker's opt-in 3-tier engine (21b); this dialog triggers translation via TRANSLATE_CTRL after session start.
- Settings do NOT persist across VLC restart unless VLC exits cleanly (`vlcrc` save happens on clean exit).

```sh
luac -p lua/extensions/vlc_whisper_settings.lua && echo "syntax OK"
```

## Packaging

- NSIS installer (`cmake/vw_installer.nsi.in`) installs to `$INSTDIR\lua\extensions\vlc_whisper_settings.lua`; uninstall deletes it + `RMDir` (non-fatal).
- CPack ZIP (`cmake/vw_packaging.cmake`) bundles `lua/` via `install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lua" DESTINATION .)`.

## Protocol

STATUS v1.3 adds `resolved_backend[16]` (`"gpu"`|`"cpu"`, NUL-padded); 60 B encoded. Decoder handles legacy 44 B (zero-fills). `whisper-backend-active` is the Lua-visible mirror.
