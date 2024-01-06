#include "read_thread.h"
#include "../utils/mpmc.h"
#include "../utils/threading_utils.h"
#include <assert.h>
#include <lauxlib.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <log.h>
#include <lua.h>

typedef struct {
  i32 stream_index;
  i32 num_buffered_packets;
  mpmc_sender sender;
} packet_stream;

typedef struct {
  AVFormatContext *fmt;
  i32 num_streams;
  mpmc_receiver cmds;
  packet_stream packets[];
} thread_data;

typedef enum {
  CMD_MSG_TAG_EXIT,
  CMD_MSG_TAG_LATE_PACKET,
} cmd_msg_tag;

typedef struct {
  cmd_msg_tag tag;
  union {};
} cmd_msg;

typedef struct {
  thread_data *td;
  i64 timeout;
  AVPacket *packet;
  bool packet_pending;
  bool packet_late;
  bool error;
} thread_context;

static inline thread_context thread_context_init(void *arg) {
  return (thread_context){
      .td = arg,
      .timeout = -1,
      .packet = NULL,
      .packet_pending = false,
      .packet_late = false,
      .error = false,
  };
}

static inline void thread_context_free(thread_context *tc) {
  thread_data *t = tc->td;
  av_packet_free(&tc->packet);
  free(t);
}

static inline bool thread_context_handle_commands(thread_context *tc,
                                                  bool *exit) {
  cmd_msg cmd;
  i32 num_messages = mpmc_receive(
      &tc->td->cmds, &(mpmc_receive_info){
                         .block = tc->timeout > 0,
                         .message_data = &cmd,
                         .num_messages = 1,
                         .timeout = tc->timeout > 0 ? &tc->timeout : NULL,
                     });
  tc->timeout = -1;
  if (num_messages == 0) {
    return true;
  }

  if (num_messages != 1) {
    log_error("error receiving message from command queue");
    return false;
  }

  switch (cmd.tag) {
  case CMD_MSG_TAG_EXIT:
    *exit = true;
    break;
  case CMD_MSG_TAG_LATE_PACKET:
    tc->packet_late = true;
    break;
  }

  tc->timeout = -1;
  return true;
}

static inline bool thread_context_read_frame(thread_context *tc, bool *eof) {
  if (!tc->packet) {
    tc->packet = av_packet_alloc();
    if (!tc->packet) {
      log_error("unable to allocate packet");
      return false;
    }
  }

  i32 error = av_read_frame(tc->td->fmt, tc->packet);
  if (error >= 0) {
    tc->packet_pending = true;
  } else if (error == AVERROR(EAGAIN)) {
    tc->timeout = 1e7;
  } else if (error == AVERROR_EOF) {
    *eof = true;
  } else {
    log_error("error reading frame: %s", av_err2str(error));
    return false;
  }

  return true;
}

static bool packet_queues_full(thread_context *tc) {
  thread_data *t = tc->td;
  for (i32 i = 0; i < t->num_streams; ++i) {
    if (mpmc_hint_num_recvable(MPMC_COMMON_HANDLE(t->packets[i].sender)) <
        t->packets[i].num_buffered_packets) {
      return false;
    }
  }

  return true;
}

static inline bool thread_context_try_send_packet(thread_context *tc) {
  if (!tc->packet_pending) {
    return false;
  }

  thread_data *t = tc->td;
  packet_stream *stream = NULL;

  for (i32 i = 0; i < t->num_streams; ++i) {
    if (t->packets[i].stream_index == tc->packet->stream_index) {
      stream = &t->packets[i];
      break;
    }
  }

  if (!stream) {
    av_packet_unref(tc->packet);
    tc->packet_pending = false;
    return true;
  }

  // wait if...
  bool should_send =
      // late packet messages
      tc->packet_late ||
      // packet queue not full
      mpmc_hint_num_sendable(MPMC_COMMON_HANDLE(stream->sender)) > 0 ||
      // all other packet queues are full
      !packet_queues_full(tc);
  if (!should_send) {
    // 10ms
    tc->timeout = 1000e6;
    return true;
  }

  i32 num_sent = mpmc_send(&stream->sender,
                           &(mpmc_send_info){.block = false,
                                             .num_messages = 1,
                                             .message_data = &(packet_msg){
                                                 .tag = PACKET_MSG_TAG_PACKET,
                                                 .pkt = tc->packet,
                                             }});
  if (num_sent == 1) {
    tc->packet = NULL;
    tc->packet_pending = false;
    return true;
  }

  log_error("unable to send packet to packet stream");
  return false;
}

