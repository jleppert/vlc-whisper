#ifdef _WIN32
#include <winsock2.h>
#endif
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// clang-format off
#include <vlc_common.h>
#include <vlc_block.h>
// clang-format on
#include <vlc_filter.h>
#include <vlc_input.h>
#include <vlc_plugin.h>

#include "vw_log.h"
#include "vw_plugin.h"

// Implements callback signature matching vw_log_sink_fn from protocol/include/vw_log.h
// Posts messages to VLC logging system (msg_Err, msg_Warn, msg_Dbg) with event_id prefix
static void vw_plugin_log_sink(vw_log_level_t level, const char* event_id, const char* formatted_msg, void* user_data) {
  vlc_object_t* obj = (vlc_object_t*)user_data;
  if (!obj) {
    return;
  }
  switch (level) {
    case VW_LOG_LEVEL_ERROR:
      msg_Err(obj, "[vw_log:%s] %s", event_id, formatted_msg);
      break;
    case VW_LOG_LEVEL_WARN:
      msg_Warn(obj, "[vw_log:%s] %s", event_id, formatted_msg);
      break;
    case VW_LOG_LEVEL_INFO:
    case VW_LOG_LEVEL_DEBUG:
    default:
      msg_Dbg(obj, "[vw_log:%s] %s", event_id, formatted_msg);
      break;
  }
}

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "vw_audio_capture.h"
#include "vw_benchmark.h"
#include "vw_caption_presenter.h"
#include "vw_paused_preview_policy.h"
#include "vw_platform.h"
#include "vw_protocol_util.h"
#include "vw_queue.h"
#include "vw_worker_client.h"

// Seek/discontinuity detection thresholds (step 17d).
// Forward jumps >= 5s trigger seek re-sync; minor network transport jitter, buffer slips,
// or re-buffers (< 5s) are suppressed to avoid false 8s caption dropouts.
// Backward jumps past 500ms immediately trigger seek re-sync.
#define VW_INPUT_JUMP_DISCONTINUITY_US 5000000LL
#define VW_PTS_JUMP_THRESHOLD_US 500000LL

static int vw_plugin_open(vlc_object_t* obj);
static void vw_plugin_close(vlc_object_t* obj);
static input_thread_t* vw_plugin_find_input(filter_t* p_filter);
static int64_t vw_plugin_input_position_us(input_thread_t* input);

static char vw_plugin_dl_anchor;

#ifdef _WIN32
// Converts a UTF-8 path to UTF-16 for Win32 filesystem and discovery APIs.
static bool vw_plugin_utf8_to_wide(const char* input, wchar_t* out, size_t out_chars) {
  if (!input || !out || out_chars == 0 || out_chars > INT_MAX) return false;
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, out, (int)out_chars) > 0;
}

// Converts a NUL-terminated UTF-16 value to the plugin's internal UTF-8 representation.
static bool vw_plugin_wide_to_utf8(const wchar_t* input, char* out, size_t out_size) {
  if (!input || !out || out_size == 0 || out_size > INT_MAX) return false;
  return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, out, (int)out_size, NULL, NULL) > 0;
}

// Reads a Windows environment variable through the Unicode API and returns UTF-8 without truncation.
static bool vw_plugin_get_environment_utf8(const wchar_t* name, char* out, size_t out_size) {
  if (!name || !out || out_size == 0) return false;
  wchar_t wide_value[VW_PATH_MAX_BYTES];
  DWORD length = GetEnvironmentVariableW(name, wide_value, (DWORD)(sizeof(wide_value) / sizeof(wide_value[0])));
  if (length == 0 || length >= sizeof(wide_value) / sizeof(wide_value[0])) return false;
  return vw_plugin_wide_to_utf8(wide_value, out, out_size);
}

// Resolves a module filename with the Unicode Win32 API and converts the complete path to UTF-8.
static bool vw_plugin_get_module_path_utf8(HMODULE module, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  wchar_t wide_path[VW_PATH_MAX_BYTES];
  DWORD length = GetModuleFileNameW(module, wide_path, (DWORD)(sizeof(wide_path) / sizeof(wide_path[0])));
  if (length == 0 || length >= sizeof(wide_path) / sizeof(wide_path[0])) return false;
  wide_path[length] = L'\0';
  return vw_plugin_wide_to_utf8(wide_path, out, out_size);
}
#endif

// Returns per-user model directory mirroring worker vw_model_download_default_dir (%LOCALAPPDATA%\\vlc-whisper\\models
// on Windows via LOCALAPPDATA; $XDG_DATA_HOME/vlc-whisper/models else $HOME/.local/share on Linux).
static bool vw_plugin_get_model_dir(char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
#ifdef _WIN32
  char base[VW_PATH_MAX_BYTES];
  char tmp[4096];
  if (vw_plugin_get_environment_utf8(L"LOCALAPPDATA", base, sizeof(base)) && base[0]) {
    int written = snprintf(tmp, sizeof(tmp), "%s\\vlc-whisper\\models", base);
    if (written < 0 || (size_t)written >= sizeof(tmp)) return false;
  } else {
    char home[VW_PATH_MAX_BYTES];
    if (vw_plugin_get_environment_utf8(L"USERPROFILE", home, sizeof(home)) && home[0]) {
      int written = snprintf(tmp, sizeof(tmp), "%s\\AppData\\Local\\vlc-whisper\\models", home);
      if (written < 0 || (size_t)written >= sizeof(tmp)) return false;
    } else {
      snprintf(tmp, sizeof(tmp), ".\\vlc-whisper\\models");
    }
  }
  if (strlen(tmp) >= out_size) return false;
  strcpy(out, tmp);
#else
  const char* xdg = getenv("XDG_DATA_HOME");
  char tmp[4096];
  if (xdg && xdg[0]) {
    snprintf(tmp, sizeof(tmp), "%s/vlc-whisper/models", xdg);
  } else {
    const char* home = getenv("HOME");
    if (home && home[0]) {
      snprintf(tmp, sizeof(tmp), "%s/.local/share/vlc-whisper/models", home);
    } else {
      snprintf(tmp, sizeof(tmp), "/tmp/vlc-whisper/models");
    }
  }
  snprintf(out, out_size, "%s", tmp);
#endif
  out[out_size - 1] = '\0';
  return true;
}

// Maps catalog model id to bundled filename; plugin cannot link worker catalog header directly.
static const char* vw_plugin_catalog_filename(const char* id) {
  if (!id) return NULL;
  static const struct {
    const char* id;
    const char* file;
  } kMap[] = {
      {"tiny.en", "ggml-tiny.en.bin"}, {"tiny", "ggml-tiny.bin"},
      {"base.en", "ggml-base.en.bin"}, {"base", "ggml-base.bin"},
      {"small", "ggml-small.bin"},     {"medium", "ggml-medium.bin"},
      {"large", "ggml-large-v3.bin"},  {"large-v3-turbo", "ggml-large-v3-turbo.bin"},
  };
  for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
    if (strcmp(kMap[i].id, id) == 0) return kMap[i].file;
  }
  return NULL;
}

// Returns the catalog id represented by a configured model path, defaulting to the bundled multilingual model.
static const char* vw_plugin_catalog_id_from_path(const char* path) {
  if (!path || !path[0]) return "tiny";
  const char* base = path;
  for (const char* p = path; *p; p++) {
    if (*p == '/' || *p == '\\') base = p + 1;
  }
  static const struct {
    const char* id;
    const char* file;
  } kMap[] = {
      {"tiny.en", "ggml-tiny.en.bin"}, {"tiny", "ggml-tiny.bin"},
      {"base.en", "ggml-base.en.bin"}, {"base", "ggml-base.bin"},
      {"small", "ggml-small.bin"},     {"medium", "ggml-medium.bin"},
      {"large", "ggml-large-v3.bin"},  {"large-v3-turbo", "ggml-large-v3-turbo.bin"},
  };
  for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
    if (strcmp(base, kMap[i].file) == 0) return kMap[i].id;
  }
  return "tiny";
}

static bool vw_plugin_path_exists(const char* path) {
#ifdef _WIN32
  wchar_t wide_path[VW_PATH_MAX_BYTES];
  if (!vw_plugin_utf8_to_wide(path, wide_path, sizeof(wide_path) / sizeof(wide_path[0]))) return false;
  DWORD attr = GetFileAttributesW(wide_path);
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
  return access(path, F_OK) == 0;
#endif
}

// Probes candidate directories (a file's own directory and up to max_up ancestors) for each of
// `names`, copying the first existing "<dir>/<name>" into out. Returns true on first hit. Shared by
// the worker-binary and model-file resolvers; on Win32 forward slashes are accepted by the APIs.
static bool vw_plugin_probe_ancestors(const char* file_path, int max_up, const char* const* names, size_t n_names,
                                      char* out, size_t out_size) {
  if (!file_path || !file_path[0]) return false;
  const char* slash = NULL;
  for (const char* p = file_path; *p; p++) {
    if (*p == '/' || *p == '\\') slash = p;
  }
  size_t dir_len = slash ? (size_t)(slash - file_path) : 0;
  if (dir_len == 0) return false;

  for (int up = 0; up <= max_up; ++up) {
    size_t try_len = dir_len;
    for (int k = 0; k < up; ++k) {
      if (try_len == 0) break;
      const char* last = NULL;
      for (size_t i = 0; i < try_len; ++i) {
        if (file_path[i] == '/' || file_path[i] == '\\') last = file_path + i;
      }
      if (!last) {
        try_len = 0;
        break;
      }
      try_len = (size_t)(last - file_path);
    }
    if (try_len == 0) continue;
    for (size_t n = 0; n < n_names; n++) {
      size_t need = try_len + 1 + strlen(names[n]) + 1;
      if (need > out_size) continue;
      char candidate[VW_PATH_MAX_BYTES];
      memcpy(candidate, file_path, try_len);
#ifdef _WIN32
      candidate[try_len] = '\\';  // Native separator for Unicode Win32 filesystem and process APIs.
#else
      candidate[try_len] = '/';
#endif
      strcpy(candidate + try_len + 1, names[n]);
      if (vw_plugin_path_exists(candidate)) {
        strcpy(out, candidate);
        return true;
      }
    }
  }
  return false;
}

#ifdef _WIN32
// Probes Windows registry and environment paths for worker or model files.
static bool vw_plugin_probe_windows_paths(const char* const* names, size_t name_count, char* out, size_t out_size) {
  char candidate[VW_PATH_MAX_BYTES];
  // 1. Probe HKCU and HKLM \Software\VLC-Whisper\InstallPath
  const HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
  for (int i = 0; i < 2; i++) {
    HKEY hkey = NULL;
    if (RegOpenKeyExW(roots[i], L"Software\\VLC-Whisper", 0, KEY_READ, &hkey) == ERROR_SUCCESS) {
      wchar_t wide_value[VW_PATH_MAX_BYTES];
      DWORD len = sizeof(wide_value);
      DWORD type = 0;
      if (RegQueryValueExW(hkey, L"InstallPath", NULL, &type, (LPBYTE)wide_value, &len) == ERROR_SUCCESS &&
          type == REG_SZ && len >= sizeof(wchar_t) && len <= sizeof(wide_value) && len % sizeof(wchar_t) == 0) {
        RegCloseKey(hkey);
        size_t value_chars = len / sizeof(wchar_t);
        wide_value[value_chars < sizeof(wide_value) / sizeof(wide_value[0])
                       ? value_chars
                       : sizeof(wide_value) / sizeof(wide_value[0]) - 1] = L'\0';
        char value[VW_PATH_MAX_BYTES];
        if (vw_plugin_wide_to_utf8(wide_value, value, sizeof(value))) {
          int written = snprintf(candidate, sizeof(candidate), "%s\\.vw_probe", value);
          if (written >= 0 && (size_t)written < sizeof(candidate) &&
              vw_plugin_probe_ancestors(candidate, 0, names, name_count, out, out_size)) {
            return true;
          }
        }
      } else {
        RegCloseKey(hkey);
      }
    }
  }

  // 2. Probe %LOCALAPPDATA%/vlc-whisper
  char local_app_data[VW_PATH_MAX_BYTES];
  if (vw_plugin_get_environment_utf8(L"LOCALAPPDATA", local_app_data, sizeof(local_app_data))) {
    int written = snprintf(candidate, sizeof(candidate), "%s\\vlc-whisper\\.vw_probe", local_app_data);
    if (written >= 0 && (size_t)written < sizeof(candidate) &&
        vw_plugin_probe_ancestors(candidate, 0, names, name_count, out, out_size)) {
      return true;
    }
  }

  // 3. Probe %PROGRAMFILES%/vlc-whisper
  char prog_files[VW_PATH_MAX_BYTES];
  if (vw_plugin_get_environment_utf8(L"PROGRAMFILES", prog_files, sizeof(prog_files))) {
    int written = snprintf(candidate, sizeof(candidate), "%s\\vlc-whisper\\.vw_probe", prog_files);
    if (written >= 0 && (size_t)written < sizeof(candidate) &&
        vw_plugin_probe_ancestors(candidate, 0, names, name_count, out, out_size)) {
      return true;
    }
  }
  return false;
}
#endif

