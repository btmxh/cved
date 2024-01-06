#include "al_util.h"
#include "../media/decode_thread.h"
#include <assert.h>
#include <log.h>
#include <threads.h>

static void find_suitable_al_format(const AVCodecContext *cp, i32 *frame_size,
                                    i32 *dst_chan_layout,
                                    enum AVSampleFormat *dst_sample_format,
                                    ALenum *format) {
  bool EnableUhj = false; // TODO: enable this?
  *frame_size = 0;
  *dst_chan_layout = 0;
  *format = AL_NONE;
  if ((cp->sample_fmt == AV_SAMPLE_FMT_FLT ||
       cp->sample_fmt == AV_SAMPLE_FMT_FLTP ||
       cp->sample_fmt == AV_SAMPLE_FMT_DBL ||
       cp->sample_fmt == AV_SAMPLE_FMT_DBLP ||
       cp->sample_fmt == AV_SAMPLE_FMT_S32 ||
       cp->sample_fmt == AV_SAMPLE_FMT_S32P ||
       cp->sample_fmt == AV_SAMPLE_FMT_S64 ||
       cp->sample_fmt == AV_SAMPLE_FMT_S64P) &&
      alIsExtensionPresent("AL_EXT_FLOAT32")) {
    *dst_sample_format = AV_SAMPLE_FMT_FLT;
    *frame_size = 4;
    if (cp->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) {
      if (alIsExtensionPresent("AL_EXT_MCFORMATS")) {
        if (cp->ch_layout.u.mask == AV_CH_LAYOUT_7POINT1) {
          *dst_chan_layout = cp->ch_layout.u.mask;
          *frame_size *= 8;
          *format = alGetEnumValue("AL_FORMAT_71CHN32");
        }
        if (cp->ch_layout.u.mask == AV_CH_LAYOUT_5POINT1 ||
            cp->ch_layout.u.mask == AV_CH_LAYOUT_5POINT1_BACK) {
          *dst_chan_layout = cp->ch_layout.u.mask;
          *frame_size *= 6;
          *format = alGetEnumValue("AL_FORMAT_51CHN32");
        }
        if (cp->ch_layout.u.mask == AV_CH_LAYOUT_QUAD) {
          *dst_chan_layout = cp->ch_layout.u.mask;
          *frame_size *= 4;
          *format = alGetEnumValue("AL_FORMAT_QUAD32");
        }
      }
      if (cp->ch_layout.u.mask == AV_CH_LAYOUT_MONO) {
        *dst_chan_layout = cp->ch_layout.u.mask;
        *frame_size *= 1;
        *format = AL_FORMAT_MONO_FLOAT32;
      }
    }
    if (!*format || *format == -1) {
      *dst_chan_layout = AV_CH_LAYOUT_STEREO;
      *frame_size *= 2;
      *format =
          EnableUhj ? AL_FORMAT_UHJ2CHN_FLOAT32_SOFT : AL_FORMAT_STEREO_FLOAT32;
    }
  }
  if (cp->sample_fmt == AV_SAMPLE_FMT_U8 ||
      cp->sample_fmt == AV_SAMPLE_FMT_U8P) {
    *dst_sample_format = AV_SAMPLE_FMT_U8;
    *frame_size = 1;
    if (cp->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) {
      if (alIsExtensionPresent("AL_EXT_MCFORMATS")) {
        if (cp->ch_layout.u.mask == AV_CH_LAYOUT_7POINT1) {
          *dst_chan_layout = cp->ch_layout.u.mask;
          *frame_size *= 8;
          *format = alGetEnumValue("AL_FORMAT_71CHN8");
        }
        if (cp->ch_layout.u.mask == AV_CH_LAYOUT_5POINT1 ||
            cp->ch_layout.u.mask == AV_CH_LAYOUT_5POINT1_BACK) {
          *dst_chan_layout = cp->ch_layout.u.mask;
          *frame_size *= 6;
          *format = alGetEnumValue("AL_FORMAT_51CHN8");
        }
        if (cp->ch_layout.u.mask == AV_CH_LAYOUT_QUAD) {
          *dst_chan_layout = cp->ch_layout.u.mask;
          *frame_size *= 4;
          *format = alGetEnumValue("AL_FORMAT_QUAD8");
        }
      }
      if (cp->ch_layout.u.mask == AV_CH_LAYOUT_MONO) {
        *dst_chan_layout = cp->ch_layout.u.mask;
        *frame_size *= 1;
        *format = AL_FORMAT_MONO8;
      }
    }
    if (!*format || *format == -1) {
      *dst_chan_layout = AV_CH_LAYOUT_STEREO;
      *frame_size *= 2;
      *format = EnableUhj ? AL_FORMAT_UHJ2CHN8_SOFT : AL_FORMAT_STEREO8;
    }
  }
  if (!*format || *format == -1) {
    *dst_sample_format = AV_SAMPLE_FMT_S16;
    *frame_size = 2;
    if (cp->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) {
      if (alIsExtensionPresent("AL_EXT_MCFORMATS")) {
        if (cp->ch_layout.u.mask == AV_CH_LAYOUT_7POINT1) {
          *dst_chan_layout = cp->ch_layout.u.mask;
          *frame_size *= 8;
          *format = alGetEnumValue("AL_FORMAT_71CHN16");
        }
        if (cp->ch_layout.u.mask == AV_CH_LAYOUT_5POINT1 ||
            cp->ch_layout.u.mask == AV_CH_LAYOUT_5POINT1_BACK) {
          *dst_chan_layout = cp->ch_layout.u.mask;
          *frame_size *= 6;
          *format = alGetEnumValue("AL_FORMAT_51CHN16");
        }
        if (cp->ch_layout.u.mask == AV_CH_LAYOUT_QUAD) {
          *dst_chan_layout = cp->ch_layout.u.mask;
          *frame_size *= 4;
          *format = alGetEnumValue("AL_FORMAT_QUAD16");
        }
      }
      if (cp->ch_layout.u.mask == AV_CH_LAYOUT_MONO) {
        *dst_chan_layout = cp->ch_layout.u.mask;
        *frame_size *= 1;
        *format = AL_FORMAT_MONO16;
      }
    }
    if (!*format || *format == -1) {
      *dst_chan_layout = AV_CH_LAYOUT_STEREO;
      *frame_size *= 2;
      *format = EnableUhj ? AL_FORMAT_UHJ2CHN16_SOFT : AL_FORMAT_STEREO16;
    }
  }
}

