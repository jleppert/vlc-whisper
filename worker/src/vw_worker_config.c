#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "vw_worker_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "whisper.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define VW_WORKER_CONFIG_GETCWD _getcwd
#else
#if defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#endif
#include <unistd.h>
#define VW_WORKER_CONFIG_GETCWD getcwd
#endif

#include "vw_log.h"
static bool vw_worker_config_file_exists(const char* path) {
  if (!path || !path[0]) return false;
#ifdef _WIN32
  wchar_t wide_path[VW_PATH_MAX_BYTES];
  int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide_path,
                                        (int)(sizeof(wide_path) / sizeof(wide_path[0])));
  if (wide_length <= 0) return false;
  DWORD attributes = GetFileAttributesW(wide_path);
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
  FILE* file = fopen(path, "rb");
  if (!file) return false;
  fclose(file);
  return true;
#endif
}

static bool vw_worker_config_is_absolute_path(const char* path) {
  if (!path || !path[0]) return false;
#ifdef _WIN32
  return (path[0] == '\\' && path[1] == '\\') ||
         (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
          (path[2] == '\\' || path[2] == '/'));
#else
  return path[0] == '/';
#endif
}

static const char* vw_worker_config_basename(const char* path) {
  if (!path) return NULL;
  const char* base = path;
  for (const char* cursor = path; *cursor; cursor++) {
    if (*cursor == '/' || *cursor == '\\') base = cursor + 1;
  }
  return base;
}

static void vw_worker_config_log_probe(const char* source, const char* candidate, bool exists) {
  vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_VAD_RESOLVE", "probe source=%s candidate='%s' result=%s", source,
               candidate ? candidate : "", exists ? "hit" : "miss");
}

static void vw_worker_config_log_cwd(void) {
  char cwd[VW_PATH_MAX_BYTES];
  if (VW_WORKER_CONFIG_GETCWD(cwd, sizeof(cwd)) != NULL) {
    vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_VAD_RESOLVE", "worker cwd='%s'", cwd);
  } else {
    vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_VAD_RESOLVE", "worker cwd unavailable");
  }
}

// Finds the worker executable directory without depending on the launcher's current working directory.
static bool vw_worker_config_get_executable_dir(char* out, size_t out_size) {
  if (!out || out_size == 0) return false;

  char executable_path[VW_PATH_MAX_BYTES];
  size_t path_length;
#ifdef _WIN32
  wchar_t wide_executable_path[VW_PATH_MAX_BYTES];
  DWORD windows_path_length = GetModuleFileNameW(
      NULL, wide_executable_path, (DWORD)(sizeof(wide_executable_path) / sizeof(wide_executable_path[0])));
  if (windows_path_length == 0 || windows_path_length >= sizeof(wide_executable_path) / sizeof(wide_executable_path[0]))
    return false;
  int utf8_length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_executable_path, (int)windows_path_length,
                                        executable_path, (int)sizeof(executable_path) - 1, NULL, NULL);
  if (utf8_length <= 0) return false;
  path_length = (size_t)utf8_length;
#elif defined(__linux__)
  ssize_t linux_path_length = readlink("/proc/self/exe", executable_path, sizeof(executable_path) - 1);
  if (linux_path_length <= 0 || (size_t)linux_path_length >= sizeof(executable_path)) return false;
  path_length = (size_t)linux_path_length;
#elif defined(__APPLE__)
  char raw_path[VW_PATH_MAX_BYTES];
  uint32_t raw_size = (uint32_t)sizeof(raw_path);
  if (_NSGetExecutablePath(raw_path, &raw_size) != 0) return false;
  // _NSGetExecutablePath may return a relative or symlinked path; canonicalize so sibling probing is stable.
  char resolved[PATH_MAX];
  const char* source = realpath(raw_path, resolved) ? resolved : raw_path;
  path_length = strlen(source);
  if (path_length == 0 || path_length >= sizeof(executable_path)) return false;
  memcpy(executable_path, source, path_length);
#else
  return false;
#endif
  executable_path[path_length] = '\0';

  const char* last_slash = strrchr(executable_path, '/');
  const char* last_bslash = strrchr(executable_path, '\\');
  const char* slash = NULL;
  if (last_slash && last_bslash) {
    slash = (last_slash > last_bslash) ? last_slash : last_bslash;
  } else if (last_slash) {
    slash = last_slash;
  } else {
    slash = last_bslash;
  }
  if (!slash) return false;

  size_t dir_length = (size_t)(slash - executable_path);
  if (dir_length == 0) dir_length = 1;  // Preserve the filesystem root (e.g. /worker).
  if (dir_length >= out_size) return false;
  memcpy(out, executable_path, dir_length);
  out[dir_length] = '\0';
  return true;
}