// Resolves the vlc-whisper-worker executable path: plugin dir ancestors, then the exe dir.
// Probes the GPU worker ("vlc-whisper-worker") first, then falls back to the CPU worker
// ("vlc-whisper-worker-cpu") if only a CPU-preset binary was built/installed.
static bool vw_plugin_resolve_worker_path(char* out, size_t out_size) {
#ifdef _WIN32
  const char* worker_names[] = {"vlc-whisper-worker.exe", "vlc-whisper-worker-cpu.exe"};
  char plugin_path[VW_PATH_MAX_BYTES];
  HMODULE hmod = NULL;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)(const void*)&vw_plugin_dl_anchor, &hmod) &&
      hmod && vw_plugin_get_module_path_utf8(hmod, plugin_path, sizeof(plugin_path))) {
    if (vw_plugin_probe_ancestors(plugin_path, 3, worker_names, 2, out, out_size)) return true;
  }
  char exe_path[VW_PATH_MAX_BYTES];
  if (vw_plugin_get_module_path_utf8(NULL, exe_path, sizeof(exe_path))) {
    if (vw_plugin_probe_ancestors(exe_path, 0, worker_names, 2, out, out_size)) return true;
  }
  if (vw_plugin_probe_windows_paths(worker_names, 2, out, out_size)) return true;
  return false;
#else
  const char* worker_names[] = {"vlc-whisper-worker", "vlc-whisper-worker-cpu"};
  Dl_info info;
  if (dladdr((void*)&vw_plugin_dl_anchor, &info) && info.dli_fname && info.dli_fname[0]) {
    if (vw_plugin_probe_ancestors(info.dli_fname, 4, worker_names, 2, out, out_size)) return true;
  }
#ifdef __linux__
  char exe_path[4096];
  ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (n > 0) {
    exe_path[n] = '\0';
    if (vw_plugin_probe_ancestors(exe_path, 0, worker_names, 2, out, out_size)) return true;
  }
#endif
  return false;
#endif
}

// Resolves only the CPU worker executable (trusted absolute path) for auto-backend GPU fallback.
static bool vw_plugin_resolve_cpu_worker_path(char* out, size_t out_size) {
#ifdef _WIN32
  const char* cpu_names[] = {"vlc-whisper-worker-cpu.exe"};
  char plugin_path[VW_PATH_MAX_BYTES];
  HMODULE hmod = NULL;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)(const void*)&vw_plugin_dl_anchor, &hmod) &&
      hmod && vw_plugin_get_module_path_utf8(hmod, plugin_path, sizeof(plugin_path))) {
    if (vw_plugin_probe_ancestors(plugin_path, 3, cpu_names, 1, out, out_size)) return true;
  }
  char exe_path[VW_PATH_MAX_BYTES];
  if (vw_plugin_get_module_path_utf8(NULL, exe_path, sizeof(exe_path))) {
    if (vw_plugin_probe_ancestors(exe_path, 0, cpu_names, 1, out, out_size)) return true;
  }
  if (vw_plugin_probe_windows_paths(cpu_names, 1, out, out_size)) return true;
  return false;
#else
  const char* cpu_names[] = {"vlc-whisper-worker-cpu"};
  Dl_info info;
  if (dladdr((void*)&vw_plugin_dl_anchor, &info) && info.dli_fname && info.dli_fname[0]) {
    if (vw_plugin_probe_ancestors(info.dli_fname, 4, cpu_names, 1, out, out_size)) return true;
  }
#ifdef __linux__
  char exe_path[4096];
  ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (n > 0) {
    exe_path[n] = '\0';
    if (vw_plugin_probe_ancestors(exe_path, 0, cpu_names, 1, out, out_size)) return true;
  }
#endif
  return false;
#endif
}

// Trusted launch helper for auto backend: try GPU-discovered path first, then exact CPU path on failure.
// Configured explicit backend (gpu/cpu) remains fail-closed; only auto backend retries CPU.
static vw_worker_client_t* vw_plugin_launch_with_auto_retry(const char* worker_path, const char* pipe_name,
                                                            const uint8_t auth_token[VW_AUTH_TOKEN_BYTES],
                                                            const char* model_path, const char* backend,
                                                            const char* language, int n_threads, int gpu_device,
                                                            const char* model_dir, bool logging_enabled,
                                                            bool worker_path_was_configured) {
  vw_worker_client_t* client =
      vw_worker_client_launch_and_connect_ex(worker_path[0] ? worker_path : NULL, pipe_name, auth_token, model_path,
                                             backend, language, n_threads, gpu_device, model_dir, logging_enabled);
  if (client) return client;
  const char* eff_backend = (backend && backend[0]) ? backend : "auto";
  bool is_auto = (strcmp(eff_backend, "auto") == 0);
  if (!is_auto || worker_path_was_configured) return NULL;
  // Only retry if original path is not already the CPU worker; inspect the basename so a directory named "cpu"
  // cannot suppress the fallback.
  const char* worker_name = strrchr(worker_path, '/');
  const char* backslash_name = strrchr(worker_path, '\\');
  if (!worker_name || (backslash_name && backslash_name > worker_name)) worker_name = backslash_name;
  worker_name = worker_name ? worker_name + 1 : worker_path;
  if (strcmp(worker_name, "vlc-whisper-worker-cpu") == 0 || strcmp(worker_name, "vlc-whisper-worker-cpu.exe") == 0) {
    return NULL;
  }
  char cpu_path[VW_PATH_MAX_BYTES];
  if (!vw_plugin_resolve_cpu_worker_path(cpu_path, sizeof(cpu_path))) return NULL;
  if (cpu_path[0] == '\0' || strcmp(cpu_path, worker_path) == 0) return NULL;
  vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_WORKER_GPU_FALLBACK",
               "GPU worker handshake/spawn failed for '%s'; retrying trusted CPU worker '%s'", worker_path, cpu_path);
  client = vw_worker_client_launch_and_connect_ex(cpu_path, pipe_name, auth_token, model_path, backend, language,
                                                  n_threads, gpu_device, model_dir, logging_enabled);
  return client;
}

// Resolves a catalog model file in install paths and then the per-user model directory. Empty out means "no model".
static bool vw_plugin_resolve_model_path(char* out, size_t out_size) {
  const char* model_names[] = {
      "ggml-tiny.bin",     "models/ggml-tiny.bin",     "ggml-tiny.en.bin",        "models/ggml-tiny.en.bin",
      "ggml-base.bin",     "models/ggml-base.bin",     "ggml-base.en.bin",        "models/ggml-base.en.bin",
      "ggml-small.bin",    "models/ggml-small.bin",    "ggml-medium.bin",         "models/ggml-medium.bin",
      "ggml-large-v3.bin", "models/ggml-large-v3.bin", "ggml-large-v3-turbo.bin", "models/ggml-large-v3-turbo.bin",
  };
  const char* model_files[] = {"ggml-tiny.bin",  "ggml-tiny.en.bin", "ggml-base.bin",     "ggml-base.en.bin",
                               "ggml-small.bin", "ggml-medium.bin",  "ggml-large-v3.bin", "ggml-large-v3-turbo.bin"};
  const size_t model_name_count = sizeof(model_names) / sizeof(model_names[0]);
  const size_t model_file_count = sizeof(model_files) / sizeof(model_files[0]);
#ifdef _WIN32
  char plugin_path[VW_PATH_MAX_BYTES];
  HMODULE hmod = NULL;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)(const void*)&vw_plugin_dl_anchor, &hmod) &&
      hmod && vw_plugin_get_module_path_utf8(hmod, plugin_path, sizeof(plugin_path))) {
    if (vw_plugin_probe_ancestors(plugin_path, 3, model_names, model_name_count, out, out_size)) return true;
  }
  char exe_path[VW_PATH_MAX_BYTES];
  if (vw_plugin_get_module_path_utf8(NULL, exe_path, sizeof(exe_path))) {
    if (vw_plugin_probe_ancestors(exe_path, 0, model_names, model_name_count, out, out_size)) return true;
  }
  if (vw_plugin_probe_windows_paths(model_names, model_name_count, out, out_size)) return true;
  // Per-user directory probe mirrors worker vw_model_download_default_dir; plugin passes --model-dir explicitly so both
  // agree.
  {
    char dir[VW_PATH_MAX_BYTES];
    if (vw_plugin_get_model_dir(dir, sizeof(dir))) {
      for (size_t i = 0; i < model_file_count; i++) {
        char cand[VW_PATH_MAX_BYTES + 64];  // dir bound + '/' + catalog name: snprintf cannot truncate
        snprintf(cand, sizeof(cand), "%s/%s", dir, model_files[i]);
        // Also try Windows separator for consistency
#ifdef _WIN32
        for (char* c = cand; *c; c++)
          if (*c == '/') *c = '\\';
#endif
        if (vw_plugin_path_exists(cand)) {
          snprintf(out, out_size, "%s", cand);
          return true;
        }
      }
    }
  }
  return false;
#else
  Dl_info info;
  if (dladdr((void*)&vw_plugin_dl_anchor, &info) && info.dli_fname && info.dli_fname[0]) {
    if (vw_plugin_probe_ancestors(info.dli_fname, 4, model_names, model_name_count, out, out_size)) return true;
  }
#ifdef __linux__
  char exe_path[4096];
  ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (n > 0) {
    exe_path[n] = '\0';
    if (vw_plugin_probe_ancestors(exe_path, 0, model_names, model_name_count, out, out_size)) return true;
  }
#endif
  // Per-user directory probe mirrors worker vw_model_download_default_dir; plugin passes --model-dir explicitly so both
  // agree.
  {
    char dir[VW_PATH_MAX_BYTES];
    if (vw_plugin_get_model_dir(dir, sizeof(dir))) {
      for (size_t i = 0; i < model_file_count; i++) {
        char cand[VW_PATH_MAX_BYTES + 64];  // dir bound + '/' + catalog name: snprintf cannot truncate
        snprintf(cand, sizeof(cand), "%s/%s", dir, model_files[i]);
        if (vw_plugin_path_exists(cand)) {
          snprintf(out, out_size, "%s", cand);
          return true;
        }
      }
    }
  }
  return false;
#endif
}

// Plugin instance state
typedef struct vw_plugin_sys {
  vw_spsc_queue_t* queue;
  vw_audio_capture_t capture;
  vw_worker_client_t* client;
  vw_caption_presenter_t presenter;  // p_filter_ctx set in open; used by sender thread only

  char pipe_name[256];
  uint8_t auth_token[VW_AUTH_TOKEN_BYTES];
  char worker_path[VW_PATH_MAX_BYTES];

  // Sender thread (14c): drains the SPSC queue to the worker and drains worker frames back.
  vw_thread_t sender_thread;
  bool sender_started;  // thread create succeeded; close joins only when true
  _Atomic bool sender_running;
  _Atomic bool worker_dead;
  // Step 17: set by the realtime filter callback (flag or PTS jump), consumed by the sender
  // thread to restart the session epoch. Callback only stores atomics — never IPC/heap/locks.
  _Atomic bool discontinuity_pending;
  _Atomic int64_t resume_pts_us;  // Media position set by poll detectors
  _Atomic bool source_mode_active;
  _Atomic bool session_active;
  vw_paused_preview_state_t paused_preview;  // sender-owned one-shot state; reset on every session identity change
  _Atomic float playback_rate;               // current playback rate polled by sender, read by audio callback (VW-019)
  _Atomic bool capture_reset_pending;        // sender requests capture resampler reset, callback clears (VW-019)
  _Atomic bool invalid_pts_pending;          // producer signals sender to drain old queued audio after invalid interval
  bool last_pts_was_invalid;                 // callback-local invalid PTS state; kept per filter instance, not TLS
  uint64_t chunks_sent;
  uint32_t frames_received;
  uint32_t segments_received;
  uint32_t status_received;
  uint32_t errors_received;
  uint32_t respawn_count;  // bounded worker respawns after transport death (Step 17d)
  vw_benchmark_t benchmark;
  char model_path[VW_PATH_MAX_BYTES];
  char active_source_url[VW_PATH_MAX_BYTES];
  // 19b: live-apply config snapshot for worker-path/model-path/backend/language/threads
  char cfg_worker_path[VW_PATH_MAX_BYTES];
  char cfg_model_path[VW_PATH_MAX_BYTES];
  char cfg_backend[16];
  char cfg_language[16];
  char cfg_model_download[40];
  char model_download_id[40];
  int cfg_threads;
  bool cfg_logging;
  bool cfg_translate_enabled;
  char cfg_translate_from[16];
  char cfg_translate_to[16];
  int cfg_translate_mode;
  bool cfg_snapshot_valid;
  _Atomic bool respawn_in_progress;
  int64_t last_config_poll_us;
  int64_t last_cfg_respawn_attempt_us;  // last failed-config-respawn attempt; paces the 10s retry
} vw_plugin_sys_t;
#define VW_MAX_WORKER_RESPAWNS 3
#define VW_WORKER_RESPAWN_DELAY_MS 1000

