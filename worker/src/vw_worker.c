#include "vw_worker.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vw_ipc_transport.h"
#include "vw_log.h"
#include "vw_model_catalog.h"
#include "vw_model_download.h"
#include "vw_platform.h"
#include "vw_protocol_codec.h"
#include "vw_protocol_types.h"
#include "vw_protocol_util.h"
#include "vw_source_decoder.h"
#include "vw_translate.h"
#include "vw_translate_async.h"
#include "vw_vad.h"
#include "vw_worker_queue.h"

#ifdef _WIN32
#include <mfapi.h>
#include <process.h>
#else
#include <unistd.h>
#endif

// Argument bundle passed to the worker IPC reader thread.
typedef struct vw_worker_reader_arg {
  vw_ipc_handle_t* handle;
  vw_worker_queue_t* queue;
  _Atomic bool* running;
  _Atomic bool* fatal_exit;
} vw_worker_reader_arg_t;

// Constant-time comparison of two 32-byte tokens to prevent timing attacks
static bool verify_token_constant_time(const uint8_t token_a[VW_AUTH_TOKEN_BYTES],
                                       const uint8_t token_b[VW_AUTH_TOKEN_BYTES]) {
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < VW_AUTH_TOKEN_BYTES; i++) {
    diff |= (token_a[i] ^ token_b[i]);
  }
  return diff == 0;
}

// Returns the symbolic name of a STOP reason code for logs, or the numeric value as a string when
// the code has no documented macro (e.g. 0 = plain user stop). STOP-only: PAUSE/RESUME also use
// reason 1U, so this must never be called for those message types (the name would mislabel them).
static const char* vw_worker_stop_reason_name(uint16_t reason) {
  switch (reason) {
    case VW_CTRL_REASON_USER_STOP:
      return "USER_STOP";
    case VW_CTRL_REASON_SEEK_DISCONTINUITY:
      return "SEEK_DISCONTINUITY";
    case VW_CTRL_REASON_MEDIA_END:
      return "MEDIA_END";
    default: {
      // Thread-local so a log from any thread cannot alias/overwrite the buffer.
      _Thread_local static char buf[16];
      snprintf(buf, sizeof(buf), "%u", reason);
      return buf;
    }
  }
}

// Builds and sends a VW_MSG_ERROR frame over IPC. Returns true on success.
static bool send_error(vw_ipc_handle_t* handle, const uint8_t session_id[VW_SESSION_ID_BYTES], vw_error_code_t code,
                       uint8_t recoverable, const char* msg, uint64_t* sequence) {
  vw_msg_error_t err_msg;
  memset(&err_msg, 0, sizeof(err_msg));
  memcpy(err_msg.session_id.bytes, session_id, VW_SESSION_ID_BYTES);
  err_msg.error_code = code;
  err_msg.recoverable = recoverable;
  snprintf(err_msg.message, sizeof(err_msg.message), "%s", msg);

  uint8_t err_payload[512];
  size_t err_len = 0;
  if (!vw_protocol_encode_payload(VW_MSG_ERROR, &err_msg, err_payload, sizeof(err_payload), &err_len)) {
    return false;
  }
  vw_frame_header_t err_hdr = {.magic = VW_PROTOCOL_MAGIC,
                               .major = VW_PROTOCOL_VERSION_MAJOR,
                               .type = VW_MSG_ERROR,
                               .payload_length = (uint32_t)err_len,
                               .sequence = ++(*sequence)};
  uint8_t err_hdr_buf[sizeof(vw_frame_header_t)];
  if (!vw_protocol_encode_header(&err_hdr, err_hdr_buf, sizeof(err_hdr_buf))) return false;
  return vw_ipc_send(handle, err_hdr_buf, sizeof(err_hdr_buf)) && vw_ipc_send(handle, err_payload, err_len);
}

// Builds and sends a model-progress frame, including terminal failures with unknown byte totals.
static bool vw_worker_send_model_progress(vw_ipc_handle_t* handle, const uint8_t session_id[VW_SESSION_ID_BYTES],
                                          uint8_t stage, uint8_t pct, uint64_t bytes_done, uint64_t bytes_total,
                                          const char* model_id, uint64_t* sequence) {
  if (!handle || !session_id || !model_id || !sequence) return false;

  vw_msg_model_progress_t progress;
  memset(&progress, 0, sizeof(progress));
  memcpy(progress.session_id.bytes, session_id, VW_SESSION_ID_BYTES);
  progress.stage = stage;
  progress.pct = pct;
  progress.bytes_done = bytes_done;
  progress.bytes_total = bytes_total;
  snprintf(progress.model_id, sizeof(progress.model_id), "%s", model_id);

  uint8_t payload[VW_MSG_MODEL_PROGRESS_PAYLOAD_BYTES];
  size_t payload_len = 0;
  if (!vw_protocol_encode_payload(VW_MSG_MODEL_PROGRESS, &progress, payload, sizeof(payload), &payload_len)) {
    return false;
  }

  vw_frame_header_t header = {.magic = VW_PROTOCOL_MAGIC,
                              .major = VW_PROTOCOL_VERSION_MAJOR,
                              .type = VW_MSG_MODEL_PROGRESS,
                              .payload_length = (uint32_t)payload_len,
                              .sequence = ++(*sequence)};
  uint8_t header_buf[sizeof(vw_frame_header_t)];
  if (!vw_protocol_encode_header(&header, header_buf, sizeof(header_buf))) return false;
  return vw_ipc_send(handle, header_buf, sizeof(header_buf)) && vw_ipc_send(handle, payload, payload_len);
}

// Sends cumulative inference timing and queue-drop status without adding a new wire message or blocking inference.
static bool vw_worker_send_status(vw_ipc_handle_t* handle, const uint8_t session_id[VW_SESSION_ID_BYTES],
                                  const vw_worker_config_t* config, const vw_whisper_engine_t* engine,
                                  const vw_worker_queue_t* queue, const vw_audio_buffer_t* audio_buf,
                                  bool session_active, bool paused, uint64_t* sequence) {
  if (!handle || !session_id || !config || !sequence) return false;
  vw_msg_status_t status;
  memset(&status, 0, sizeof(status));
  memcpy(status.session_id.bytes, session_id, VW_SESSION_ID_BYTES);
  if (!session_active) {
    status.state = VW_SESSION_STATE_IDLE;
  } else if (paused) {
    status.state = VW_SESSION_STATE_PAUSED;
  } else {
    status.state = VW_SESSION_STATE_PLAYING;
  }
  uint64_t queued_audio_us = queue ? vw_worker_queue_get_queued_audio_us(queue) : 0;
  if (audio_buf) {
    size_t samples = vw_audio_buffer_get_available(audio_buf);
    queued_audio_us += (uint64_t)samples * 1000000ULL / VW_AUDIO_SAMPLE_RATE;
  }
  status.queued_audio_us = (int64_t)queued_audio_us;
  status.inference_us = (int64_t)(engine ? vw_whisper_engine_get_total_inference_us(engine) : 0);
  status.dropped_audio_us = queue ? (int64_t)vw_worker_queue_get_dropped_audio_us(queue) : 0;
  const char* resolved =
      (config->backend == VW_WORKER_BACKEND_CPU || !engine || !vw_whisper_engine_is_gpu_active(engine)) ? "cpu" : "gpu";
  snprintf(status.resolved_backend, sizeof(status.resolved_backend), "%s", resolved);

  uint8_t payload[64];
  size_t payload_len = 0;
  if (!vw_protocol_encode_payload(VW_MSG_STATUS, &status, payload, sizeof(payload), &payload_len)) return false;
  vw_frame_header_t header = {.magic = VW_PROTOCOL_MAGIC,
                              .major = VW_PROTOCOL_VERSION_MAJOR,
                              .type = VW_MSG_STATUS,
                              .payload_length = (uint32_t)payload_len,
                              .sequence = ++(*sequence)};
  uint8_t header_buf[sizeof(vw_frame_header_t)];
  if (!vw_protocol_encode_header(&header, header_buf, sizeof(header_buf))) return false;
  return vw_ipc_send(handle, header_buf, sizeof(header_buf)) && vw_ipc_send(handle, payload, payload_len);
}

// Serializes one completed caption on the worker's single-writer main loop. Translation threads never touch IPC.
static bool vw_worker_send_caption_segment(vw_ipc_handle_t* handle, const vw_caption_segment_t* seg,
                                           uint64_t* sequence) {
  if (!handle || !seg || !sequence) return false;
  uint8_t payload[VW_CAPTION_SEGMENT_FIXED_BYTES + VW_MAX_TEXT_BYTES * 2U + 64U];
  size_t payload_len = 0;
  if (!vw_protocol_encode_payload(VW_MSG_CAPTION_SEGMENT, seg, payload, sizeof(payload), &payload_len)) return false;
  vw_frame_header_t header = {.magic = VW_PROTOCOL_MAGIC,
                              .major = VW_PROTOCOL_VERSION_MAJOR,
                              .type = VW_MSG_CAPTION_SEGMENT,
                              .payload_length = (uint32_t)payload_len,
                              .sequence = ++(*sequence)};
  uint8_t header_buf[sizeof(vw_frame_header_t)];
  if (!vw_protocol_encode_header(&header, header_buf, sizeof(header_buf))) return false;
  return vw_ipc_send(handle, header_buf, sizeof(header_buf)) && vw_ipc_send(handle, payload, payload_len);
}

