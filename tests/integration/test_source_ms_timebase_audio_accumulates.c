// Regression: containers with a coarse millisecond time base (Matroska/WebM) round every AAC frame PTS by up to half a
// tick. The source decoder used to re-anchor its output timeline to each rounded frame PTS, so consecutive 2 s
// look-ahead chunks disagreed with the worker audio buffer's sample-accurate expectation by hundreds of microseconds.
// The buffer treated every chunk as a discontinuity and cleared itself, never reaching the VAD minimum window, so MKV
// media produced captions only for the EOF tail. Contiguous source audio must accumulate; seeks must still re-anchor.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vw_audio_buffer.h"
#include "vw_source_decoder.h"
#include "vw_test.h"
#include "vw_worker.h"

#ifndef VW_TEST_FIXTURE_DIR
#error "VW_TEST_FIXTURE_DIR must point at tests/fixtures"
#endif

#define VW_TEST_MKV_FIXTURE VW_TEST_FIXTURE_DIR "/aac48k_mono_8s.mkv"

static int16_t chunk[VW_LOOKAHEAD_CHUNK_SAMPLES];

static void contiguous_source_audio_accumulates_in_lookahead_buffer(void) {
  vw_source_decoder_info_t info = {0};
  vw_source_decoder_t* decoder = vw_source_decoder_open(VW_TEST_MKV_FIXTURE, &info);
  vw_test_check_true("millisecond-timebase fixture opens", decoder != NULL);
  if (!decoder) return;

  vw_audio_buffer_t* buffer = vw_audio_buffer_create(VW_LOOKAHEAD_BUFFER_SAMPLES);
  vw_test_check_true("look-ahead buffer allocates", buffer != NULL);

  size_t total = 0;
  size_t chunks = 0;
  bool accumulated = true;
  bool monotonic = true;
  int64_t previous_pts = INT64_MIN;
  vw_source_decoder_read_status_t status = VW_SOURCE_DECODER_READ_OK;
  while (buffer) {
    size_t n = 0;
    int64_t pts = -1;
    status = vw_source_decoder_read_s16le(decoder, chunk, VW_LOOKAHEAD_CHUNK_SAMPLES, &n, &pts);
    if (status == VW_SOURCE_DECODER_READ_AGAIN) continue;
    if (status != VW_SOURCE_DECODER_READ_OK || n == 0) break;
    if (pts <= previous_pts) monotonic = false;
    previous_pts = pts;
    vw_audio_buffer_append_s16le(buffer, chunk, n, pts);
    total += n;
    chunks++;
    if (vw_audio_buffer_get_count(buffer) != total) {
      if (accumulated) {
        fprintf(stderr, "  buffer reset at chunk %zu (pts=%lldus): holds %zu of %zu decoded samples\n", chunks,
                (long long)pts, vw_audio_buffer_get_count(buffer), total);
      }
      accumulated = false;
    }
  }

  vw_test_check_true("source decode reaches EOF cleanly", status == VW_SOURCE_DECODER_READ_EOF);
  vw_test_check_true("fixture yields several look-ahead chunks", chunks >= 4);
  vw_test_check_true("chunk timestamps strictly increase", monotonic);
  vw_test_check_true("contiguous source audio is never mistaken for a discontinuity", accumulated);
  vw_test_check_true("look-ahead buffer reaches the VAD minimum window before EOF",
                     total >= VW_CHUNK_MIN_SAMPLES && accumulated);

  vw_audio_buffer_free(buffer);
  vw_source_decoder_close(decoder);
}

static void seek_reanchors_timeline_to_target(void) {
  vw_source_decoder_t* decoder = vw_source_decoder_open(VW_TEST_MKV_FIXTURE, NULL);
  vw_test_check_true("fixture reopens for seek", decoder != NULL);
  if (!decoder) return;

  size_t n = 0;
  int64_t pts = -1;
  (void)vw_source_decoder_read_s16le(decoder, chunk, VW_LOOKAHEAD_CHUNK_SAMPLES, &n, &pts);
  vw_test_check_true("seek to 4 s succeeds", vw_source_decoder_seek(decoder, 4000000));
  vw_source_decoder_read_status_t status;
  do {
    status = vw_source_decoder_read_s16le(decoder, chunk, VW_LOOKAHEAD_CHUNK_SAMPLES, &n, &pts);
  } while (status == VW_SOURCE_DECODER_READ_AGAIN);
  vw_test_check_true("first post-seek read returns audio", status == VW_SOURCE_DECODER_READ_OK && n > 0);
  int64_t error_us = pts - 4000000;
  if (error_us < 0) error_us = -error_us;
  vw_test_check_true("post-seek timeline starts at the seek target (within 2 ms)", error_us <= 2000);
  vw_source_decoder_close(decoder);
}

int main(void) {
  contiguous_source_audio_accumulates_in_lookahead_buffer();
  seek_reanchors_timeline_to_target();
  return vw_test_finish("source_ms_timebase_audio_accumulates");
}