static bool vw_plugin_send_model_request(vw_plugin_sys_t* sys, const char* request);
static bool vw_plugin_activate_downloaded_model(vw_plugin_sys_t* sys, const char* model_id, bool paused);

static bool vw_plugin_send_model_request(vw_plugin_sys_t* sys, const char* request) {
  if (!sys || !sys->client || !request || !request[0]) return false;

  uint8_t action = (strcmp(request, "abort") == 0) ? VW_MODEL_ACTION_ABORT : VW_MODEL_ACTION_DOWNLOAD;
  const char* model_id = action == VW_MODEL_ACTION_ABORT ? "" : request;
  vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_MODEL_CTRL", "sending %s request for model '%s'",
               action == VW_MODEL_ACTION_ABORT ? "abort" : "download", model_id[0] ? model_id : "(active)");
  if (!vw_worker_client_send_model_ctrl(sys->client, action, model_id)) {
    vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_MODEL_CTRL", "MODEL_CTRL send failed for request '%s'", request);
    return false;
  }
  vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_MODEL_CTRL", "MODEL_CTRL sent for request '%s'", request);

  vlc_object_t* obj = VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx);
  config_PutPsz(obj, "whisper-model-download", "");
  config_PutInt(obj, "whisper-model-progress", 0);
  config_PutPsz(obj, "whisper-model-status", action == VW_MODEL_ACTION_ABORT ? "aborting" : "downloading");
  if (action == VW_MODEL_ACTION_DOWNLOAD) {
    snprintf(sys->model_download_id, sizeof(sys->model_download_id), "%s", model_id);
  } else {
    sys->model_download_id[0] = '\0';
  }
  sys->cfg_model_download[0] = '\0';
  return true;
}
// Sender thread (14c): the only consumer of the SPSC queue and the only user of the worker client.
// Starts one session, then alternates draining queue -> send AUDIO frames with draining worker ->
// plugin frames (SEGMENT/STATUS/ERROR), degrading to passthrough on any fatal transport condition.
// Bounded worker respawn after a transport death (Step 17d): disconnect the dead client, relaunch
// the worker with the same pipe/auth/model, re-extract the current media URI, and restart the
// caption session. Called from the sender thread only; paused re-applies the paused state to the
// fresh session. The old worker exits once its pipe end is closed (disconnect waits up to 5s for
// it), freeing the pipe name before the delay elapses. Returns false (permanent passthrough) when
// the respawn budget is exhausted or the new worker cannot start a session.
// transport_recovery: true for transport-death recovery (consumes one of VW_MAX_WORKER_RESPAWNS
// per filter lifetime); false for user-initiated config-change respawns, which must NEVER consume
// that budget — otherwise three settings changes would leave later settings silently unapplied
// (snapshot already refreshed) and a later transport death would kill captions permanently.
static bool vw_plugin_respawn_worker(vw_plugin_sys_t* sys, bool paused, bool transport_recovery) {
  vw_paused_preview_state_reset_session(&sys->paused_preview);
  if (transport_recovery) {
    if (sys->respawn_count >= VW_MAX_WORKER_RESPAWNS) {
      vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_WORKER_RESPAWN_EXHAUSTED",
                   "worker respawn limit (%u) reached; captions disabled, passthrough only",
                   (unsigned)VW_MAX_WORKER_RESPAWNS);
      return false;
    }
    sys->respawn_count++;
  }
  if (sys->client) {
    if (sys->model_download_id[0]) {
      sys->model_download_id[0] = '\0';
      config_PutPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-model-status", "failed:worker");
      vw_caption_presenter_clear_model_progress(&sys->presenter);
    }
    vw_worker_client_disconnect(sys->client);
    sys->client = NULL;
  }
  // The old transport is gone; its dead-flag is obsolete bookkeeping. Clear it unconditionally
  // (even when client was already NULL) so a config respawn attempted while worker_dead was set
  // — transport failure followed by a settings change inside one 2s poll window — cannot leave
  // the flag set across a failed launch: the transport block in the sender loop would otherwise
  // reclassify that config failure as transport recovery, consume the bounded budget, and break
  // the loop. A failed respawn returns with client == NULL; the sender loop's NULL-client guard
  // idles on that until the next config diff.
  atomic_store(&sys->worker_dead, false);
  vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_WORKER_RESPAWN", "%s; respawning worker (%u/%u)",
               transport_recovery ? "transport death" : "config change", sys->respawn_count,
               (unsigned)VW_MAX_WORKER_RESPAWNS);
  vw_platform_sleep_ms(VW_WORKER_RESPAWN_DELAY_MS);  // let the old worker exit and free the pipe name
  char* respawn_be = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-backend");
  char* respawn_lg = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-language");
  int64_t respawn_thr = config_GetInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-threads");
  bool respawn_logging = config_GetInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-logging") != 0;
  int respawn_gpu = -1;
  if (config_FindConfig("whisper-gpu-device")) {
    respawn_gpu = (int)config_GetInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-gpu-device");
  }
  char respawn_model_dir[VW_PATH_MAX_BYTES];
  respawn_model_dir[0] = '\0';
  vw_plugin_get_model_dir(respawn_model_dir, sizeof(respawn_model_dir));
  // VW-004: Re-resolve worker path if unconfigured/empty to prevent repeated 2s connection freeze
  if (sys->worker_path[0] == '\0') {
    if (vw_plugin_resolve_worker_path(sys->worker_path, sizeof(sys->worker_path))) {
      // Resolved to a concrete file next to the plugin or VLC executable
    }
#ifdef _WIN32
    if (sys->worker_path[0] == '\0') {
      vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_WORKER_UNAVAILABLE",
                   "worker discovery failed on Windows; captions disabled, passthrough only (no bare fallback)");
      if (respawn_be) free(respawn_be);
      if (respawn_lg) free(respawn_lg);
      return false;
    }
#else
    if (sys->worker_path[0] == '\0') {
      snprintf(sys->worker_path, sizeof(sys->worker_path), "%s", "vlc-whisper-worker");
    }
#endif
  }
  // Use auto-retry wrapper for GPU->CPU fallback when backend is auto and worker_path was not explicitly configured.
  bool respawn_worker_path_configured = (sys->cfg_worker_path[0] != '\0');
  // Also consider current worker_path: if it was empty fallback via PATH on Linux, treat as not configured.
  sys->client = vw_plugin_launch_with_auto_retry(
      sys->worker_path, sys->pipe_name, sys->auth_token, sys->model_path[0] ? sys->model_path : NULL, respawn_be,
      respawn_lg, (int)respawn_thr, respawn_gpu, respawn_model_dir[0] ? respawn_model_dir : NULL, respawn_logging,
      respawn_worker_path_configured);
  if (respawn_be) free(respawn_be);
  if (respawn_lg) free(respawn_lg);
  if (!sys->client) {
    vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_WORKER_UNAVAILABLE",
                 "caption worker respawn failed; running passthrough only");
    return false;
  }
  vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_WORKER_CONNECT", "worker respawned (HELLO handshake ok)");

  // Re-extract the current media URI and restart the session on the fresh worker. The input is
  // HELD by find_input; the item/URI are borrowed while the input lives — copy what we need.
  // Apply the SAME local-file filter and capability gate as session init: only file:// or
  // absolute paths qualify for source look-ahead; network streams must keep live PCM mode, and a
  // worker without SOURCE_MODE must not be handed a URI it will reject.
  char* source_url = NULL;
  input_thread_t* input = vw_plugin_find_input((filter_t*)sys->presenter.p_filter_ctx);
  if (input) {
    if (sys->client->worker_capabilities & VW_CAPABILITY_SOURCE_MODE) {
      input_item_t* item = input_GetItem(input);
      if (item) {
        char* uri = input_item_GetURI(item);
        if (uri) {
          size_t uri_len = strlen(uri);
          if (strncmp(uri, "file://", 7) == 0 || uri[0] == '/' ||
              (uri_len >= 3 && uri[1] == ':' && (uri[2] == '\\' || uri[2] == '/'))) {
            source_url = uri;
          } else {
            free(uri);
          }
        }
      }
    }
    vlc_object_release(VLC_OBJECT(input));
  }
  vw_caption_presenter_blank(&sys->presenter);  // erase stale captions from the dead epoch
  bool started =
      vw_worker_client_start_session(sys->client, 0, vw_plugin_catalog_id_from_path(sys->model_path), source_url);
  if (source_url) {
    snprintf(sys->active_source_url, sizeof(sys->active_source_url), "%s", source_url);
  } else {
    sys->active_source_url[0] = '\0';
  }
  free(source_url);
  atomic_store(&sys->session_active, started);
  atomic_store(&sys->source_mode_active, started && vw_worker_client_is_source_active(sys->client));
  if (!started) {
    vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_SESSION_START_FAIL", "worker rejected respawn session");
    return false;
  }
  vw_worker_client_send_translate_ctrl(sys->client, sys->cfg_translate_enabled, sys->cfg_translate_from,
                                       sys->cfg_translate_to, (uint8_t)sys->cfg_translate_mode);
  if (!sys->benchmark.active && !vw_benchmark_begin(&sys->benchmark, vw_plugin_catalog_id_from_path(sys->model_path),
                                                    sys->cfg_backend, vw_platform_get_monotonic_time_us())) {
    vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_BENCHMARK", "could not create temporary session report");
  }
  if (paused) {
    vw_worker_client_pause_session(sys->client);  // restart in the paused state the death left us in
  }
  sys->chunks_sent = 0;
  sys->frames_received = 0;
  sys->segments_received = 0;
  vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_SESSION_STARTED", "caption session restarted (respawn)");
  return true;
}

static bool vw_plugin_activate_downloaded_model(vw_plugin_sys_t* sys, const char* model_id, bool paused) {
  if (!sys || !model_id || !model_id[0]) return false;
  const char* filename = vw_plugin_catalog_filename(model_id);
  if (!filename) return false;

  char model_dir[VW_PATH_MAX_BYTES];
  if (!vw_plugin_get_model_dir(model_dir, sizeof(model_dir))) return false;
  char model_path[VW_PATH_MAX_BYTES];
#ifdef _WIN32
  int written = snprintf(model_path, sizeof(model_path), "%s\\%s", model_dir, filename);
#else
  int written = snprintf(model_path, sizeof(model_path), "%s/%s", model_dir, filename);
#endif
  if (written < 0 || (size_t)written >= sizeof(model_path) || !vw_plugin_path_exists(model_path)) {
    vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_MODEL_ACTIVATE", "downloaded model path is unavailable for %s", model_id);
    return false;
  }

  vlc_object_t* obj = VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx);
  config_PutPsz(obj, "model-path", model_path);
  snprintf(sys->model_path, sizeof(sys->model_path), "%s", model_path);
  snprintf(sys->cfg_model_path, sizeof(sys->cfg_model_path), "%s", model_path);
  vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_MODEL_ACTIVATE", "activating downloaded model %s from %s", model_id,
               model_path);
  vw_caption_presenter_clear_model_progress(&sys->presenter);
  return vw_plugin_respawn_worker(sys, paused, false);
}

