#ifndef _WIN32

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vw_log.h"
#include "vw_source_decoder.h"

#ifdef VW_WITH_FFMPEG

// Containers with a coarse time base (Matroska/WebM: 1 ms) round every frame PTS by up to half a tick, so frame PTS
// jitters around the true sample position by hundreds of microseconds. The worker's audio buffer treats any mismatch
// above one 16 kHz output sample (62.5 us) as a discontinuity and clears itself. Output timestamps are therefore
// derived from one anchor plus an exact output-sample count; a frame PTS re-anchors the timeline only when it disagrees
// by more than the container can explain by rounding (two ticks, floor 1 ms), which preserves real gaps and seeks.
#define VW_SOURCE_DECODER_MIN_REANCHOR_US 1000LL

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

struct vw_source_decoder {
  AVFormatContext* fmt_ctx;
  AVCodecContext* codec_ctx;
  SwrContext* swr_ctx;
  AVPacket* pkt;
  AVFrame* frame;
  int audio_stream_idx;
  int64_t duration_us;
  int64_t current_pts_us;
  int64_t seek_target_us;
  int64_t stream_start_time;      // Container start_time in stream TB; media-relative normalization base.
  bool pts_anchored;              // timeline anchored to a decoded frame since open/seek
  int64_t anchor_pts_us;          // media-relative PTS of the anchor output sample
  uint64_t anchor_samples;        // 16 kHz output samples emitted (or trimmed) since the anchor
  int64_t reanchor_tolerance_us;  // max frame-PTS disagreement attributable to container rounding
  bool demux_eof;
  bool flush_sent;
  bool eof_reached;
  bool pkt_pending;
  int16_t leftover_buffer[4096];
  size_t leftover_count;
};

// Resets the output timeline so the next output sample sits at pts_us.
static void vw_source_decoder_anchor(vw_source_decoder_t* decoder, int64_t pts_us) {
  decoder->anchor_pts_us = pts_us;
  decoder->anchor_samples = 0;
  decoder->current_pts_us = pts_us;
}

// Advances the timeline by whole 16 kHz output samples (62.5 us each) without accumulating rounding error.
static void vw_source_decoder_advance(vw_source_decoder_t* decoder, size_t samples) {
  decoder->anchor_samples += samples;
  decoder->current_pts_us = decoder->anchor_pts_us + (int64_t)((decoder->anchor_samples * 125U) / 2U);
}

// Returns the PTS of the output sample `samples` positions before the current timeline position.
static int64_t vw_source_decoder_pts_before(const vw_source_decoder_t* decoder, size_t samples) {
  if (decoder->anchor_samples >= samples)
    return decoder->anchor_pts_us + (int64_t)(((decoder->anchor_samples - samples) * 125U) / 2U);
  return decoder->current_pts_us - (int64_t)((samples * 125U) / 2U);
}

static const char* vw_source_decoder_normalize_posix_path(const char* url, char* buf, size_t buf_len) {
  if (!url || !buf || buf_len == 0) return NULL;
  const char* src = url;
  if (strncmp(src, "file://localhost/", 17) == 0) {
    src += 16;
  } else if (strncmp(src, "file://", 7) == 0) {
    src += 7;
  }
  size_t idx = 0;
  while (*src && idx + 1 < buf_len) {
    if (*src == '%' && src[1] && src[2]) {
      char hex[3] = {src[1], src[2], '\0'};
      char* end = NULL;
      long val = strtol(hex, &end, 16);
      if (end == hex + 2) {
        if (val == 0) return NULL;
        buf[idx++] = (char)val;
        src += 3;
        continue;
      }
    }
    buf[idx++] = *src++;
  }
  buf[idx] = '\0';
  if (*src) return NULL;
  return buf;
}

