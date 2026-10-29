#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "vw_model_catalog.h"
#include "vw_model_download.h"
#include "vw_sha256.h"
#include "vw_test.h"

static void test_sha256_vectors(void) {
  uint8_t hash[32];
  char hex[65];

  // Empty string.
  vw_sha256((const uint8_t*)"", 0, hash);
  vw_sha256_to_hex(hash, hex);
  EXPECT_EQ_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  // "abc"
  vw_sha256((const uint8_t*)"abc", 3, hash);
  vw_sha256_to_hex(hash, hex);
  EXPECT_EQ_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  // 1,000,000 x 'a' via streaming update loop (tests 64-byte block buffering).
  vw_sha256_context_t ctx;
  vw_sha256_init(&ctx);
  char chunk[1000];
  memset(chunk, 'a', sizeof(chunk));
  for (int i = 0; i < 1000; i++) {
    vw_sha256_update(&ctx, (const uint8_t*)chunk, sizeof(chunk));
  }
  vw_sha256_final(&ctx, hash);
  vw_sha256_to_hex(hash, hex);
  EXPECT_EQ_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  // Incremental "a"+"bc" should equal "abc".
  vw_sha256_init(&ctx);
  vw_sha256_update(&ctx, (const uint8_t*)"a", 1);
  vw_sha256_update(&ctx, (const uint8_t*)"bc", 2);
  vw_sha256_final(&ctx, hash);
  vw_sha256_to_hex(hash, hex);
  EXPECT_EQ_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

static void test_pct_math(void) {
  EXPECT(vw_model_download_pct(0, 100) == 0);
  EXPECT(vw_model_download_pct(50, 100) == 50);
  EXPECT(vw_model_download_pct(100, 100) == 100);
  EXPECT(vw_model_download_pct(150, 100) == 100);
  EXPECT(vw_model_download_pct(0, 0) == 0);
  EXPECT(vw_model_download_pct(77704715ULL, 77704715ULL) == 100);
  EXPECT(vw_model_download_pct(38852358ULL, 77704715ULL) == 50);
  // Saturating when .part exceeds total (should clamp).
  EXPECT(vw_model_download_pct(80000000ULL, 77704715ULL) == 100);
  EXPECT(vw_model_download_pct(UINT64_MAX, 100) == 100);
}

static void test_catalog(void) {
  EXPECT(vw_model_catalog_count() == 8);

  const vw_model_catalog_entry_t* e = vw_model_catalog_find("tiny.en");
  EXPECT(e != NULL);
  EXPECT_EQ_STR(e->id, "tiny.en");
  EXPECT_EQ_STR(e->filename, "ggml-tiny.en.bin");
  EXPECT_EQ_STR(e->sha256_hex, "921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f");
  EXPECT(e->bytes == 77704715ULL);
  EXPECT(e->multilingual == false);
  EXPECT(strstr(e->url, "ggml-tiny.en.bin") != NULL);

  e = vw_model_catalog_find("tiny");
  EXPECT(e != NULL);
  EXPECT(e->bytes == 77691713ULL);
  EXPECT(e->multilingual == true);

  e = vw_model_catalog_find("base.en");
  EXPECT(e != NULL);
  EXPECT(e->bytes == 147964211ULL);
  EXPECT(e->multilingual == false);

  e = vw_model_catalog_find("base");
  EXPECT(e != NULL);
  EXPECT(e->bytes == 147951465ULL);
  EXPECT(e->multilingual == true);

  e = vw_model_catalog_find("small");
  EXPECT(e != NULL);
  EXPECT(e->bytes == 487601967ULL);

  e = vw_model_catalog_find("medium");
  EXPECT(e != NULL);
  EXPECT(e->bytes == 1533763059ULL);

  e = vw_model_catalog_find("large");
  EXPECT(e != NULL);
  EXPECT(e->bytes == 3095033483ULL);
  EXPECT(e->multilingual == true);

  // large-v3-turbo: pruned-decoder large-v3; near large-v3 accuracy at roughly half the size and twice the speed.
  e = vw_model_catalog_find("large-v3-turbo");
  EXPECT(e != NULL);
  EXPECT_EQ_STR(e->filename, "ggml-large-v3-turbo.bin");
  EXPECT_EQ_STR(e->sha256_hex, "1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69");
  EXPECT(e->bytes == 1624555275ULL);
  EXPECT(e->multilingual == true);

  // Every id must be findable and have correct invariants.
  const char* ids[] = {"tiny.en", "tiny", "base.en", "base", "small", "medium", "large", "large-v3-turbo"};
  for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
    const vw_model_catalog_entry_t* ent = vw_model_catalog_find(ids[i]);
    EXPECT(ent != NULL);
    EXPECT(strlen(ent->sha256_hex) == 64);
    EXPECT(ent->bytes > 0);
    EXPECT(strstr(ent->url, "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/") != NULL);
    EXPECT(strstr(ent->url, ent->filename) != NULL);
  }

  // Miss cases.
  EXPECT(vw_model_catalog_find("unknown") == NULL);
  EXPECT(vw_model_catalog_find("") == NULL);
  EXPECT(vw_model_catalog_find(NULL) == NULL);
  EXPECT(vw_model_catalog_find("TINY") == NULL);
}