static void* vw_plugin_sender_main(void* arg) {
  vw_plugin_sys_t* sys = (vw_plugin_sys_t*)arg;

  char* source_url = NULL;
  if (sys->client && (sys->client->worker_capabilities & VW_CAPABILITY_SOURCE_MODE)) {
    input_thread_t* init_input = vw_plugin_find_input((filter_t*)sys->presenter.p_filter_ctx);
    if (init_input) {
      input_item_t* item = input_GetItem(init_input);
      if (item) {
        char* uri = input_item_GetURI(item);
        if (uri) {
          size_t uri_len = strlen(uri);
          if (strncmp(uri, "file://", 7) == 0 || uri[0] == '/' ||
              (uri_len >= 3 && uri[1] == ':' && (uri[2] == '\\' || uri[2] == '/'))) {
            source_url = uri;
            strncpy(sys->active_source_url, source_url, sizeof(sys->active_source_url) - 1);
            sys->active_source_url[sizeof(sys->active_source_url) - 1] = '\0';
          } else {
            free(uri);
          }
        }
      }
      vlc_object_release((vlc_object_t*)init_input);
    }
  }

  // First iteration: start the caption session. A worker rejection (e.g. E_MODEL_MISSING) means
  // captions stay off until a model is provisioned / settings change; playback is untouched.
  bool session_started = false;
  if (sys->client) {
    session_started =
        vw_worker_client_start_session(sys->client, 0, vw_plugin_catalog_id_from_path(sys->model_path), source_url);
    if (!session_started) {
      vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_SESSION_START_FAIL",
                   "worker rejected session; captions disabled, passthrough only");
    }
  }
  if (source_url) free(source_url);
  atomic_store(&sys->session_active, session_started);
  atomic_store(&sys->source_mode_active, session_started && vw_worker_client_is_source_active(sys->client));
  if (session_started) {
    vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_SESSION_STARTED",
                 "caption session started (STARTED confirmed source_active=%d)",
                 atomic_load(&sys->source_mode_active) ? 1 : 0);
    if (!vw_benchmark_begin(&sys->benchmark, vw_plugin_catalog_id_from_path(sys->model_path), sys->cfg_backend,
                            vw_platform_get_monotonic_time_us())) {
      vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_BENCHMARK", "could not create temporary session report");
    }
  }

  // Play/pause lifecycle: poll the input thread once per iteration (cadence is 5-20ms). On the
  // playing->paused transition send PAUSE; on paused->playing send RESUME. While paused the
  // queue is drained and DISCARDED (stale pre-pause/during-pause PCM must never reach the worker,
  // which cleared its window on PAUSE); worker frames are still drained so the client stays live
  // and STATUS/ERROR flow and worker death is still detected.
  bool paused = false;
  int64_t last_pause_poll_us = 0;
  int64_t last_position_us = -1;     // -1 = no baseline yet (first poll only samples)
  int64_t paused_position_us = -1;   // media position captured at the pause transition
  int64_t current_position_us = -1;  // latest sampled media position for SPU timing
  float last_playback_rate = 1.0f;   // VW-011: rate change detection
  int64_t last_source_seek_us = -1;  // VW-014: source-mode stale segment filter
  while (atomic_load(&sys->sender_running)) {
    // 19b: 2s-cadence snapshot compare of worker-path/model-path/backend/language/threads.
    // single respawn via vw_plugin_respawn_worker, guarded against re-entry.
    int64_t cfg_now_us = vw_platform_get_monotonic_time_us();
    if (!sys->cfg_snapshot_valid) {
      char* wp = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "worker-path");
      char* mp = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "model-path");
      char* be = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-backend");
      char* lg = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-language");
      char* dl = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-model-download");
      int64_t thr = config_GetInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-threads");
      bool logging = config_GetInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-logging") != 0;
      if (wp) {
        snprintf(sys->cfg_worker_path, sizeof(sys->cfg_worker_path), "%s", wp);
        free(wp);
      } else {
        sys->cfg_worker_path[0] = '\0';
      }
      if (mp) {
        snprintf(sys->cfg_model_path, sizeof(sys->cfg_model_path), "%s", mp);
        free(mp);
      } else {
        sys->cfg_model_path[0] = '\0';
      }
      if (be && be[0]) {
        snprintf(sys->cfg_backend, sizeof(sys->cfg_backend), "%s", be);
        free(be);
      } else {
        if (be) free(be);
        snprintf(sys->cfg_backend, sizeof(sys->cfg_backend), "auto");
      }
      if (lg && lg[0]) {
        snprintf(sys->cfg_language, sizeof(sys->cfg_language), "%s", lg);
        free(lg);
      } else {
        if (lg) free(lg);
        snprintf(sys->cfg_language, sizeof(sys->cfg_language), "en");
      }
      if (thr < 1 || thr > 16) thr = 4;
      sys->cfg_threads = (int)thr;
      if (dl && dl[0]) {
        snprintf(sys->cfg_model_download, sizeof(sys->cfg_model_download), "%s", dl);
      } else {
        sys->cfg_model_download[0] = '\0';
      }
      if (dl) free(dl);
      bool trans_en =
          config_GetInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-translate-enabled") != 0;
      char* trans_from = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-translate-from");
      char* trans_to = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-translate-to");
      int64_t trans_mode = config_GetInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-translate-mode");
      sys->cfg_translate_enabled = trans_en;
      if (trans_from && trans_from[0]) {
        snprintf(sys->cfg_translate_from, sizeof(sys->cfg_translate_from), "%s", trans_from);
      } else {
        snprintf(sys->cfg_translate_from, sizeof(sys->cfg_translate_from), "auto");
      }
      if (trans_from) free(trans_from);
      if (trans_to && trans_to[0]) {
        snprintf(sys->cfg_translate_to, sizeof(sys->cfg_translate_to), "%s", trans_to);
      } else {
        snprintf(sys->cfg_translate_to, sizeof(sys->cfg_translate_to), "en");
      }
      if (trans_to) free(trans_to);
      sys->cfg_translate_mode = (trans_mode == 0) ? 0 : 1;

      if (sys->client && atomic_load(&sys->session_active)) {
        if (!vw_worker_client_send_translate_ctrl(sys->client, sys->cfg_translate_enabled, sys->cfg_translate_from,
                                                  sys->cfg_translate_to, (uint8_t)sys->cfg_translate_mode)) {
          if ((sys->client->worker_capabilities & VW_CAPABILITY_TRANSLATION) != 0) {
            atomic_store(&sys->worker_dead, true);
          }
          sys->cfg_translate_enabled = false;
          config_PutInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-translate-enabled", 0);
          vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_TRANSLATE_UNSUPPORTED",
                       "worker lacks translation capability; disabling translation setting");
        }
      }
      sys->cfg_logging = logging;
      vw_log_set_enabled(logging);
      sys->last_config_poll_us = cfg_now_us;
      sys->cfg_snapshot_valid = true;
      // A request made before media playback is intentionally present in the first snapshot. Relay it now that
      // this filter has spawned a worker; MODEL_CTRL is valid without a caption session.
      if (sys->cfg_model_download[0]) {
        vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_MODEL_CTRL", "observed pending request '%s' in initial config snapshot",
                     sys->cfg_model_download);
      }
      if (sys->cfg_model_download[0] && !vw_plugin_send_model_request(sys, sys->cfg_model_download)) {
        atomic_store(&sys->worker_dead, true);
        config_PutPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-model-status", "failed:worker");
      }
    } else if (cfg_now_us - sys->last_config_poll_us >= 2000000) {
      sys->last_config_poll_us = cfg_now_us;
      if (!atomic_load(&sys->respawn_in_progress)) {
        char* wp_new = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "worker-path");
        char* mp_new = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "model-path");
        char* be_new = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-backend");
        char* lg_new = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-language");
        char* dl_new = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-model-download");
        int64_t thr_new = config_GetInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-threads");
        bool logging_new = config_GetInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-logging") != 0;
        bool trans_en_new =
            config_GetInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-translate-enabled") != 0;
        char* trans_from_new =
            config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-translate-from");
        char* trans_to_new = config_GetPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-translate-to");
        int64_t trans_mode_new =
            config_GetInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-translate-mode");
        const char* wp_cmp = wp_new ? wp_new : "";
        const char* mp_cmp = mp_new ? mp_new : "";
        const char* be_cmp = (be_new && be_new[0]) ? be_new : "auto";
        const char* lg_cmp_raw = (lg_new && lg_new[0]) ? lg_new : "en";
        const char* lg_cmp = lg_cmp_raw;
        // Concrete transcription language validation: reject 'auto' visibly and keep prior valid language.
        bool lg_invalid_auto = (lg_cmp_raw && strcmp(lg_cmp_raw, "auto") == 0);
        if (lg_invalid_auto) {
          vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_LANGUAGE_INVALID",
                       "language 'auto' is not a concrete transcription language; keeping '%s'", sys->cfg_language);
          lg_cmp = sys->cfg_language;
        }
        const char* dl_cmp = dl_new ? dl_new : "";
        const char* trans_from_cmp = (trans_from_new && trans_from_new[0]) ? trans_from_new : "auto";
        const char* trans_to_cmp = (trans_to_new && trans_to_new[0]) ? trans_to_new : "en";
        int trans_mode_cmp = (trans_mode_new == 0) ? 0 : 1;

        if (thr_new < 1 || thr_new > 16) thr_new = 4;
        bool trans_diff =
            (trans_en_new != sys->cfg_translate_enabled) || (strcmp(trans_from_cmp, sys->cfg_translate_from) != 0) ||
            (strcmp(trans_to_cmp, sys->cfg_translate_to) != 0) || (trans_mode_cmp != sys->cfg_translate_mode);

        if (trans_diff) {
          sys->cfg_translate_enabled = trans_en_new;
          snprintf(sys->cfg_translate_from, sizeof(sys->cfg_translate_from), "%s", trans_from_cmp);
          snprintf(sys->cfg_translate_to, sizeof(sys->cfg_translate_to), "%s", trans_to_cmp);
          sys->cfg_translate_mode = trans_mode_cmp;
          if (sys->client && atomic_load(&sys->session_active)) {
            if (!vw_worker_client_send_translate_ctrl(sys->client, sys->cfg_translate_enabled, sys->cfg_translate_from,
                                                      sys->cfg_translate_to, (uint8_t)sys->cfg_translate_mode)) {
              if ((sys->client->worker_capabilities & VW_CAPABILITY_TRANSLATION) != 0) {
                atomic_store(&sys->worker_dead, true);
              }
              config_PutInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-translate-enabled", 0);
              vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_TRANSLATE_UNSUPPORTED",
                           "worker lacks translation capability; disabling translation setting");
            }
          }
          vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_TRANSLATE_CTRL",
                       "updated translation config: enabled=%d from=%s to=%s mode=%d", (int)sys->cfg_translate_enabled,
                       sys->cfg_translate_from, sys->cfg_translate_to, sys->cfg_translate_mode);
        }
        if (trans_from_new) free(trans_from_new);
        if (trans_to_new) free(trans_to_new);

        bool diff = false;
        if (strcmp(wp_cmp, sys->cfg_worker_path) != 0) diff = true;
        if (strcmp(mp_cmp, sys->cfg_model_path) != 0) diff = true;
        if (strcmp(be_cmp, sys->cfg_backend) != 0) diff = true;
        if (strcmp(lg_cmp, sys->cfg_language) != 0) diff = true;
        if ((int)thr_new != sys->cfg_threads) diff = true;
        if (logging_new != sys->cfg_logging) diff = true;
        // Model download control does not trigger a respawn; relay as MODEL_CTRL.
        if (strcmp(dl_cmp, sys->cfg_model_download) != 0) {
          // Normalize empty download request: ignore if empty and not abort. The request is edge-triggered: the
          // helper clears both the config value and the sender snapshot after a successful relay.
          if (dl_cmp[0] || strcmp(dl_cmp, "abort") == 0) {
            vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_MODEL_CTRL", "observed config request '%s'", dl_cmp);
            if (!vw_plugin_send_model_request(sys, dl_cmp)) {
              atomic_store(&sys->worker_dead, true);
              config_PutPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-model-status",
                            "failed:worker");
            }
          } else {
            snprintf(sys->cfg_model_download, sizeof(sys->cfg_model_download), "%s", dl_cmp);
          }
        }
        if (diff) {
          bool expected = false;
          if (atomic_compare_exchange_strong(&sys->respawn_in_progress, &expected, true)) {
            vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_CONFIG_CHANGED",
                         "config changed (backend=%s language=%s threads=%d); respawning worker", be_cmp, lg_cmp,
                         (int)thr_new);
            // Refresh snapshot before respawn so a second diff doesn't re-trigger immediately
            snprintf(sys->cfg_worker_path, sizeof(sys->cfg_worker_path), "%s", wp_cmp);
            snprintf(sys->cfg_model_path, sizeof(sys->cfg_model_path), "%s", mp_cmp);
            snprintf(sys->cfg_backend, sizeof(sys->cfg_backend), "%s", be_cmp);
            snprintf(sys->cfg_language, sizeof(sys->cfg_language), "%s", lg_cmp);
            sys->cfg_threads = (int)thr_new;
            sys->cfg_logging = logging_new;
            vw_log_set_enabled(logging_new);
            // Keep sys->worker_path / model_path in sync for respawn's argv — resolve effective executable at each
            // respawn, never silently reuse stale path. Overflow and clear are explicitly rejected/logged (VW-027).
            if (wp_new && wp_new[0]) {
              if (strlen(wp_new) < sizeof(sys->worker_path)) {
                snprintf(sys->worker_path, sizeof(sys->worker_path), "%s", wp_new);
              } else {
                vw_log_event(
                    VW_LOG_LEVEL_ERROR, "PLUGIN_CONFIG_PATH",
                    "new worker-path exceeds %zu bytes; retaining previous executable '%s' but reporting overflow",
                    sizeof(sys->worker_path) - 1, sys->worker_path);
              }
            } else {
              // Config cleared or empty: clear stored executable so next respawn re-resolves trusted absolute path
              // (Windows fail-closed, Linux PATH discovery) instead of silently reusing stale path.
              if (sys->worker_path[0] != '\0') {
                vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_CONFIG_PATH",
                             "worker-path cleared; next respawn will re-resolve trusted executable");
              }
              sys->worker_path[0] = '\0';
            }
            if (mp_new) {
              if (mp_new[0] && strlen(mp_new) < sizeof(sys->model_path)) {
                snprintf(sys->model_path, sizeof(sys->model_path), "%s", mp_new);
              } else if (!mp_new[0]) {
                sys->model_path[0] = '\0';
              } else {
                vw_log_event(VW_LOG_LEVEL_ERROR, "PLUGIN_CONFIG_PATH",
                             "new model-path exceeds %zu bytes; retaining previous model path",
                             sizeof(sys->model_path) - 1);
              }
            }
            // so the same broken settings will not re-trigger. Log and leave the loop alive with
            // a NULL client — the NULL-client guard below idles safely and the next settings
            // change (any config diff) starts a fresh config respawn without touching the
            sys->last_cfg_respawn_attempt_us = vw_platform_get_monotonic_time_us();
            if (!vw_plugin_respawn_worker(sys, paused, false)) {
              vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_CONFIG_RESPAWN_FAILED",
                           "new settings could not start a worker; captions idle until the next settings change");
            }
            atomic_store(&sys->respawn_in_progress, false);
          }
        }
        if (wp_new) free(wp_new);
        if (mp_new) free(mp_new);
        if (be_new) free(be_new);
        if (lg_new) free(lg_new);
        if (dl_new) free(dl_new);
      }
    }
    // Transport death (Step 17d resilience): respawn the worker (bounded) and restart the session
    // with the current MRL instead of disabling captions for the rest of playback.
    if (atomic_load(&sys->worker_dead)) {
      if (!vw_plugin_respawn_worker(sys, paused, true)) {
        break;
      }
      last_source_seek_us = -1;
      vw_benchmark_reset_live_clock(&sys->benchmark);
      continue;
    }
    // No worker (failed config respawn, or initial session start rejected): idle safely. All
    // client I/O below requires a client; treating NULL as transport death here would consume
    // the bounded recovery budget for a non-transport failure and could break the loop
    // permanently. A pending discontinuity stays latched until a worker returns.
    // Reconnect path: the committed snapshot means the same settings never re-diff, so a failed
    // config respawn would otherwise idle forever (even if the failure was transient — file
    // lock, AV scan — or the model appears later). Retry the launch every 10s; still a config
    // respawn, so the transport-recovery budget is never touched. Success re-enters the normal
    // loop on the next iteration.
    if (!sys->client) {
      int64_t idle_now_us = vw_platform_get_monotonic_time_us();
      if (idle_now_us - sys->last_cfg_respawn_attempt_us >= 10000000) {
        sys->last_cfg_respawn_attempt_us = idle_now_us;
        vw_log_event(VW_LOG_LEVEL_DEBUG, "PLUGIN_CONFIG_RESPAWN_RETRY",
                     "no worker; retrying launch with current settings");
        vw_plugin_respawn_worker(sys, paused, false);
      } else {
        vw_platform_sleep_ms(20);
      }
      continue;
    }
    // Throttle the object-tree walk to ~100ms: vlc_list_children allocates per level, and pause
    // state only changes at human timescale (8s windows make 100ms detection lag irrelevant).
    bool now_paused = paused;
    bool show_paused_subtitles =
        config_GetInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-show-paused") != 0;
    bool source_pause_preview = now_paused && show_paused_subtitles && atomic_load(&sys->source_mode_active);
    int64_t pause_transition_position_us = -1;
    int64_t now_us = vw_platform_get_monotonic_time_us();
    if (now_us - last_pause_poll_us >= 100000) {
      last_pause_poll_us = now_us;
      // find_input returns a HELD object (guards the input's lifetime across media swap/teardown
      // while we poll state); release it at the end of this throttled block.
      input_thread_t* input = vw_plugin_find_input((filter_t*)sys->presenter.p_filter_ctx);
      now_paused = input != NULL && input_GetState(input) == PAUSE_S;
      float playback_rate = 1.0f;
      if (input) {
        vlc_value_t rval;
        if (var_Get((vlc_object_t*)input, "rate", &rval) == VLC_SUCCESS && rval.f_float > 0.05f) {
          playback_rate = rval.f_float;
        }
        atomic_store(&sys->playback_rate, playback_rate);

        // Media swap detection mid-session
        if (sys->client && (sys->client->worker_capabilities & VW_CAPABILITY_SOURCE_MODE)) {
          input_item_t* item = input_GetItem(input);
          if (item) {
            char* raw_uri = input_item_GetURI(item);
            if (raw_uri) {
              char normalized_uri[VW_MAX_SOURCE_URL_BYTES];
              normalized_uri[0] = '\0';
              size_t raw_len = strlen(raw_uri);
              if (raw_len < sizeof(normalized_uri) &&
                  (strncmp(raw_uri, "file://", 7) == 0 || raw_uri[0] == '/' ||
                   (raw_len >= 3 && raw_uri[1] == ':' && (raw_uri[2] == '\\' || raw_uri[2] == '/')))) {
                strncpy(normalized_uri, raw_uri, sizeof(normalized_uri) - 1);
                normalized_uri[sizeof(normalized_uri) - 1] = '\0';
              }
              free(raw_uri);

              if (strcmp(normalized_uri, sys->active_source_url) != 0) {
                last_source_seek_us = -1;
                vw_benchmark_reset_live_clock(&sys->benchmark);
                vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_MEDIA_SWAP",
                             "media swap detected: '%s' -> '%s'; restarting session", sys->active_source_url,
                             normalized_uri);
                vw_paused_preview_state_reset_session(&sys->paused_preview);
                vw_caption_presenter_blank(&sys->presenter);
                vw_worker_client_stop_session(sys->client, VW_CTRL_REASON_MEDIA_END);
                vw_audio_chunk_t stale_chunk;
                while (vw_spsc_queue_pop(sys->queue, &stale_chunk)) {
                }
                strncpy(sys->active_source_url, normalized_uri, sizeof(sys->active_source_url) - 1);
                sys->active_source_url[sizeof(sys->active_source_url) - 1] = '\0';
                if (vw_worker_client_start_session(sys->client, 0, vw_plugin_catalog_id_from_path(sys->model_path),
                                                   normalized_uri[0] ? normalized_uri : NULL)) {
                  atomic_store(&sys->session_active, true);
                  atomic_store(&sys->source_mode_active, vw_worker_client_is_source_active(sys->client));
                  last_position_us = -1;
                  paused_position_us = -1;
                } else {
                  atomic_store(&sys->session_active, false);
                  atomic_store(&sys->source_mode_active, false);
                  vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_SESSION_START_FAIL",
                               "worker rejected media-swap session; captions idle, playback continues");
                }
              }
            }
          }
        }
      }
      int64_t position_us = vw_plugin_input_position_us(input);  // -1 when unavailable
      pause_transition_position_us = position_us;
      source_pause_preview = now_paused && show_paused_subtitles && atomic_load(&sys->source_mode_active);
      {
        float rate_diff = playback_rate - last_playback_rate;
        if (rate_diff < 0) rate_diff = -rate_diff;
        if (rate_diff > 0.02f) {  // meaningful threshold: >2% change
          vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_RATE_CHANGE",
                       "playback rate %.2f -> %.2f; blanking presenter and re-anchoring", (double)last_playback_rate,
                       (double)playback_rate);
          vw_caption_presenter_blank(&sys->presenter);  // erases already-scheduled SPU + pending
          last_source_seek_us = -1;  // rate re-anchor invalidates source seek filter baseline as well
          // Worker pacing re-anchor: next POSITION send uses new rate; if position known, send immediately.
          if (position_us >= 0 && atomic_load(&sys->session_active)) {
            current_position_us = position_us;
            if (!vw_worker_client_send_position(sys->client, current_position_us, current_position_us, playback_rate,
                                                now_paused && !source_pause_preview ? VW_POSITION_FLAG_PAUSED : 0)) {
              atomic_store(&sys->worker_dead, true);
              if (input) vlc_object_release((vlc_object_t*)input);
              last_playback_rate = playback_rate;
              continue;
            }
          }
          last_playback_rate = playback_rate;
        } else if (position_us >= 0 && atomic_load(&sys->session_active)) {
          current_position_us = position_us;
          if (!vw_worker_client_send_position(sys->client, current_position_us, current_position_us, playback_rate,
                                              now_paused && !source_pause_preview ? VW_POSITION_FLAG_PAUSED : 0)) {
            atomic_store(&sys->worker_dead, true);
            if (input) vlc_object_release((vlc_object_t*)input);
            continue;  // top of loop: respawn the worker
          }
        }
      }
      // BLOCK_FLAG_DISCONTINUITY never arrives, and the input time variable is clock-driven so
      // it stays frozen during the paused seek. Compare the position captured at the PAUSE
      // transition against the live position on RESUME: a >= 5s jump or backward jump means seek.
      if (now_paused != paused) {
        if (now_paused) {
          // Baseline at pause; only commit a readable position (-1 = input lookup failed, which
          // can be transient — a later poll while still paused backfills it below).
          paused_position_us = (position_us >= 0) ? position_us : -1;
        } else {
          if (paused_position_us >= 0 && position_us >= 0 &&
              ((position_us - paused_position_us >= VW_INPUT_JUMP_DISCONTINUITY_US) ||
               (paused_position_us - position_us > VW_PTS_JUMP_THRESHOLD_US))) {
            vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_SEEK_WHILE_PAUSED",
                         "position jumped %lldus during pause; restarting on resume",
                         (long long)(position_us - paused_position_us));
            atomic_store(&sys->discontinuity_pending, true);
            atomic_store(&sys->resume_pts_us, position_us);
          }
          paused_position_us = -1;
        }
      } else if (now_paused && position_us >= 0 && paused_position_us < 0) {
        // The pause edge raced input availability; backfill the baseline so a later resume can
        // still detect a paused-seek instead of silently skipping the jump check.
        paused_position_us = position_us;
      }

      // Continuous seek detection via input position (covers unflagged playing-case seeks and
      // paused seeks in builds where the time variable does advance). Scale 5s threshold with playback rate.
      int64_t seek_threshold_us =
          (int64_t)(VW_INPUT_JUMP_DISCONTINUITY_US * (playback_rate > 1.0f ? (playback_rate * 1.5f) : 1.0f));
      bool is_pos_forward_seek =
          (position_us >= 0 && last_position_us >= 0 && (position_us - last_position_us >= seek_threshold_us));
      bool is_pos_backward_seek =
          (position_us >= 0 && last_position_us >= 0 && (last_position_us - position_us > VW_PTS_JUMP_THRESHOLD_US));
      bool is_paused_seek = (now_paused && paused_position_us >= 0 && position_us >= 0 &&
                             (position_us - paused_position_us > VW_PTS_JUMP_THRESHOLD_US ||
                              paused_position_us - position_us > VW_PTS_JUMP_THRESHOLD_US));
      if (is_pos_forward_seek || is_pos_backward_seek || is_paused_seek) {
        vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_SEEK_POSITION", "position jumped %lldus; seek signaled",
                     (long long)(position_us - last_position_us));
        atomic_store(&sys->discontinuity_pending, true);
        atomic_store(&sys->resume_pts_us, position_us);
        if (is_paused_seek) paused_position_us = position_us;
      }
      if (position_us >= 0) last_position_us = position_us;
      if (input) vlc_object_release((vlc_object_t*)input);
    }
    if (now_paused != paused) {
      if (now_paused) {
        if (source_pause_preview) {
          int64_t preview_target_us = -1;
          if (vw_paused_preview_capture_target(source_pause_preview, pause_transition_position_us,
                                               &preview_target_us)) {
            current_position_us = preview_target_us;
            atomic_store(&sys->discontinuity_pending, true);
            atomic_store(&sys->resume_pts_us, preview_target_us);
          }
          vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_PAUSE", "local caption held; source look-ahead remains active");
        } else {
          vw_caption_presenter_blank(&sys->presenter);
          vw_worker_client_pause_session(sys->client);
          vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_PAUSE", "playback paused; PCM forwarding suspended");
        }
      } else {
        vw_paused_preview_state_reset_session(&sys->paused_preview);
        vw_caption_presenter_blank(&sys->presenter);
        vw_worker_client_resume_session(sys->client);
        vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_RESUME", "playback resumed; PCM forwarding active");
      }
      paused = now_paused;
    }

    // VW-003/VW-014: Live discontinuities must STOP old session, discard/re-anchor, START fresh epoch.
    // Source look-ahead keeps documented POSITION-seek behavior. Unavailable targets still reset state.
    // Plugin-side stale rejection via new session_id (live) and PTS filter (source) covers buffered frames
    // and translated results.
    if (atomic_load(&sys->discontinuity_pending)) {
      int64_t resume_pts_us = atomic_load(&sys->resume_pts_us);
      int64_t seek_target_us = (current_position_us >= 0) ? current_position_us : resume_pts_us;
      atomic_store(&sys->discontinuity_pending, false);
      bool is_source_mode = atomic_load(&sys->source_mode_active);
      bool preview_after_paused_seek = paused && is_source_mode && show_paused_subtitles;
      vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_DISCONTINUITY", "seek=%lld mode=%s paused_preview=%d",
                   (long long)seek_target_us, is_source_mode ? "source" : "live", preview_after_paused_seek ? 1 : 0);
      if (preview_after_paused_seek) {
        vw_paused_preview_arm(&sys->paused_preview);
      } else {
        vw_caption_presenter_blank(&sys->presenter);
        vw_paused_preview_state_reset_session(&sys->paused_preview);
      }
      vw_audio_chunk_t stale;
      vw_benchmark_reset_live_clock(&sys->benchmark);
      while (vw_spsc_queue_pop(sys->queue, &stale)) {
      }
      // Request callback to reset resampler accumulator/PTS (no cross-thread field writes, VW-019).
      atomic_store(&sys->capture_reset_pending, true);
      if (!is_source_mode) {
        // Live: STOP old epoch and START fresh session_id/epoch even when target unavailable.
        if (sys->client) {
          vw_worker_client_stop_session(sys->client, VW_CTRL_REASON_SEEK_DISCONTINUITY);
        }
        last_position_us = -1;
        paused_position_us = -1;
        if (seek_target_us >= 0) current_position_us = seek_target_us;
        last_source_seek_us = -1;
        if (sys->client) {
          const char* model_id = vw_plugin_catalog_id_from_path(sys->model_path);
          bool started = vw_worker_client_start_session(sys->client, 0, model_id, NULL);
          atomic_store(&sys->session_active, started);
          atomic_store(&sys->source_mode_active, started && vw_worker_client_is_source_active(sys->client));
          if (started) {
            vw_worker_client_send_translate_ctrl(sys->client, sys->cfg_translate_enabled, sys->cfg_translate_from,
                                                 sys->cfg_translate_to, (uint8_t)sys->cfg_translate_mode);
            // Pace new epoch immediately if we have a target.
            if (seek_target_us >= 0) {
              vw_worker_client_send_position(sys->client, seek_target_us, seek_target_us, last_playback_rate,
                                             (paused ? VW_POSITION_FLAG_PAUSED : 0));
            }
            vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_DISCONTINUITY_LIVE", "live epoch restarted; new session_id");
          } else {
            vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_DISCONTINUITY_LIVE", "live epoch restart failed; captions idle");
          }
        }
      } else {
        // Source: preserve POSITION-seek, but track seek target for stale segment filtering.
        if (seek_target_us >= 0) {
          last_source_seek_us = seek_target_us;
          uint8_t position_flags = vw_paused_preview_seek_flags(paused, preview_after_paused_seek);
          if (!vw_worker_client_send_position(sys->client, seek_target_us, seek_target_us, last_playback_rate,
                                              position_flags)) {
            atomic_store(&sys->worker_dead, true);
          }
        } else {
          vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_SEEK_TARGET_MISSING",
                       "source seek: no media position; preserving prior anchor %lld, not re-anchoring to 0",
                       (long long)last_source_seek_us);
          // Do not send fake POSITION(0,0,SEEK); preserve prior anchor and avoid false zero timeline.
        }
      }
    }
    // Drain old queued audio if producer signaled an invalid-PTS epoch transition (VW-013).
    // This discards audio queued before the invalid interval without touching the SPSC from the producer callback.
    if (atomic_load(&sys->invalid_pts_pending)) {
      vw_audio_chunk_t drain_tmp;
      while (vw_spsc_queue_pop(sys->queue, &drain_tmp)) {
      }
      atomic_store(&sys->invalid_pts_pending, false);
      vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_PTS_DRAIN", "drained old queued audio after invalid PTS interval");
    }
    // Drain the SPSC queue (send burst), then one receive: 5ms after sends (audio latency
    // priority), 20ms when idle (the idle wait doubles as cadence, no extra sleep).
    vw_audio_chunk_t chunk;
    bool sent_any = false;
    bool is_source_mode = atomic_load(&sys->source_mode_active);
    bool is_session_active = atomic_load(&sys->session_active);
    while (vw_spsc_queue_pop(sys->queue, &chunk)) {
      if (paused || is_source_mode || !is_session_active) {
        continue;  // discard audio captured before/during pause, in source mode, or when session is not active
      }
      sys->chunks_sent++;
      if (!vw_worker_client_send_audio(sys->client, &chunk)) {
        atomic_store(&sys->worker_dead, true);
        vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_WORKER_DEAD",
                     "send_audio failed after %llu chunks; captions disabled, passthrough only",
                     (unsigned long long)sys->chunks_sent);
        break;
      }
      vw_benchmark_record_audio(&sys->benchmark, chunk.start_pts_us, chunk.duration_us,
                                vw_platform_get_monotonic_time_us());
      sent_any = true;
    }
    if (atomic_load(&sys->worker_dead)) continue;  // top of loop: respawn the worker

    vw_worker_recv_t recv;
    int recv_status = vw_worker_client_receive_frame(sys->client, sent_any ? 5000 : 20000, &recv);
    if (recv_status == VW_IPC_RECV_FATAL) {
      atomic_store(&sys->worker_dead, true);
      sys->model_download_id[0] = '\0';
      config_PutPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-model-status", "failed:worker");
      vw_caption_presenter_clear_model_progress(&sys->presenter);
      vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_WORKER_DEAD",
                   "receive_frame fatal (transport dead); captions disabled, passthrough only");
      continue;  // top of loop: respawn the worker
    }
    if (recv_status == VW_IPC_RECV_OK) {
      sys->frames_received++;
      vw_benchmark_record_frame(&sys->benchmark);
      switch (recv.type) {
        case VW_MSG_CAPTION_SEGMENT:
          // A segment transcribed BEFORE a seek can still be in flight when the restart completes.
          // Its session_id predates the new epoch — never render stale pre-seek text over the OSD.
          // For live mode this catches STOP+START epoch change; for source mode, also filter by PTS against last
          // seek.
          if (memcmp(recv.segment.session_id.bytes, sys->client->session_id, VW_SESSION_ID_BYTES) != 0) {
            vw_log_event(VW_LOG_LEVEL_DEBUG, "PLUGIN_STALE_SEGMENT",
                         "dropping segment from previous epoch (session mismatch)");
            vw_benchmark_record_caption_filtered(&sys->benchmark, false, true, false);
            break;
          }
          if (is_source_mode && last_source_seek_us >= 0 && recv.segment.start_pts_us < last_source_seek_us) {
            vw_log_event(VW_LOG_LEVEL_DEBUG, "PLUGIN_STALE_SEGMENT_PTS",
                         "dropping source segment start=%lld < seek %lld", (long long)recv.segment.start_pts_us,
                         (long long)last_source_seek_us);
            vw_benchmark_record_caption_filtered(&sys->benchmark, false, true, false);
            break;
          }
          sys->segments_received++;
          int64_t segment_now_us = vw_platform_get_monotonic_time_us();
          vw_benchmark_record_caption_received(&sys->benchmark, &recv.segment, segment_now_us, is_source_mode);
          if (recv.segment.translation_attempted) {
            vw_benchmark_record_translation(&sys->benchmark, recv.segment.translation_tier,
                                            recv.segment.translation_latency_us,
                                            recv.segment.translated_text_bytes > 0);
          }
          vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_SEGMENT",
                       "segment received: id=%llu text_len=%zu start=%lld end=%lld is_final=%d",
                       (unsigned long long)recv.segment.segment_id,
                       recv.segment.text_utf8 ? strlen(recv.segment.text_utf8) : 0,
                       (long long)recv.segment.start_pts_us, (long long)recv.segment.end_pts_us, recv.segment.is_final);
          // While paused the look-ahead backlog (decoded pre-pause) keeps arriving; the pause
          // transition already blanked the channel and the playhead is frozen, so rendering these
          // would show captions for audio not being played. Drain without rendering (Step 17d).
          if (!paused) {
            // Synchronous render: recv.text_buf owns the segment text for this iteration, so the
            // presenter may copy/format it safely. No OSD when the vout walk fails (passthrough).
            // input_time_us is reserved for media-domain scheduling (17c); the presenter renders
            // in the OSD clock domain (mdate), which this VLC build displays reliably.
            if (vw_caption_presenter_show_segment(&sys->presenter, &recv.segment, current_position_us,
                                                  is_source_mode)) {
              vw_benchmark_record_caption_sent(&sys->benchmark, vw_platform_get_monotonic_time_us());
            } else {
              vw_benchmark_record_caption_filtered(&sys->benchmark, false, false, true);
            }
          } else if (is_source_mode && show_paused_subtitles && vw_paused_preview_take(&sys->paused_preview)) {
            if (vw_caption_presenter_show_paused(&sys->presenter, &recv.segment)) {
              vw_benchmark_record_caption_sent(&sys->benchmark, vw_platform_get_monotonic_time_us());
            } else {
              vw_benchmark_record_caption_filtered(&sys->benchmark, false, false, true);
            }
          } else {
            vw_benchmark_record_caption_filtered(&sys->benchmark, true, false, false);
            vw_log_event(VW_LOG_LEVEL_DEBUG, "PLUGIN_PAUSED_DROP", "segment id=%llu dropped while paused",
                         (unsigned long long)recv.segment.segment_id);
          }
          break;
        case VW_MSG_STATUS:
          sys->status_received++;
          vw_benchmark_update_status(&sys->benchmark, &recv.status);
          vw_log_event(VW_LOG_LEVEL_DEBUG, "PLUGIN_STATUS", "queued=%lld inference=%lld dropped=%lld",
                       (long long)recv.status.queued_audio_us, (long long)recv.status.inference_us,
                       (long long)recv.status.dropped_audio_us);
          if (recv.status.resolved_backend[0] != '\0') {
            // Cross-thread config write is safe: VLC config API is internally locked
            // (config_PutPsz takes the config lock). Mirrors worker's resolved backend
            // (gpu|cpu, NUL-padded) for the GUI's informational whisper-backend-active key.
            config_PutPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-backend-active",
                          recv.status.resolved_backend);
          }
          break;
        case VW_MSG_MODEL_PROGRESS: {
          config_PutInt(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-model-progress",
                        (int)recv.progress.pct);
          const char* stage_name = "idle";
          switch (recv.progress.stage) {
            case 0:
              stage_name = "idle";
              break;
            case 1:
              stage_name = "downloading";
              break;
            case 2:
              stage_name = "verifying";
              break;
            case 3:
              stage_name = "done";
              break;
            case 4:
              stage_name = "failed";
              break;
            case 5:
              stage_name = "aborting";
              break;
            default:
              stage_name = "idle";
              break;
          }
          char prog_status[80];
          snprintf(prog_status, sizeof(prog_status), "%s:%s", stage_name, recv.progress.model_id);
          config_PutPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-model-status", prog_status);
          if (recv.progress.stage == VW_MODEL_STAGE_IDLE) {
            // The worker emits an initial IDLE snapshot before starting the asynchronous download. Keep the
            // pending correlation id intact so the later DONE frame can activate and respawn the new model.
            vw_caption_presenter_clear_model_progress(&sys->presenter);
          } else {
            vw_caption_presenter_show_model_progress(&sys->presenter, &recv.progress);
          }
          vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_MODEL_PROGRESS",
                       "model '%s' stage=%s pct=%u bytes=%llu/%llu pending='%s'", recv.progress.model_id, stage_name,
                       (unsigned)recv.progress.pct, (unsigned long long)recv.progress.bytes_done,
                       (unsigned long long)recv.progress.bytes_total,
                       sys->model_download_id[0] ? sys->model_download_id : "(none)");
          if (recv.progress.stage == VW_MODEL_STAGE_FAILED) {
            vw_caption_presenter_clear_model_progress(&sys->presenter);
            sys->model_download_id[0] = '\0';
          }
          if (recv.progress.stage == VW_MODEL_STAGE_DONE &&
              strcmp(sys->model_download_id, recv.progress.model_id) == 0) {
            // VW-002: clear progress channel BEFORE zeroing model_download_id so clear guard is not bypassed
            vw_caption_presenter_clear_model_progress(&sys->presenter);
            sys->model_download_id[0] = '\0';
            if (!vw_plugin_activate_downloaded_model(sys, recv.progress.model_id, paused)) {
              config_PutPsz(VLC_OBJECT((filter_t*)sys->presenter.p_filter_ctx), "whisper-model-status",
                            "failed:activation");
              vw_caption_presenter_clear_model_progress(&sys->presenter);
            }
          }
          break;
        }
        case VW_MSG_ERROR:
          sys->errors_received++;
          vw_log_event(VW_LOG_LEVEL_ERROR, "PLUGIN_WORKER_ERROR", "code=%u recoverable=%u msg=%.*s",
                       recv.error.error_code, recv.error.recoverable,
                       (int)strnlen(recv.error.message, VW_MAX_ERROR_MSG_BYTES), recv.error.message);
          // A non-recoverable worker error disables captions; playback continues (api-contracts).
          if (!recv.error.recoverable) atomic_store(&sys->worker_dead, true);
          break;
        default:
          break;
      }
    }

    // Look-ahead cues flush as the media position approaches; live cues flush immediately in the OSD clock domain
    // because their system-date PTS cannot be compared with INPUT_GET_TIME.
    if (sys->presenter.has_pending && !paused) {
      if (!is_source_mode || current_position_us < 0 ||
          sys->presenter.pending_segment.start_pts_us <= current_position_us + 100000LL) {
        vw_caption_presenter_flush(&sys->presenter, current_position_us, is_source_mode);
      }
    }

    if (sent_any && (sys->chunks_sent % 1024) == 0) {
      vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_SENDER", "sent %llu chunks, received %u worker frames",
                   (unsigned long long)sys->chunks_sent, sys->frames_received);
    }
    vw_benchmark_flush_if_due(&sys->benchmark, vw_platform_get_monotonic_time_us());
  }
  return NULL;
}

