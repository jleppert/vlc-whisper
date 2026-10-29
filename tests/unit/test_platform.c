// Platform-agnostic unit tests for the vw_platform.h abstraction layer.
// The matching platform implementation (vw_platform_linux.c on POSIX,
// vw_platform_win32.c on Windows) is compiled into this test by CMake.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <errno.h>
#include <signal.h>
#endif

#include "vw_platform.h"
#include "vw_test.h"

#if defined(_WIN32) || defined(__MINGW32__)
// Bare cmd.exe with no args is an interactive shell that never exits; /c exit terminates it so
// vw_platform_wait_process(proc, 2000) can observe a clean completion (mirrors /bin/true).
static const char* kSpawnOk = "cmd.exe";
static const char* kSpawnOkArg[] = {"cmd.exe", "/c", "exit", NULL};
static const char* kSpawnMissing = "Z:\\definitely_missing_vw.exe";
#else
#if defined(__APPLE__)
static const char* kSpawnOk = "/usr/bin/true";  // macOS has no /bin/true
static const char* kSpawnOkArg[] = {"/usr/bin/true", NULL};
#else
static const char* kSpawnOk = "/bin/true";
static const char* kSpawnOkArg[] = {"/bin/true", NULL};
#endif
static const char* kSpawnMissing = "/nonexistent/vw_missing_binary";
#endif

// Bare executable names are rejected so worker launch cannot search an untrusted PATH.
#ifndef _WIN32
static const char* kSpawnBarePath = "true";
#endif

int main(void) {
  // --- vw_platform_get_random_bytes ---
  uint8_t buf[32];

  EXPECT(!vw_platform_get_random_bytes(NULL, sizeof(buf)));  // NULL buffer rejected
  EXPECT(!vw_platform_get_random_bytes(buf, 0));             // zero size rejected
  EXPECT(!vw_platform_get_random_bytes(NULL, 0));            // NULL buffer and zero size rejected

  EXPECT(vw_platform_get_random_bytes(buf, sizeof(buf)));  // fills buffer, returns true

  uint8_t buf2[32];
  EXPECT(vw_platform_get_random_bytes(buf2, sizeof(buf2)));
  EXPECT(memcmp(buf, buf2, sizeof(buf)) != 0);  // consecutive draws must differ

  // --- vw_platform_get_time_us ---
  int64_t t1 = vw_platform_get_time_us();
  EXPECT(t1 > 0);  // post-epoch

  int64_t t2 = vw_platform_get_time_us();
  EXPECT(t2 >= t1);  // non-decreasing

  // Sanity: within 5 seconds of the wall clock
  int64_t now_us = (int64_t)time(NULL) * 1000000;
  EXPECT(llabs(t1 - now_us) < 5000000);

  // --- vw_platform_get_monotonic_time_us ---
  int64_t mt1 = vw_platform_get_monotonic_time_us();
  EXPECT(mt1 > 0);
  int64_t mt2 = vw_platform_get_monotonic_time_us();
  EXPECT(mt2 >= mt1);

  // --- vw_platform_spawn_process ---
  EXPECT(!vw_platform_spawn_process(NULL, NULL, NULL));

  const char* argv_ok[] = {kSpawnOk, NULL};
  EXPECT(vw_platform_spawn_process(kSpawnOk, kSpawnOkArg, NULL));

  vw_process_t proc = 0;
  EXPECT(vw_platform_spawn_process(kSpawnOk, kSpawnOkArg, &proc));
  EXPECT(vw_platform_wait_process(proc, 2000));

  // Failure paths: partial-NULL arguments must be rejected
  EXPECT(!vw_platform_spawn_process(NULL, argv_ok, NULL));   // NULL executable
  EXPECT(!vw_platform_spawn_process(kSpawnOk, NULL, NULL));  // NULL argv

  const char* argv_missing[] = {kSpawnMissing, NULL};
  EXPECT(!vw_platform_spawn_process(kSpawnMissing, argv_missing, NULL));  // non-existent executable

#ifndef _WIN32
  // Bare-name spawn must fail closed instead of searching PATH.
  const char* argv_bare[] = {kSpawnBarePath, NULL};
  EXPECT(!vw_platform_spawn_process(kSpawnBarePath, argv_bare, NULL));
#endif

  // --- vw_platform_terminate_process reaps the child ---
  // A SIGKILLed child must be fully reaped (no zombie): wait_process after
  // terminate must succeed, and the pid must no longer exist. This exercises
  // the SIGKILL + bounded-reap path in terminate_process.
#ifndef _WIN32
  const char* argv_sleep[] = {"/bin/sleep", "30", NULL};
  vw_process_t sleeper = 0;
  EXPECT(vw_platform_spawn_process("/bin/sleep", argv_sleep, &sleeper));
  EXPECT(sleeper > 0);
  vw_platform_terminate_process(sleeper);
  EXPECT(vw_platform_wait_process(sleeper, 2000));          // reaped, not left a zombie
  EXPECT(kill((pid_t)sleeper, 0) == -1 && errno == ESRCH);  // process fully gone
#endif

  return 0;
}
