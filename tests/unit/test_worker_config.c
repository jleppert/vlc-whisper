#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

// Unit tests for worker CLI config parsing (vw_worker_config_parse_args).
// Covers the --token/--pipe/--model success paths and the worker argv startup
// failure paths: malformed --token (bad length / non-hex), unknown option,
// dangling flag with no value, and NULL config (all map to exit code 2).
#include <errno.h>
#include <stdio.h>
#if defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#endif
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#define VW_TEST_MKDIR(path) _mkdir(path)
#define VW_TEST_RMDIR(path) _rmdir(path)
#define VW_TEST_PID() _getpid()
#define VW_TEST_GETCWD(path, size) _getcwd(path, size)
#define VW_TEST_CHDIR(path) _chdir(path)
#define VW_TEST_PATH_SEPARATOR "\\"
#else
#include <sys/stat.h>
#include <unistd.h>
#define VW_TEST_MKDIR(path) mkdir(path, 0700)
#define VW_TEST_RMDIR(path) rmdir(path)
#define VW_TEST_PID() getpid()
#define VW_TEST_GETCWD(path, size) getcwd(path, size)
#define VW_TEST_CHDIR(path) chdir(path)
#define VW_TEST_PATH_SEPARATOR "/"
#endif

#include "vw_test.h"
#include "vw_worker_config.h"