// Finds the VLC input thread reachable from the filter. Mirrors the caption presenter's object
// walk exactly — checks each node and its children list for an "input" object (the input thread
// is a child of the playlist/libvlc hierarchy, not an ancestor of the audio filter, as the
// presenter's live log shows). Deliberately avoids vlc_object_find_name (deprecated, and
// weak-linkable to NULL on MinGW per the milestone-3 postmortem). Safe from the sender thread.
// Returns a HELD input_thread_t (vlc_object_hold) — the caller MUST vlc_object_release it when
// done, since the children list that protected the object is released before returning and the
// input may be destroyed at any time (media swap, teardown) while the sender polls state.
static input_thread_t* vw_plugin_find_input(filter_t* p_filter) {
  if (!p_filter) return NULL;
  vlc_object_t* cur = VLC_OBJECT(p_filter);
  while (cur) {
    if (cur->obj.object_type && strcmp(cur->obj.object_type, "input") == 0) {
      vlc_object_hold(cur);
      return (input_thread_t*)cur;
    }
    vlc_list_t* children = vlc_list_children(cur);
    if (children) {
      for (int i = 0; i < children->i_count; i++) {
        vlc_object_t* child = (vlc_object_t*)children->p_values[i].p_address;
        if (child && child->obj.object_type && strcmp(child->obj.object_type, "input") == 0) {
          vlc_object_hold(child);  // hold BEFORE releasing the list that guards the child's lifetime
          vlc_list_release(children);
          return (input_thread_t*)child;
        }
      }
      vlc_list_release(children);
    }
    cur = cur->obj.parent;
  }
  return NULL;
}