typedef struct vw_worker_translation_delivery {
  vw_ipc_handle_t* handle;
  uint64_t* sequence;
  const vw_session_id_t* session_id;
  const bool* session_active;
  _Atomic bool* running;
  _Atomic bool* fatal_exit;
} vw_worker_translation_delivery_t;

static void vw_worker_deliver_translation(const vw_translate_async_result_t* translated, void* user_data) {
  vw_worker_translation_delivery_t* delivery = (vw_worker_translation_delivery_t*)user_data;
  if (!translated || !delivery || !*delivery->session_active ||
      memcmp(translated->segment.session_id.bytes, delivery->session_id->bytes, VW_SESSION_ID_BYTES) != 0) {
    return;
  }
  if (vw_worker_send_caption_segment(delivery->handle, &translated->segment, delivery->sequence)) {
    vw_log_event(translated->success ? VW_LOG_LEVEL_INFO : VW_LOG_LEVEL_WARN, "WORKER_TRANSLATE",
                 "translation complete segment=%llu success=%d tier=%u latency=%uus source_bytes=%u "
                 "translated_bytes=%u",
                 (unsigned long long)translated->segment.segment_id, (int)translated->success,
                 (unsigned int)translated->segment.translation_tier,
                 (unsigned int)translated->segment.translation_latency_us, (unsigned int)translated->segment.text_bytes,
                 (unsigned int)translated->segment.translated_text_bytes);
  } else {
    atomic_store(delivery->fatal_exit, true);
    atomic_store(delivery->running, false);
  }
}

// Dedicated IPC reader thread (ADR-013): the only thread that reads from the pipe. Continuously
// drains frames into the bounded worker frame queue so inference on the main loop never stalls
// transport reads. Never sends; all replies stay single-writer in vw_worker_run's main loop.
static void* vw_worker_reader_main(void* arg) {
  vw_worker_reader_arg_t* a = (vw_worker_reader_arg_t*)arg;
  uint8_t header_buf[sizeof(vw_frame_header_t)];
  uint8_t* payload_buf = NULL;
  uint8_t* discard_buf = (uint8_t*)malloc(VW_MAX_PAYLOAD_BYTES);
  if (!discard_buf) {
    atomic_store(a->fatal_exit, true);
    atomic_store(a->running, false);
    return NULL;
  }
  uint64_t last_plugin_sequence = 0;
  bool plugin_seq_valid = false;

  while (atomic_load(a->running)) {
    int32_t bytes_read = 0;
    while (bytes_read < (int32_t)sizeof(vw_frame_header_t)) {
      int32_t res = vw_ipc_receive(a->handle, header_buf + bytes_read, sizeof(vw_frame_header_t) - bytes_read);
      if (res < 0) {
        if (res == VW_IPC_RECV_TIMEOUT) {
          if (!atomic_load(a->running)) {
            free(discard_buf);
            return NULL;
          }
          continue;
        }
        goto fatal;
      }
      bytes_read += res;
    }

    vw_frame_header_t header;
    if (!vw_protocol_decode_header(header_buf, sizeof(vw_frame_header_t), &header)) {
      goto fatal;
    }
    if (!vw_protocol_validate_header(&header)) {
      goto fatal;
    }
    if (plugin_seq_valid && header.sequence <= last_plugin_sequence) {
      vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_SEQUENCE", "stale sequence %llu <= %llu type=%u; discarding",
                   (unsigned long long)header.sequence, (unsigned long long)last_plugin_sequence, header.type);
      if (header.payload_length > 0) {
        uint32_t drained = 0;
        while (drained < header.payload_length) {
          int32_t r = vw_ipc_receive(a->handle, discard_buf + drained, header.payload_length - drained);
          if (r < 0) {
            if (r == VW_IPC_RECV_TIMEOUT) {
              if (!atomic_load(a->running)) {
                free(discard_buf);
                return NULL;
              }
              continue;
            }
            goto fatal;
          }
          drained += (uint32_t)r;
        }
      }
      continue;
    }
    plugin_seq_valid = true;
    last_plugin_sequence = header.sequence;
    if (header.payload_length > 0) {
      payload_buf = (uint8_t*)malloc(header.payload_length);
      if (!payload_buf) {
        goto fatal;
      }
      uint32_t payload_read = 0;
      while (payload_read < header.payload_length) {
        int32_t res = vw_ipc_receive(a->handle, payload_buf + payload_read, header.payload_length - payload_read);
        if (res < 0) {
          if (res == VW_IPC_RECV_TIMEOUT) {
            if (!atomic_load(a->running)) {
              free(payload_buf);
              return NULL;
            }
            continue;
          }
          goto fatal;
        }
        payload_read += (uint32_t)res;
      }
    }

    if (!vw_worker_queue_push(a->queue, header.type, payload_buf, header.payload_length)) {
      vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_QUEUE", "frame queue push failed (type=%u); frame dropped", header.type);
    }
    payload_buf = NULL;
  }
  free(discard_buf);
  return NULL;

fatal:
  free(discard_buf);
  free(payload_buf);
  atomic_store(a->fatal_exit, true);
  atomic_store(a->running, false);
  vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_READER", "fatal pipe read (peer closed); waking main loop");
  vw_worker_queue_push(a->queue, VW_MSG_SHUTDOWN, NULL, 0);
  return NULL;
}

static bool vw_worker_flush_audio_tail(vw_audio_buffer_t* audio_buf, float* window_samples, vw_whisper_engine_t* engine,
                                       struct whisper_vad_context* vad_ctx, vw_segment_builder_t* builder) {
  if (!audio_buf || !window_samples || !engine || !builder) return false;

  while (vw_audio_buffer_get_count(audio_buf) > 0) {
    int64_t window_pts_us = 0;
    size_t available = vw_audio_buffer_get_samples(audio_buf, window_samples, VW_CHUNK_MAX_SAMPLES, &window_pts_us);
    if (available == 0) break;

    size_t cut_samples = 0;
    size_t silence_drain = 0;
    if (!vw_vad_find_chunk_boundary(window_samples, available, vad_ctx, true, &cut_samples, &silence_drain) ||
        (cut_samples == 0 && silence_drain == 0)) {
      return false;
    }
    if (silence_drain > 0) {
      vw_audio_buffer_drain(audio_buf, silence_drain);
      continue;
    }
    if (!vw_whisper_engine_transcribe_pcm(engine, window_samples, cut_samples)) return false;

    int segment_count = vw_whisper_engine_get_segment_count(engine);
    for (int index = 0; index < segment_count; index++) {
      vw_whisper_segment_t segment;
      if (!vw_whisper_engine_get_segment(engine, index, &segment) || segment.no_speech_prob >= 0.60f) continue;
      int64_t start_pts_us = vw_saturating_add_i64(window_pts_us, segment.t0_us);
      int64_t end_pts_us = vw_saturating_add_i64(window_pts_us, segment.t1_us);
      vw_segment_builder_push_hypothesis(builder, segment.text_utf8, start_pts_us, end_pts_us);
    }
    vw_audio_buffer_drain(audio_buf, cut_samples);
  }
  return true;
}

static bool vw_worker_emit_tail_segments(vw_ipc_handle_t* handle, vw_segment_builder_t* builder,
                                         const vw_session_id_t* session_id, uint64_t* sequence,
                                         vw_translate_async_t* translator, bool translate_enabled,
                                         const char* source_lang, const char* target_lang) {
  if (!handle || !builder || !session_id || !sequence) return false;
  vw_caption_segment_t segment;
  while (vw_segment_builder_pop(builder, &segment)) {
    memcpy(segment.session_id.bytes, session_id->bytes, VW_SESSION_ID_BYTES);
    segment.translated_text_utf8 = NULL;
    segment.translated_text_bytes = 0;
    segment.translation_attempted = false;
    segment.translation_tier = VW_TRANSLATE_TIER_NONE;
    segment.translation_latency_us = 0;
    bool sent = false;
    if (translate_enabled && translator && segment.text_utf8 && segment.text_utf8[0] &&
        vw_translate_async_submit(translator, &segment, source_lang, target_lang)) {
      sent = true;
    } else {
      segment.translation_attempted = translate_enabled;
      sent = vw_worker_send_caption_segment(handle, &segment, sequence);
    }
    free(segment.text_utf8);
    if (!sent) return false;
  }
  return true;
}

