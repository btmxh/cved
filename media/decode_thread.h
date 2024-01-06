#pragma once

#include "../utils/mpmc.h"
#include "../utils/types.h"
#include "read_thread.h"
#include <glad/egl.h>
#include <glad/gles2.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_drm.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>

typedef struct {
  enum AVHWDeviceType type;
} hwdevice_context;

typedef struct {
  AVFormatContext *fmt;
  AVCodecContext *cc;
  hwdevice_context hw;
  read_thread_handle *rt;
  stream_info si;
  bool (*preprocess_callback)(AVFrame **, AVSubtitle *, void *);
  void *userdata;

  AVFrame *frame;
} decode_context;

typedef struct {
  mpmc_receive_info packet_receive_info;
} decode_frame_info;

typedef struct {
  AVFormatContext *fmt;
  stream_info si;
  read_thread_handle *rt;
  AVDictionary *dec_ctx_open_dict;
  i32 num_buffered_frames;
  bool (*preprocess_frame)(AVFrame **, AVSubtitle *, void *);
  void *userdata;
  bool hwaccel;
} decode_thread_init_info;

typedef enum {
  FRAME_MSG_TAG_FRAME,
  FRAME_MSG_TAG_SUBTITLE,
  FRAME_MSG_TAG_ERROR,
  FRAME_MSG_TAG_EOF,
} frame_msg_tag;

typedef struct {
  frame_msg_tag tag;
  union {
    AVFrame *frame;
    AVSubtitle subs;
  };
} frame_msg;

typedef struct {
  enum AVPixelFormat pixfmt;
  GLuint textures[AV_DRM_MAX_PLANES];
  EGLImageKHR images[AV_DRM_MAX_PLANES];
  int vaapi_fds[AV_DRM_MAX_PLANES];
  i32 width, height;
} hw_texture;

typedef enum {
  DECODE_FRAME_RESULT_SUCCESS,
  DECODE_FRAME_RESULT_TIMEOUT,
  DECODE_FRAME_RESULT_EOF,
  DECODE_FRAME_RESULT_ERROR,
} decode_frame_result;

bool decode_context_init(decode_context *d, decode_thread_init_info *info);
void decode_context_free(decode_context *d);
decode_frame_result decode_context_decode_frame(decode_context *d,
                                                AVFrame *frame,
                                                decode_frame_info *info);
bool decode_context_map_texture(decode_context *d, AVFrame *frame,
                                hw_texture *tex);
void decode_thread_free_texture(hw_texture *texture);