// Reads the input's current media position in microseconds, or -1 when unavailable.
static int64_t vw_plugin_input_position_us(input_thread_t* input) {
  if (!input) return -1;
  int64_t position_us = 0;
  if (input_Control(input, INPUT_GET_TIME, &position_us) != VLC_SUCCESS) return -1;
  return position_us;
}

// Passthrough filter callback required by VLC filter pipeline (100% lock-free, Rule 4 compliant)
static block_t* vw_plugin_filter(filter_t* p_filter, block_t* p_block) {
  vw_plugin_sys_t* sys = (vw_plugin_sys_t*)p_filter->p_sys;
  if (!sys || !p_block) {
    return p_block;  // Passthrough unchanged
  }

  // Determine format from VLC pipeline
  vw_audio_format_t fmt;
  if (p_filter->fmt_in.audio.i_format == VLC_CODEC_FL32) {
    fmt = VW_AUDIO_FORMAT_FL32;
  } else if (p_filter->fmt_in.audio.i_format == VLC_CODEC_S16N) {
    fmt = VW_AUDIO_FORMAT_S16;
  } else if (p_filter->fmt_in.audio.i_format == VLC_CODEC_S32N) {
    fmt = VW_AUDIO_FORMAT_S32;
  } else {
    // Unsupported codec; skip tapping but allow passthrough
    return p_block;
  }

  vw_audio_input_t input = {.pcm_data = p_block->p_buffer,
                            .frame_count = p_block->i_nb_samples,
                            .pts_us = p_block->i_pts,
                            .format = fmt,
                            .sample_rate = p_filter->fmt_in.audio.i_rate,
                            .channels = p_filter->fmt_in.audio.i_channels};

  // Invalid VLC PTS must not contaminate live caption timing (VW-013). Skip capture for invalid PTS
  // and explicitly clear buffered audio when valid PTS resumes after invalid stretch to avoid mislabeling.
  // Producer never drains the SPSC queue directly; it signals the sender to drain old queued audio before new
  // valid audio is forwarded.
  bool pts_valid = (p_block->i_pts >= VLC_TS_0 && p_block->i_pts >= 0 && p_block->i_pts <= 315360000000000LL);
  if (!pts_valid) {
    sys->last_pts_was_invalid = true;
    // Do not capture this block — would inject negative timestamp hypothesis.
    return p_block;
  }
  if (sys->last_pts_was_invalid) {
    // Valid PTS resumed after invalid stretch: reset resampler accumulator to avoid cross-epoch repetition,
    // and signal sender to drain any old queued chunks before new valid audio is sent.
    sys->capture.resample_acc = 0;
    sys->capture.sample_remainder = 0;
    sys->capture.resample_source_rate = 0;
    atomic_store(&sys->invalid_pts_pending, true);
    sys->last_pts_was_invalid = false;
    // Skip this transition block's audio to ensure old queue is drained before new audio is enqueued.
    // Next block's audio will be the first new-epoch audio forwarded.
    return p_block;
  }
  // Step 17d seek/discontinuity detection — realtime-safe: atomics only.
  // 5s forward threshold prevents network transport jitter / buffer slips (< 5s) from wiping captions.
  // Backward jumps > 500ms trigger seek re-sync immediately.
  // NOTE: block->i_pts is in the aout's SYSTEM-DATE domain (µs since boot on Windows, compared
  // against mdate() in aout_DecPlay), NOT the media position — it is never stored as the seek
  // target; the sender derives the target from the polled media position (INPUT_GET_TIME).
  int64_t pts = p_block->i_pts;
  if (pts >= VLC_TS_0 && sys->capture.last_pts_us > 0) {
    int64_t diff = pts - sys->capture.last_pts_us;
    bool is_flagged = (p_block->i_flags & BLOCK_FLAG_DISCONTINUITY) != 0;
    bool is_forward_seek = (diff >= VW_INPUT_JUMP_DISCONTINUITY_US);
    bool is_backward_seek = (diff < -VW_PTS_JUMP_THRESHOLD_US);

    if (is_backward_seek || (is_forward_seek && is_flagged) || (diff >= VW_INPUT_JUMP_DISCONTINUITY_US)) {
      atomic_store(&sys->discontinuity_pending, true);
    }
  } else if ((p_block->i_flags & BLOCK_FLAG_DISCONTINUITY) && pts >= VLC_TS_0) {
    atomic_store(&sys->discontinuity_pending, true);
  }

  if (pts >= VLC_TS_0) {
    sys->capture.last_pts_us = pts;
  }

  // Rate-based throttling/dropping (VW-019): drop audio blocks when playback rate exceeds 4.0x
  if (atomic_load(&sys->playback_rate) > 4.0f) {
    return p_block;
  }

  if (!atomic_load(&sys->source_mode_active)) {
    vw_audio_capture_process_block(&sys->capture, &input);
  }

  // Return the original block untouched to preserve user playback quality
  return p_block;
}

