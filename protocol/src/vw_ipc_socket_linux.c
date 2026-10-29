#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#if defined(__APPLE__)
#include <sys/ucred.h>
#endif
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/types.h>
#endif

#include "vw_ipc_transport.h"

// Darwin's AF_UNIX has no SOCK_SEQPACKET (socket() fails with EPROTONOSUPPORT). Fall back to a byte stream:
// every production reader already accumulates by frame-header byte counts (vw_worker.c / vw_worker_client.c), so
// record boundaries are a safety belt rather than a protocol requirement. Only MSG_TRUNC detection is lost.
#if defined(__APPLE__)
#define VW_IPC_SOCKET_TYPE SOCK_STREAM
#define VW_IPC_SOCKET_BUFFER_BYTES (256 * 1024)
#else
#define VW_IPC_SOCKET_TYPE SOCK_SEQPACKET
#endif

static void vw_ipc_configure_fd(int fd) {
  struct timeval tv;
  tv.tv_sec = 3;
  tv.tv_usec = 0;
  // Set 3 second timeout for send/receive operations
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#if defined(SO_NOSIGPIPE)
  int nosigpipe = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif
#if defined(VW_IPC_SOCKET_BUFFER_BYTES)
  // Darwin defaults AF_UNIX stream buffers to 8 KiB; PCM audio frames would otherwise fragment into hundreds of
  // partial sends per frame and stall the realtime path.
  int buffer_bytes = VW_IPC_SOCKET_BUFFER_BYTES;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_bytes, sizeof(buffer_bytes));
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_bytes, sizeof(buffer_bytes));
#endif
}

vw_ipc_handle_t* vw_ipc_listen(const char* endpoint_name) {
  if (!endpoint_name || endpoint_name[0] == '\0') return NULL;
  // Reject overlong endpoint names that cannot fit sun_path without truncation.
  // Truncation would bind a truncated path while unlink uses the original, causing
  // connection/cleanup mismatch. sun_path capacity is sizeof(addr.sun_path); a
  // name that needs truncation (len >= sizeof) must fail closed.
  if (strlen(endpoint_name) >= sizeof(((struct sockaddr_un*)0)->sun_path)) return NULL;
  int server_fd = socket(AF_UNIX, VW_IPC_SOCKET_TYPE, 0);
  if (server_fd < 0) return NULL;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  // Safe after length check: fits with NUL terminator.
  memcpy(addr.sun_path, endpoint_name, strlen(endpoint_name) + 1);

  if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(server_fd);
    return NULL;
  }
  if (chmod(endpoint_name, S_IRUSR | S_IWUSR) != 0) {
    unlink(endpoint_name);
    close(server_fd);
    return NULL;
  }

  if (listen(server_fd, 1) < 0) {
    unlink(endpoint_name);
    close(server_fd);
    return NULL;
  }

  struct pollfd pfd = {.fd = server_fd, .events = POLLIN};
  struct timespec wait_started;
  if (clock_gettime(CLOCK_MONOTONIC, &wait_started) != 0) {
    unlink(endpoint_name);
    close(server_fd);
    return NULL;
  }
  int wait_ms = 10000;
  for (;;) {
    int poll_result = poll(&pfd, 1, wait_ms);
    if (poll_result > 0) break;
    if (poll_result == 0 || errno != EINTR) {
      unlink(endpoint_name);
      close(server_fd);
      return NULL;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
      unlink(endpoint_name);
      close(server_fd);
      return NULL;
    }
    int64_t elapsed_ms =
        (int64_t)(now.tv_sec - wait_started.tv_sec) * 1000 + (int64_t)(now.tv_nsec - wait_started.tv_nsec) / 1000000;
    if (elapsed_ms >= 10000) {
      unlink(endpoint_name);
      close(server_fd);
      return NULL;
    }
    wait_ms = 10000 - (int)elapsed_ms;
  }

  int client_fd = accept(server_fd, NULL, NULL);
  close(server_fd);  // We only accept one connection
  if (client_fd < 0) {
    unlink(endpoint_name);
    return NULL;
  }

#if defined(__linux__)
  struct ucred peer;
  socklen_t peer_size = sizeof(peer);
  if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 || peer.uid != geteuid()) {
    close(client_fd);
    unlink(endpoint_name);
    return NULL;
  }
#elif defined(__APPLE__)
  struct xucred peer;
  socklen_t peer_size = sizeof(peer);
  if (getsockopt(client_fd, 0, LOCAL_PEERCRED, &peer, &peer_size) != 0 || peer.cr_uid != geteuid()) {
    close(client_fd);
    unlink(endpoint_name);
    return NULL;
  }
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  uid_t peer_uid;
  gid_t peer_gid;
  if (getpeereid(client_fd, &peer_uid, &peer_gid) != 0 || peer_uid != geteuid()) {
    close(client_fd);
    unlink(endpoint_name);
    return NULL;
  }
#endif
  // The pathname is no longer needed once the connection is accepted. Removing it
  // prevents stale endpoint reuse while keeping this handle independent of filesystem state.
  unlink(endpoint_name);

  vw_ipc_configure_fd(client_fd);

  vw_ipc_handle_t* handle = (vw_ipc_handle_t*)calloc(1, sizeof(vw_ipc_handle_t));
  if (!handle) {
    close(client_fd);
    return NULL;
  }
  handle->pipe_handle = (void*)(intptr_t)client_fd;
  return handle;
}