// 64 hex chars decoding to bytes 0x00, 0x01, ..., 0x1f
static char kTokenHex[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

static bool vw_test_join_path(char* out, size_t out_size, const char* directory, const char* name) {
  if (!out || !out_size || !directory || !name) return false;
  size_t directory_len = strlen(directory);
  size_t name_len = strlen(name);
  size_t separator_len = directory_len && directory[directory_len - 1] != VW_TEST_PATH_SEPARATOR[0] ? 1 : 0;
  if (directory_len + separator_len + name_len + 1 > out_size) return false;
  memcpy(out, directory, directory_len);
  if (separator_len) out[directory_len] = VW_TEST_PATH_SEPARATOR[0];
  memcpy(out + directory_len + separator_len, name, name_len + 1);
  return true;
}

static bool vw_test_file_exists(const char* path) {
  FILE* file = fopen(path, "rb");
  if (!file) return false;
  fclose(file);
  return true;
}

static bool vw_test_get_executable_dir(char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  char executable_path[VW_PATH_MAX_BYTES];
  size_t path_length;
#ifdef _WIN32
  DWORD windows_path_length = GetModuleFileNameA(NULL, executable_path, (DWORD)sizeof(executable_path));
  if (windows_path_length == 0 || windows_path_length >= sizeof(executable_path)) return false;
  path_length = (size_t)windows_path_length;
#elif defined(__linux__)
  ssize_t linux_path_length = readlink("/proc/self/exe", executable_path, sizeof(executable_path) - 1);
  if (linux_path_length <= 0 || (size_t)linux_path_length >= sizeof(executable_path)) return false;
  path_length = (size_t)linux_path_length;
#elif defined(__APPLE__)
  char raw_path[VW_PATH_MAX_BYTES];
  uint32_t raw_size = (uint32_t)sizeof(raw_path);
  if (_NSGetExecutablePath(raw_path, &raw_size) != 0) return false;
  char resolved[PATH_MAX];
  const char* source = realpath(raw_path, resolved) ? resolved : raw_path;
  path_length = strlen(source);
  if (path_length == 0 || path_length >= sizeof(executable_path)) return false;
  memcpy(executable_path, source, path_length);
#else
  return false;
#endif
  executable_path[path_length] = '\0';

  const char* slash = strrchr(executable_path, '/');
  const char* bslash = strrchr(executable_path, '\\');
  if (bslash && (!slash || bslash > slash)) slash = bslash;
  if (!slash) return false;
  size_t directory_length = (size_t)(slash - executable_path);
  if (directory_length == 0) directory_length = 1;
  if (directory_length >= out_size) return false;
  memcpy(out, executable_path, directory_length);
  out[directory_length] = '\0';
  return true;
}

int main(void) {
  uint8_t zeros[VW_AUTH_TOKEN_BYTES] = {0};

  // --- success: no args keeps defaults and leaves token zeroed ---
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_none[] = {"vlc-whisper-worker", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 1, argv_none) == 0);
    EXPECT_EQ_STR(cfg.model_path, "models/ggml-tiny.bin");
    EXPECT_EQ_STR(cfg.language, "en");
    EXPECT(cfg.sample_rate == 16000u);
    EXPECT(cfg.pipe_name[0] == '\0');
    EXPECT(memcmp(cfg.auth_token, zeros, VW_AUTH_TOKEN_BYTES) == 0);
    EXPECT(cfg.backend == VW_WORKER_BACKEND_AUTO);
    EXPECT(cfg.gpu_device == 0);
  }

  // --- success: valid --token, --pipe, --model ---
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_ok[] = {"vlc-whisper-worker",   "--pipe", "/tmp/vw.sock", "--token", kTokenHex, "--model",
                       "models/ggml-base.bin", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 7, argv_ok) == 0);
    EXPECT_EQ_STR(cfg.pipe_name, "/tmp/vw.sock");
    EXPECT_EQ_STR(cfg.model_path, "models/ggml-base.bin");
    for (size_t i = 0; i < VW_AUTH_TOKEN_BYTES; i++) {
      EXPECT(cfg.auth_token[i] == (uint8_t)i);
    }
  }

  // --- success: logging is disabled by default; explicit file/flag enables it ---
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    EXPECT(!cfg.logging_enabled);
    char* argv_log[] = {"vlc-whisper-worker", "--log-file", "/var/log/vw.log", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_log) == 0);
    EXPECT_EQ_STR(cfg.log_file, "/var/log/vw.log");
    EXPECT(cfg.logging_enabled);
  }
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_none[] = {"vlc-whisper-worker", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 1, argv_none) == 0);
    EXPECT(cfg.log_file[0] == '\0');
    EXPECT(!cfg.logging_enabled);
  }
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_enabled[] = {"vlc-whisper-worker", "--enable-logging", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 2, argv_enabled) == 0);
    EXPECT(cfg.logging_enabled);
  }

  // --- success: --backend and --gpu-device parsing (step 17a) ---
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_gpu[] = {"vlc-whisper-worker", "--backend", "gpu", "--gpu-device", "1", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 5, argv_gpu) == 0);
    EXPECT(cfg.backend == VW_WORKER_BACKEND_GPU);
    EXPECT(cfg.gpu_device == 1);

    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_cpu[] = {"vlc-whisper-worker", "--backend", "cpu", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_cpu) == 0);
    EXPECT(cfg.backend == VW_WORKER_BACKEND_CPU);
    EXPECT(cfg.gpu_device == 0);  // untouched

    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_auto[] = {"vlc-whisper-worker", "--backend", "auto", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_auto) == 0);
    EXPECT(cfg.backend == VW_WORKER_BACKEND_AUTO);
  }

  // --- failure: bad --backend value, negative --gpu-device, dangling flags ---
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_bad_backend[] = {"vlc-whisper-worker", "--backend", "cuda", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_bad_backend) == 2);

    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_neg_device[] = {"vlc-whisper-worker", "--gpu-device", "-1", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_neg_device) == 2);

    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_dangling[] = {"vlc-whisper-worker", "--backend", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 2, argv_dangling) == 2);

    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_bad_device[] = {"vlc-whisper-worker", "--gpu-device", "abc", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_bad_device) == 2);
  }

  // --- failure: --token too short ---
  {
    vw_worker_config_t cfg;
    vw_worker_config_init_defaults(&cfg);
    char* argv_short[] = {"vlc-whisper-worker", "--token", "aabbccdd", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_short) == 2);
  }

  // --- failure: --token with non-hex digit ---
  {
    vw_worker_config_t cfg;
    vw_worker_config_init_defaults(&cfg);
    char bad_hex[65];
    memset(bad_hex, '0', 64);
    bad_hex[10] = 'g';  // invalid hex digit
    bad_hex[64] = '\0';
    char* argv_badhex[] = {"vlc-whisper-worker", "--token", bad_hex, NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_badhex) == 2);
  }

  // --- failure: unknown option ---
  {
    vw_worker_config_t cfg;
    vw_worker_config_init_defaults(&cfg);
    char* argv_unknown[] = {"vlc-whisper-worker", "--bogus", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 2, argv_unknown) == 2);
  }

  // --- failure: dangling options with no value ---
  {
    vw_worker_config_t cfg;
    vw_worker_config_init_defaults(&cfg);
    char* argv_dangling_tok[] = {"vlc-whisper-worker", "--pipe", "/tmp/vw.sock", "--token", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 4, argv_dangling_tok) == 2);

    char* argv_dangling_pipe[] = {"vlc-whisper-worker", "--pipe", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 2, argv_dangling_pipe) == 2);

    char* argv_dangling_model[] = {"vlc-whisper-worker", "--model", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 2, argv_dangling_model) == 2);

    char* argv_dangling_log[] = {"vlc-whisper-worker", "--log-file", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 2, argv_dangling_log) == 2);

    char* argv_dangling_gpu[] = {"vlc-whisper-worker", "--gpu-device", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 2, argv_dangling_gpu) == 2);

    char* argv_dangling_vad[] = {"vlc-whisper-worker", "--vad-model", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 2, argv_dangling_vad) == 2);
  }

  // --- success: --vad-model override ---
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_vad[] = {"vlc-whisper-worker", "--vad-model", "models/custom-vad.bin", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_vad) == 0);
    EXPECT_EQ_STR(cfg.vad_model_path, "models/custom-vad.bin");
  }

  // --- success: --language and --n-threads parse (step 19b) ---
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    EXPECT_EQ_STR(cfg.language, "en");
    EXPECT(cfg.n_threads == 4);
    char* argv_lang[] = {"vlc-whisper-worker", "--language", "ro", "--n-threads", "8", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 5, argv_lang) == 0);
    EXPECT_EQ_STR(cfg.language, "ro");
    EXPECT(cfg.n_threads == 8);
  }
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_clamp_low[] = {"vlc-whisper-worker", "--n-threads", "0", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_clamp_low) == 0);
    EXPECT(cfg.n_threads == 1);
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_clamp_high[] = {"vlc-whisper-worker", "--n-threads", "32", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_clamp_high) == 0);
    EXPECT(cfg.n_threads == 16);
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_clamp_mid[] = {"vlc-whisper-worker", "--n-threads", "16", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_clamp_mid) == 0);
    EXPECT(cfg.n_threads == 16);
  }
  // --- failure: --language auto is rejected; concrete Whisper language required ---
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_auto[] = {"vlc-whisper-worker", "--language", "auto", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_auto) == 2);
    EXPECT_EQ_STR(cfg.language, "en");
  }
  // --- failure: --language empty, too long, dangling ---
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_long[] = {"vlc-whisper-worker", "--language", "this_is_way_too_long_for_lang", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_long) == 2);
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_empty[] = {"vlc-whisper-worker", "--language", "", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_empty) == 2);
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_dangling_lang[] = {"vlc-whisper-worker", "--language", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 2, argv_dangling_lang) == 2);
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_dangling_thr[] = {"vlc-whisper-worker", "--n-threads", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 2, argv_dangling_thr) == 2);
    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_bad_thr[] = {"vlc-whisper-worker", "--n-threads", "abc", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_bad_thr) == 2);
  }

  // --- success: --model-dir accepted (roundtrip) ---
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    EXPECT(cfg.model_dir[0] == '\0');
    char* argv_dir[] = {"vlc-whisper-worker", "--model-dir", "/tmp/vlc-whisper/models", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_dir) == 0);
    EXPECT_EQ_STR(cfg.model_dir, "/tmp/vlc-whisper/models");
  }

  // --- VAD discovery: absolute model-dir and a genuinely unrelated working directory ---
  {
    char original_cwd[VW_PATH_MAX_BYTES];
    char root_dir[VW_PATH_MAX_BYTES];
    char sibling_dir[VW_PATH_MAX_BYTES];
    char model_dir[VW_PATH_MAX_BYTES];
    char unrelated_dir[VW_PATH_MAX_BYTES];
    char cwd_models_dir[VW_PATH_MAX_BYTES];
    char sibling_model[VW_PATH_MAX_BYTES];
    char sibling_vad[VW_PATH_MAX_BYTES];
    char model_dir_model[VW_PATH_MAX_BYTES];
    char model_dir_vad[VW_PATH_MAX_BYTES];
    char cwd_vad[VW_PATH_MAX_BYTES];
    char executable_dir[VW_PATH_MAX_BYTES];
    char install_models_dir[VW_PATH_MAX_BYTES];
    char install_model[VW_PATH_MAX_BYTES];
    char install_vad[VW_PATH_MAX_BYTES];
    bool install_model_owned = false;
    bool install_vad_owned = false;
    char root_name[64];
    EXPECT(VW_TEST_GETCWD(original_cwd, sizeof(original_cwd)) != NULL);
    snprintf(root_name, sizeof(root_name), "vw_test_vad_%ld", (long)VW_TEST_PID());
    EXPECT(vw_test_join_path(root_dir, sizeof(root_dir), original_cwd, root_name));
    EXPECT(vw_test_join_path(sibling_dir, sizeof(sibling_dir), root_dir, "sibling"));
    EXPECT(vw_test_join_path(model_dir, sizeof(model_dir), root_dir, "models"));
    EXPECT(vw_test_join_path(unrelated_dir, sizeof(unrelated_dir), root_dir, "unrelated"));
    EXPECT(vw_test_join_path(cwd_models_dir, sizeof(cwd_models_dir), unrelated_dir, "models"));
    EXPECT(vw_test_join_path(sibling_model, sizeof(sibling_model), sibling_dir, "ggml-base.en.bin"));
    EXPECT(vw_test_join_path(sibling_vad, sizeof(sibling_vad), sibling_dir, "ggml-silero-vad.bin"));
    EXPECT(vw_test_join_path(model_dir_model, sizeof(model_dir_model), model_dir, "ggml-base.en.bin"));
    EXPECT(vw_test_join_path(model_dir_vad, sizeof(model_dir_vad), model_dir, "ggml-silero-vad.bin"));
    EXPECT(vw_test_join_path(cwd_vad, sizeof(cwd_vad), cwd_models_dir, "ggml-silero-vad.bin"));
    EXPECT(vw_test_get_executable_dir(executable_dir, sizeof(executable_dir)));
    EXPECT(vw_test_join_path(install_models_dir, sizeof(install_models_dir), executable_dir, "models"));
    EXPECT(vw_test_join_path(install_model, sizeof(install_model), install_models_dir, "ggml-vw-test.bin"));
    EXPECT(vw_test_join_path(install_vad, sizeof(install_vad), install_models_dir, "ggml-silero-vad.bin"));

    EXPECT(VW_TEST_MKDIR(root_dir) == 0);
    EXPECT(VW_TEST_MKDIR(sibling_dir) == 0);
    EXPECT(VW_TEST_MKDIR(model_dir) == 0);
    EXPECT(VW_TEST_MKDIR(unrelated_dir) == 0);
    EXPECT(VW_TEST_MKDIR(cwd_models_dir) == 0);
    FILE* sibling_model_file = fopen(sibling_model, "wb");
    EXPECT(sibling_model_file != NULL);
    if (sibling_model_file) fclose(sibling_model_file);
    FILE* sibling_vad_file = fopen(sibling_vad, "wb");
    EXPECT(sibling_vad_file != NULL);
    if (sibling_vad_file) fclose(sibling_vad_file);
    FILE* model_dir_model_file = fopen(model_dir_model, "wb");
    EXPECT(model_dir_model_file != NULL);
    if (model_dir_model_file) fclose(model_dir_model_file);
    FILE* model_dir_vad_file = fopen(model_dir_vad, "wb");
    EXPECT(model_dir_vad_file != NULL);
    if (model_dir_vad_file) fclose(model_dir_vad_file);
    FILE* cwd_vad_file = fopen(cwd_vad, "wb");
    EXPECT(cwd_vad_file != NULL);
    if (cwd_vad_file) fclose(cwd_vad_file);
    int install_mkdir_result = VW_TEST_MKDIR(install_models_dir);
    bool install_models_created = install_mkdir_result == 0;
    if (!install_models_created) EXPECT(errno == EEXIST);
    if (!vw_test_file_exists(install_model)) {
      FILE* install_model_file = fopen(install_model, "wb");
      EXPECT(install_model_file != NULL);
      if (install_model_file) {
        fclose(install_model_file);
        install_model_owned = true;
      }
    }
    if (!vw_test_file_exists(install_vad)) {
      FILE* install_vad_file = fopen(install_vad, "wb");
      EXPECT(install_vad_file != NULL);
      if (install_vad_file) {
        fclose(install_vad_file);
        install_vad_owned = true;
      }
    }

    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    snprintf(cfg.model_path, sizeof(cfg.model_path), "%s", "models/ggml-base.en.bin");
    snprintf(cfg.model_dir, sizeof(cfg.model_dir), "%s", model_dir);
    char resolved[VW_PATH_MAX_BYTES];
    EXPECT(vw_worker_config_resolve_model_path(&cfg, resolved, sizeof(resolved)));
    EXPECT_EQ_STR(resolved, model_dir_model);

    EXPECT(VW_TEST_CHDIR(unrelated_dir) == 0);
    char resolved_vad[VW_PATH_MAX_BYTES];
    // Relative --model resolves from an absolute --model-dir independently of launch CWD.
    EXPECT(vw_worker_config_resolve_vad_model_path(&cfg, resolved, resolved_vad, sizeof(resolved_vad)));
    EXPECT_EQ_STR(resolved_vad, model_dir_vad);

    // An explicit --vad-model wins over effective-model and model-dir candidates.
    snprintf(cfg.vad_model_path, sizeof(cfg.vad_model_path), "%s", "models/custom-vad.bin");
    EXPECT(vw_worker_config_resolve_vad_model_path(&cfg, resolved, resolved_vad, sizeof(resolved_vad)));
    EXPECT_EQ_STR(resolved_vad, "models/custom-vad.bin");
    cfg.vad_model_path[0] = '\0';

    // A VAD sibling of a different effective model wins over the configured model directory.
    EXPECT(vw_worker_config_resolve_vad_model_path(&cfg, sibling_model, resolved_vad, sizeof(resolved_vad)));
    EXPECT_EQ_STR(resolved_vad, sibling_vad);

    // With no effective-model sibling, use the VAD in --model-dir.
    EXPECT(remove(sibling_vad) == 0);
    EXPECT(vw_worker_config_resolve_vad_model_path(&cfg, sibling_model, resolved_vad, sizeof(resolved_vad)));
    EXPECT_EQ_STR(resolved_vad, model_dir_vad);

    // With no model directory, the worker executable's adjacent models directory wins over CWD candidates.
    cfg.model_dir[0] = '\0';
    snprintf(cfg.model_path, sizeof(cfg.model_path), "%s", "models/ggml-vw-test.bin");
    EXPECT(vw_worker_config_resolve_model_path(&cfg, resolved, sizeof(resolved)));
    EXPECT_EQ_STR(resolved, install_model);

    // Remove the test install candidate to verify the legacy CWD-relative compatibility fallback.
    if (install_vad_owned) {
      EXPECT(remove(install_vad) == 0);
      EXPECT(vw_worker_config_resolve_vad_model_path(&cfg, sibling_model, resolved_vad, sizeof(resolved_vad)));
      EXPECT_EQ_STR(resolved_vad, "models/ggml-silero-vad.bin");
    }

    EXPECT(VW_TEST_CHDIR(original_cwd) == 0);

    EXPECT(remove(cwd_vad) == 0);
    EXPECT(remove(model_dir_vad) == 0);
    EXPECT(remove(model_dir_model) == 0);
    EXPECT(remove(sibling_model) == 0);
    EXPECT(VW_TEST_RMDIR(cwd_models_dir) == 0);
    EXPECT(VW_TEST_RMDIR(unrelated_dir) == 0);
    if (install_model_owned) EXPECT(remove(install_model) == 0);
    EXPECT(VW_TEST_RMDIR(model_dir) == 0);
    EXPECT(VW_TEST_RMDIR(sibling_dir) == 0);
    EXPECT(VW_TEST_RMDIR(root_dir) == 0);
    if (install_vad_owned) EXPECT(!vw_test_file_exists(install_vad));
    if (install_models_created) EXPECT(VW_TEST_RMDIR(install_models_dir) == 0);
  }

  // --- failure: --model-dir oversize, missing arg ---
  {
    vw_worker_config_t cfg;
    EXPECT(vw_worker_config_init_defaults(&cfg));
    // Build an oversize path >= VW_PATH_MAX_BYTES
    char oversize[VW_PATH_MAX_BYTES + 16];
    memset(oversize, 'a', sizeof(oversize) - 1);
    oversize[sizeof(oversize) - 1] = '\0';
    oversize[0] = '/';
    char* argv_oversize[] = {"vlc-whisper-worker", "--model-dir", oversize, NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_oversize) == 2);

    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_dangling[] = {"vlc-whisper-worker", "--model-dir", NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 2, argv_dangling) == 2);

    EXPECT(vw_worker_config_init_defaults(&cfg));
    char* argv_oversize_model[] = {"vlc-whisper-worker", "--model", oversize, NULL};
    EXPECT(vw_worker_config_parse_args(&cfg, 3, argv_oversize_model) == 2);
  }

  // --- failure: NULL config ---
  EXPECT(vw_worker_config_parse_args(NULL, 1, NULL) == 2);

  return 0;
}