static inline bool thread_context_send_last_packets(thread_context *tc) {
  bool error = false;
  thread_data *t = tc->td;
  for (i32 i = 0; i < t->num_streams; ++i) {
    if (t->packets[i].stream_index >= 0) {
      if (mpmc_send(
              &t->packets[i].sender,
              &(mpmc_send_info){.block = false,
                                .num_messages = 1,
                                .message_data = &(packet_msg){
                                    .tag = tc->error ? PACKET_MSG_TAG_ERROR
                                                     : PACKET_MSG_TAG_EOF,
                                }}) != 1) {
        log_warn("unable to send %s packet message for stream %d",
                 tc->error ? "ERROR" : "EOF", t->packets[i].stream_index);
        error = true;
      }
    }
  }

  return !error;
}

static int thread_callback(void *arg) {
  thread_context tc = thread_context_init(arg);
  while (true) {
    bool exit;
    if (!thread_context_handle_commands(&tc, &exit)) {
      log_warn("read thread errored while handling commands");
      tc.error = true;
      break;
    }

    if (exit) {
      break;
    }

    if (!tc.packet_pending) {
      if (!thread_context_read_frame(&tc, &exit)) {
        log_warn("read thread errored while trying to read frame");
        tc.error = true;
        break;
      }

      if (exit) {
        break;
      }
    }

    if (!thread_context_try_send_packet(&tc)) {
      log_warn("read thread errored while trying to send packets");
      tc.error = true;
      break;
    }
  }

  if (!thread_context_send_last_packets(&tc)) {
    log_warn("read thread errored while sending last packets");
    tc.error = true;
  }

  thread_context_free(&tc);
  return tc.error ? 1 : 0;
}

bool read_thread_init(read_thread_handle *t, const read_thread_init_info *info,
                      stream_info *streams) {
  thread_data *td =
      malloc(sizeof *td + info->num_streams * sizeof(td->packets[0]));
  if (!td) {
    log_error("unable to allocate thread data");
    goto fail_alloc_thread_data;
  }

  td->fmt = info->format_context;
  td->num_streams = info->num_streams;

  i32 num_packet_mpmc = 0;
  for (num_packet_mpmc = 0; num_packet_mpmc < info->num_streams;
       ++num_packet_mpmc) {
    i32 index = info->stream_indices[num_packet_mpmc];
    if (index < 0) {
      enum AVMediaType type = -(index + 1);
      index = av_find_best_stream(info->format_context, type, -1, -1, NULL, 0);
      if (index < 0) {
        log_warn("unable to find %s stream in media",
                 av_get_media_type_string(type));
        info->stream_indices[num_packet_mpmc] = -1;
        continue;
      }
    }
    td->packets[num_packet_mpmc].num_buffered_packets =
        info->num_buffered_packets ? info->num_buffered_packets[num_packet_mpmc]
                                   : READ_THREAD_NUM_BUFFERED_PACKETS_DEFAULT;
    td->packets[num_packet_mpmc].stream_index = index;
    streams[num_packet_mpmc].index = index;
    if (!mpmc_init(
            &(mpmc_init_info){
                .enable_timeout = true,
                .message_size = sizeof(packet_msg),
                .auto_grow = true,
                .initial_num_messages =
                    td->packets[num_packet_mpmc].stream_index,
            },
            &td->packets[num_packet_mpmc].sender,
            &streams[num_packet_mpmc].receiver)) {
      log_error("unable to initialize packet MPMC channels");
      goto fail_packet_mpmcs;
    }
  }

  if (!mpmc_init(
          &(mpmc_init_info){
              .enable_timeout = true,
              .message_size = sizeof(cmd_msg),
              .auto_grow = true,
              .initial_num_messages = 1,
          },
          &t->cmds, &td->cmds)) {
    log_error("unable to initialize command MPMC channels");
    goto fail_cmd_mpmc;
  }

  int error;
  if ((error = thrd_create(&t->thread, thread_callback, td)) != thrd_success) {
    log_error("unable to start read thread");
    goto fail_thread;
  }

  return true;

fail_thread:
  mpmc_free(MPMC_COMMON_HANDLE(t->cmds));
fail_cmd_mpmc:
fail_packet_mpmcs:
  for (i32 i = 0; i < num_packet_mpmc; ++i) {
    if (td->packets[i].stream_index >= 0) {
      mpmc_free(MPMC_COMMON_HANDLE(td->packets[i].sender));
    }
  }

  free(td);
fail_alloc_thread_data:
  return false;
}

