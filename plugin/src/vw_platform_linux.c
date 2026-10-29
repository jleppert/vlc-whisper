#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vw_platform.h"

// PIDs of children that were SIGKILLed but did not become waitable within the
// termination grace period (e.g. stuck in D-state). They are reaped
// opportunistically at every platform process call once they become waitable.
#define VW_MAX_UNREAPED_PIDS 16
static pid_t vw_unreaped_pids[VW_MAX_UNREAPED_PIDS];
static size_t vw_unreaped_count = 0;
static pthread_mutex_t vw_unreaped_mutex = PTHREAD_MUTEX_INITIALIZER;

static void vw_platform_register_unreaped(pid_t pid) {
  if (pid <= 0) return;
  pthread_mutex_lock(&vw_unreaped_mutex);
  if (vw_unreaped_count < VW_MAX_UNREAPED_PIDS) {
    vw_unreaped_pids[vw_unreaped_count++] = pid;
  }
  pthread_mutex_unlock(&vw_unreaped_mutex);
}

// Reap any previously-unkillable children that have since become waitable.
// Called from every process entry point; WNOHANG never blocks.
static void vw_platform_reap_unreaped(void) {
  pthread_mutex_lock(&vw_unreaped_mutex);
  size_t i = 0;
  while (i < vw_unreaped_count) {
    int status;
    pid_t ret = waitpid(vw_unreaped_pids[i], &status, WNOHANG);
    if (ret == vw_unreaped_pids[i] || (ret == -1 && errno == ECHILD)) {
      vw_unreaped_pids[i] = vw_unreaped_pids[vw_unreaped_count - 1];
      vw_unreaped_count--;
    } else {
      i++;
    }
  }
  pthread_mutex_unlock(&vw_unreaped_mutex);
}

static void* vw_platform_detached_reaper(void* arg) {
  pid_t pid = *(pid_t*)arg;
  free(arg);
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return NULL;
}

static void vw_platform_reap_detached(pid_t pid) {
  pid_t* arg = (pid_t*)malloc(sizeof(*arg));
  if (!arg) {
    vw_platform_register_unreaped(pid);
    return;
  }
  *arg = pid;
  pthread_t thread;
  if (pthread_create(&thread, NULL, vw_platform_detached_reaper, arg) != 0) {
    free(arg);
    vw_platform_register_unreaped(pid);
    return;
  }
  pthread_detach(thread);
}

#if defined(__linux__)
#include <sys/random.h>
#endif
#include <fcntl.h>

#if defined(__APPLE__)
// Darwin forbids direct `environ` references from shared libraries; _NSGetEnviron is the sanctioned accessor.
#include <crt_externs.h>
#define VW_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define VW_ENVIRON environ
#endif

bool vw_platform_get_random_bytes(void* buffer, size_t size) {
  if (!buffer || size == 0) return false;
#if defined(__linux__)
  size_t total = 0;
  while (total < size) {
    ssize_t ret = getrandom((uint8_t*)buffer + total, size - total, 0);
    if (ret > 0) {
      total += (size_t)ret;
      continue;
    }
    if (ret < 0 && errno == EINTR) continue;
    break;
  }
  if (total == size) return true;
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    size_t total = 0;
    while (total < size) {
      ssize_t r = read(fd, (uint8_t*)buffer + total, size - total);
      if (r <= 0) {
        if (r < 0 && errno == EINTR) continue;
        break;
      }
      total += (size_t)r;
    }
    close(fd);
    if (total == size) return true;
  }
  // Authentication tokens must never silently degrade to a predictable PRNG.
  return false;
}

int64_t vw_platform_get_time_us(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
    return (int64_t)ts.tv_sec * 1000000LL + (ts.tv_nsec / 1000LL);
  }
  return (int64_t)time(NULL) * 1000000LL;
}

int64_t vw_platform_get_monotonic_time_us(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    return (int64_t)ts.tv_sec * 1000000LL + (ts.tv_nsec / 1000LL);
  }
#ifdef CLOCK_MONOTONIC_RAW
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0) {
    return (int64_t)ts.tv_sec * 1000000LL + (ts.tv_nsec / 1000LL);
  }
#endif
  // Never fall back to civil time (time(NULL)) for monotonic timing; return error sentinel.
  return -1;
}

bool vw_platform_spawn_process(const char* executable_path, const char* const argv[], vw_process_t* out_process) {
  vw_platform_reap_unreaped();
  if (!executable_path || !argv) return false;

  pid_t pid;
  // Worker launch paths are identities, so never search the caller's PATH.
  if (executable_path[0] != '/') return false;
  if (access(executable_path, F_OK) != 0) return false;
  int ret = posix_spawn(&pid, executable_path, NULL, NULL, (char* const*)argv, VW_ENVIRON);
  if (ret != 0) return false;

  if (out_process)
    *out_process = pid;
  else
    vw_platform_reap_detached(pid);
  return true;
}

bool vw_platform_wait_process(vw_process_t process, uint32_t timeout_ms) {
  if (process <= 0) return false;
  vw_platform_reap_unreaped();
  pid_t pid = process;
  uint32_t elapsed_ms = 0;
  uint32_t sleep_ms = 10;

  while (elapsed_ms <= timeout_ms) {
    int status;
    pid_t ret = waitpid(pid, &status, WNOHANG);
    if (ret == pid) return true;
    if (ret == -1 && errno == ECHILD) return true;

    if (elapsed_ms >= timeout_ms) break;
    vw_platform_sleep_ms(sleep_ms);
    elapsed_ms += sleep_ms;
  }
  return false;
}

void vw_platform_terminate_process(vw_process_t process) {
  if (process > 0) {
    pid_t pid = (pid_t)process;
    vw_platform_reap_unreaped();
    kill(pid, SIGKILL);
    if (!vw_platform_wait_process(process, 1000)) vw_platform_register_unreaped(pid);
  }
}

void vw_platform_close_process(vw_process_t process) { (void)process; }

bool vw_platform_thread_create(vw_thread_t* thread, void* (*func)(void*), void* arg) {
  if (!thread || !func) return false;
  return pthread_create(thread, NULL, func, arg) == 0;
}

void vw_platform_thread_join(vw_thread_t thread) { pthread_join(thread, NULL); }

void vw_platform_sleep_ms(uint32_t ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

#endif  // defined(__linux__) || defined(__APPLE__) || defined(__unix__)

typedef int vw_platform_linux_unused_t;
