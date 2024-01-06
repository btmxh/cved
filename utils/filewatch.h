#pragma once

#include "types.h"

typedef struct filewatch filewatch;
typedef struct {
  char *name;
  i32 name_len;
  bool created;
  bool deleted;
  bool isdir;
  bool movedfrom;
  bool movedto;
  bool modified;
} filewatch_event;

filewatch *filewatch_init(const char *monitor_dir);
void filewatch_free(filewatch *fw);

bool filewatch_poll(filewatch *fw, filewatch_event *e);
void filewatch_free_event(filewatch *fw, filewatch_event *e);