static void test_default_dir(void) {
  char out[4096];
  EXPECT(vw_model_download_default_dir(out, sizeof(out)) == true);
  EXPECT(strstr(out, "vlc-whisper") != NULL);
  EXPECT(strstr(out, "models") != NULL);
  struct stat st;
  EXPECT(stat(out, &st) == 0);
  EXPECT(S_ISDIR(st.st_mode));

  // Null and zero-size should fail gracefully.
  EXPECT(vw_model_download_default_dir(NULL, 0) == false);
  EXPECT(vw_model_download_default_dir(out, 0) == false);

  // Tiny buffer should still null-terminate without overflow (still creates dir).
  char tiny[8];
  // This may truncate but should not crash; result depends on implementation.
  // Just ensure it doesn't overflow: if buffer too small, function may still succeed
  // or fail; we only check it doesn't crash.
  (void)vw_model_download_default_dir(tiny, sizeof(tiny));

#ifndef _WIN32
  char oversized_xdg[4096];
  memset(oversized_xdg, 'x', sizeof(oversized_xdg) - 1U);
  oversized_xdg[sizeof(oversized_xdg) - 1U] = '\0';
  const char* old_xdg = getenv("XDG_DATA_HOME");
  char* old_xdg_copy = old_xdg ? strdup(old_xdg) : NULL;
  setenv("XDG_DATA_HOME", oversized_xdg, 1);
  EXPECT(vw_model_download_default_dir(out, sizeof(out)) == false);
  if (old_xdg_copy) {
    setenv("XDG_DATA_HOME", old_xdg_copy, 1);
    free(old_xdg_copy);
  } else {
    unsetenv("XDG_DATA_HOME");
  }
#endif
}

static void test_local_file_download(void) {
#ifdef _WIN32
  (void)0;
  return;
#else
  char source_tmpl[] = "/tmp/vw_test_source_XXXXXX";
  int source_fd = mkstemp(source_tmpl);
  EXPECT(source_fd >= 0);
  const char payload[] = "local model download test\n";
  EXPECT(write(source_fd, payload, sizeof(payload) - 1) == (ssize_t)(sizeof(payload) - 1));
  EXPECT(close(source_fd) == 0);

  char dest_tmpl[] = "/tmp/vw_test_local_dl_XXXXXX";
  char* dest_dir = mkdtemp(dest_tmpl);
  EXPECT(dest_dir != NULL);

  char url[4096];
  snprintf(url, sizeof(url), "file://%s", source_tmpl);
  uint8_t hash[32];
  char hash_hex[65];
  vw_sha256((const uint8_t*)payload, sizeof(payload) - 1, hash);
  vw_sha256_to_hex(hash, hash_hex);
  vw_model_catalog_entry_t entry = {"local", "local.bin", url, hash_hex, sizeof(payload) - 1, false};

  vw_model_download_t* dl = vw_model_download_start(&entry, dest_dir);
  EXPECT(dl != NULL);
  vw_download_progress_t progress = {0};
  for (int i = 0; i < 200; i++) {
    EXPECT(vw_model_download_poll(dl, &progress));
    if (progress.stage == VW_MODEL_STAGE_DONE || progress.stage == VW_MODEL_STAGE_FAILED) break;
    struct timespec ts = {0, 10000000L};
    nanosleep(&ts, NULL);
  }
  EXPECT(progress.stage == VW_MODEL_STAGE_DONE);
  EXPECT(progress.pct == 100);
  EXPECT_EQ_STR(progress.model_id, "local");

  char final_path[4096];
  snprintf(final_path, sizeof(final_path), "%s/local.bin", dest_dir);
  struct stat st;
  EXPECT(stat(final_path, &st) == 0);
  EXPECT((uint64_t)st.st_size == sizeof(payload) - 1);

  vw_model_download_free(dl);
  char lock_path[4096];
  snprintf(lock_path, sizeof(lock_path), "%s/local.bin.lock", dest_dir);
  EXPECT(unlink(final_path) == 0);
  EXPECT(access(lock_path, F_OK) == 0);
  EXPECT(unlink(lock_path) == 0);
  EXPECT(unlink(source_tmpl) == 0);
  EXPECT(rmdir(dest_dir) == 0);
#endif
}