static bool vw_worker_config_join_path(char* out, size_t out_size, const char* directory, const char* name) {
  if (!out || out_size == 0 || !directory || !directory[0] || !name || !name[0]) return false;
  size_t directory_length = strlen(directory);
  bool needs_separator = directory[directory_length - 1] != '/' && directory[directory_length - 1] != '\\';
  size_t required = directory_length + (needs_separator ? 1 : 0) + strlen(name) + 1;
  if (required > out_size) return false;
#ifdef _WIN32
  snprintf(out, out_size, "%s%s%s", directory, needs_separator ? "\\" : "", name);
#else
  snprintf(out, out_size, "%s%s%s", directory, needs_separator ? "/" : "", name);
#endif
  return true;
}

// Parse a 64-char hex string into a 32-byte token. Returns true on success.
static bool vw_token_from_hex(const char* hex, uint8_t out[VW_AUTH_TOKEN_BYTES]) {
  if (strlen(hex) != VW_AUTH_TOKEN_BYTES * 2) return false;  // must be exactly 64 hex chars
  for (size_t i = 0; i < VW_AUTH_TOKEN_BYTES; i++) {
    unsigned hi, lo;
    char c1 = hex[i * 2], c2 = hex[i * 2 + 1];
    if (c1 >= '0' && c1 <= '9')
      hi = (unsigned)(c1 - '0');
    else if (c1 >= 'a' && c1 <= 'f')
      hi = (unsigned)(c1 - 'a' + 10);
    else if (c1 >= 'A' && c1 <= 'F')
      hi = (unsigned)(c1 - 'A' + 10);
    else
      return false;
    if (c2 >= '0' && c2 <= '9')
      lo = (unsigned)(c2 - '0');
    else if (c2 >= 'a' && c2 <= 'f')
      lo = (unsigned)(c2 - 'a' + 10);
    else if (c2 >= 'A' && c2 <= 'F')
      lo = (unsigned)(c2 - 'A' + 10);
    else
      return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static bool vw_worker_config_vad_beside(const char* path, char* out, size_t out_size) {
  static const char k_vad_filename[] = "ggml-silero-vad.bin";
  if (!path || !out || out_size == 0) {
    vw_worker_config_log_probe("effective-model-sibling", NULL, false);
    return false;
  }

  const char* last_slash = strrchr(path, '/');
  const char* last_bslash = strrchr(path, '\\');
  const char* slash = NULL;
  if (last_slash && last_bslash) {
    slash = (last_slash > last_bslash) ? last_slash : last_bslash;
  } else if (last_slash) {
    slash = last_slash;
  } else {
    slash = last_bslash;
  }
  if (!slash) {
    vw_worker_config_log_probe("effective-model-sibling", NULL, false);
    return false;
  }

  size_t dir_len = (size_t)(slash - path) + 1;
  if (dir_len + sizeof(k_vad_filename) > out_size) {
    vw_worker_config_log_probe("effective-model-sibling", NULL, false);
    return false;
  }
  memcpy(out, path, dir_len);
  memcpy(out + dir_len, k_vad_filename, sizeof(k_vad_filename));
  bool exists = vw_worker_config_file_exists(out);
  vw_worker_config_log_probe("effective-model-sibling", out, exists);
  return exists;
}

static bool vw_worker_config_vad_in_dir(const char* source, const char* dir, char* out, size_t out_size) {
  if (!dir || !dir[0]) {
    vw_worker_config_log_probe(source, NULL, false);
    return false;
  }
  size_t dir_len = strlen(dir);
  bool needs_separator = dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\';
  size_t required = dir_len + (needs_separator ? 1 : 0) + strlen("ggml-silero-vad.bin") + 1;
  if (required > out_size) {
    vw_worker_config_log_probe(source, NULL, false);
    return false;
  }
#ifdef _WIN32
  snprintf(out, out_size, "%s%s%s", dir, needs_separator ? "\\" : "", "ggml-silero-vad.bin");
#else
  snprintf(out, out_size, "%s%s%s", dir, needs_separator ? "/" : "", "ggml-silero-vad.bin");
#endif
  bool exists = vw_worker_config_file_exists(out);
  vw_worker_config_log_probe(source, out, exists);
  return exists;
}

static bool vw_worker_config_vad_in_install_dir(char* out, size_t out_size) {
  char executable_dir[VW_PATH_MAX_BYTES];
  if (!vw_worker_config_get_executable_dir(executable_dir, sizeof(executable_dir))) {
    vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_VAD_RESOLVE", "worker executable path unavailable");
    return false;
  }
  vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_VAD_RESOLVE", "worker executable directory='%s'", executable_dir);

  char install_model_dir[VW_PATH_MAX_BYTES];
  if (!vw_worker_config_join_path(install_model_dir, sizeof(install_model_dir), executable_dir, "models")) {
    vw_worker_config_log_probe("worker-install-model-dir", NULL, false);
    return false;
  }
  return vw_worker_config_vad_in_dir("worker-install-model-dir", install_model_dir, out, out_size);
}

bool vw_worker_config_resolve_vad_model_path(const vw_worker_config_t* config, const char* effective_model_path,
                                             char* out, size_t out_size) {
  if (!config || !out || out_size == 0) return false;

  vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_VAD_RESOLVE", "begin effective_model='%s' model_dir='%s' explicit='%s'",
               effective_model_path ? effective_model_path : "", config->model_dir, config->vad_model_path);
  vw_worker_config_log_cwd();

  // 1. Explicit --vad-model always wins, even if the worker later reports it invalid.
  if (config->vad_model_path[0] != '\0') {
    if (strlen(config->vad_model_path) >= out_size) return false;
    snprintf(out, out_size, "%s", config->vad_model_path);
    vw_worker_config_log_probe("explicit", out, vw_worker_config_file_exists(out));
    return true;
  }

  // 2. Prefer the VAD sibling of the fully resolved Whisper model.
  if (vw_worker_config_vad_beside(effective_model_path, out, out_size)) return true;

  // 3. Probe --model-dir directly when the effective model has no VAD sibling.
  if (vw_worker_config_vad_in_dir("model-dir", config->model_dir, out, out_size)) return true;

  // 4. Probe the installed VLC models directory independently of the launcher's working directory.
  if (vw_worker_config_vad_in_install_dir(out, out_size)) return true;

  // 5. Retain standard candidate paths relative to CWD for compatibility.
  static const char* const k_vad_candidates[] = {
      "models/ggml-silero-vad.bin",       "ggml-silero-vad.bin",           "../../../models/ggml-silero-vad.bin",
      "../../models/ggml-silero-vad.bin", "../models/ggml-silero-vad.bin", NULL};
  for (int i = 0; k_vad_candidates[i] != NULL; i++) {
    bool exists = vw_worker_config_file_exists(k_vad_candidates[i]);
    vw_worker_config_log_probe("working-directory", k_vad_candidates[i], exists);
    if (exists) {
      if (strlen(k_vad_candidates[i]) >= out_size) return false;
      snprintf(out, out_size, "%s", k_vad_candidates[i]);
      return true;
    }
  }

  vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_VAD_RESOLVE", "no VAD candidate found; RMS fallback will be used");
  return false;
}

bool vw_worker_config_init_defaults(vw_worker_config_t* config) {
  if (!config) {
    return false;
  }
  memset(config, 0, sizeof(vw_worker_config_t));
  strncpy(config->model_path, "models/ggml-tiny.bin", sizeof(config->model_path) - 1);
  strncpy(config->language, "en", sizeof(config->language) - 1);
  config->n_threads = 4;
  config->sample_rate = 16000;
  config->backend = VW_WORKER_BACKEND_AUTO;
  config->gpu_device = 0;
  config->vad_model_path[0] = '\0';
  return true;
}

int vw_worker_config_parse_args(vw_worker_config_t* config, int argc, char** argv) {
  if (!config) {
    return 2;
  }
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--token") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for --token\n");
        return 2;
      }
      if (!vw_token_from_hex(argv[++i], config->auth_token)) {
        fprintf(stderr, "bad --token: expected 64 hex chars\n");
        return 2;
      }
    } else if (strcmp(argv[i], "--pipe") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for --pipe\n");
        return 2;
      }
      const char* v = argv[++i];
      if (strlen(v) >= sizeof(config->pipe_name)) {
        fprintf(stderr, "bad --pipe: too long (max %zu)\n", sizeof(config->pipe_name) - 1);
        return 2;
      }
      snprintf(config->pipe_name, sizeof(config->pipe_name), "%s", v);
    } else if (strcmp(argv[i], "--model") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for --model\n");
        return 2;
      }
      const char* v = argv[++i];
      if (strlen(v) >= sizeof(config->model_path)) {
        fprintf(stderr, "bad --model: too long (max %zu)\n", sizeof(config->model_path) - 1);
        return 2;
      }
      snprintf(config->model_path, sizeof(config->model_path), "%s", v);
    } else if (strcmp(argv[i], "--model-dir") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for --model-dir\n");
        return 2;
      }
      const char* v = argv[++i];
      if (strlen(v) >= sizeof(config->model_dir)) {
        fprintf(stderr, "bad --model-dir: too long (max %zu)\n", sizeof(config->model_dir) - 1);
        return 2;
      }
      snprintf(config->model_dir, sizeof(config->model_dir), "%s", v);
    } else if (strcmp(argv[i], "--vad-model") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for --vad-model\n");
        return 2;
      }
      const char* v = argv[++i];
      if (strlen(v) >= sizeof(config->vad_model_path)) {
        fprintf(stderr, "bad --vad-model: too long (max %zu)\n", sizeof(config->vad_model_path) - 1);
        return 2;
      }
      snprintf(config->vad_model_path, sizeof(config->vad_model_path), "%s", v);
    } else if (strcmp(argv[i], "--log-file") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for --log-file\n");
        return 2;
      }
      const char* v = argv[++i];
      if (strlen(v) >= sizeof(config->log_file)) {
        fprintf(stderr, "bad --log-file: too long (max %zu)\n", sizeof(config->log_file) - 1);
        return 2;
      }
      snprintf(config->log_file, sizeof(config->log_file), "%s", v);
      config->logging_enabled = true;
    } else if (strcmp(argv[i], "--enable-logging") == 0) {
      config->logging_enabled = true;
    } else if (strcmp(argv[i], "--backend") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for --backend\n");
        return 2;
      }
      const char* b = argv[++i];
      if (strcmp(b, "auto") == 0) {
        config->backend = VW_WORKER_BACKEND_AUTO;
      } else if (strcmp(b, "gpu") == 0) {
        config->backend = VW_WORKER_BACKEND_GPU;
      } else if (strcmp(b, "cpu") == 0) {
        config->backend = VW_WORKER_BACKEND_CPU;
      } else {
        fprintf(stderr, "bad --backend: expected auto|gpu|cpu, got '%s'\n", b);
        return 2;
      }
    } else if (strcmp(argv[i], "--gpu-device") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for --gpu-device\n");
        return 2;
      }
      char* end = NULL;
      long id = strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || id < 0 || id > 65535) {
        fprintf(stderr, "bad --gpu-device: expected a non-negative integer\n");
        return 2;
      }
      config->gpu_device = (int)id;
    } else if (strcmp(argv[i], "--language") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for --language\n");
        return 2;
      }
      const char* lang = argv[++i];
      if (lang[0] == '\0' || strlen(lang) >= sizeof(config->language)) {
        fprintf(stderr, "bad --language: expected 1..%zu char code, got '%s'\n", sizeof(config->language) - 1, lang);
        return 2;
      }
      if (strcmp(lang, "auto") == 0) {
        fprintf(stderr, "bad --language: 'auto' is not a concrete transcription language\n");
        return 2;
      }
      if (whisper_lang_id(lang) < 0) {
        fprintf(stderr, "bad --language: unknown Whisper language '%s' (use whisper_lang_id list)\n", lang);
        return 2;
      }
      snprintf(config->language, sizeof(config->language), "%s", lang);
    } else if (strcmp(argv[i], "--n-threads") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for --n-threads\n");
        return 2;
      }
      char* end = NULL;
      long n = strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0') {
        fprintf(stderr, "bad --n-threads: expected integer 1..16\n");
        return 2;
      }
      if (n < 1) n = 1;
      if (n > 16) n = 16;
      config->n_threads = (int)n;
    } else {
      fprintf(stderr, "unknown option: %s\n", argv[i]);
      return 2;
    }
  }

  return 0;
}