vw_ipc_handle_t* vw_ipc_connect(const char* endpoint_name) {
  if (!endpoint_name || endpoint_name[0] == '\0') return NULL;
  if (strlen(endpoint_name) >= sizeof(((struct sockaddr_un*)0)->sun_path)) return NULL;
  int client_fd = socket(AF_UNIX, VW_IPC_SOCKET_TYPE, 0);
  if (client_fd < 0) return NULL;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, endpoint_name, strlen(endpoint_name) + 1);

  if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(client_fd);
    return NULL;
  }

  vw_ipc_configure_fd(client_fd);

  vw_ipc_handle_t* handle = (vw_ipc_handle_t*)calloc(1, sizeof(vw_ipc_handle_t));
  if (!handle) {
    close(client_fd);
    return NULL;
  }
  handle->pipe_handle = (void*)(intptr_t)client_fd;
  return handle;
}

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#define VW_IPC_MAX_RECORD_BYTES 65536U

// Preserve SOCK_SEQPACKET record boundaries and detect a record larger than the
// caller's buffer instead of silently discarding its tail.
static ssize_t vw_ipc_recv_record(int fd, void* buffer, size_t buffer_size) {
  struct iovec iov = {.iov_base = buffer, .iov_len = buffer_size};
  struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1};
  ssize_t bytes;
  do {
    bytes = recvmsg(fd, &message, 0);
  } while (bytes < 0 && errno == EINTR);
  if (bytes >= 0 && (message.msg_flags & MSG_TRUNC)) {
    errno = EMSGSIZE;
    return -EMSGSIZE;
  }
  return bytes;
}

static int64_t vw_ipc_monotonic_us(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  return (int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_nsec / 1000LL;
}

bool vw_ipc_send(vw_ipc_handle_t* handle, const void* data, size_t size) {
  if (!handle || (!data && size != 0)) return false;
  int fd = (int)(intptr_t)handle->pipe_handle;
  const unsigned char* bytes = (const unsigned char*)data;
  int64_t deadline_us = vw_ipc_monotonic_us();
  if (deadline_us < 0) return false;
  deadline_us += 3000000LL;
  while (size > 0) {
    size_t record_size = size < VW_IPC_MAX_RECORD_BYTES ? size : VW_IPC_MAX_RECORD_BYTES;
    size_t record_sent = 0;
    while (record_sent < record_size) {
      int64_t now_us = vw_ipc_monotonic_us();
      if (now_us < 0 || now_us >= deadline_us) return false;
      int64_t remaining_us = deadline_us - now_us;
      int timeout_ms = (int)((remaining_us + 999) / 1000);
      struct pollfd pfd = {.fd = fd, .events = POLLOUT};
      int poll_result = poll(&pfd, 1, timeout_ms);
      if (poll_result < 0 && errno == EINTR) continue;
      if (poll_result == 0 || poll_result < 0) return false;
      ssize_t sent = send(fd, bytes + record_sent, record_size - record_sent, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (sent > 0) {
        // SOCK_SEQPACKET delivers the whole record or nothing; a byte stream may make partial progress, so
        // advance and keep the single overall deadline rather than treating the short write as a failure.
        record_sent += (size_t)sent;
        continue;
      }
      if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
      return false;
    }
    bytes += record_size;
    size -= record_size;
  }
  return true;
}

int32_t vw_ipc_receive(vw_ipc_handle_t* handle, void* buffer, size_t buffer_size) {
  if (!handle) return VW_IPC_RECV_FATAL;  // fatal: invalid handle
  int fd = (int)(intptr_t)handle->pipe_handle;
  ssize_t bytes = vw_ipc_recv_record(fd, buffer, buffer_size);
  if (bytes > 0) return (int32_t)bytes;
  if (bytes == 0) return VW_IPC_RECV_FATAL;  // EOF — peer closed connection (fatal)
  // bytes < 0: check for timeout vs real error
  if (errno == EAGAIN || errno == EWOULDBLOCK) return VW_IPC_RECV_TIMEOUT;  // timeout — keep waiting
  return VW_IPC_RECV_FATAL;                                                 // real error (fatal)
}

int32_t vw_ipc_receive_timeout(vw_ipc_handle_t* handle, void* buffer, size_t buffer_size, uint32_t timeout_us) {
  if (!handle) return VW_IPC_RECV_FATAL;
  int fd = (int)(intptr_t)handle->pipe_handle;

  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int timeout_ms = (int)(((uint64_t)timeout_us + 999) / 1000);  // round up: >=1us must not poll 0ms
  int ret;
  do {
    ret = poll(&pfd, 1, timeout_ms);
  } while (ret < 0 && errno == EINTR);  // signal interrupted the wait; connection is intact, retry
  if (ret == 0) return VW_IPC_RECV_TIMEOUT;
  if (ret < 0) return VW_IPC_RECV_FATAL;

  ssize_t bytes = vw_ipc_recv_record(fd, buffer, buffer_size);
  if (bytes > 0) return (int32_t)bytes;
  if (bytes == 0) return VW_IPC_RECV_FATAL;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return VW_IPC_RECV_TIMEOUT;
  return VW_IPC_RECV_FATAL;
}

void vw_ipc_close(vw_ipc_handle_t* handle) {
  if (handle) {
    int fd = (int)(intptr_t)handle->pipe_handle;
    if (fd >= 0) close(fd);
    free(handle);
  }
}
#endif
