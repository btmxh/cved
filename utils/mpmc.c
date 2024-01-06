#include "mpmc.h"
#include "threading_utils.h"
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/fifo.h>
#include <log.h>
#include <string.h>
#include <time.h>

bool mpmc_init(const mpmc_init_info *info, mpmc_sender *sender,
               mpmc_receiver *receiver) {
  mpmc *m = malloc(sizeof *m);
  if (!m) {
    log_error("unable to allocate MPMC struct");
    goto fail_alloc_mpmc;
  }

  if (!(m->fifo =
            av_fifo_alloc2(info->initial_num_messages, info->message_size,
                           info->auto_grow ? AV_FIFO_FLAG_AUTO_GROW : 0))) {
    log_error("unable to allocate FIFO queue");
    goto fail_fifo;
  }

  i32 error;
  if ((error =
           mtx_init(&m->mutex, info->enable_timeout ? mtx_timed : mtx_plain)) !=
      thrd_success) {
    log_error("unable to create mutex: %s", thrd_error_to_string(error));
    goto fail_mutex;
  }

  if (!info->auto_grow) {
    if ((error = cnd_init(&m->send_condvar)) != thrd_success) {
      log_error("unable to create mutex: %s", thrd_error_to_string(error));
      goto fail_send_condvar;
    }
  }

  if ((error = cnd_init(&m->recv_condvar)) != thrd_success) {
    log_error("unable to create mutex: %s", thrd_error_to_string(error));
    goto fail_recv_condvar;
  }

  m->auto_grow = info->auto_grow;
  sender->m = m;
  receiver->m = m;
  return true;

  cnd_destroy(&m->recv_condvar);
fail_recv_condvar:
  if (!info->auto_grow) {
    cnd_destroy(&m->send_condvar);
  }
fail_send_condvar:
  mtx_destroy(&m->mutex);
fail_mutex:
  av_fifo_freep2(&m->fifo);
fail_fifo:
  free(m);
fail_alloc_mpmc:
  return false;
}

void mpmc_free(mpmc *m) {
  if (!m->auto_grow) {
    cnd_destroy(&m->send_condvar);
  }
  cnd_destroy(&m->recv_condvar);
  mtx_destroy(&m->mutex);
  av_fifo_freep2(&m->fifo);
  free(m);
}

void mpmc_clone_sender(mpmc_sender *dst, mpmc_sender *src) {
  memcpy(dst, src, sizeof *src);
}

void mpmc_clone_receiver(mpmc_receiver *dst, mpmc_receiver *src) {
  memcpy(dst, src, sizeof *src);
}

bool get_deadline(struct timespec *deadline, struct timespec *optional_deadline,
                  i64 *optional_timeout) {
  if (optional_deadline) {
    *deadline = *optional_deadline;
    return true;
  }

  if (optional_timeout && timespec_get(deadline, TIME_UTC) == TIME_UTC) {
    static const i64 one_e9 = 1000000000;
    i64 timeout = *optional_timeout;
    i64 ns = deadline->tv_nsec + (timeout % one_e9);
    deadline->tv_sec += ns / one_e9 + timeout / one_e9;
    deadline->tv_nsec = ns % one_e9;
    return true;
  }

  return false;
}

i32 mpmc_send(mpmc_sender *sender, const mpmc_send_info *info) {
  struct timespec deadline;
  bool has_deadline = get_deadline(&deadline, info->deadline, info->timeout);
  mpmc *m = sender->m;
  i32 num_write = 0;

  i32 error;
  if ((error = mtx_lock(&m->mutex)) != thrd_success) {
    log_fatal("unable to lock mpmc mutex: %s", thrd_error_to_string(error));
    return AVERROR_EXTERNAL;
  }

  while (info->block && !m->auto_grow &&
         (i32)av_fifo_can_write(m->fifo) < info->num_messages) {
    if (has_deadline) {
      i32 result = cnd_timedwait(&m->send_condvar, &m->mutex, &deadline);
      if (result == thrd_timedout) {
        break;
      } else if (result != thrd_success) {
        log_error("unable to wait for condvar: %s",
                  thrd_error_to_string(error));
      }
    } else {
      if ((error = cnd_wait(&m->send_condvar, &m->mutex)) != thrd_success) {
        log_error("unable to wait for condvar: %s",
                  thrd_error_to_string(error));
      }
    }
  }

  i32 can_write = av_fifo_can_write(m->fifo);
  num_write = info->num_messages;
  if (num_write > can_write && !m->auto_grow) {
    num_write = can_write;
  }

  if (num_write > 0) {
    if ((error = av_fifo_write(m->fifo, info->message_data, num_write))) {
      log_error("unable to send messages: %s", av_err2str(error));
    }

    if ((error = cnd_signal(&m->recv_condvar)) != thrd_success) {
      log_fatal("unable to signal recv condvar: %s",
                thrd_error_to_string(error));
    }
  }

  if ((error = mtx_unlock(&m->mutex)) != thrd_success) {
    log_fatal("unable to unlock mpmc mutex: %s", thrd_error_to_string(error));
    return AVERROR_EXTERNAL;
  }

  return num_write;
}
i32 mpmc_receive(mpmc_receiver *receiver, const mpmc_receive_info *info) {
  struct timespec deadline;
  bool has_deadline = get_deadline(&deadline, info->deadline, info->timeout);
  mpmc *m = receiver->m;
  i32 num_read = 0;

  i32 error;
  if ((error = mtx_lock(&m->mutex)) != thrd_success) {
    log_fatal("unable to lock mpmc mutex: %s", thrd_error_to_string(error));
    return AVERROR_EXTERNAL;
  }

  while (info->block && (i32)av_fifo_can_read(m->fifo) < info->num_messages) {
    if (has_deadline) {
      i32 result = cnd_timedwait(&m->recv_condvar, &m->mutex, &deadline);
      if (result == thrd_timedout) {
        break;
      } else if (result != thrd_success) {
        log_error("unable to wait for condvar: %s",
                  thrd_error_to_string(error));
      }
    } else {
      if ((error = cnd_wait(&m->recv_condvar, &m->mutex)) != thrd_success) {
        log_error("unable to wait for condvar: %s",
                  thrd_error_to_string(error));
      }
    }
  }

  i32 can_read = av_fifo_can_read(m->fifo);
  num_read = info->num_messages > can_read ? can_read : info->num_messages;
  if (num_read > 0) {
    if ((error = av_fifo_read(m->fifo, info->message_data, num_read))) {
      log_error("unable to receive messages: %s", av_err2str(error));
    }

    if (!m->auto_grow &&
        (error = cnd_signal(&m->send_condvar)) != thrd_success) {
      log_fatal("unable to signal send condvar: %s",
                thrd_error_to_string(error));
    }
  }

  if ((error = mtx_unlock(&m->mutex)) != thrd_success) {
    log_fatal("unable to unlock mpmc mutex: %s", thrd_error_to_string(error));
    return AVERROR_EXTERNAL;
  }

  return num_read;
}

i32 mpmc_hint_num_sendable(mpmc *m) { return av_fifo_can_write(m->fifo); }
i32 mpmc_hint_num_recvable(mpmc *m) { return av_fifo_can_read(m->fifo); }