vw_source_decoder_t* vw_source_decoder_open(const char* url, vw_source_decoder_info_t* info) {
  if (!url || url[0] == '\0') return NULL;

  char clean_path[4096];
  if (!vw_source_decoder_normalize_posix_path(url, clean_path, sizeof(clean_path))) return NULL;

  AVFormatContext* fmt_ctx = NULL;
  if (avformat_open_input(&fmt_ctx, clean_path, NULL, NULL) < 0) return NULL;
  if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
    avformat_close_input(&fmt_ctx);
    return NULL;
  }

  int audio_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (audio_idx < 0) {
    avformat_close_input(&fmt_ctx);
    return NULL;
  }

  AVStream* stream = fmt_ctx->streams[audio_idx];
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) {
    avformat_close_input(&fmt_ctx);
    return NULL;
  }

  AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
  if (!codec_ctx) {
    avformat_close_input(&fmt_ctx);
    return NULL;
  }
  if (avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0 || avcodec_open2(codec_ctx, codec, NULL) < 0) {
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return NULL;
  }

  SwrContext* swr_ctx = NULL;
  AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_MONO;
  if (swr_alloc_set_opts2(&swr_ctx, &out_ch_layout, AV_SAMPLE_FMT_S16, 16000, &codec_ctx->ch_layout,
                          codec_ctx->sample_fmt, codec_ctx->sample_rate, 0, NULL) < 0 ||
      !swr_ctx || swr_init(swr_ctx) < 0) {
    if (swr_ctx) swr_free(&swr_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return NULL;
  }

  vw_source_decoder_t* dec = (vw_source_decoder_t*)calloc(1, sizeof(vw_source_decoder_t));
  if (!dec) {
    swr_free(&swr_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return NULL;
  }

  dec->fmt_ctx = fmt_ctx;
  dec->codec_ctx = codec_ctx;
  dec->swr_ctx = swr_ctx;
  dec->audio_stream_idx = audio_idx;
  // Normalize to a media-relative timeline: containers may begin at a nonzero
  // start_time (e.g. MPEG-TS), while VLC positions are media-relative from 0.
  dec->stream_start_time = (stream->start_time == AV_NOPTS_VALUE) ? 0 : stream->start_time;
  dec->pkt = av_packet_alloc();
  dec->frame = av_frame_alloc();
  if (!dec->pkt || !dec->frame) {
    if (dec->pkt) av_packet_free(&dec->pkt);
    if (dec->frame) av_frame_free(&dec->frame);
    swr_free(&swr_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    free(dec);
    return NULL;
  }
  dec->duration_us = fmt_ctx->duration > 0 ? (int64_t)av_rescale(fmt_ctx->duration, 1000000, AV_TIME_BASE) : -1;
  int64_t tick_us = av_rescale_q(1, stream->time_base, (AVRational){1, 1000000});
  dec->reanchor_tolerance_us =
      2 * tick_us > VW_SOURCE_DECODER_MIN_REANCHOR_US ? 2 * tick_us : VW_SOURCE_DECODER_MIN_REANCHOR_US;

  if (info) {
    info->duration_us = dec->duration_us;
    info->channels = 1;
    info->sample_rate = 16000;
    snprintf(info->container_format, sizeof(info->container_format), "%s",
             fmt_ctx->iformat ? fmt_ctx->iformat->name : "ffmpeg");
  }
  dec->seek_target_us = INT64_MIN;
  return dec;
}
bool vw_source_decoder_seek(vw_source_decoder_t* decoder, int64_t target_pts_us) {
  if (!decoder || !decoder->fmt_ctx || decoder->audio_stream_idx < 0) return false;

  AVStream* stream = decoder->fmt_ctx->streams[decoder->audio_stream_idx];
  int64_t target_ts = av_rescale_q(target_pts_us, (AVRational){1, 1000000}, stream->time_base);
  // Media-relative position -> raw stream timestamp: add the container offset
  // so seeks land on the requested media time even when start_time != 0.
  target_ts += decoder->stream_start_time;

  // Prepare replacement resampling state before mutating the demuxer; allocation/init failure leaves reads usable.
  SwrContext* replacement = NULL;
  AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_MONO;
  if (swr_alloc_set_opts2(&replacement, &out_layout, AV_SAMPLE_FMT_S16, 16000, &decoder->codec_ctx->ch_layout,
                          decoder->codec_ctx->sample_fmt, decoder->codec_ctx->sample_rate, 0, NULL) < 0 ||
      !replacement || swr_init(replacement) < 0) {
    swr_free(&replacement);
    return false;
  }

  if (av_seek_frame(decoder->fmt_ctx, decoder->audio_stream_idx, target_ts, AVSEEK_FLAG_BACKWARD) >= 0) {
    avcodec_flush_buffers(decoder->codec_ctx);
    swr_free(&decoder->swr_ctx);
    decoder->swr_ctx = replacement;
    vw_source_decoder_anchor(decoder, target_pts_us);
    decoder->seek_target_us = target_pts_us;
    decoder->pts_anchored = false;  // the first decoded frame after the seek re-anchors the timeline
    decoder->demux_eof = false;
    decoder->flush_sent = false;
    decoder->eof_reached = false;
    decoder->leftover_count = 0;
    if (decoder->pkt_pending) {
      av_packet_unref(decoder->pkt);
      decoder->pkt_pending = false;
    }
    return true;
  }
  swr_free(&replacement);
  return false;
}

static void vw_source_decoder_copy_samples(vw_source_decoder_t* decoder, const int16_t* samples, size_t count,
                                           int16_t* out_pcm, size_t max_samples, size_t* inout_total_samples) {
  if (decoder->seek_target_us != INT64_MIN && decoder->current_pts_us < decoder->seek_target_us) {
    int64_t delta = decoder->seek_target_us - decoder->current_pts_us;
    if (delta > 0) {
      uint64_t udelta = (uint64_t)delta;
      uint64_t skip = udelta / 125U * 2U + ((udelta % 125U) * 2U + 124U) / 125U;
      size_t trimmed = skip < count ? (size_t)skip : count;
      samples += trimmed;
      count -= trimmed;
      vw_source_decoder_advance(decoder, trimmed);
    }
    if (decoder->current_pts_us >= decoder->seek_target_us) {
      decoder->seek_target_us = INT64_MIN;
    }
  }
  size_t needed = max_samples - *inout_total_samples;
  size_t to_copy = count < needed ? count : needed;
  if (to_copy > 0) {
    memcpy(out_pcm + *inout_total_samples, samples, to_copy * sizeof(int16_t));
    *inout_total_samples += to_copy;
    vw_source_decoder_advance(decoder, to_copy);
  }
  size_t remainder = count - to_copy;
  if (remainder > 0) {
    size_t cap = sizeof(decoder->leftover_buffer) / sizeof(int16_t);
    if (remainder > cap) remainder = cap;
    memcpy(decoder->leftover_buffer, samples + to_copy, remainder * sizeof(int16_t));
    decoder->leftover_count = remainder;
  }
}

static bool vw_source_decoder_process_frame(vw_source_decoder_t* decoder, int16_t* out_pcm, size_t max_samples,
                                            size_t* inout_total_samples, int64_t* out_pts_us) {
  AVStream* stream = decoder->fmt_ctx->streams[decoder->audio_stream_idx];
  int64_t frame_pts = decoder->frame->pts;
  if (frame_pts == AV_NOPTS_VALUE) frame_pts = decoder->frame->best_effort_timestamp;
  if (frame_pts == AV_NOPTS_VALUE) frame_pts = decoder->frame->pkt_dts;
  if (frame_pts != AV_NOPTS_VALUE && stream) {
    // Raw stream timestamp -> media-relative: strip the container offset so
    // caption scheduling aligns with VLC's media timeline (mirror of seek).
    frame_pts -= decoder->stream_start_time;
    int64_t frame_pts_us = (int64_t)av_rescale_q(frame_pts, stream->time_base, (AVRational){1, 1000000});
    frame_pts_us -= swr_get_delay(decoder->swr_ctx, 1000000);
    int64_t deviation_us = frame_pts_us - decoder->current_pts_us;
    if (!decoder->pts_anchored || deviation_us > decoder->reanchor_tolerance_us ||
        deviation_us < -decoder->reanchor_tolerance_us) {
      if (decoder->pts_anchored) {
        vw_log_event(VW_LOG_LEVEL_DEBUG, "DECODER_FFMPEG_PTS", "timeline discontinuity %lldus -> %lldus (delta %lldus)",
                     (long long)decoder->current_pts_us, (long long)frame_pts_us, (long long)deviation_us);
      }
      vw_source_decoder_anchor(decoder, frame_pts_us);
      decoder->pts_anchored = true;
    }
  }

  int16_t resample_buf[4096];
  uint8_t* out_ptrs[1] = {(uint8_t*)resample_buf};
  int converted = swr_convert(decoder->swr_ctx, out_ptrs, (int)(sizeof(resample_buf) / sizeof(int16_t)),
                              (const uint8_t**)decoder->frame->extended_data, decoder->frame->nb_samples);
  if (converted < 0) {
    vw_log_event(VW_LOG_LEVEL_ERROR, "DECODER_FFMPEG_RESAMPLE", "swr_convert failed (%d)", converted);
    return false;
  }
  if (converted > 0) {
    size_t before = *inout_total_samples;
    vw_source_decoder_copy_samples(decoder, resample_buf, (size_t)converted, out_pcm, max_samples, inout_total_samples);
    if (out_pts_us && before == 0 && *inout_total_samples > 0)
      *out_pts_us = vw_source_decoder_pts_before(decoder, *inout_total_samples);
  }
  return true;
}

static vw_source_decoder_read_status_t vw_source_decoder_drain_swr(vw_source_decoder_t* decoder, int16_t* out_pcm,
                                                                   size_t max_samples, size_t* inout_total_samples,
                                                                   int64_t* out_pts_us) {
  int16_t resample_buf[4096];
  uint8_t* out_ptrs[1] = {(uint8_t*)resample_buf};
  int converted = swr_convert(decoder->swr_ctx, out_ptrs, (int)(sizeof(resample_buf) / sizeof(int16_t)), NULL, 0);
  if (converted < 0) {
    vw_log_event(VW_LOG_LEVEL_ERROR, "DECODER_FFMPEG_RESAMPLE", "swr_convert drain failed (%d)", converted);
    return VW_SOURCE_DECODER_READ_ERROR;
  }
  if (converted == 0) return VW_SOURCE_DECODER_READ_EOF;
  size_t before = *inout_total_samples;
  vw_source_decoder_copy_samples(decoder, resample_buf, (size_t)converted, out_pcm, max_samples, inout_total_samples);
  if (out_pts_us && before == 0 && *inout_total_samples > 0)
    *out_pts_us = vw_source_decoder_pts_before(decoder, *inout_total_samples);
  return VW_SOURCE_DECODER_READ_OK;
}

vw_source_decoder_read_status_t vw_source_decoder_read_s16le(vw_source_decoder_t* decoder, int16_t* out_pcm,
                                                             size_t max_samples, size_t* out_sample_count,
                                                             int64_t* out_pts_us) {
  if (out_sample_count) *out_sample_count = 0;
  if (!decoder || !decoder->fmt_ctx || !out_pcm || max_samples == 0 || !out_sample_count)
    return VW_SOURCE_DECODER_READ_ERROR;
  if (decoder->eof_reached) return VW_SOURCE_DECODER_READ_EOF;

  size_t total_samples = 0;
  if (decoder->leftover_count > 0) {
    size_t to_copy = decoder->leftover_count < max_samples ? decoder->leftover_count : max_samples;
    memcpy(out_pcm, decoder->leftover_buffer, to_copy * sizeof(int16_t));
    if (out_pts_us) *out_pts_us = decoder->current_pts_us;
    total_samples += to_copy;
    vw_source_decoder_advance(decoder, to_copy);
    if (to_copy < decoder->leftover_count) {
      memmove(decoder->leftover_buffer, decoder->leftover_buffer + to_copy,
              (decoder->leftover_count - to_copy) * sizeof(int16_t));
      decoder->leftover_count -= to_copy;
      *out_sample_count = total_samples;
      return VW_SOURCE_DECODER_READ_OK;
    }
    decoder->leftover_count = 0;
  }

  int defer_no_progress = 0;
  while (total_samples < max_samples && !decoder->eof_reached) {
    if (decoder->demux_eof) {
      if (!decoder->flush_sent) {
        int flush_ret = avcodec_send_packet(decoder->codec_ctx, NULL);
        if (flush_ret >= 0 || flush_ret == AVERROR_EOF) {
          decoder->flush_sent = true;
        } else if (flush_ret != AVERROR(EAGAIN)) {
          vw_log_event(VW_LOG_LEVEL_WARN, "DECODER_FFMPEG_FLUSH", "avcodec_send_packet(NULL) failed (%d)", flush_ret);
          return VW_SOURCE_DECODER_READ_ERROR;
        }
      }

      int recv_ret = avcodec_receive_frame(decoder->codec_ctx, decoder->frame);
      if (recv_ret >= 0) {
        if (!vw_source_decoder_process_frame(decoder, out_pcm, max_samples, &total_samples, out_pts_us))
          return VW_SOURCE_DECODER_READ_ERROR;
        continue;
      }
      if (recv_ret == AVERROR_EOF) {
        vw_source_decoder_read_status_t drain_status =
            vw_source_decoder_drain_swr(decoder, out_pcm, max_samples, &total_samples, out_pts_us);
        if (drain_status == VW_SOURCE_DECODER_READ_ERROR) return drain_status;
        if (drain_status == VW_SOURCE_DECODER_READ_EOF && decoder->leftover_count == 0) decoder->eof_reached = true;
        continue;
      }
      if (recv_ret == AVERROR(EAGAIN)) {
        if (!decoder->flush_sent) continue;
        break;
      }
      vw_log_event(VW_LOG_LEVEL_WARN, "DECODER_FFMPEG_RECV", "avcodec_receive_frame failed (%d)", recv_ret);
      return VW_SOURCE_DECODER_READ_ERROR;
    }

    if (!decoder->pkt_pending) {
      int ret = av_read_frame(decoder->fmt_ctx, decoder->pkt);
      if (ret == AVERROR_EOF) {
        decoder->demux_eof = true;
        continue;
      }
      if (ret == AVERROR(EAGAIN)) {
        *out_sample_count = total_samples;
        return total_samples > 0 ? VW_SOURCE_DECODER_READ_OK : VW_SOURCE_DECODER_READ_AGAIN;
      }
      if (ret < 0) {
        vw_log_event(VW_LOG_LEVEL_WARN, "DECODER_FFMPEG_READ", "av_read_frame failed (%d)", ret);
        return VW_SOURCE_DECODER_READ_ERROR;
      }
      decoder->pkt_pending = true;
    }

    if (decoder->pkt->stream_index == decoder->audio_stream_idx) {
      size_t progress_total = total_samples;
      int progress_leftover = (int)decoder->leftover_count;
      int send_ret = avcodec_send_packet(decoder->codec_ctx, decoder->pkt);
      if (send_ret == AVERROR(EAGAIN)) {
        int recv_ret = 0;
        while (total_samples < max_samples && decoder->leftover_count == 0 &&
               (recv_ret = avcodec_receive_frame(decoder->codec_ctx, decoder->frame)) >= 0) {
          if (!vw_source_decoder_process_frame(decoder, out_pcm, max_samples, &total_samples, out_pts_us))
            return VW_SOURCE_DECODER_READ_ERROR;
        }
        if (recv_ret < 0 && recv_ret != AVERROR(EAGAIN) && recv_ret != AVERROR_EOF) {
          vw_log_event(VW_LOG_LEVEL_WARN, "DECODER_FFMPEG_RECV", "avcodec_receive_frame failed (%d)", recv_ret);
          return VW_SOURCE_DECODER_READ_ERROR;
        }
        if (total_samples < max_samples && decoder->leftover_count == 0)
          send_ret = avcodec_send_packet(decoder->codec_ctx, decoder->pkt);
      }

      if (send_ret >= 0) {
        int recv_ret = 0;
        while (total_samples < max_samples && decoder->leftover_count == 0 &&
               (recv_ret = avcodec_receive_frame(decoder->codec_ctx, decoder->frame)) >= 0) {
          if (!vw_source_decoder_process_frame(decoder, out_pcm, max_samples, &total_samples, out_pts_us))
            return VW_SOURCE_DECODER_READ_ERROR;
        }
        if (recv_ret < 0 && recv_ret != AVERROR(EAGAIN) && recv_ret != AVERROR_EOF) {
          vw_log_event(VW_LOG_LEVEL_WARN, "DECODER_FFMPEG_RECV", "avcodec_receive_frame failed (%d)", recv_ret);
          return VW_SOURCE_DECODER_READ_ERROR;
        }
        av_packet_unref(decoder->pkt);
        decoder->pkt_pending = false;
        defer_no_progress = 0;
      } else if (send_ret == AVERROR(EAGAIN)) {
        if (total_samples == progress_total && (int)decoder->leftover_count == progress_leftover)
          defer_no_progress++;
        else
          defer_no_progress = 0;
        if (defer_no_progress >= 2) break;
        decoder->pkt_pending = true;
      } else if (send_ret == AVERROR_EOF) {
        av_packet_unref(decoder->pkt);
        decoder->pkt_pending = false;
        decoder->demux_eof = true;
      } else {
        vw_log_event(VW_LOG_LEVEL_WARN, "DECODER_FFMPEG_SEND", "avcodec_send_packet failed (%d)", send_ret);
        av_packet_unref(decoder->pkt);
        decoder->pkt_pending = false;
        return VW_SOURCE_DECODER_READ_ERROR;
      }
    } else {
      av_packet_unref(decoder->pkt);
      decoder->pkt_pending = false;
    }
  }

  *out_sample_count = total_samples;
  if (total_samples > 0) return VW_SOURCE_DECODER_READ_OK;
  return decoder->eof_reached ? VW_SOURCE_DECODER_READ_EOF : VW_SOURCE_DECODER_READ_AGAIN;
}

int64_t vw_source_decoder_get_duration_us(const vw_source_decoder_t* decoder) {
  return decoder ? decoder->duration_us : -1;
}

void vw_source_decoder_close(vw_source_decoder_t* decoder) {
  if (decoder) {
    if (decoder->pkt) av_packet_free(&decoder->pkt);
    if (decoder->frame) av_frame_free(&decoder->frame);
    if (decoder->swr_ctx) swr_free(&decoder->swr_ctx);
    if (decoder->codec_ctx) avcodec_free_context(&decoder->codec_ctx);
    if (decoder->fmt_ctx) avformat_close_input(&decoder->fmt_ctx);
    free(decoder);
  }
}

#else  // !VW_WITH_FFMPEG

struct vw_source_decoder {
  int dummy;
};

vw_source_decoder_t* vw_source_decoder_open(const char* url, vw_source_decoder_info_t* info) {
  (void)url;
  (void)info;
  return NULL;
}

bool vw_source_decoder_seek(vw_source_decoder_t* decoder, int64_t target_pts_us) {
  (void)decoder;
  (void)target_pts_us;
  return false;
}

vw_source_decoder_read_status_t vw_source_decoder_read_s16le(vw_source_decoder_t* decoder, int16_t* out_pcm,
                                                             size_t max_samples, size_t* out_sample_count,
                                                             int64_t* out_pts_us) {
  (void)decoder;
  (void)out_pcm;
  (void)max_samples;
  (void)out_pts_us;
  if (out_sample_count) *out_sample_count = 0;
  return VW_SOURCE_DECODER_READ_ERROR;
}

int64_t vw_source_decoder_get_duration_us(const vw_source_decoder_t* decoder) {
  (void)decoder;
  return -1;
}

void vw_source_decoder_close(vw_source_decoder_t* decoder) {
  if (decoder) free(decoder);
}

#endif  // VW_WITH_FFMPEG

#endif  // !_WIN32
