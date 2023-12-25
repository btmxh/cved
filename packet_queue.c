#include "packet_queue.h"
#include <libavutil/fifo.h>
#include <log.h>
#include <stdlib.h>
#include <threads.h>

typedef struct {
  AVPacket *packet;
  i32 serial;
} av_packet_list;

bool packet_queue_init(packet_queue *queue) {
  queue->packet_serial = 0;

  if ((queue->fifo = av_fifo_alloc2(1, sizeof(av_packet_list),
                                    AV_FIFO_FLAG_AUTO_GROW)) == NULL) {
    log_error("unable to allocate fifo packet queue");
    goto fail_alloc;
  }

  if (mtx_init(&queue->mutex, mtx_plain) != thrd_success) {
    goto fail_mutex;
  }

  if (cnd_init(&queue->condvar) != thrd_success) {
    goto fail_condvar;
  }

  return true;

  cnd_destroy(&queue->condvar);
fail_condvar:
  mtx_destroy(&queue->mutex);
fail_mutex:
  av_fifo_freep2(&queue->fifo);
fail_alloc:
  return false;
}

void packet_queue_free(packet_queue *queue) {
  if (!packet_queue_flush(queue)) {
    log_warn("unable to flush packet queue");
  }
  cnd_destroy(&queue->condvar);
  mtx_destroy(&queue->mutex);
  av_fifo_freep2(&queue->fifo);
}

bool packet_queue_push(packet_queue *queue, AVPacket *packet) {
  bool pushed = false;
  if (mtx_lock(&queue->mutex) != thrd_success) {
    return false;
  }

  if (!(pushed = av_fifo_write(queue->fifo,
                               &(av_packet_list){
                                   .packet = packet,
                                   .serial = ++queue->packet_serial,
                               },
                               1) == 1)) {
    log_warn("unable to push to packet queue");
  }

  if (cnd_signal(&queue->condvar) != thrd_success) {
    log_fatal("unable to signal condvar");
    return false;
  }

  if (mtx_unlock(&queue->mutex) != thrd_success) {
    log_fatal("unable to signal mutex");
    return false;
  }

  return pushed;
}

bool packet_queue_pop(packet_queue *queue, AVPacket **packet) {
  bool pop = false;
  if (mtx_lock(&queue->mutex) != thrd_success) {
    return false;
  }

  av_packet_list packet_item;
  while (av_fifo_read(queue->fifo, &packet_item, 1) != 1) {
    cnd_wait(&queue->condvar, &queue->mutex);
  }

  *packet = packet_item.packet;

  if (mtx_unlock(&queue->mutex) != thrd_success) {
    return false;
  }

  return pop;
}

bool packet_queue_flush(packet_queue *queue) {
  if (mtx_lock(&queue->mutex) != thrd_success) {
    return false;
  }

  av_packet_list packet_item;
  while (av_fifo_read(queue->fifo, &packet_item, 1) == 1) {
    av_packet_free(&packet_item.packet);
  }

  if (mtx_unlock(&queue->mutex) != thrd_success) {
    return false;
  }

  return true;
}
