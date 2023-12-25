#include "ffmpeg.h"
#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <log.h>

static void ffmpeg_ref_fmt_context(refcount_format_context *c);
static void ffmpeg_unref_fmt_context(refcount_format_context *c);
static void ffmpeg_ref_codec_context(refcount_codec_context *c);
static void ffmpeg_unref_codec_context(refcount_codec_context *c);

static bool open_format_context(const char *path, refcount_format_context **c,
                                open_args *args) {
  if (args && args->shared_fmt_context) {
    *c = args->shared_fmt_context;
    ffmpeg_ref_fmt_context(*c);
    return true;
  }

  AVFormatContext **fmt = &(**c).ctx;
  int error;
  if ((error =
           avformat_open_input(fmt, path, args ? args->open_input_format : NULL,
                               args ? &args->open_input_dict : NULL))) {
    log_error("unable to create AVFormatContext: %s", av_err2str(error));
    goto fail_open_input;
  }

  ffmpeg_ref_fmt_context(*c);

  if ((error = avformat_find_stream_info(*fmt, args ? &args->find_streams_dict
                                                    : NULL))) {
    log_error("unable to find stream info: %s", av_err2str(error));
    goto fail_find_streams;
  }

  return true;
fail_find_streams:
  ffmpeg_unref_fmt_context(*c);
fail_open_input:
  return false;
}

static bool check_hwtype_supported(const AVCodecHWConfig *config) {
  enum AVHWDeviceType type = config->device_type;
  enum AVHWDeviceType iter = AV_HWDEVICE_TYPE_NONE;
  while ((iter = av_hwdevice_iterate_types(iter)) != AV_HWDEVICE_TYPE_NONE) {
    if (iter == type) {
      return true;
    }
  }

  return false;
}

static enum AVPixelFormat vaapi_get_format(struct AVCodecContext *s,
                                           const enum AVPixelFormat *fmt) {
  (void)s;
  (void)fmt;
  return AV_PIX_FMT_VAAPI;
}

static bool init_hw_accel_ctx(AVBufferRef **hw_device_ctx, AVCodecContext *c,
                              const AVCodecHWConfig *config, open_args *args) {
  if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
    return false;
  }

  if (!check_hwtype_supported(config)) {
    return false;
  }

  int error;
  if ((error = av_hwdevice_ctx_create(hw_device_ctx, config->device_type, NULL,
                                      args ? args->hwdevice_create_dict : NULL,
                                      0))) {
    log_error("unable to create HWDevice for hardware acceleration");
    return false;
  }

  if (config->device_type == AV_HWDEVICE_TYPE_VAAPI) {
    c->get_format = vaapi_get_format;
  }

  return true;
}

static bool enable_hw_accel(AVBufferRef **hw_device_ctx, AVCodecContext *c,
                            open_args *args) {
  i32 i = 0;
  const AVCodecHWConfig *config;
  while ((config = avcodec_get_hw_config(c->codec, i++)) != NULL) {
    if (init_hw_accel_ctx(hw_device_ctx, c, config, args)) {
      return true;
    }
  }

  return false;
}

static bool open_codec_context(AVFormatContext *fmt, refcount_codec_context **c,
                               i32 *stream_index, open_args *args) {
  if (args && args->shared_codec_context) {
    *c = args->shared_codec_context;
    if (args->stream_index == FFMPEG_NULL_STREAM_INDEX) {
      log_error("args->stream_index must be set if "
                "using shared codec contexts");
      return false;
    }
    *stream_index = args->stream_index;

    ffmpeg_ref_codec_context(*c);
    return true;
  }

  const AVCodec *codec = NULL;
  if (args && args->stream_index != FFMPEG_NULL_STREAM_INDEX) {
    *stream_index = args->stream_index;
    enum AVCodecID codec_id = fmt->streams[*stream_index]->codecpar->codec_id;
    codec = avcodec_find_decoder(codec_id);
    if (codec == NULL) {
      goto fail_codec;
    }
  } else {
    *stream_index =
        av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, 0, 0, &codec, 0);
    if (*stream_index < 0) {
      log_error("unable to find video stream: %s", av_err2str(*stream_index));
      goto fail_codec;
    }
  }

  assert(codec && "codec should be initialized by now");
  (**c).ctx = avcodec_alloc_context3(codec);
  AVCodecContext *codec_ctx = (**c).ctx;
  if (!codec_ctx) {
    log_error("unable to allocate codec context: %s",
              av_err2str(*stream_index));
    goto fail_codec_alloc;
  }

  int error;
  if ((error = avcodec_parameters_to_context(
           codec_ctx, fmt->streams[*stream_index]->codecpar))) {
    log_error("unable to copy codec parameters from stream to context: %s",
              av_err2str(error));
    goto fail_codec_copy;
  }

  bool hw_disable = args ? args->disable_hw_accel : false;
  (**c).hwctx = NULL;
  if (!hw_disable && codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
    if (!enable_hw_accel(&(**c).hwctx, codec_ctx, args)) {
      log_warn("unable to enable HW decoding acceleration");
    }
  }

  if ((error = avcodec_open2(codec_ctx, NULL,
                             args ? &args->codec_open_dict : NULL))) {
    log_error("unable to open AVCodecContext: %s", av_err2str(error));
    goto fail_codec_open;
  }

  ffmpeg_ref_codec_context(*c);

  return true;

  avcodec_close(codec_ctx);
fail_codec_open:
  av_buffer_unref(&(**c).hwctx);
fail_codec_copy:
  avcodec_free_context(&codec_ctx);
fail_codec_alloc:
fail_codec:
  return false;
}

bool open_media_stream(const char *path, media_stream_codec *c,
                       open_args *args) {
  if (!open_format_context(path, &c->fmt, args)) {
    log_error("unable to open format context for media file");
    goto fail_fmt;
  }

  if (!open_codec_context(c->fmt->ctx, &c->codec, &c->stream_index, args)) {
    log_error("unable to open codec context for media file");
    goto fail_codec;
  }

  ffmpeg_unref_codec_context(c->codec);
fail_codec:
  ffmpeg_unref_fmt_context(c->fmt);
fail_fmt:
  return false;
}

static bool create_decoding_sm(media_stream_codec *c,
                               packet_frame_state_machine *sm) {

}

bool ffmpeg_open_video(const char *path, video_context *c, open_args *args) {
  if (!open_media_stream(path, &c->stream, args)) {
    log_error("unable to open video stream for media file '%s'", path);
    goto fail_stream;
  }

  if (!create_decoding_sm(&c->stream, &c->decode_sm)) {
    log_error("unable to create decoding state machine");
  }

  return true;
fail_stream:
  return false;
}

bool ffmpeg_get_video_frame(video_context *c) {}

static void ffmpeg_ref_fmt_context(refcount_format_context *c) {
  atomic_fetch_add(&c->refcount, 1);
}
static void ffmpeg_unref_fmt_context(refcount_format_context *c) {
  if (atomic_fetch_sub(&c->refcount, 1) == 1) {
    avformat_close_input(&c->ctx);
  }
}
static void ffmpeg_ref_codec_context(refcount_codec_context *c) {
  atomic_fetch_add(&c->refcount, 1);
}
static void ffmpeg_unref_codec_context(refcount_codec_context *c) {
  if (atomic_fetch_sub(&c->refcount, 1) == 1) {
    av_buffer_unref(&c->hwctx);
    avcodec_close(c->ctx);
    avcodec_free_context(&c->ctx);
  }
}