// Internal plugin callbacks adhering to Rule 3 vw_ prefix
static int vw_plugin_open(vlc_object_t* obj) {
  filter_t* p_filter = (filter_t*)obj;

  vw_plugin_sys_t* sys = calloc(1, sizeof(vw_plugin_sys_t));
  if (!sys) {
    return VLC_ENOMEM;
  }

  // Create an 8-second buffer capacity assuming 16kHz Mono chunks (16384 bytes = 512ms per chunk)
  // 8 seconds / 0.512 seconds = ~16 chunks
  sys->queue = vw_spsc_queue_create(16);
  if (!sys->queue) {
    free(sys);
    return VLC_ENOMEM;
  }

  p_filter->p_sys = (filter_sys_t*)sys;
  atomic_init(&sys->sender_running, false);
  atomic_init(&sys->worker_dead, false);
  atomic_init(&sys->discontinuity_pending, false);
  atomic_init(&sys->resume_pts_us, -1);
  atomic_init(&sys->source_mode_active, false);
  atomic_init(&sys->session_active, false);
  vw_paused_preview_state_init(&sys->paused_preview);
  atomic_init(&sys->playback_rate, 1.0f);
  atomic_init(&sys->capture_reset_pending, false);
  atomic_init(&sys->invalid_pts_pending, false);
  atomic_init(&sys->respawn_in_progress, false);

  sys->capture.target_sample_rate = VW_AUDIO_TARGET_RATE;
  sys->capture.target_channels = 1;
  sys->capture.sample_remainder = 0;
  sys->capture.total_input_frames = 0;
  sys->capture.resample_acc = 0;
  sys->capture.resample_source_rate = 0;
  sys->capture.total_samples_processed = 0;
  sys->capture.last_pts_us = 0;
  sys->capture.reset_pending = &sys->capture_reset_pending;
  sys->capture.queue = sys->queue;
  sys->capture.playback_rate = &sys->playback_rate;

  p_filter->pf_audio_filter = vw_plugin_filter;
  p_filter->fmt_out.audio = p_filter->fmt_in.audio;

  sys->presenter.p_filter_ctx = p_filter;  // sender thread renders SEGMENT frames via this context
  sys->presenter.spu_channel_id = -1;
  sys->presenter.spu_channel_registered = false;
  sys->presenter.model_progress_channel_id = -1;
  sys->presenter.model_progress_channel_registered = false;

  vw_log_set_sink(vw_plugin_log_sink, obj);
  bool open_logging = config_GetInt(obj, "whisper-logging") != 0;
  vw_log_set_enabled(open_logging);

#ifdef _WIN32
  // 256-bit CSPRNG-derived endpoint: 32 random bytes hex-encoded (64 hex chars) bounded within pipe namespace.
  // Fail closed if CSPRNG fails; never fall back to PID-derived name (VW-001).
  uint8_t rnd[32];
  if (vw_platform_get_random_bytes(rnd, sizeof(rnd))) {
    char hex[65];
    for (size_t i = 0; i < sizeof(rnd); i++) snprintf(hex + i * 2, 3, "%02x", rnd[i]);
    hex[64] = '\0';
    // Bounded: prefix 19 + 64 hex = 83 < 256; valid hex chars only.
    snprintf(sys->pipe_name, sizeof(sys->pipe_name), "\\\\.\\pipe\\vlc-whisper-%s", hex);
    // Best-effort scrub of stack copy
    volatile uint8_t* vrnd = rnd;
    for (size_t i = 0; i < sizeof(rnd); i++) vrnd[i] = 0;
    for (size_t i = 0; i < sizeof(hex); i++) ((volatile char*)hex)[i] = 0;
  } else {
    // Scrub rnd even on failure path
    volatile uint8_t* vrnd = rnd;
    for (size_t i = 0; i < sizeof(rnd); i++) vrnd[i] = 0;
    vw_log_event(VW_LOG_LEVEL_ERROR, "PLUGIN_PIPE_RNG_FAIL",
                 "CSPRNG failed for pipe name; failing closed (no PID fallback)");
    vw_log_set_sink(NULL, NULL);
    vw_spsc_queue_destroy(sys->queue);
    p_filter->p_sys = NULL;
    free(sys);
    return VLC_EGENERIC;
  }
#else
  // Use an unpredictable, per-instance endpoint. The transport authenticates separately,
  // but predictable names permit stale-file collisions and endpoint hijacking attempts.
  uint8_t endpoint_random[16];
  if (!vw_platform_get_random_bytes(endpoint_random, sizeof(endpoint_random))) {
    vw_log_event(VW_LOG_LEVEL_ERROR, "PLUGIN_PIPE_RNG_FAIL", "CSPRNG failed for Unix socket name");
    vw_log_set_sink(NULL, NULL);
    vw_spsc_queue_destroy(sys->queue);
    p_filter->p_sys = NULL;
    free(sys);
    return VLC_EGENERIC;
  }
  char endpoint_hex[sizeof(endpoint_random) * 2 + 1];
  for (size_t i = 0; i < sizeof(endpoint_random); i++) snprintf(endpoint_hex + i * 2, 3, "%02x", endpoint_random[i]);
  endpoint_hex[sizeof(endpoint_hex) - 1] = '\0';
  const char* runtime_dir = "/tmp";
  const char* configured_runtime_dir = getenv("XDG_RUNTIME_DIR");
  struct stat runtime_stat;
  if (configured_runtime_dir && configured_runtime_dir[0] == '/' && stat(configured_runtime_dir, &runtime_stat) == 0 &&
      S_ISDIR(runtime_stat.st_mode) && runtime_stat.st_uid == geteuid() &&
      (runtime_stat.st_mode & (S_IWGRP | S_IWOTH)) == 0) {
    runtime_dir = configured_runtime_dir;
  }
  int endpoint_length =
      snprintf(sys->pipe_name, sizeof(sys->pipe_name), "%s/vlc-whisper-%s.sock", runtime_dir, endpoint_hex);
  // sockaddr_un::sun_path is 108 bytes on supported Linux builds. An unusually long runtime directory falls back to
  // the bounded random /tmp name instead of passing a truncated endpoint to the worker.
  if (endpoint_length < 0 || endpoint_length >= 108) {
    snprintf(sys->pipe_name, sizeof(sys->pipe_name), "/tmp/vlc-whisper-%s.sock", endpoint_hex);
  }
  memset(endpoint_random, 0, sizeof(endpoint_random));
  memset(endpoint_hex, 0, sizeof(endpoint_hex));
#endif

  // Explicit per-install override for layouts outside the bounded discovery paths.
  char* configured = config_GetPsz(obj, "worker-path");
  bool worker_path_configured = false;
  if (configured && configured[0]) {
    if (strlen(configured) >= sizeof(sys->worker_path)) {
      vw_log_event(VW_LOG_LEVEL_ERROR, "PLUGIN_CONFIG_PATH",
                   "worker-path exceeds %zu bytes (OS path limit); refusing to launch a different worker",
                   sizeof(sys->worker_path) - 1);
      free(configured);
      vw_log_set_sink(NULL, NULL);
      vw_spsc_queue_destroy(sys->queue);
      p_filter->p_sys = NULL;
      free(sys);
      return VLC_EGENERIC;
    }
    snprintf(sys->worker_path, sizeof(sys->worker_path), "%s", configured);
    worker_path_configured = true;
  }
  if (sys->worker_path[0] == '\0' && vw_plugin_resolve_worker_path(sys->worker_path, sizeof(sys->worker_path))) {
    // Resolved to a concrete path next to the plugin or VLC executable.
  }
  if (sys->worker_path[0] == '\0') {
    vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_WORKER_UNAVAILABLE",
                 "worker discovery failed; captions disabled, passthrough only");
  }
  bool open_worker_path_configured = worker_path_configured;
  free(configured);
  // path surfaces as E_MODEL_MISSING at session start — do not pre-check existence here. An
  // oversized configured path fails closed (refuse to open) rather than load a different model or
  // silently truncate.
  char* model_cfg = config_GetPsz(obj, "model-path");
  if (model_cfg && model_cfg[0]) {
    if (strlen(model_cfg) >= sizeof(sys->model_path)) {
      vw_log_event(VW_LOG_LEVEL_ERROR, "PLUGIN_CONFIG_PATH",
                   "model-path exceeds %zu bytes (OS path limit); refusing to load a different model",
                   sizeof(sys->model_path) - 1);
      free(model_cfg);
      vw_log_set_sink(NULL, NULL);  // close() never runs after a failed open; drop the dangling sink
      vw_spsc_queue_destroy(sys->queue);
      p_filter->p_sys = NULL;
      free(sys);
      return VLC_EGENERIC;
    }
    snprintf(sys->model_path, sizeof(sys->model_path), "%s", model_cfg);
  } else if (vw_plugin_resolve_model_path(sys->model_path, sizeof(sys->model_path))) {
    // Resolved to a concrete file next to the plugin or VLC executable.
  }
  free(model_cfg);

  if (!vw_platform_get_random_bytes(sys->auth_token, VW_AUTH_TOKEN_BYTES)) {
    vw_log_event(VW_LOG_LEVEL_ERROR, "PLUGIN_RNG_FAIL", "failed to generate random auth_token; failing closed");
    vw_log_set_sink(NULL, NULL);
    vw_spsc_queue_destroy(sys->queue);
    p_filter->p_sys = NULL;
    free(sys);
    return VLC_EGENERIC;
  }

  vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_WORKER_LAUNCH", "spawning worker: %s", sys->worker_path);
  char* open_be = config_GetPsz(obj, "whisper-backend");
  char* open_lg = config_GetPsz(obj, "whisper-language");
  int64_t open_thr = config_GetInt(obj, "whisper-threads");
  int open_gpu = -1;
  if (config_FindConfig("whisper-gpu-device")) {
    open_gpu = (int)config_GetInt(obj, "whisper-gpu-device");
  }
  char open_model_dir[VW_PATH_MAX_BYTES];
  open_model_dir[0] = '\0';
  vw_plugin_get_model_dir(open_model_dir, sizeof(open_model_dir));
  vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_MODEL_PATH", "worker model='%s' download_dir='%s'",
               sys->model_path[0] ? sys->model_path : "(bundled/default)",
               open_model_dir[0] ? open_model_dir : "(unavailable)");
  sys->client = vw_plugin_launch_with_auto_retry(
      sys->worker_path, sys->pipe_name, sys->auth_token, sys->model_path[0] ? sys->model_path : NULL, open_be, open_lg,
      (int)open_thr, open_gpu, open_model_dir[0] ? open_model_dir : NULL, open_logging, open_worker_path_configured);
  if (sys->client && open_worker_path_configured == false) {
    // If retry succeeded, update stored worker_path to reflect actual launched CPU binary for future respawns.
    // The wrapper does not mutate worker_path; detect CPU fallback via capability or path.
    // For simplicity, if backend auto and worker_path contains no cpu but client succeeded via fallback,
    // we could keep original path — respawn will retry again. That's acceptable (bounded retry each time).
  }
  if (open_be) free(open_be);
  atomic_store(&sys->sender_running, true);
  atomic_store(&sys->worker_dead, false);
  atomic_store(&sys->discontinuity_pending, false);
  // Keep -1 as the "no known media position" sentinel until poll detectors observe a real target.
  atomic_store(&sys->resume_pts_us, -1);
  atomic_store(&sys->source_mode_active, false);
  atomic_store(&sys->session_active, false);
  vw_paused_preview_state_reset_session(&sys->paused_preview);
  atomic_store(&sys->respawn_in_progress, false);
  sys->cfg_snapshot_valid = false;
  sys->last_config_poll_us = 0;
  sys->cfg_worker_path[0] = '\0';
  sys->cfg_model_path[0] = '\0';
  sys->cfg_backend[0] = '\0';
  sys->cfg_language[0] = '\0';
  sys->cfg_model_download[0] = '\0';
  sys->cfg_threads = 4;
  sys->cfg_logging = open_logging;
  if (vw_platform_thread_create(&sys->sender_thread, vw_plugin_sender_main, sys)) {
    sys->sender_started = true;
    vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_SENDER_START", "sender thread started (5/20 ms cadence)");
  } else {
    atomic_store(&sys->sender_running, false);
    vw_log_event(VW_LOG_LEVEL_WARN, "PLUGIN_SENDER_START_FAIL",
                 "sender thread creation failed; captions disabled, passthrough only");
  }

  vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_OPEN", "vlc-whisper audio filter module opened");
  return VLC_SUCCESS;
}