bool vw_worker_config_default_model_dir(char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
#ifdef _WIN32
  wchar_t wbase[4096] = {0};
  DWORD blen = GetEnvironmentVariableW(L"LOCALAPPDATA", wbase, 4096);
  char tmp[4096] = {0};
  int tmp_written = -1;
  if (blen > 0 && blen < 4096) {
    char utf8_base[4096] = {0};
    int ulen = WideCharToMultiByte(CP_UTF8, 0, wbase, -1, utf8_base, sizeof(utf8_base), NULL, NULL);
    if (ulen > 0)
      tmp_written = snprintf(tmp, sizeof(tmp), "%s\\vlc-whisper\\models", utf8_base);
    else
      tmp_written = snprintf(tmp, sizeof(tmp), ".\\vlc-whisper\\models");
  } else {
    wchar_t whome[4096] = {0};
    DWORD hlen = GetEnvironmentVariableW(L"USERPROFILE", whome, 4096);
    if (hlen > 0 && hlen < 4096) {
      char utf8_home[4096] = {0};
      int ulen = WideCharToMultiByte(CP_UTF8, 0, whome, -1, utf8_home, sizeof(utf8_home), NULL, NULL);
      if (ulen > 0)
        tmp_written = snprintf(tmp, sizeof(tmp), "%s\\AppData\\Local\\vlc-whisper\\models", utf8_home);
      else
        tmp_written = snprintf(tmp, sizeof(tmp), ".\\vlc-whisper\\models");
    } else {
      tmp_written = snprintf(tmp, sizeof(tmp), ".\\vlc-whisper\\models");
    }
  }
  if (tmp_written < 0 || (size_t)tmp_written >= sizeof(tmp)) return false;
  if ((size_t)tmp_written >= out_size) return false;
  snprintf(out, out_size, "%s", tmp);
  return true;
#else
  const char* xdg = getenv("XDG_DATA_HOME");
  char tmp[4096];
  int tmp_written;
  if (xdg && xdg[0]) {
    tmp_written = snprintf(tmp, sizeof(tmp), "%s/vlc-whisper/models", xdg);
  } else {
    const char* home = getenv("HOME");
    if (home && home[0])
      tmp_written = snprintf(tmp, sizeof(tmp), "%s/.local/share/vlc-whisper/models", home);
    else
      tmp_written = snprintf(tmp, sizeof(tmp), "/tmp/vlc-whisper/models");
  }
  if (tmp_written < 0 || (size_t)tmp_written >= sizeof(tmp)) return false;
  if ((size_t)tmp_written >= out_size) return false;
  snprintf(out, out_size, "%s", tmp);
  return true;
#endif
}

