#include "decode_thread.h"
#include "../utils/mpmc.h"
#include "read_thread.h"
#include <assert.h>
#include <glad/egl.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
#include <libdrm/drm_fourcc.h>
#include <log.h>
#include <threads.h>
#include <unistd.h>

typedef struct {
  AVFormatContext *fmt;
  AVCodecContext *dec_ctx;
  enum AVHWDeviceType hw_api;
  stream_info si;
  bool (*preprocess_frame)(AVFrame **, AVSubtitle *, void *);
  void *userdata;
  mpmc_sender frames;
  mpmc_receiver cmds;
} thread_context;

typedef enum {
  CMD_MSG_TAG_EXIT,
} cmd_msg_tag;

typedef struct {
  cmd_msg_tag tag;
  union {};
} cmd_msg;

static bool hw_device_supported(const AVCodecHWConfig *config) {
  if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
    return false;
  }

  for (enum AVHWDeviceType type =
           av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
       type != AV_HWDEVICE_TYPE_NONE; type = av_hwdevice_iterate_types(type)) {
    if (type == config->device_type) {
      return true;
    }
  }

  return false;
}

static enum AVPixelFormat get_format_vaapi(struct AVCodecContext *s,
                                           const enum AVPixelFormat *fmt) {
  (void)s;
  (void)fmt;
  return AV_PIX_FMT_VAAPI;
}

static bool init_hwdevice(AVCodecContext *c, const AVCodecHWConfig *config,
                          hwdevice_context *user_context) {
  i32 error;
  if ((error = av_hwdevice_ctx_create(&c->hw_device_ctx, config->device_type,
                                      NULL, NULL, 0)) < 0) {
    log_error("unable to create HWDevice: %s", av_err2str(error));
    goto fail_create_hwctx;
  }

  user_context->type = config->device_type;
  switch (config->device_type) {
  case AV_HWDEVICE_TYPE_VAAPI:
    c->get_format = get_format_vaapi;
    c->pix_fmt = AV_PIX_FMT_VAAPI;
    break;

  default:
    goto fail_unsupported;
  }

  return true;

fail_unsupported:
  av_buffer_unref(&c->hw_device_ctx);
fail_create_hwctx:
  return false;
}

static bool init_nv12_texture(i32 w, i32 h, const AVDRMFrameDescriptor *drm,
                              hw_texture *tex) {
  memset(tex, 0, sizeof *tex);
  for (i32 i = 0; i < AV_DRM_MAX_PLANES; ++i) {
    tex->vaapi_fds[i] = -1;
  }
  i32 i = 0;
  for (i32 pl = 0; pl < drm->nb_layers; ++pl) {
    for (i32 pp = 0; pp < drm->layers[pl].nb_planes; ++pp) {
      if (i >= 2) {
        log_error("invalid format");
        goto fail;
      }
      i32 layer_w = w / (i + 1);
      i32 layer_h = h / (i + 1);
      const AVDRMPlaneDescriptor *plane = &drm->layers[pl].planes[pp];
      // clang-format off
      EGLint attr[] = {
         EGL_LINUX_DRM_FOURCC_EXT, drm->layers[pl].format, 
         EGL_WIDTH, layer_w,
         EGL_HEIGHT, layer_h,
         EGL_DMA_BUF_PLANE0_FD_EXT, drm->objects[plane->object_index].fd,
         EGL_DMA_BUF_PLANE0_OFFSET_EXT, plane->offset,
         EGL_DMA_BUF_PLANE0_PITCH_EXT, plane->pitch,
         EGL_NONE
      };
      // clang-format on

      tex->images[i] = eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT,
                                         EGL_LINUX_DMA_BUF_EXT, NULL, attr);
      if (!tex->images[i]) {
        goto fail;
      }

      glGenTextures(1, &tex->textures[i]);
      if (tex->textures[i] == 0) {
        goto fail;
      }

      glBindTexture(GL_TEXTURE_2D, tex->textures[i]);
      glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, tex->images[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      ++i;
    }
  }

  return true;
fail:
  for (i32 i = 0; i < 2; ++i) {
    if (tex->textures[i]) {
      glDeleteTextures(1, &tex->textures[i]);
    }
    if (tex->images[i]) {
      eglDestroyImageKHR(eglGetCurrentDisplay(), &tex->images[i]);
    }
  }
  for (i32 i = 0; i < AV_DRM_MAX_PLANES; ++i) {
    int fd = tex->vaapi_fds[i];
    if (fd >= 0) {
      close(fd);
    }
  }
  return false;
}