void read_thread_free(read_thread_handle *t) {
  if (!read_thread_cmd_exit(t) || !read_thread_join(t)) {
    log_warn("unable to join read thread");
  }
  mpmc_free(MPMC_COMMON_HANDLE(t->cmds));
}

void flush_packet_receiver(mpmc_receiver *receiver) {
  packet_msg msg;
  while (mpmc_receive(receiver, &(mpmc_receive_info){
                                    .block = false,
                                    .num_messages = 1,
                                    .message_data = &msg,
                                }) == 1) {
    av_packet_free(&msg.pkt);
  }
}

void stream_info_free(stream_info *streams, i32 num_streams) {
  for (i32 i = 0; i < num_streams; ++i) {
    flush_packet_receiver(&streams[i].receiver);
    mpmc_free(MPMC_COMMON_HANDLE(streams[i].receiver));
  }
}

static bool send_message(read_thread_handle *t, mpmc_send_info *si) {
  return mpmc_send(&t->cmds, si) == 1;
}

bool read_thread_cmd_exit(read_thread_handle *t) {
  return send_message(t, &(mpmc_send_info){
                             .num_messages = 1,
                             .message_data =
                                 &(cmd_msg){
                                     .tag = CMD_MSG_TAG_EXIT,
                                 },
                         });
}

bool read_thread_cmd_late_packet(read_thread_handle *t) {
  return send_message(t, &(mpmc_send_info){
                             .num_messages = 1,
                             .message_data =
                                 &(cmd_msg){
                                     .tag = CMD_MSG_TAG_LATE_PACKET,
                                 },
                         });
}

bool read_thread_receive(read_thread_handle *t, stream_info *si,
                         packet_msg *msg, bool *packet_received,
                         bool cmd_late_packet) {
  *packet_received = mpmc_receive(&si->receiver, &(mpmc_receive_info){
                                                     .block = false,
                                                     .num_messages = 1,
                                                     .message_data = &msg,
                                                 }) == 1;
  if (cmd_late_packet && !*packet_received) {
    if (!read_thread_cmd_late_packet(t)) {
      log_error("unable to send late packet command to read thread");
      return false;
    }
  }

  return true;
}

bool read_thread_join(read_thread_handle *t) {
  int ret;
  int error;
  if ((error = thrd_join(t->thread, &ret)) != thrd_success) {
    log_error("unable to join read thread: %s", thrd_error_to_string(error));
    return false;
  }

  if (ret != 0) {
    log_warn("read thread exited with error code %d", ret);
  }

  return true;
}

receive_packet_result read_thread_receive_packet(read_thread_handle *t,
                                                 stream_info *si,
                                                 packet_msg *msg,
                                                 mpmc_receive_info *info) {
  assert(info->num_messages == 1 && "num_messages must be set to 1");

  i32 num_messages = mpmc_receive(&si->receiver, &(mpmc_receive_info){
                                                     .num_messages = 1,
                                                     .message_data = msg,
                                                     .block = false,
                                                 }) == 1;
  if (num_messages < 0) {
    log_error("error while receiving packet from read thread: %s",
              av_err2str(num_messages));
    return RECEIVE_PACKET_RESULT_ERROR;
  }

  if (num_messages == 1) {
    return RECEIVE_PACKET_RESULT_SUCCESS;
  }

  if (!read_thread_cmd_late_packet(t)) {
    log_warn("unable to issue late packet command to read thread");
  }

  info->message_data = msg;
  num_messages = mpmc_receive(&si->receiver, info);
  if (num_messages < 0) {
    log_error("error while receiving packet from read thread: %s",
              av_err2str(num_messages));
    return RECEIVE_PACKET_RESULT_ERROR;
  }

  if (num_messages == 1) {
    return RECEIVE_PACKET_RESULT_SUCCESS;
  }

  return RECEIVE_PACKET_RESULT_TIMEOUT;
}
