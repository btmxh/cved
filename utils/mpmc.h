#pragma once

#include "types.h"
#include <libavutil/fifo.h>
#include <threads.h>
#include <time.h>

typedef struct {
  AVFifo *fifo;
  bool auto_grow;
  mtx_t mutex;
  cnd_t send_condvar;
  cnd_t recv_condvar;
} mpmc;

typedef struct {
  mpmc *m;
} mpmc_sender;

typedef struct {
  mpmc *m;
} mpmc_receiver;

typedef struct {
  i32 message_size;
  i32 initial_num_messages;
  bool auto_grow;
  bool enable_timeout;
} mpmc_init_info;

bool mpmc_init(const mpmc_init_info *info, mpmc_sender *sender,
               mpmc_receiver *receiver);
void mpmc_clone_sender(mpmc_sender *dst, mpmc_sender *src);
void mpmc_clone_receiver(mpmc_receiver *dst, mpmc_receiver *src);

typedef struct {
  i64 *timeout;
  struct timespec *deadline;

  bool block;
  i32 num_messages;
  const void *message_data;
} mpmc_send_info;

typedef struct {
  i64 *timeout;
  struct timespec *deadline;

  bool block;
  i32 num_messages;
  void *message_data;
} mpmc_receive_info;

i32 mpmc_send(mpmc_sender *sender, const mpmc_send_info *info);
i32 mpmc_receive(mpmc_receiver *receiver, const mpmc_receive_info *info);

#define MPMC_COMMON_HANDLE(mpmc) (mpmc).m
// pass NULL if sender/receiver is on another thread
// no refcount, so make sure that no other senders and receivers are in use
void mpmc_free(mpmc *m);
i32 mpmc_num_messages(mpmc *m);
i32 mpmc_hint_num_sendable(mpmc *m);
i32 mpmc_hint_num_recvable(mpmc *m);
