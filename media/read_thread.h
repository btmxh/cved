#pragma once

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <lua.h>
#include <threads.h>

#include "../utils/mpmc.h"
#include "../utils/types.h"
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
#define READ_THREAD_STREAM_INDEX_AUTO_AUDIO ((i32) -(AVMEDIA_TYPE_AUDIO + 1))
#define READ_THREAD_STREAM_INDEX_AUTO_SUBTITLE ((i32) -(AVMEDIA_TYPE_SUBTITLE + 1))

typedef struct {
  i32 num_streams;
  i32 *stream_indices;
  i32 *num_buffered_packets;
  AVFormatContext *format_context;
} read_thread_init_info;

#define READ_THREAD_NUM_BUFFERED_PACKETS_DEFAULT 10

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

typedef enum {
  RECEIVE_PACKET_RESULT_SUCCESS,
  RECEIVE_PACKET_RESULT_TIMEOUT,
  RECEIVE_PACKET_RESULT_ERROR,
} receive_packet_result;

bool read_thread_init(read_thread_handle *t, const read_thread_init_info *info,
                      stream_info *streams);
void read_thread_free(read_thread_handle *t);
void flush_packet_receiver(mpmc_receiver *receiver);
void stream_info_free(stream_info *streams, i32 num_streams);

bool read_thread_cmd_exit(read_thread_handle *t);
bool read_thread_cmd_late_packet(read_thread_handle *t);
receive_packet_result read_thread_receive_packet(read_thread_handle *t,
                                                 stream_info *si,
                                                 packet_msg *msg,
                                                 mpmc_receive_info *info);
bool read_thread_join(read_thread_handle *t);
