#pragma once

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <stdatomic.h>
#include <threads.h>

#include "types.h"

typedef struct {
  AVFormatContext *ctx;
  AVPacket* packet;
  _Atomic usize refcount;
} refcount_format_context;

typedef struct {
  AVCodecContext *ctx;
  AVBufferRef *hwctx;
  _Atomic usize refcount;
} refcount_codec_context;

typedef struct {
  refcount_format_context *fmt;
  refcount_codec_context *codec;
  i32 stream_index;
} media_stream_codec;

typedef struct {
  media_stream_codec stream;
} video_context;

typedef struct {
  media_stream_codec stream;
} audio_context;

typedef struct {
  thrd_t thread;
  refcount_format_context* format_ctx;
  AVPacket* packet_queue;
  usize queue_len;
  usize queue_cap;
} packet_thread;

typedef struct {
} ffmpeg_context;

typedef struct {
  refcount_format_context *shared_fmt_context;
  refcount_codec_context *shared_codec_context;
  i32 stream_index;
  const AVInputFormat *open_input_format;
  AVDictionary *open_input_dict;
  AVDictionary *find_streams_dict;
  AVDictionary *codec_open_dict;
  bool disable_hw_accel;
  AVDictionary *hwdevice_create_dict;
} open_args;

#define FFMPEG_NULL_STREAM_INDEX (-1)

bool ffmpeg_open_video(const char *path, video_context *c, open_args *args);
bool ffmpeg_open_audio(const char *path, audio_context *c, open_args *args);
void ffmpeg_close_video(video_context* c);
void ffmpeg_close_audio(audio_context* c);
void ffmpeg_seek(media_stream_codec *stream, i64 timestamp, int flags);
bool ffmpeg_get_video_frame(video_context *c);
void ffmpeg_get_audio_frame(audio_context *c);
