#pragma once

#include "types.h"
#include <libavcodec/packet.h>
#include "mpmc.h"
#include <threads.h>

typedef struct {
  AVFifo* fifo;
  mtx_t mutex;
  cnd_t condvar;
  i32 packet_serial;
} packet_queue;

bool packet_queue_init(packet_queue *queue);
void packet_queue_free(packet_queue *queue);

bool packet_queue_push(packet_queue *queue, AVPacket *packet);
bool packet_queue_pop(packet_queue *queue, AVPacket **packet);
bool packet_queue_flush(packet_queue* queue);

