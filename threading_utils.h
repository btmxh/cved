#pragma once

#include "types.h"
#include <threads.h>

static inline const char *thrd_error_to_string(i32 error) {
  switch (error) {
  case thrd_success:
    return "success";
  case thrd_nomem:
    return "out of memory";
  case thrd_timedout:
    return "timed out";
  case thrd_busy:
    return "resource temporarily unavailable";
  case thrd_error:
    return "error";
  default:
    return "invalid error";
  }
}