static void vw_worker_drain_translation(vw_translate_async_t* translator, vw_ipc_handle_t* handle,
                                        const vw_session_id_t* session_id, bool* session_active, _Atomic bool* running,
                                        _Atomic bool* fatal_exit, uint64_t* sequence) {
  if (!translator || !handle || !session_id || !session_active || !running || !sequence) return;
  vw_worker_translation_delivery_t delivery = {.handle = handle,
                                               .sequence = sequence,
                                               .session_id = session_id,
                                               .session_active = session_active,
                                               .running = running,
                                               .fatal_exit = fatal_exit};
  // Accepted work is bounded; each request retains its own deadline, including source-only timeout results.
  // Do not invalidate those results merely because shutdown began before a request finished.
  while (atomic_load(running) && vw_translate_async_has_pending(translator)) {
    if (!vw_translate_async_try_deliver(translator, vw_worker_deliver_translation, &delivery)) vw_platform_sleep_ms(5);
  }
}

int vw_worker_run(const vw_worker_config_t* config) {
  if (!config) {
    return 1;
  }

#ifdef _WIN32
  MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
#endif

  vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_LIFECYCLE", "worker started (pid %d)", (int)getpid());
  vw_ipc_handle_t* handle = vw_ipc_listen(config->pipe_name);
  if (!handle) {
    vw_log_event(VW_LOG_LEVEL_ERROR, "WORKER_LIFECYCLE", "vw_ipc_listen FAILED for %s; worker exiting",
                 config->pipe_name);
#ifdef _WIN32
    MFShutdown();
#endif
    return 1;
  }
  vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_LIFECYCLE", "listening on %s", config->pipe_name);

  _Atomic bool running = true;
  _Atomic bool fatal_exit = false;
  bool authenticated = false;

  float* window_samples = (float*)malloc(VW_WHISPER_MAX_CHUNK_SAMPLES * sizeof(float));
  if (!window_samples) {
    vw_ipc_close(handle);
#ifdef _WIN32
    MFShutdown();
#endif
    return 1;
  }

  char resolved_model_path[VW_PATH_MAX_BYTES];
  const char* effective_model_path = config->model_path;
  if (vw_worker_config_resolve_model_path(config, resolved_model_path, sizeof(resolved_model_path))) {
    effective_model_path = resolved_model_path;
    if (strcmp(effective_model_path, config->model_path) != 0) {
      vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_ENGINE", "resolved model '%s' via model directory to '%s'",
                   config->model_path, effective_model_path);
    }
  }
  vw_worker_backend_t effective_backend = config->backend;
  const char* force_cpu_env = getenv("VW_FORCE_CPU");
  if (force_cpu_env && strcmp(force_cpu_env, "1") == 0) {
    effective_backend = VW_WORKER_BACKEND_CPU;
    vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_ENGINE", "VW_FORCE_CPU=1 override active; forcing CPU backend");
  }
  const char* effective_language = config->language[0] ? config->language : "en";
  vw_whisper_engine_t* engine = vw_whisper_engine_init(effective_model_path, effective_backend, config->gpu_device);
  if (engine) {
    if (!vw_whisper_engine_set_language(engine, effective_language)) {
      vw_log_event(VW_LOG_LEVEL_ERROR, "WORKER_ENGINE", "invalid Whisper language '%s'; refusing to start engine",
                   effective_language);
      vw_whisper_engine_free(engine);
      engine = NULL;
    } else {
      vw_whisper_engine_set_n_threads(engine, config->n_threads);
      vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_ENGINE", "engine language=%s threads=%d", engine->language,
                   engine->n_threads);
    }
  }
  vw_log_event(
      engine ? VW_LOG_LEVEL_INFO : VW_LOG_LEVEL_WARN, "WORKER_ENGINE",
      engine ? "whisper engine loaded from '%s'" : "whisper engine init FAILED for '%s' (model missing/invalid)",
      effective_model_path);
  char resolved_vad_model_path[VW_PATH_MAX_BYTES];
  const char* effective_vad_model_path = NULL;
  if (vw_worker_config_resolve_vad_model_path(config, effective_model_path, resolved_vad_model_path,
                                              sizeof(resolved_vad_model_path))) {
    effective_vad_model_path = resolved_vad_model_path;
    vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_VAD_RESOLVE", "selected VAD path='%s'", effective_vad_model_path);
  }
  struct whisper_vad_context* vad_ctx = NULL;
  if (effective_vad_model_path) {
    vad_ctx = vw_vad_init_default(effective_vad_model_path);
    if (!vad_ctx) {
      vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_VAD",
                   "Silero VAD model failed to load from '%s'; operating on RMS Energy fallback",
                   effective_vad_model_path);
    } else {
      vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_VAD", "Silero VAD model loaded (%s)", effective_vad_model_path);
    }
  } else {
    vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_VAD",
                 "Silero VAD model not specified; operating on zero-config RMS Energy fallback");
  }

  vw_model_download_t* model_dl = NULL;
  char dl_dir[VW_PATH_MAX_BYTES];
  dl_dir[0] = '\0';
  bool dl_dir_ready = false;
  if (config->model_dir[0] != '\0') {
    snprintf(dl_dir, sizeof(dl_dir), "%s", config->model_dir);
    dl_dir_ready = true;
  } else {
    dl_dir_ready = vw_model_download_default_dir(dl_dir, sizeof(dl_dir));
  }
  if (dl_dir_ready) {
    vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_MODEL_DL", "model download directory '%s'", dl_dir);
  } else {
    vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_MODEL_DL", "model download directory unavailable");
  }
  int last_stage = -1;
  int64_t last_progress_send_us = 0;

  vw_audio_buffer_t* audio_buf = vw_audio_buffer_create(VW_LOOKAHEAD_BUFFER_SAMPLES);
  vw_segment_builder_t* builder = vw_segment_builder_create();
  if (!audio_buf || !builder) {
    if (audio_buf) vw_audio_buffer_free(audio_buf);
    if (builder) vw_segment_builder_free(builder);
    if (engine) vw_whisper_engine_free(engine);
    if (vad_ctx) vw_vad_free(vad_ctx);
    if (model_dl) {
      vw_model_download_abort(model_dl);
      vw_model_download_free(model_dl);
    }
    free(window_samples);
    vw_ipc_close(handle);
#ifdef _WIN32
    MFShutdown();
#endif
    return 1;
  }
  vw_session_id_t session_id;
  memset(&session_id, 0, sizeof(session_id));
  bool session_active = false;
  bool paused = false;
  bool live_progressive_mode = false;
  size_t live_next_inference_samples = VW_WINDOW_SAMPLES;
  uint64_t sequence = 1;

  vw_source_decoder_t* source_decoder = NULL;
  bool source_mode = false;
  bool source_eof = false;
  bool seek_failed_pending = false;
  int64_t current_playback_pts_us = 0;
  int64_t last_playback_pts_us = -1;
  int64_t decoded_pts_us = 0;
  const int64_t lead_target_us = 30000000LL;

  bool translate_enabled = false;
  char translate_src_lang[16] = "auto";
  char translate_dst_lang[16] = "en";
  uint8_t translate_mode = 1;

  vw_worker_queue_t* queue = vw_worker_queue_create(VW_WORKER_FRAME_QUEUE_CAPACITY);
  if (!queue) {
    free(window_samples);
    if (audio_buf) vw_audio_buffer_free(audio_buf);
    if (builder) vw_segment_builder_free(builder);
    if (engine) vw_whisper_engine_free(engine);
    if (vad_ctx) vw_vad_free(vad_ctx);
    if (model_dl) {
      vw_model_download_abort(model_dl);
      vw_model_download_free(model_dl);
    }
    vw_ipc_close(handle);
#ifdef _WIN32
    MFShutdown();
#endif
    return 1;
  }

  vw_translate_async_t* translator = vw_translate_async_create();
  if (!translator) {
    vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_TRANSLATE",
                 "translation thread unavailable; enabled translation will degrade immediately to source captions");
  }

  vw_worker_reader_arg_t reader_arg = {
      .handle = handle, .queue = queue, .running = &running, .fatal_exit = &fatal_exit};
  vw_thread_t reader_thread;
  if (!vw_platform_thread_create(&reader_thread, vw_worker_reader_main, &reader_arg)) {
    vw_log_event(VW_LOG_LEVEL_ERROR, "WORKER_READER", "reader thread creation FAILED; worker exiting");
    if (translator) vw_translate_async_destroy(translator);
    vw_worker_queue_destroy(queue);
    free(window_samples);
    if (audio_buf) vw_audio_buffer_free(audio_buf);
    if (builder) vw_segment_builder_free(builder);
    if (engine) vw_whisper_engine_free(engine);
    if (vad_ctx) vw_vad_free(vad_ctx);
    if (model_dl) {
      vw_model_download_abort(model_dl);
      vw_model_download_free(model_dl);
    }
    vw_ipc_close(handle);
#ifdef _WIN32
    MFShutdown();
#endif
    return 1;
  }
  vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_READER", "reader thread started; draining pipe into frame queue");

  while (atomic_load(&running)) {
    vw_worker_frame_t frame;
    bool has_frame = false;

    while (atomic_load(&running)) {
      if (vw_worker_queue_pop(queue, &frame)) {
        has_frame = true;
        break;
      }
      if (translator && vw_translate_async_has_result(translator)) break;
      if (session_active && source_mode && source_decoder && !paused && !source_eof &&
          (decoded_pts_us < vw_saturating_add_i64(current_playback_pts_us, lead_target_us))) {
        break;
      }
      if (model_dl) {
        vw_platform_sleep_ms(5);
        break;
      }
      vw_platform_sleep_ms(5);
    }
    if (!atomic_load(&running)) {
      break;
    }

    if (has_frame) {
      union {
        vw_msg_hello_t hello;
        vw_msg_start_t start;
        vw_msg_audio_t audio;
        vw_msg_control_t control;
        vw_msg_status_t status;
        vw_msg_position_t position;
        vw_msg_model_ctrl_t model_ctrl;
        vw_msg_translate_ctrl_t translate_ctrl;
      } payload_decoded;

      memset(&payload_decoded, 0, sizeof(payload_decoded));

      bool valid_payload = false;
      bool decoded_payload = vw_protocol_decode_payload(frame.type, frame.payload, frame.payload_len, &payload_decoded);
      if (decoded_payload) {
        if (vw_protocol_validate_payload(frame.type, &payload_decoded)) {
          valid_payload = true;
        }
      }

      if (!valid_payload) {
        bool is_valid_empty_shutdown = (frame.type == VW_MSG_SHUTDOWN && frame.payload_len == 0);
        bool is_invalid_start_audio =
            authenticated && decoded_payload && frame.type == VW_MSG_START_SESSION &&
            (payload_decoded.start.sample_rate != VW_AUDIO_SAMPLE_RATE || payload_decoded.start.channels != 1U ||
             payload_decoded.start.sample_format != VW_SAMPLE_FORMAT_S16LE);
        if (is_invalid_start_audio) {
          if (!send_error(handle, payload_decoded.start.session_id.bytes, E_AUDIO_FORMAT, 1,
                          "Unsupported audio format (expected 16kHz mono S16LE)", &sequence)) {
            atomic_store(&fatal_exit, true);
            free(frame.payload);
            break;
          }
          free(frame.payload);
          continue;
        }
        if (!is_valid_empty_shutdown) {
          atomic_store(&fatal_exit, true);
          vw_log_event(VW_LOG_LEVEL_ERROR, "WORKER_PROTOCOL", "invalid payload (type=%u len=%u); exiting", frame.type,
                       frame.payload_len);
          free(frame.payload);
          break;
        }
      }

      if (!authenticated) {
        if (frame.type != VW_MSG_HELLO) {
          atomic_store(&fatal_exit, true);
          free(frame.payload);
          break;
        }
        if (payload_decoded.hello.min_major > VW_PROTOCOL_VERSION_MAJOR ||
            payload_decoded.hello.max_major < VW_PROTOCOL_VERSION_MAJOR) {
          const uint8_t no_session[VW_SESSION_ID_BYTES] = {0};
          (void)send_error(handle, no_session, E_PROTOCOL_VERSION, 0, "No compatible protocol major", &sequence);
          atomic_store(&fatal_exit, true);
          free(frame.payload);
          break;
        }
        if (!verify_token_constant_time(config->auth_token, payload_decoded.hello.auth_token)) {
          atomic_store(&fatal_exit, true);
          vw_log_event(VW_LOG_LEVEL_ERROR, "WORKER_AUTH", "HELLO token mismatch; rejecting connection");
          free(frame.payload);
          break;
        }
        authenticated = true;
        vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_AUTH", "HELLO authenticated; replying HELLO_ACK");

        vw_msg_hello_ack_t ack = {.selected_major = VW_PROTOCOL_VERSION_MAJOR,
                                  .selected_minor = VW_PROTOCOL_VERSION_MINOR,
                                  .capability_flags = VW_CAPABILITY_PCM_S16LE_16K_MONO | VW_CAPABILITY_SOURCE_MODE |
                                                      VW_CAPABILITY_TRANSLATION,
                                  .worker_version = VW_WORKER_VERSION,
                                  .worker_version_length = VW_WORKER_VERSION_LENGTH};
        uint8_t ack_payload[256];
        size_t ack_len = 0;
        if (!vw_protocol_encode_payload(VW_MSG_HELLO_ACK, &ack, ack_payload, sizeof(ack_payload), &ack_len)) {
          atomic_store(&fatal_exit, true);
          free(frame.payload);
          break;
        }
        vw_frame_header_t ack_hdr = {.magic = VW_PROTOCOL_MAGIC,
                                     .major = VW_PROTOCOL_VERSION_MAJOR,
                                     .type = VW_MSG_HELLO_ACK,
                                     .payload_length = (uint32_t)ack_len,
                                     .sequence = 1};
        uint8_t ack_hdr_buf[sizeof(vw_frame_header_t)];
        if (!vw_protocol_encode_header(&ack_hdr, ack_hdr_buf, sizeof(ack_hdr_buf))) {
          atomic_store(&fatal_exit, true);
          free(frame.payload);
          break;
        }
        if (!vw_ipc_send(handle, ack_hdr_buf, sizeof(ack_hdr_buf)) || !vw_ipc_send(handle, ack_payload, ack_len)) {
          vw_log_event(VW_LOG_LEVEL_ERROR, "WORKER_AUTH", "failed to send HELLO_ACK");
          atomic_store(&fatal_exit, true);
          atomic_store(&running, false);
          free(frame.payload);
          break;
        }

        free(frame.payload);
        continue;
      }

      switch (frame.type) {
        case VW_MSG_START_SESSION: {
          if (session_active) {
            if (memcmp(session_id.bytes, payload_decoded.start.session_id.bytes, VW_SESSION_ID_BYTES) == 0) {
              if (!send_error(handle, session_id.bytes, E_INTERNAL, 1, "Session already active", &sequence)) {
                atomic_store(&fatal_exit, true);
                atomic_store(&running, false);
              }
              break;
            }
            vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_SESSION",
                         "media swap / new epoch detected (restarting worker session)");
            if (source_decoder) {
              vw_source_decoder_close(source_decoder);
              source_decoder = NULL;
            }
            if (audio_buf) vw_audio_buffer_clear(audio_buf);
            live_progressive_mode = false;
            live_next_inference_samples = VW_WINDOW_SAMPLES;
            if (builder) {
              vw_caption_segment_t stale_seg;
              while (vw_segment_builder_pop(builder, &stale_seg)) {
                if (stale_seg.text_utf8) free(stale_seg.text_utf8);
              }
            }
            if (vad_ctx) vw_vad_reset_state(vad_ctx);
            session_active = false;
          }
          if (payload_decoded.start.sample_rate != VW_AUDIO_SAMPLE_RATE) {
            vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_SESSION", "START rejected: E_AUDIO_FORMAT (rate=%u)",
                         payload_decoded.start.sample_rate);
            if (!send_error(handle, payload_decoded.start.session_id.bytes, E_AUDIO_FORMAT, 1,
                            "Unsupported sample rate (expected 16000)", &sequence)) {
              atomic_store(&fatal_exit, true);
              atomic_store(&running, false);
            }
            break;
          }
          if (!engine) {
            vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_SESSION", "START rejected: E_MODEL_MISSING");
            if (!send_error(handle, payload_decoded.start.session_id.bytes, E_MODEL_MISSING, 0,
                            "Whisper model file missing or invalid", &sequence)) {
              atomic_store(&fatal_exit, true);
              atomic_store(&running, false);
            }
            break;
          }

          if (!vw_whisper_engine_set_language(engine, payload_decoded.start.language)) {
            if (!send_error(handle, payload_decoded.start.session_id.bytes, E_INTERNAL, 1,
                            "Invalid session transcription language", &sequence)) {
              atomic_store(&fatal_exit, true);
              atomic_store(&running, false);
            }
            break;
          }

          memcpy(session_id.bytes, payload_decoded.start.session_id.bytes, VW_SESSION_ID_BYTES);
          session_active = true;
          paused = false;
          live_progressive_mode = (payload_decoded.start.source_kind == VW_SOURCE_LIVE_AUDIO);
          live_next_inference_samples = live_progressive_mode ? VW_LIVE_STARTUP_SAMPLES : VW_WINDOW_SAMPLES;
          source_eof = false;
          seek_failed_pending = false;
          current_playback_pts_us = payload_decoded.start.timeline_origin_pts_us;
          last_playback_pts_us = -1;
          decoded_pts_us = payload_decoded.start.timeline_origin_pts_us;
          if (audio_buf) vw_audio_buffer_clear(audio_buf);
          if (builder) {
            vw_segment_builder_clear(builder);
          }
          if (vad_ctx) {
            vw_vad_reset_state(vad_ctx);
          }
          if (translator) {
            vw_translate_async_invalidate(translator);
          }

          if (source_decoder) {
            vw_source_decoder_close(source_decoder);
            source_decoder = NULL;
            source_mode = false;
          }
          if (payload_decoded.start.source_url_len > 0 && payload_decoded.start.source_url[0] != '\0') {
            vw_source_decoder_info_t sinfo = {0};
            source_decoder = vw_source_decoder_open(payload_decoded.start.source_url, &sinfo);
            if (source_decoder) {
              source_mode = true;
              source_eof = false;
              current_playback_pts_us = payload_decoded.start.timeline_origin_pts_us;
              last_playback_pts_us = current_playback_pts_us;
              decoded_pts_us = current_playback_pts_us;
              if (current_playback_pts_us > 0 && !vw_source_decoder_seek(source_decoder, current_playback_pts_us)) {
                vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_SOURCE",
                             "initial source seek to %lldus failed; falling back to live PCM",
                             (long long)current_playback_pts_us);
                vw_source_decoder_close(source_decoder);
                source_decoder = NULL;
                source_mode = false;
              }
              if (source_mode) {
                source_eof = false;
                vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_SOURCE",
                             "source look-ahead mode ACTIVE for '%s' (dur=%lldus fmt=%s origin=%lldus paused=%d)",
                             payload_decoded.start.source_url, (long long)sinfo.duration_us, sinfo.container_format,
                             (long long)current_playback_pts_us, (int)paused);
              }
            } else {
              source_mode = false;
              source_eof = false;
              vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_SOURCE",
                           "failed to open source url '%s'; falling back to live PCM stream",
                           payload_decoded.start.source_url);
              if (!send_error(handle, payload_decoded.start.session_id.bytes, E_SOURCE_OPEN, 1,
                              "Failed to open source MRL; falling back to live PCM stream", &sequence)) {
                atomic_store(&fatal_exit, true);
                atomic_store(&running, false);
                break;
              }
            }
          } else {
            source_mode = false;
            source_eof = false;
          }

          vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_SESSION",
                       "session started (STARTED sent source_active=%d progressive_live=%d)", source_mode ? 1 : 0,
                       live_progressive_mode ? 1 : 0);

          vw_msg_started_t started_payload = {.source_active =
                                                  source_mode ? VW_SOURCE_ACTIVE_ACTIVE : VW_SOURCE_ACTIVE_INACTIVE};
          memcpy(started_payload.session_id.bytes, payload_decoded.start.session_id.bytes, VW_SESSION_ID_BYTES);
          uint8_t started_payload_buf[VW_MSG_STARTED_PAYLOAD_BYTES];
          size_t started_written = 0;
          vw_protocol_encode_payload(VW_MSG_STARTED, &started_payload, started_payload_buf, sizeof(started_payload_buf),
                                     &started_written);

          vw_frame_header_t started_hdr = {.magic = VW_PROTOCOL_MAGIC,
                                           .major = VW_PROTOCOL_VERSION_MAJOR,
                                           .type = VW_MSG_STARTED,
                                           .payload_length = (uint32_t)started_written,
                                           .sequence = ++sequence};
          uint8_t started_hdr_buf[sizeof(vw_frame_header_t)];
          bool started_sent = vw_protocol_encode_header(&started_hdr, started_hdr_buf, sizeof(started_hdr_buf)) &&
                              vw_ipc_send(handle, started_hdr_buf, sizeof(started_hdr_buf));
          if (started_sent && started_written > 0)
            started_sent = vw_ipc_send(handle, started_payload_buf, started_written);
          if (!started_sent) {
            atomic_store(&fatal_exit, true);
            atomic_store(&running, false);
            break;
          }
          if (!vw_worker_send_status(handle, payload_decoded.start.session_id.bytes, config, engine, queue, audio_buf,
                                     session_active, paused, &sequence)) {
            atomic_store(&fatal_exit, true);
            atomic_store(&running, false);
          }
          vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_STATUS", "STATUS sent resolved backend and zero inference time");
          break;
        }

        case VW_MSG_POSITION: {
          if (!session_active ||
              memcmp(payload_decoded.position.session_id.bytes, session_id.bytes, VW_SESSION_ID_BYTES) != 0) {
            break;
          }
          const int64_t requested_pts_us = payload_decoded.position.current_pts_us;
          bool is_pos_paused = (payload_decoded.position.flags & VW_POSITION_FLAG_PAUSED) != 0;
          if (is_pos_paused != paused) {
            paused = is_pos_paused;
            if (paused) {
              if (audio_buf) vw_audio_buffer_clear(audio_buf);
              live_next_inference_samples = live_progressive_mode ? VW_LIVE_STARTUP_SAMPLES : VW_WINDOW_SAMPLES;
              if (builder) vw_segment_builder_clear(builder);
              if (vad_ctx) vw_vad_reset_state(vad_ctx);
            } else {
              if (source_mode && source_decoder && requested_pts_us >= 0) {
                if (!vw_source_decoder_seek(source_decoder, requested_pts_us)) {
                  vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_SEEK",
                               "RESUME seek to %lldus failed; retaining anchor %lldus", (long long)requested_pts_us,
                               (long long)decoded_pts_us);
                } else {
                  current_playback_pts_us = requested_pts_us;
                  decoded_pts_us = requested_pts_us;
                  last_playback_pts_us = requested_pts_us;
                  source_eof = false;
                  if (audio_buf) vw_audio_buffer_clear(audio_buf);
                  live_next_inference_samples = live_progressive_mode ? VW_LIVE_STARTUP_SAMPLES : VW_WINDOW_SAMPLES;
                  if (builder) vw_segment_builder_clear(builder);
                  if (vad_ctx) vw_vad_reset_state(vad_ctx);
                }
              }
            }
            if (!vw_worker_send_status(handle, session_id.bytes, config, engine, queue, audio_buf, session_active,
                                       paused, &sequence)) {
              atomic_store(&fatal_exit, true);
              atomic_store(&running, false);
              break;
            }
          }

          if (!source_mode || !source_decoder) current_playback_pts_us = requested_pts_us;

          if (source_mode && source_decoder) {
            bool seek_flag = (payload_decoded.position.flags & VW_POSITION_FLAG_SEEK) != 0;
            bool backward_jump =
                (last_playback_pts_us >= 0 &&
                 payload_decoded.position.current_pts_us < vw_saturating_sub_i64(last_playback_pts_us, 500000LL));
            bool forward_past_decoded =
                (payload_decoded.position.current_pts_us > vw_saturating_add_i64(decoded_pts_us, 1000000LL));

            bool requires_seek = seek_flag || ((backward_jump || forward_past_decoded) &&
                                               payload_decoded.position.current_pts_us != last_playback_pts_us &&
                                               (!seek_failed_pending || seek_flag));
            if (requires_seek) {
              vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_SEEK",
                           "re-seeking source decoder to %lldus (flag=%d back=%d fwd=%d)",
                           (long long)payload_decoded.position.current_pts_us, (int)seek_flag, (int)backward_jump,
                           (int)forward_past_decoded);
              if (!vw_source_decoder_seek(source_decoder, payload_decoded.position.current_pts_us)) {
                seek_failed_pending = !seek_flag;
                vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_SEEK", "source seek to %lldus failed; retaining anchor %lldus",
                             (long long)payload_decoded.position.current_pts_us, (long long)decoded_pts_us);
              } else {
                seek_failed_pending = false;
                if (translator) vw_translate_async_invalidate(translator);
                current_playback_pts_us = requested_pts_us;
                decoded_pts_us = payload_decoded.position.current_pts_us;
                source_eof = false;
                if (audio_buf) vw_audio_buffer_clear(audio_buf);
                live_next_inference_samples = live_progressive_mode ? VW_LIVE_STARTUP_SAMPLES : VW_WINDOW_SAMPLES;
                if (builder) vw_segment_builder_clear(builder);
                if (vad_ctx) vw_vad_reset_state(vad_ctx);
              }
            }
            if (!requires_seek || current_playback_pts_us == requested_pts_us) {
              current_playback_pts_us = requested_pts_us;
              last_playback_pts_us = requested_pts_us;
            }
          }
          break;
        }

        case VW_MSG_AUDIO_PCM: {
          if (!session_active || paused || source_mode ||
              memcmp(payload_decoded.audio.session_id.bytes, session_id.bytes, VW_SESSION_ID_BYTES) != 0) {
            break;
          }

          const int16_t* pcm16 = (const int16_t*)payload_decoded.audio.pcm_data;
          size_t sample_count = payload_decoded.audio.pcm_bytes / sizeof(int16_t);
          int64_t pts_us = payload_decoded.audio.start_pts_us;
          if (pts_us < 0 || pts_us > 315360000000000LL) {
            vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_AUDIO", "invalid audio PTS %lldus; clearing buffer",
                         (long long)pts_us);
            if (audio_buf) vw_audio_buffer_clear(audio_buf);
            live_next_inference_samples = live_progressive_mode ? VW_LIVE_STARTUP_SAMPLES : VW_WINDOW_SAMPLES;
            if (builder) vw_segment_builder_clear(builder);
            if (vad_ctx) vw_vad_reset_state(vad_ctx);
            break;
          }

          if (audio_buf && pcm16 && sample_count > 0) {
            vw_audio_buffer_append_s16le(audio_buf, pcm16, sample_count, pts_us);
            while (vw_audio_buffer_get_count(audio_buf) >= live_next_inference_samples) {
              int64_t window_pts_us = 0;
              size_t buffered_samples = vw_audio_buffer_get_count(audio_buf);
              size_t requested_samples = buffered_samples < VW_WINDOW_SAMPLES ? buffered_samples : VW_WINDOW_SAMPLES;
              size_t read_cnt =
                  vw_audio_buffer_get_samples(audio_buf, window_samples, requested_samples, &window_pts_us);
              bool growing_live_window = live_progressive_mode && read_cnt < VW_WINDOW_SAMPLES;
              int64_t read_duration_us = (int64_t)((read_cnt * 1000000ULL) / VW_AUDIO_SAMPLE_RATE);

              if (read_cnt > 0 && engine) {
                if (vw_vad_detect_speech(window_samples, read_cnt, vad_ctx)) {
                  vw_log_event(VW_LOG_LEVEL_DEBUG, "WORKER_INFERENCE",
                               "PCM speech window %zu samples @%lldus progressive=%d; transcribing", read_cnt,
                               (long long)window_pts_us, live_progressive_mode ? 1 : 0);
                  if (vw_whisper_engine_transcribe_pcm(engine, window_samples, read_cnt)) {
                    if (builder) {
                      int n_segs = vw_whisper_engine_get_segment_count(engine);
                      for (int s_idx = 0; s_idx < n_segs; s_idx++) {
                        vw_whisper_segment_t seg_info;
                        if (vw_whisper_engine_get_segment(engine, s_idx, &seg_info)) {
                          if (seg_info.no_speech_prob >= 0.60f) {
                            continue;
                          }
                          if (growing_live_window && seg_info.t1_us > read_duration_us - VW_LIVE_EDGE_HOLDBACK_US) {
                            vw_log_event(VW_LOG_LEVEL_DEBUG, "WORKER_INFERENCE",
                                         "holding progressive trailing segment end=%lldus frontier=%lldus",
                                         (long long)seg_info.t1_us, (long long)read_duration_us);
                            continue;
                          }
                          int64_t seg_start_pts = vw_saturating_add_i64(window_pts_us, seg_info.t0_us);
                          int64_t seg_end_pts = vw_saturating_add_i64(window_pts_us, seg_info.t1_us);
                          vw_segment_builder_push_hypothesis(builder, seg_info.text_utf8, seg_start_pts, seg_end_pts);
                        }
                      }
                    }
                  } else {
                    vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_INFERENCE", "whisper_full FAILED @%lldus",
                                 (long long)window_pts_us);
                    (void)send_error(handle, session_id.bytes, E_INTERNAL, 0, "Whisper inference failed", &sequence);
                    atomic_store(&fatal_exit, true);
                    atomic_store(&running, false);
                    break;
                  }
                }
              }

              if (live_progressive_mode && read_cnt < VW_WINDOW_SAMPLES) {
                size_t next_target = read_cnt + VW_LIVE_HOP_SAMPLES;
                live_next_inference_samples = next_target < VW_WINDOW_SAMPLES ? next_target : VW_WINDOW_SAMPLES;
                if (atomic_load(&running) && !vw_worker_send_status(handle, session_id.bytes, config, engine, queue,
                                                                    audio_buf, session_active, paused, &sequence)) {
                  atomic_store(&fatal_exit, true);
                  atomic_store(&running, false);
                }
                break;
              }

              if (!atomic_load(&running)) break;
              live_next_inference_samples = VW_WINDOW_SAMPLES;
              size_t drain_samples = live_progressive_mode ? VW_LIVE_HOP_SAMPLES : VW_LOCAL_FALLBACK_HOP_SAMPLES;
              vw_audio_buffer_drain(audio_buf, drain_samples);
              if (!vw_worker_send_status(handle, session_id.bytes, config, engine, queue, audio_buf, session_active,
                                         paused, &sequence)) {
                atomic_store(&fatal_exit, true);
                atomic_store(&running, false);
              }
            }
          }
          break;
        }

        case VW_MSG_PAUSE: {
          if (!session_active) break;
          if (memcmp(payload_decoded.control.session_id.bytes, session_id.bytes, VW_SESSION_ID_BYTES) != 0) {
            vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_SESSION", "PAUSE with wrong session_id; ignoring");
            break;
          }
          paused = true;
          if (audio_buf) vw_audio_buffer_clear(audio_buf);
          live_next_inference_samples = live_progressive_mode ? VW_LIVE_STARTUP_SAMPLES : VW_WINDOW_SAMPLES;
          if (builder) vw_segment_builder_clear(builder);
          if (vad_ctx) vw_vad_reset_state(vad_ctx);
          if (translator) vw_translate_async_invalidate(translator);
          if (!vw_worker_send_status(handle, session_id.bytes, config, engine, queue, audio_buf, session_active, paused,
                                     &sequence)) {
            atomic_store(&fatal_exit, true);
            atomic_store(&running, false);
          }
          vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_SESSION", "paused; window cleared, transcription suspended");
          break;
        }

        case VW_MSG_RESUME: {
          if (!session_active) break;
          if (memcmp(payload_decoded.control.session_id.bytes, session_id.bytes, VW_SESSION_ID_BYTES) != 0) {
            vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_SESSION", "RESUME with wrong session_id; ignoring");
            break;
          }
          paused = false;
          if (source_mode && source_decoder && current_playback_pts_us >= 0) {
            if (!vw_source_decoder_seek(source_decoder, current_playback_pts_us)) {
              vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_SEEK",
                           "RESUME seek to %lldus failed; retaining old anchor %lldus",
                           (long long)current_playback_pts_us, (long long)decoded_pts_us);
            } else {
              decoded_pts_us = current_playback_pts_us;
              last_playback_pts_us = current_playback_pts_us;
              source_eof = false;
              if (audio_buf) vw_audio_buffer_clear(audio_buf);
              live_next_inference_samples = live_progressive_mode ? VW_LIVE_STARTUP_SAMPLES : VW_WINDOW_SAMPLES;
              if (builder) vw_segment_builder_clear(builder);
              if (vad_ctx) vw_vad_reset_state(vad_ctx);
              if (translator) vw_translate_async_invalidate(translator);
            }
          }
          if (!vw_worker_send_status(handle, session_id.bytes, config, engine, queue, audio_buf, session_active, paused,
                                     &sequence)) {
            atomic_store(&fatal_exit, true);
            atomic_store(&running, false);
          }
          vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_SESSION", "resumed; transcription active");
          break;
        }

        case VW_MSG_STOP_SESSION: {
          if (memcmp(payload_decoded.control.session_id.bytes, session_id.bytes, VW_SESSION_ID_BYTES) != 0) {
            if (session_active) {
              vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_SESSION", "STOP with wrong session_id; ignoring");
              break;
            }
            break;
          }
          if (payload_decoded.control.reason == VW_CTRL_REASON_MEDIA_END && !source_mode && audio_buf &&
              vw_audio_buffer_get_count(audio_buf) > 0) {
            bool flushed = vw_worker_flush_audio_tail(audio_buf, window_samples, engine, vad_ctx, builder);
            if (!flushed) {
              vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_INFERENCE", "MEDIA_END tail inference failed");
              (void)send_error(handle, session_id.bytes, E_INTERNAL, 0, "MEDIA_END tail inference failed", &sequence);
              atomic_store(&fatal_exit, true);
              atomic_store(&running, false);
            } else if (!vw_worker_emit_tail_segments(handle, builder, &session_id, &sequence, translator,
                                                     translate_enabled, translate_src_lang, translate_dst_lang)) {
              atomic_store(&fatal_exit, true);
              atomic_store(&running, false);
            }
          }
          if (translator && session_active && payload_decoded.control.reason != VW_CTRL_REASON_SEEK_DISCONTINUITY) {
            vw_worker_drain_translation(translator, handle, &session_id, &session_active, &running, &fatal_exit,
                                        &sequence);
          }
          session_active = false;
          seek_failed_pending = false;
          last_playback_pts_us = -1;
          vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_SESSION", "session stopped (reason=%s)",
                       vw_worker_stop_reason_name(payload_decoded.control.reason));
          if (source_decoder) {
            vw_source_decoder_close(source_decoder);
            source_decoder = NULL;
            source_mode = false;
          }
          if (audio_buf) vw_audio_buffer_clear(audio_buf);
          live_progressive_mode = false;
          live_next_inference_samples = VW_WINDOW_SAMPLES;
          if (builder) vw_segment_builder_clear(builder);
          if (vad_ctx) vw_vad_reset_state(vad_ctx);
          if (translator) vw_translate_async_invalidate(translator);
          if (atomic_load(&running) && !vw_worker_send_status(handle, session_id.bytes, config, engine, queue,
                                                              audio_buf, false, false, &sequence)) {
            atomic_store(&fatal_exit, true);
            atomic_store(&running, false);
          }
          break;
        }

        case VW_MSG_TRANSLATE_CTRL: {
          if (memcmp(payload_decoded.translate_ctrl.session_id.bytes, session_id.bytes, VW_SESSION_ID_BYTES) != 0) {
            vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_TRANSLATE", "TRANSLATE_CTRL with wrong session_id; ignoring");
            break;
          }
          translate_enabled = (payload_decoded.translate_ctrl.enabled != 0);
          snprintf(translate_src_lang, sizeof(translate_src_lang), "%s", payload_decoded.translate_ctrl.source_lang);
          snprintf(translate_dst_lang, sizeof(translate_dst_lang), "%s", payload_decoded.translate_ctrl.target_lang);
          translate_mode = payload_decoded.translate_ctrl.mode;
          if (translator) vw_translate_async_invalidate(translator);
          vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_TRANSLATE", "translate config: enabled=%d from=%s to=%s mode=%d",
                       (int)translate_enabled, translate_src_lang, translate_dst_lang, (int)translate_mode);
          break;
        }

        case VW_MSG_MODEL_CTRL: {
          uint8_t action = payload_decoded.model_ctrl.action;
          const char* req_id = payload_decoded.model_ctrl.model_id;
          vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_MODEL_DL", "received model_ctrl action=%u model='%s'", action,
                       req_id ? req_id : "");
          if (action == VW_MODEL_ACTION_DOWNLOAD) {
            const vw_model_catalog_entry_t* entry = vw_model_catalog_find(req_id);
            const uint8_t* progress_session =
                session_active ? session_id.bytes : payload_decoded.model_ctrl.session_id.bytes;
            if (!entry) {
              if (!vw_worker_send_model_progress(handle, progress_session, VW_MODEL_STAGE_FAILED, 0, 0, 0, req_id,
                                                 &sequence)) {
                atomic_store(&fatal_exit, true);
                atomic_store(&running, false);
              }
            } else if (!dl_dir_ready) {
              vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_MODEL_DL", "cannot download '%s': destination unavailable",
                           req_id);
              if (!vw_worker_send_model_progress(handle, progress_session, VW_MODEL_STAGE_FAILED, 0, 0, 0, req_id,
                                                 &sequence)) {
                atomic_store(&fatal_exit, true);
                atomic_store(&running, false);
              }
            } else {
              bool is_active = false;
              vw_download_progress_t cur;
              if (model_dl && vw_model_download_poll(model_dl, &cur)) {
                if (cur.stage == VW_MODEL_STAGE_DOWNLOADING || cur.stage == VW_MODEL_STAGE_VERIFYING ||
                    cur.stage == VW_MODEL_STAGE_ABORTING) {
                  is_active = true;
                }
              }
              if (is_active) {
                vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_MODEL_DL", "rejecting model '%s': another download is active",
                             req_id);
                if (!vw_worker_send_model_progress(handle, progress_session, VW_MODEL_STAGE_FAILED, 0, 0, 0, req_id,
                                                   &sequence)) {
                  atomic_store(&fatal_exit, true);
                  atomic_store(&running, false);
                }
              } else {
                if (model_dl) {
                  vw_model_download_free(model_dl);
                  model_dl = NULL;
                  last_stage = -1;
                }
                vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_MODEL_DL", "starting download for model '%s' (url=%s dest=%s)",
                             entry->id, entry->url, dl_dir);
                model_dl = vw_model_download_start(entry, dl_dir);
                if (!model_dl) {
                  vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_MODEL_DL", "vw_model_download_start failed for model '%s'",
                               entry->id);
                  if (!vw_worker_send_model_progress(handle, progress_session, VW_MODEL_STAGE_FAILED, 0, 0, 0,
                                                     entry->id, &sequence)) {
                    atomic_store(&fatal_exit, true);
                    atomic_store(&running, false);
                  }
                }
              }
            }
          } else if (action == VW_MODEL_ACTION_ABORT) {
            vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_MODEL_DL", "abort requested for model download");
            if (model_dl) {
              vw_model_download_abort(model_dl);
            } else {
              const uint8_t* progress_session =
                  session_active ? session_id.bytes : payload_decoded.model_ctrl.session_id.bytes;
              if (!vw_worker_send_model_progress(handle, progress_session, VW_MODEL_STAGE_IDLE, 0, 0, 0, "",
                                                 &sequence)) {
                atomic_store(&fatal_exit, true);
                atomic_store(&running, false);
              }
            }
          }
          break;
        }

        case VW_MSG_SHUTDOWN:
          vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_SESSION", "shutdown requested; exiting");
          if (session_active && !source_mode && audio_buf && vw_audio_buffer_get_count(audio_buf) > 0) {
            if (!vw_worker_flush_audio_tail(audio_buf, window_samples, engine, vad_ctx, builder)) {
              (void)send_error(handle, session_id.bytes, E_INTERNAL, 0, "SHUTDOWN tail inference failed", &sequence);
              atomic_store(&fatal_exit, true);
              atomic_store(&running, false);
            } else if (!vw_worker_emit_tail_segments(handle, builder, &session_id, &sequence, translator,
                                                     translate_enabled, translate_src_lang, translate_dst_lang)) {
              atomic_store(&fatal_exit, true);
              atomic_store(&running, false);
            } else if (!vw_worker_send_status(handle, session_id.bytes, config, engine, queue, audio_buf,
                                              session_active, paused, &sequence)) {
              atomic_store(&fatal_exit, true);
              atomic_store(&running, false);
            }
          }
          if (translator && session_active) {
            vw_worker_drain_translation(translator, handle, &session_id, &session_active, &running, &fatal_exit,
                                        &sequence);
          }
          if (source_decoder) {
            vw_source_decoder_close(source_decoder);
            source_decoder = NULL;
            source_mode = false;
          }
          atomic_store(&running, false);
          break;

        default:
          break;
      }

      free(frame.payload);
    }

    if (translator) {
      vw_worker_translation_delivery_t delivery = {.handle = handle,
                                                   .sequence = &sequence,
                                                   .session_id = &session_id,
                                                   .session_active = &session_active,
                                                   .running = &running,
                                                   .fatal_exit = &fatal_exit};
      while (vw_translate_async_try_deliver(translator, vw_worker_deliver_translation, &delivery)) {
      }
    }

    if (model_dl) {
      vw_download_progress_t prog;
      if (vw_model_download_poll(model_dl, &prog)) {
        int64_t now_us = vw_platform_get_monotonic_time_us();
        int prev_stage = last_stage;
        bool should_send = false;
        if ((int)prog.stage != prev_stage) {
          should_send = true;
        } else if ((prog.stage == VW_MODEL_STAGE_DOWNLOADING || prog.stage == VW_MODEL_STAGE_VERIFYING ||
                    prog.stage == VW_MODEL_STAGE_ABORTING) &&
                   now_us - last_progress_send_us >= 1000000) {
          should_send = true;
        }
        if (should_send) {
          const uint8_t zero_session[VW_SESSION_ID_BYTES] = {0};
          const uint8_t* progress_session = session_active ? session_id.bytes : zero_session;
          if (!vw_worker_send_model_progress(handle, progress_session, (uint8_t)prog.stage, (uint8_t)prog.pct,
                                             prog.bytes_done, prog.bytes_total, prog.model_id, &sequence)) {
            atomic_store(&fatal_exit, true);
            atomic_store(&running, false);
          }
          last_stage = (int)prog.stage;
          last_progress_send_us = now_us;
          if (prog.stage == VW_MODEL_STAGE_DONE) {
            vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_MODEL", "model %s downloaded+verified", prog.model_id);
          }
        }
        bool is_done = (prog.stage == VW_MODEL_STAGE_DONE || prog.stage == VW_MODEL_STAGE_FAILED);
        bool is_abort_idle = (prog.stage == VW_MODEL_STAGE_IDLE && prev_stage == VW_MODEL_STAGE_ABORTING);
        bool is_fast_abort_idle = (prog.stage == VW_MODEL_STAGE_IDLE && (prev_stage == VW_MODEL_STAGE_DOWNLOADING ||
                                                                         prev_stage == VW_MODEL_STAGE_VERIFYING));
        if (is_done || is_abort_idle || is_fast_abort_idle) {
          vw_model_download_free(model_dl);
          model_dl = NULL;
          last_stage = -1;
        }
      }
    }

    if (session_active && source_mode && source_decoder && !paused && !source_eof &&
        (decoded_pts_us < vw_saturating_add_i64(current_playback_pts_us, lead_target_us))) {
      int16_t decode_chunk[VW_LOOKAHEAD_CHUNK_SAMPLES];
      int64_t chunk_pts_us = -1;
      size_t samples_read = 0;
      vw_source_decoder_read_status_t read_status = vw_source_decoder_read_s16le(
          source_decoder, decode_chunk, VW_LOOKAHEAD_CHUNK_SAMPLES, &samples_read, &chunk_pts_us);
      if (read_status == VW_SOURCE_DECODER_READ_OK && samples_read > 0 && audio_buf) {
        int64_t actual_pts = (chunk_pts_us >= 0) ? chunk_pts_us : decoded_pts_us;
        vw_audio_buffer_append_s16le(audio_buf, decode_chunk, samples_read, actual_pts);
        decoded_pts_us = actual_pts + (int64_t)((samples_read * 1000000ULL) / 16000ULL);

        while (audio_buf && (vw_audio_buffer_get_count(audio_buf) >= VW_CHUNK_MIN_SAMPLES ||
                             (source_eof && vw_audio_buffer_get_count(audio_buf) > 0))) {
          int64_t boundary_pts_us = 0;
          size_t avail = vw_audio_buffer_get_samples(audio_buf, window_samples, VW_CHUNK_MAX_SAMPLES, &boundary_pts_us);
          if (avail == 0) {
            break;
          }

          size_t cut_samples = 0;
          size_t silence_drain = 0;
          bool evaluated =
              vw_vad_find_chunk_boundary(window_samples, avail, vad_ctx, source_eof, &cut_samples, &silence_drain);
          if (!evaluated) {
            break;
          }

          if (silence_drain > 0) {
            vw_log_event(VW_LOG_LEVEL_DEBUG, "WORKER_VAD", "lookahead silence drain %zu samples @%lldus", silence_drain,
                         (long long)boundary_pts_us);
            vw_audio_buffer_drain(audio_buf, silence_drain);
          } else if (cut_samples > 0) {
            vw_log_event(VW_LOG_LEVEL_DEBUG, "WORKER_INFERENCE",
                         "lookahead speech chunk %zu samples @%lldus; transcribing", cut_samples,
                         (long long)boundary_pts_us);
            if (engine && vw_whisper_engine_transcribe_pcm(engine, window_samples, cut_samples)) {
              if (builder) {
                int n_segs = vw_whisper_engine_get_segment_count(engine);
                for (int s_idx = 0; s_idx < n_segs; s_idx++) {
                  vw_whisper_segment_t seg_info;
                  if (vw_whisper_engine_get_segment(engine, s_idx, &seg_info)) {
                    if (seg_info.no_speech_prob >= 0.60f) {
                      continue;
                    }
                    int64_t seg_start_pts = vw_saturating_add_i64(boundary_pts_us, seg_info.t0_us);
                    int64_t seg_end_pts = vw_saturating_add_i64(boundary_pts_us, seg_info.t1_us);
                    vw_segment_builder_push_hypothesis(builder, seg_info.text_utf8, seg_start_pts, seg_end_pts);
                  }
                }
              }
            } else {
              (void)send_error(handle, session_id.bytes, E_INTERNAL, 0, "Whisper inference failed", &sequence);
              atomic_store(&fatal_exit, true);
              atomic_store(&running, false);
              break;
            }
            if (!atomic_load(&running)) break;
            vw_audio_buffer_drain(audio_buf, cut_samples);
            if (!vw_worker_send_status(handle, session_id.bytes, config, engine, queue, audio_buf, session_active,
                                       paused, &sequence)) {
              atomic_store(&fatal_exit, true);
              atomic_store(&running, false);
            }
          } else {
            break;
          }
        }
      } else if (read_status == VW_SOURCE_DECODER_READ_EOF) {
        source_eof = true;
        if (!vw_worker_flush_audio_tail(audio_buf, window_samples, engine, vad_ctx, builder)) {
          vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_INFERENCE", "source EOF tail inference failed");
          (void)send_error(handle, session_id.bytes, E_INTERNAL, 0, "Source tail inference failed", &sequence);
          atomic_store(&fatal_exit, true);
          atomic_store(&running, false);
          break;
        }
        if (!vw_worker_send_status(handle, session_id.bytes, config, engine, queue, audio_buf, session_active, paused,
                                   &sequence)) {
          atomic_store(&fatal_exit, true);
          atomic_store(&running, false);
        }
      } else if (read_status == VW_SOURCE_DECODER_READ_ERROR) {
        if (!send_error(handle, session_id.bytes, E_INTERNAL, 0, "Source decoder read failed", &sequence)) {
          atomic_store(&fatal_exit, true);
          atomic_store(&running, false);
        }
        session_active = false;
        source_eof = true;
        vw_source_decoder_close(source_decoder);
        source_decoder = NULL;
        source_mode = false;
        if (audio_buf) vw_audio_buffer_clear(audio_buf);
        if (builder) vw_segment_builder_clear(builder);
        if (translator) vw_translate_async_invalidate(translator);
      } else {
        vw_platform_sleep_ms(5);
      }
    }

    if (builder) {
      vw_caption_segment_t seg;
      while (vw_segment_builder_pop(builder, &seg)) {
        memcpy(seg.session_id.bytes, session_id.bytes, VW_SESSION_ID_BYTES);
        seg.translated_text_utf8 = NULL;
        seg.translated_text_bytes = 0;
        seg.translation_attempted = false;
        seg.translation_tier = VW_TRANSLATE_TIER_NONE;
        seg.translation_latency_us = 0;

        if (translate_enabled && translator && seg.text_utf8 && seg.text_utf8[0]) {
          if (vw_translate_async_submit(translator, &seg, translate_src_lang, translate_dst_lang)) {
            vw_log_event(VW_LOG_LEVEL_DEBUG, "WORKER_TRANSLATE", "queued segment=%llu source_bytes=%u",
                         (unsigned long long)seg.segment_id, (unsigned int)seg.text_bytes);
          } else {
            vw_log_event(VW_LOG_LEVEL_WARN, "WORKER_TRANSLATE",
                         "translation pipeline saturated/unavailable; emitting source segment=%llu",
                         (unsigned long long)seg.segment_id);
            seg.translated_text_utf8 = NULL;
            seg.translated_text_bytes = 0;
            seg.translation_attempted = true;
            seg.translation_tier = VW_TRANSLATE_TIER_NONE;
            seg.translation_latency_us = 0;
            if (vw_worker_send_caption_segment(handle, &seg, &sequence)) {
              vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_SEGMENT",
                           "emitted source segment id=%llu start=%lld end=%lld (translation saturated)",
                           (unsigned long long)seg.segment_id, (long long)seg.start_pts_us, (long long)seg.end_pts_us);
            } else {
              atomic_store(&fatal_exit, true);
              atomic_store(&running, false);
            }
          }
        } else {
          if (vw_worker_send_caption_segment(handle, &seg, &sequence)) {
            vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_SEGMENT",
                         "emitted segment id=%llu start=%lld end=%lld is_final=%d text_len=%zu trans_len=0",
                         (unsigned long long)seg.segment_id, (long long)seg.start_pts_us, (long long)seg.end_pts_us,
                         seg.is_final, seg.text_utf8 ? strlen(seg.text_utf8) : 0);
          } else {
            atomic_store(&fatal_exit, true);
            atomic_store(&running, false);
          }
        }
        if (seg.text_utf8) free(seg.text_utf8);
      }
    }

    if (translator) {
      vw_worker_translation_delivery_t delivery = {.handle = handle,
                                                   .sequence = &sequence,
                                                   .session_id = &session_id,
                                                   .session_active = &session_active,
                                                   .running = &running,
                                                   .fatal_exit = &fatal_exit};
      while (vw_translate_async_try_deliver(translator, vw_worker_deliver_translation, &delivery)) {
      }
    }
  }

  atomic_store(&running, false);
  vw_platform_thread_join(reader_thread);
  if (translator) {
    vw_translate_async_destroy(translator);
    translator = NULL;
  }
  const uint64_t dropped_audio_us = vw_worker_queue_get_dropped_audio_us(queue);
  vw_worker_queue_destroy(queue);

  if (source_decoder) {
    vw_source_decoder_close(source_decoder);
    source_decoder = NULL;
  }

  if (model_dl) {
    vw_model_download_abort(model_dl);
    vw_model_download_free(model_dl);
    model_dl = NULL;
  }

  free(window_samples);
  if (audio_buf) vw_audio_buffer_free(audio_buf);
  if (builder) vw_segment_builder_free(builder);
  if (engine) vw_whisper_engine_free(engine);
  if (vad_ctx) vw_vad_free(vad_ctx);

  vw_ipc_close(handle);
#ifdef _WIN32
  MFShutdown();
#endif
  int exit_code = (atomic_load(&fatal_exit) || !authenticated) ? 1 : 0;
  vw_log_event(VW_LOG_LEVEL_INFO, "WORKER_LIFECYCLE", "worker exiting (rc=%d, dropped_audio_us=%llu)", exit_code,
               (unsigned long long)dropped_audio_us);
  return exit_code;
}