static bool init_p010_texture(i32 w, i32 h, const AVDRMFrameDescriptor *drm,
                              hw_texture *tex) {
  memset(tex, 0, sizeof *tex);
  for (i32 i = 0; i < AV_DRM_MAX_PLANES; ++i) {
    tex->vaapi_fds[i] = -1;
  }
  EGLint egl_fmts[] = {DRM_FORMAT_R16, DRM_FORMAT_GR1616};
  i32 i = 0;
  for (i32 pl = 0; pl < drm->nb_layers; ++pl) {
    for (i32 pp = 0; pp < drm->layers[pl].nb_planes; ++pp) {
      if (i >= 2) {
        log_error("invalid format");
        goto fail;
      }
      i32 layer_w = w / (i + 1);
      i32 layer_h = h / (i + 1);
      const AVDRMPlaneDescriptor *plane = &drm->layers[pl].planes[pp];
      // clang-format off
      EGLint attr[] = {
         EGL_LINUX_DRM_FOURCC_EXT, egl_fmts[i], 
         EGL_WIDTH, layer_w,
         EGL_HEIGHT, layer_h,
         EGL_DMA_BUF_PLANE0_FD_EXT, drm->objects[plane->object_index].fd,
         EGL_DMA_BUF_PLANE0_OFFSET_EXT, plane->offset,
         EGL_DMA_BUF_PLANE0_PITCH_EXT, plane->pitch,
         EGL_NONE
      };
      // clang-format on

      tex->images[i] = eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT,
                                         EGL_LINUX_DMA_BUF_EXT, NULL, attr);
      if (!tex->images[i]) {
        goto fail;
      }

      glGenTextures(1, &tex->textures[i]);
      if (tex->textures[i] == 0) {
        goto fail;
      }

      glBindTexture(GL_TEXTURE_2D, tex->textures[i]);
      glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, tex->images[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      ++i;
    }
  }

  return true;
fail:
  for (i32 i = 0; i < 2; ++i) {
    if (tex->textures[i]) {
      glDeleteTextures(1, &tex->textures[i]);
    }
    if (tex->images[i]) {
      eglDestroyImageKHR(eglGetCurrentDisplay(), &tex->images[i]);
    }
  }
  for (i32 i = 0; i < AV_DRM_MAX_PLANES; ++i) {
    int fd = tex->vaapi_fds[i];
    if (fd >= 0) {
      close(fd);
    }
  }
  return false;
}

void decode_thread_free_texture(hw_texture *texture) {
  if (texture->pixfmt == AV_PIX_FMT_NONE) {
    return;
  }
  for (i32 i = 0; i < 2; ++i) {
    glDeleteTextures(1, &texture->textures[i]);
    eglDestroyImageKHR(eglGetCurrentDisplay(), &texture->images[i]);
  }
  for (i32 i = 0; i < AV_DRM_MAX_PLANES; ++i) {
    int fd = texture->vaapi_fds[i];
    if (fd >= 0) {
      close(fd);
    }
  }

  texture->pixfmt = AV_PIX_FMT_NONE;
}

bool decode_context_init(decode_context *d, decode_thread_init_info *info) {
  d->fmt = info->fmt;
  d->rt = info->rt;
  d->si = info->si;
  d->preprocess_callback = info->preprocess_frame;
  d->userdata = info->userdata;

  AVStream *s = d->fmt->streams[d->si.index];
  const AVCodec *codec = avcodec_find_decoder(s->codecpar->codec_id);
  if (!codec) {
    log_error("unable to find decoder for codec id: %s",
              avcodec_get_name(s->codecpar->codec_id));
    goto fail_codec;
  }

  d->cc = avcodec_alloc_context3(codec);
  if (!d->cc) {
    log_error("unable to allocate AVCodecContext");
    goto fail_alloc_dec_ctx;
  }

  i32 error;
  if ((error = avcodec_parameters_to_context(d->cc, s->codecpar)) < 0) {
    log_error(
        "unable to copy codec parameters from stream to codec context: %s",
        av_err2str(error));
    goto fail_copy_codecpar;
  }

  if (info->hwaccel) {
    for (i32 i = 0;; ++i) {
      const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
      if (!config) {
        break;
      }

      if (!hw_device_supported(config)) {
        continue;
      }

      if (!init_hwdevice(d->cc, config, &d->hw)) {
        log_warn("unable to initialize HWDevice %s",
                 av_hwdevice_get_type_name(config->device_type));
      } else {
        log_info("initialized HWDevice %s",
                 av_hwdevice_get_type_name(config->device_type));
        d->hw.type = config->device_type;
        break;
      }
    }

    switch (d->hw.type) {
    case AV_HWDEVICE_TYPE_VAAPI:
      d->cc->pix_fmt = AV_PIX_FMT_VAAPI;
      break;
    default:
      break;
    }
  } else {
    d->hw.type = AV_HWDEVICE_TYPE_NONE;
  }

  if ((error = avcodec_open2(d->cc, codec, &info->dec_ctx_open_dict)) < 0) {
    log_error("unable to open AVCodecContext for decoding: %s",
              av_err2str(error));
    goto fail_open_dec_ctx;
  }

  return true;

fail_open_dec_ctx:
fail_copy_codecpar:
  avcodec_free_context(&d->cc);
fail_alloc_dec_ctx:
fail_codec:
  return false;
}