static void test_poll_and_lifecycle(void) {
#ifdef _WIN32
  (void)0;
  return;  // mkdtemp is POSIX-only; Linux coverage suffices
#else
  vw_download_progress_t prog;
  EXPECT(vw_model_download_poll(NULL, &prog) == false);
  vw_model_download_abort(NULL);
  vw_model_download_free(NULL);

  // Start with NULL entry or dir should return NULL without network.
  EXPECT(vw_model_download_start(NULL, "/tmp") == NULL);
  vw_model_catalog_entry_t invalid_entry = {"invalid", "invalid.bin", "file:///invalid", "", 1, false};
  EXPECT(vw_model_download_start(&invalid_entry, NULL) == NULL);
  EXPECT(vw_model_download_start(&invalid_entry, "") == NULL);

  // Create a FIFO so curl blocks locally; abort/free must stop it without network access.
  char tmpl[] = "/tmp/vw_test_dl_XXXXXX";
  char* tmpdir = mkdtemp(tmpl);
  EXPECT(tmpdir != NULL);
  char fifo_path[4096];
  snprintf(fifo_path, sizeof(fifo_path), "%s/input", tmpdir);
  EXPECT(mkfifo(fifo_path, 0600) == 0);
  char fifo_url[8192];
  snprintf(fifo_url, sizeof(fifo_url), "file://%s", fifo_path);
  vw_model_catalog_entry_t fifo_entry = {"fifo", "fifo.bin", fifo_url, "", 1, false};
  vw_model_download_t* dl = vw_model_download_start(&fifo_entry, tmpdir);
  if (dl) {
    // A second worker cannot claim the same destination while the first owns its interprocess lock.
    EXPECT(vw_model_download_start(&fifo_entry, tmpdir) == NULL);
    EXPECT(vw_model_download_poll(dl, NULL) == false);
    EXPECT(vw_model_download_poll(dl, &prog) == true);
    EXPECT_EQ_STR(prog.model_id, "fifo");
    vw_model_download_abort(dl);
    // free() joins the downloader thread and reaps curl after abort.
    EXPECT(vw_model_download_poll(dl, &prog) == true);
    vw_model_download_free(dl);
  }
  char part_path[4096];
  snprintf(part_path, sizeof(part_path), "%s/fifo.bin.part", tmpdir);
  char lock_path[4096];
  snprintf(lock_path, sizeof(lock_path), "%s/fifo.bin.lock", tmpdir);
  struct stat st;
  EXPECT(stat(part_path, &st) != 0);
  EXPECT(unlink(fifo_path) == 0);
  EXPECT(access(lock_path, F_OK) == 0);
  EXPECT(unlink(lock_path) == 0);
  EXPECT(rmdir(tmpdir) == 0);
#endif
}

int main(void) {
  test_sha256_vectors();
  test_pct_math();
  test_catalog();
  test_default_dir();
  test_local_file_download();
  test_poll_and_lifecycle();
  printf("test_model_download: all checks passed\n");
  return 0;
}