static void vw_plugin_close(vlc_object_t* obj) {
  filter_t* p_filter = (filter_t*)obj;
  vw_plugin_sys_t* sys = (vw_plugin_sys_t*)p_filter->p_sys;

  if (sys) {
    // Detach the callback-visible state before teardown. VLC normally serializes filter close with
    // audio callbacks, but this also makes a late callback a harmless passthrough.
    p_filter->p_sys = NULL;
    // Stop and join the sender thread before touching the client/queue it uses.
    atomic_store(&sys->sender_running, false);
    if (sys->sender_started) {
      vw_platform_thread_join(sys->sender_thread);
    }
    // Clear VLC overlays while the filter/vout hierarchy is still in the close call's lifetime.
    vw_plugin_bind_close_accounting(&sys->benchmark, &sys->frames_received, &sys->segments_received,
                                    &sys->status_received, &sys->errors_received);
    vw_caption_presenter_clear(&sys->presenter);
    if (sys->client) {
      if (!atomic_load(&sys->worker_dead)) {
        vw_worker_client_stop_session(sys->client, 0);
      }
      vw_worker_client_shutdown(sys->client);
      vw_worker_client_disconnect(sys->client);
      sys->client = NULL;
    }
    if (sys->queue) {
      vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_CLOSE",
                   "Dropped %llu us of audio during session; sent %llu chunks, %u segments, %u status, %u errors",
                   (unsigned long long)vw_spsc_queue_get_dropped_microseconds(sys->queue),
                   (unsigned long long)sys->chunks_sent, sys->segments_received, sys->status_received,
                   sys->errors_received);
      vw_spsc_queue_destroy(sys->queue);
    }
    if (sys->benchmark.active) {
      vw_benchmark_finalize(&sys->benchmark, vw_platform_get_monotonic_time_us());
      if (sys->benchmark.report_path[0]) {
        vw_log_event(VW_LOG_LEVEL_INFO, "PLUGIN_BENCHMARK", "session report: %s", sys->benchmark.report_path);
      }
    }
    free(sys);
  }
  vw_log_set_sink(NULL, NULL);
}

// VLC module definition macro expands to vlc_entry__3_0_0f ABI symbol
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
vlc_module_begin() set_shortname("VLC-Whisper") set_description("Offline Whisper AI Captions Filter")
    set_capability("audio filter", 0) set_category(CAT_AUDIO) set_subcategory(SUBCAT_AUDIO_AFILTER) add_shortcut(
        "vlc_whisper", "whisper") add_loadfile("worker-path", NULL, "Path to vlc-whisper-worker executable (optional)",
                                               "Explicit location of vlc-whisper-worker[.exe] for installs where it is "
                                               "not co-located with the plugin; defaults to discovery",
                                               false)
        add_loadfile("model-path", NULL, "Path to bundled ggml-tiny.bin or another model (optional)",
                     "Explicit location of the whisper model; absent user selection discovers bundled tiny first",
                     false) add_string("whisper-backend", "auto", "Inference backend",
                                       "auto|gpu|cpu (auto probes Vulkan)",
                                       false) add_string("whisper-language", "en", "Caption language",
                                                         "Whisper language code (en|ro|tr|de|fr|es...)", false)
            add_integer("whisper-threads", 4, "CPU threads", "Threads for Whisper inference (1..16)", false)
                change_integer_range(1, 16) add_bool("whisper-logging", false, "Enable diagnostic logging",
                                                     "Enable VLC-Whisper and worker diagnostic logging", false)
                    add_string("whisper-backend-active", "", "Active backend (read-only)",
                               "Mirrors resolved backend from worker STATUS (gpu|cpu); informational",
                               false) add_string("whisper-model-download", "", "Model download control",
                                                 "Catalog id to download or abort; plugin relays as MODEL_CTRL", false)
                        add_integer("whisper-model-progress", 0, "Download progress percent (read-only mirror)",
                                    "Mirrors worker MODEL_PROGRESS pct (0..100) for Lua GUI",
                                    true) change_integer_range(0, 100)
                            add_string("whisper-model-status", "", "Model download status (read-only mirror)",
                                       "Mirrors worker MODEL_PROGRESS stage:model_id for GUI", true)
                                add_bool("whisper-translate-enabled", false, "Enable real-time subtitle translation",
                                         "Opt-in real-time translation of subtitles (keyless Google engine)", false)
                                    add_string("whisper-translate-from", "auto", "Translation source language",
                                               "Source language code or 'auto' for automatic detection", false)
                                        add_string("whisper-translate-to", "en", "Translation target language",
                                                   "Target language code for translated subtitles", false)
                                            add_integer("whisper-translate-mode", 1, "Translation display mode",
                                                        "0=translation only, 1=dual line (source + translation)", false)
                                                change_integer_range(0, 1)
                                                    add_bool("whisper-show-paused", true,
                                                             "Show subtitles while paused (local files only)",
                                                             "Hold the current cue and preview paused seeks", false)
                                                        set_callbacks(vw_plugin_open, vw_plugin_close) vlc_module_end()
#pragma GCC diagnostic pop

#if defined(__linux__) && defined(__GNUC__)
                                                            EXTERN_SYMBOL DLL_SYMBOL
    int vlc_entry__3_0_0ft64(vlc_set_cb vlc_set, void* opaque) __attribute__((alias("vlc_entry__3_0_0f")));
#endif