void decode_context_free(decode_context *d) {
  avcodec_close(d->cc);
  avcodec_free_context(&d->cc);
}

#define DECODE_FRAME_RESULT_EAGAIN DECODE_FRAME_RESULT_TIMEOUT
static decode_frame_result receive_frame(AVCodecContext *cc, AVFrame *frame) {
  i32 error = avcodec_receive_frame(cc, frame);
  if (error >= 0) {
    return DECODE_FRAME_RESULT_SUCCESS;
  } else if (error == AVERROR_EOF) {
    return DECODE_FRAME_RESULT_EOF;
  } else if (error == AVERROR(EAGAIN)) {
    return DECODE_FRAME_RESULT_EAGAIN;
  }

  log_error("error decoding frame: %s", av_err2str(error));
  return DECODE_FRAME_RESULT_ERROR;
}

static decode_frame_result send_packet(decode_context *d,
                                       decode_frame_info *info) {
  packet_msg msg;
  receive_packet_result result = read_thread_receive_packet(
      d->rt, &d->si, &msg, &info->packet_receive_info);
  switch (result) {
  case RECEIVE_PACKET_RESULT_TIMEOUT:
    return DECODE_FRAME_RESULT_TIMEOUT;
  case RECEIVE_PACKET_RESULT_ERROR:
    return DECODE_FRAME_RESULT_ERROR;
  case RECEIVE_PACKET_RESULT_SUCCESS:
    break;
  }

  switch (msg.tag) {
  case PACKET_MSG_TAG_EOF:
    msg.tag = PACKET_MSG_TAG_PACKET;
    msg.pkt = NULL;
    break;
  case PACKET_MSG_TAG_ERROR:
    return DECODE_FRAME_RESULT_ERROR;
  case PACKET_MSG_TAG_PACKET:
    break;
  }

  i32 error = avcodec_send_packet(d->cc, msg.pkt);
  assert(error != AVERROR(EAGAIN) && "not logically possible");
  if (error == AVERROR_EOF) {
    return DECODE_FRAME_RESULT_EOF;
  } else if (error < 0) {
    log_error("unable to send packet for decoding: %s", av_err2str(error));
    return DECODE_FRAME_RESULT_ERROR;
  }

  av_packet_free(&msg.pkt);
  return DECODE_FRAME_RESULT_SUCCESS;
}

decode_frame_result decode_context_decode_frame(decode_context *d,
                                                AVFrame *frame,
                                                decode_frame_info *info) {
  decode_frame_result result;
  while ((result = receive_frame(d->cc, frame)) == DECODE_FRAME_RESULT_EAGAIN) {
    if ((result = send_packet(d, info)) != DECODE_FRAME_RESULT_SUCCESS) {
      break;
    }
  }

  return result;
}
bool decode_context_map_texture(decode_context *d, AVFrame *frame,
                                hw_texture *tex) {
  switch (d->hw.type) {
  case AV_HWDEVICE_TYPE_VAAPI: {
    AVFrame *hw_frame = av_frame_alloc();
    if (!hw_frame) {
      log_error("unable to allocate HWFrame");
      goto fail_hwframe_alloc;
    }
    hw_frame->format = AV_PIX_FMT_DRM_PRIME;
    i32 error;
    if ((error =
             av_hwframe_map(hw_frame, frame,
                            AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT) < 0)) {
      log_error("unable to map HWFrame");
      goto fail_map_hwframe;
    }
    const AVDRMFrameDescriptor *drm =
        (const AVDRMFrameDescriptor *)hw_frame->data[0];
    if (!init_nv12_texture(frame->width, frame->height, drm, tex)) {
      log_error("unable to create NV12 textures");
      goto fail_init_tex;
    }
    tex->pixfmt = ((AVHWFramesContext *)d->cc->hw_frames_ctx->data)->sw_format;
    tex->width = frame->width;
    tex->height = frame->height;
    for (i32 i = 0; i < AV_DRM_MAX_PLANES; ++i) {
      tex->vaapi_fds[i] = i < drm->nb_objects ? drm->objects[i].fd : -1;
    }
    av_frame_free(&hw_frame);

    return true;
  fail_init_tex:
  fail_map_hwframe:
  fail_hwframe_alloc:
    av_frame_free(&hw_frame);
    return false;
  }
  default:
    log_error("unsupported HWDevice API");
    return false;
  }

  (void)init_p010_texture;
}
