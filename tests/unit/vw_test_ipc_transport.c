#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vw_ipc_transport.h"

typedef struct {
  const char* endpoint;
  vw_ipc_handle_t* handle;
  size_t drain_bytes;
  size_t drained;
} listener_args_t;

static void vw_test_signal_handler(int signo) { (void)signo; }

static void vw_test_sleep(void) {
  struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};
  nanosleep(&delay, NULL);
}

static int64_t vw_test_now_us(void) {
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_nsec / 1000LL;
}

static void* vw_test_listener(void* opaque) {
  listener_args_t* args = (listener_args_t*)opaque;
  args->handle = vw_ipc_listen(args->endpoint);
  if (args->handle && args->drain_bytes > 0) {
    unsigned char chunk[65536];
    while (args->drained < args->drain_bytes) {
      int32_t got = vw_ipc_receive(args->handle, chunk, sizeof(chunk));
      if (got <= 0) break;
      args->drained += (size_t)got;
    }
  }
  return NULL;
}

static void vw_test_endpoint(char* out, size_t capacity, const char* suffix) {
  snprintf(out, capacity, "/tmp/vw-ipc-%s-%ld.sock", suffix, (long)getpid());
  unlink(out);
}

#if !defined(__APPLE__)
static void vw_test_seqpacket_truncation_is_fatal(void) {
  char endpoint[128];
  vw_test_endpoint(endpoint, sizeof(endpoint), "truncation");
  listener_args_t args = {.endpoint = endpoint, .handle = NULL, .drain_bytes = 0};
  pthread_t listener;
  assert(pthread_create(&listener, NULL, vw_test_listener, &args) == 0);
  vw_test_sleep();
  vw_ipc_handle_t* client = vw_ipc_connect(endpoint);
  assert(client != NULL);
  unsigned char oversized[64] = {0};
  assert(vw_ipc_send(client, oversized, sizeof(oversized)));
  pthread_join(listener, NULL);
  assert(args.handle != NULL);
  unsigned char short_buffer[8] = {0};
  errno = EAGAIN;
  assert(vw_ipc_receive(args.handle, short_buffer, sizeof(short_buffer)) == VW_IPC_RECV_FATAL);
  vw_ipc_close(client);
  vw_ipc_close(args.handle);
  unlink(endpoint);
}
#endif

static void vw_test_large_send_is_chunked(void) {
  char endpoint[128];
  vw_test_endpoint(endpoint, sizeof(endpoint), "large");
  unsigned char payload[960000];
  listener_args_t args = {.endpoint = endpoint, .handle = NULL, .drain_bytes = sizeof(payload)};
  pthread_t listener;
  assert(pthread_create(&listener, NULL, vw_test_listener, &args) == 0);
  vw_test_sleep();
  vw_ipc_handle_t* client = vw_ipc_connect(endpoint);
  assert(client != NULL);
  for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (unsigned char)(i * 31U);
  assert(vw_ipc_send(client, payload, sizeof(payload)));
  pthread_join(listener, NULL);
  assert(args.handle != NULL);
  assert(args.drained == sizeof(payload));
  vw_ipc_close(client);
  vw_ipc_close(args.handle);
  unlink(endpoint);
}

static void vw_test_listener_retries_eintr(void) {
  char endpoint[128];
  vw_test_endpoint(endpoint, sizeof(endpoint), "eintr");
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = vw_test_signal_handler;
  sigemptyset(&action.sa_mask);
  assert(sigaction(SIGUSR1, &action, NULL) == 0);
  listener_args_t args = {.endpoint = endpoint, .handle = NULL};
  pthread_t listener;
  assert(pthread_create(&listener, NULL, vw_test_listener, &args) == 0);
  vw_test_sleep();
  assert(pthread_kill(listener, SIGUSR1) == 0);
  vw_test_sleep();
  vw_ipc_handle_t* client = vw_ipc_connect(endpoint);
  assert(client != NULL);
  pthread_join(listener, NULL);
  assert(args.handle != NULL);
  vw_ipc_close(client);
  vw_ipc_close(args.handle);
  unlink(endpoint);
}

static void vw_test_stalled_send_has_one_deadline(void) {
  char endpoint[128];
  vw_test_endpoint(endpoint, sizeof(endpoint), "stalled-send");
  listener_args_t args = {.endpoint = endpoint, .handle = NULL, .drain_bytes = 0};
  pthread_t listener;
  assert(pthread_create(&listener, NULL, vw_test_listener, &args) == 0);
  vw_test_sleep();
  vw_ipc_handle_t* client = vw_ipc_connect(endpoint);
  assert(client != NULL);
  unsigned char payload[960000] = {0};
  int64_t started_us = vw_test_now_us();
  assert(!vw_ipc_send(client, payload, sizeof(payload)));
  int64_t elapsed_us = vw_test_now_us() - started_us;
  assert(elapsed_us < 5000000LL);
  pthread_join(listener, NULL);
  assert(args.handle != NULL);
  vw_ipc_close(client);
  vw_ipc_close(args.handle);
  unlink(endpoint);
}

int main(void) {
#if !defined(__APPLE__)
  // Darwin has no AF_UNIX SOCK_SEQPACKET; the transport is a byte stream there and cannot detect record truncation.
  vw_test_seqpacket_truncation_is_fatal();
#endif
  vw_test_large_send_is_chunked();
  vw_test_listener_retries_eintr();
  vw_test_stalled_send_has_one_deadline();
  return 0;
}
