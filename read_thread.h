#pragma once

#include <libavformat/avformat.h>
#include <threads.h>

#include "mpmc.h"
#include "types.h"
#include <libavutil/avutil.h>

typedef struct {
  thrd_t thread;
  mpmc_sender cmds;
} read_thread_handle;

typedef struct {
  i32 index;
  mpmc_receiver receiver;
} stream_info;

#define READ_THREAD_STREAM_INDEX_AUTO_VIDEO ((i32) - (AVMEDIA_TYPE_VIDEO + 1))
#define READ_THREAD_STREAM_INDEX_AUTO_AUDIO ((i32) -(AVMEDIA_TYPE_AUDIO + 1)
#define READ_THREAD_STREAM_INDEX_AUTO_SUBTITLE ((i32) -(AVMEDIA_TYPE_SUBTITLE + 1)

typedef struct {
  i32 num_streams;
  i32 *stream_indices;
  AVFormatContext *format_context;
} read_thread_init_info;

typedef enum {
  PACKET_MSG_TAG_PACKET,
  PACKET_MSG_TAG_ERROR,
  PACKET_MSG_TAG_EOF,
} packet_msg_tag;

typedef struct {
  packet_msg_tag tag;
  union {
    AVPacket *pkt;
  };
} packet_msg;

bool read_thread_init(read_thread_handle *t, const read_thread_init_info *info,
                      stream_info *streams);
void read_thread_free(read_thread_handle *t);
void flush_packet_receiver(mpmc_receiver *receiver);
void stream_info_free(stream_info *streams, i32 num_streams);