bool audio_playback_context_init(audio_playback_context *c, decode_context dc) {
  c->dc = dc;
  AVCodecContext *cc = c->dc.cc;
  i32 dst_chan_layout;
  find_suitable_al_format(cc, &c->frame_size, &dst_chan_layout,
                          &c->out_sample_format, &c->al_format);
  c->swr = NULL;
  av_channel_layout_from_mask(&c->out_layout, dst_chan_layout);
  i32 error;
  if ((error = swr_alloc_set_opts2(
           &c->swr, &c->out_layout, c->out_sample_format, cc->sample_rate,
           &cc->ch_layout, cc->sample_fmt, cc->sample_rate, 0, NULL)) < 0) {
    log_error("unable to allocate and set SwrContext options: %s",
              av_err2str(error));
    goto fail_alloc_setopt;
  }

  if ((error = swr_init(c->swr)) < 0) {
    log_error("unable to initialize SwrContext: %s", av_err2str(error));
    goto fail_swr_init;
  }
  return true;

fail_swr_init:
  swr_free(&c->swr);
fail_alloc_setopt:
  return false;
}

void audio_playback_context_free(audio_playback_context *c) {
  swr_free(&c->swr);
  decode_context_free(&c->dc);
}

i32 audio_playback_context_fill_buffer(audio_playback_context *c, ALuint buffer,
                                       i32 total_samples) {
  i32 num_samples = 0;
  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    log_error("unable to allocate frame");
    goto fail_alloc_frame;
  }

  i32 error;
  u8 *data;
  if ((error = av_samples_alloc(&data, NULL, c->out_layout.nb_channels,
                                total_samples, c->out_sample_format, 0)) < 0) {
    log_error("unable to allocate audio samples: %s", av_err2str(error));
    goto fail_alloc_samples;
  }

  const u8 **in_data = NULL;
  i32 num_in_data = 0;
  bool flush = true, eof = false;
  while (num_samples < total_samples && !eof) {
    if (!flush) {
      decode_frame_result decode_result =
          decode_context_decode_frame(&c->dc, frame,
                                      &(decode_frame_info){
                                          .packet_receive_info =
                                              {
                                                  .block = true,
                                                  .num_messages = 1,
                                              },
                                      });
      assert(decode_result != DECODE_FRAME_RESULT_TIMEOUT);
      if (decode_result == DECODE_FRAME_RESULT_SUCCESS) {
        in_data = (const u8 **)frame->data;
        num_in_data = frame->nb_samples;
      } else if (decode_result == DECODE_FRAME_RESULT_ERROR) {
        log_error("error receiving frame from decoding context");
        goto fail_decode;
      } else if (decode_result == DECODE_FRAME_RESULT_EOF) {
        in_data = NULL;
        num_in_data = 0;
        eof = true;
      }
    } else {
      flush = false;
    }

    u8 *dst = &data[num_samples * c->frame_size];
    i32 num_converted = swr_convert(c->swr, &dst, total_samples - num_samples,
                                    in_data, num_in_data);
    av_frame_unref(frame);
    if (num_converted < 0) {
      log_error("error converting samples: %s", av_err2str(num_converted));
      goto fail_convert;
    }

    num_samples += num_converted;
  }

  alBufferData(buffer, c->al_format, data, num_samples * c->frame_size,
               c->dc.cc->sample_rate);
  av_freep(&data);
  av_frame_free(&frame);
  return num_samples;

fail_convert:
fail_decode:
  av_freep(&data);
fail_alloc_samples:
  av_frame_free(&frame);
fail_alloc_frame:
  return -1;
}
