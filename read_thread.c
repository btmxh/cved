#include "read_thread.h"
#include "mpmc.h"
#include "threading_utils.h"
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <log.h>

typedef struct {
  i32 stream_index;
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
} cmd_msg_tag;

typedef struct {
  cmd_msg_tag tag;
  union {};
} cmd_msg;

int thread_callback(void *arg) {
  thread_data *t = arg;

  i32 error;
  i64 timeout = -1;
  AVPacket *pkt = NULL;
  while (true) {
    cmd_msg cmd;
    error = mpmc_receive(&t->cmds, &(mpmc_receive_info){
                                       .block = false,
                                       .message_data = &cmd,
                                       .num_messages = 1,
                                       .timeout = timeout > 0 ? &timeout : NULL,
                                   });
    if (error == 1) {
      // handle message
      switch (cmd.tag) {
      case CMD_MSG_TAG_EXIT:
        goto out_of_loop;
      }
    } else if (error != 0) {
      log_error("error receiving command");
    }

    if (!pkt) {
      pkt = av_packet_alloc();
      if (!pkt) {
        log_error("unable to allocate packet");
        goto fail;
      }
    }
    error = av_read_frame(t->fmt, pkt);
    if (error == 0) {
      i32 stream_idx = -1;
      for (i32 i = 0; i < t->num_streams; ++i) {
        if (t->packets[i].stream_index == pkt->stream_index) {
          stream_idx = i;
          break;
        }
      }

      if (stream_idx >= 0) {
        if (mpmc_send(&t->packets[stream_idx].sender,
                      &(mpmc_send_info){
                          .block = true,
                          .num_messages = 1,
                          .message_data =
                              &(packet_msg){
                                  .tag = PACKET_MSG_TAG_PACKET,
                                  .pkt = pkt,
                              },
                      }) == 1) {
          pkt = NULL;
          continue;
        } else {
          log_error("unable to send packet to packet queue");
        }
      }

      av_packet_unref(pkt);
    }

    if (error == AVERROR(EAGAIN)) {
      // wait for 1ms
      timeout = 10000000;
      continue;
    }

    if (error == AVERROR_EOF) {
      break;
    }

    if (error != 0) {
      log_warn("error reading frame from AVFormatContext: %s",
               av_err2str(error));
      continue;
    }
  }
out_of_loop:
  log_info("finished reading");
  for (i32 i = 0; i < t->num_streams; ++i) {
    if (t->packets[i].stream_index >= 0) {
      if (mpmc_send(&t->packets[i].sender,
                    &(mpmc_send_info){.block = false,
                                      .num_messages = 1,
                                      .message_data = &(packet_msg){
                                          .tag = PACKET_MSG_TAG_EOF,
                                      }}) != 1) {
        log_warn("unable to send EOF packet message for stream %d",
                 t->packets[i].stream_index);
      }
    }
  }
  av_packet_free(&pkt);
  free(t);
  return 0;

fail:
  for (i32 i = 0; i < t->num_streams; ++i) {
    if (t->packets[i].stream_index >= 0) {
      if (mpmc_send(&t->packets[i].sender,
                    &(mpmc_send_info){.block = false,
                                      .num_messages = 1,
                                      .message_data = &(packet_msg){
                                          .tag = PACKET_MSG_TAG_ERROR,
                                      }}) != 1) {
        log_warn("unable to send ERROR packet message for stream %d",
                 t->packets[i].stream_index);
      }
    }
  }
  av_packet_free(&pkt);
  free(t);
  return 1;
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
      index = av_find_best_stream(info->format_context, type, 0, 0, NULL, 0);
      if (index < 0) {
        log_warn("unable to find %s stream in media",
                 av_get_media_type_string(type));
        info->stream_indices[num_packet_mpmc] = -1;
        continue;
      }
    }
    td->packets[num_packet_mpmc].stream_index = index;
    streams[num_packet_mpmc].index = index;
    if (!mpmc_init(
            &(mpmc_init_info){
                .enable_timeout = true,
                .message_size = sizeof(packet_msg),
                .auto_grow = true,
                .initial_num_messages = 1,
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
  mpmc_free(&t->cmds, &td->cmds);
fail_cmd_mpmc:
fail_packet_mpmcs:
  for (i32 i = 0; i < num_packet_mpmc; ++i) {
    if (td->packets[i].stream_index >= 0) {
      mpmc_free(&td->packets[i].sender, &streams[i].receiver);
    }
  }

  free(td);
fail_alloc_thread_data:
  return false;
}

static void join_read_thread(read_thread_handle *t) {
  if (mpmc_send(&t->cmds, &(mpmc_send_info){.block = true,
                                            .num_messages = 1,
                                            .message_data = &(cmd_msg){
                                                .tag = CMD_MSG_TAG_EXIT,
                                            }}) != 1) {
    log_fatal("unable to send exit command to read thread");
    return;
  }

  int ret;
  int error;
  if ((error = thrd_join(t->thread, &ret)) != thrd_success) {
    log_error("unable to join read thread: %s", thrd_error_to_string(error));
    return;
  }

  if (ret != 0) {
    log_warn("read thread exited with error code %d", ret);
  }
}

void read_thread_free(read_thread_handle *t) {
  join_read_thread(t);
  mpmc_free(&t->cmds, NULL);
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
    mpmc_free(NULL, &streams[i].receiver);
  }
}