bool vw_worker_config_resolve_model_path(const vw_worker_config_t* config, char* out, size_t out_size) {
  if (!config || !out || out_size == 0 || !config->model_path[0]) return false;
  if (vw_worker_config_file_exists(config->model_path)) {
    if (strlen(config->model_path) >= out_size) return false;
    snprintf(out, out_size, "%s", config->model_path);
    return true;
  }
  if (vw_worker_config_is_absolute_path(config->model_path)) return false;

  const char* filename = vw_worker_config_basename(config->model_path);
  if (!filename || !filename[0]) return false;

  // Prefer the per-user directory because downloaded models are intentionally user-owned.
  if (config->model_dir[0]) {
    char candidate[VW_PATH_MAX_BYTES];
    if (vw_worker_config_join_path(candidate, sizeof(candidate), config->model_dir, filename) &&
        vw_worker_config_file_exists(candidate)) {
      if (strlen(candidate) >= out_size) return false;
      snprintf(out, out_size, "%s", candidate);
      return true;
    }
  }

  // Bundled models live beside the worker installation, not necessarily beneath the launcher's CWD.
  char executable_dir[VW_PATH_MAX_BYTES];
  char install_model_dir[VW_PATH_MAX_BYTES];
  if (vw_worker_config_get_executable_dir(executable_dir, sizeof(executable_dir)) &&
      vw_worker_config_join_path(install_model_dir, sizeof(install_model_dir), executable_dir, "models") &&
      vw_worker_config_join_path(out, out_size, install_model_dir, filename) && vw_worker_config_file_exists(out)) {
    return true;
  }

  // Fallback probe for the default per-user model directory before giving up.
  char default_dir[VW_PATH_MAX_BYTES];
  if (vw_worker_config_default_model_dir(default_dir, sizeof(default_dir))) {
    char candidate[VW_PATH_MAX_BYTES];
    if (vw_worker_config_join_path(candidate, sizeof(candidate), default_dir, filename) &&
        vw_worker_config_file_exists(candidate)) {
      if (strlen(candidate) >= out_size) return false;
      snprintf(out, out_size, "%s", candidate);
      return true;
    }
  }

  return false;
}
