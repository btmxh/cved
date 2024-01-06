#include "filewatch.h"
#include "fs.h"
#include <dirent.h>
#include <errno.h>
#include <linux/limits.h>
#include <log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

typedef struct {
  char *path;
  int wd;
} filewatch_dir;

typedef struct {
  filewatch_dir *data;
  i32 len, cap;
} filewatch_dirs;

struct filewatch {
  int fd;
  filewatch_dirs dirs;
  char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
  i32 bufsize;
};

static filewatch_dir *find_dir(filewatch *fw, int wd, const char *name) {
  for (i32 i = 0; i < fw->dirs.len; ++i) {
    filewatch_dir *d = &fw->dirs.data[i];
    if (d->wd == wd || (name && strcmp(d->path, name) == 0)) {
      return d;
    }
  }

  return NULL;
}

static bool create_watch(filewatch *fw, const char *parent, const char *child) {
  char *path = parent ? path_concat(parent, child, true) : path_absolute(child);
  if (!path) {
    goto fail_path;
  }

  int watch = inotify_add_watch(fw->fd, path,
                                IN_MODIFY | IN_MOVE | IN_CREATE | IN_DELETE);
  if (watch < 0) {
    goto fail_add_watch;
  }

  if (fw->dirs.len >= fw->dirs.cap) {
    i32 new_cap = (fw->dirs.cap + 1) * 3 / 2;
    filewatch_dir *new_data =
        realloc(fw->dirs.data, new_cap * sizeof(filewatch_dir));
    if (!new_data) {
      goto fail_realloc;
    }

    fw->dirs.data = new_data;
    fw->dirs.cap = new_cap;
  }

  fw->dirs.data[fw->dirs.len++] = (filewatch_dir){
      .path = path,
      .wd = watch,
  };

  DIR *dir = opendir(path);
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 &&
        strcmp(entry->d_name, "..") != 0) {
      if (!create_watch(fw, path, entry->d_name)) {
        // no need to clean up here
        return false;
      }
    }
  }
  closedir(dir);

  log_trace("recursively add watch for directory '%s'", path);
  return true;

fail_realloc:
  inotify_rm_watch(fw->fd, watch);
fail_add_watch:
  free(path);
fail_path:
  return false;
}

filewatch *filewatch_init(const char *monitor_dir) {
  filewatch *fw = malloc(sizeof *fw);
  if (!fw) {
    goto fail_filewatch_malloc;
  }

  fw->fd = inotify_init1(IN_NONBLOCK);
  if (fw->fd == -1) {
    char msg[100];
    strerror_r(errno, msg, sizeof msg);
    log_error("unable to initialize inotify for filewatch: %s", msg);
    goto fail_inotify_init;
  }

  fw->dirs.data = NULL;
  fw->dirs.len = 0;
  fw->dirs.cap = 0;
  create_watch(fw, NULL, monitor_dir);

  fw->bufsize = 0;
  return fw;

fail_inotify_init:
  free(fw);
fail_filewatch_malloc:
  return NULL;
}

void filewatch_free(filewatch *fw) {
  for (i32 i = 0; i < fw->dirs.len; ++i) {
    free(fw->dirs.data[i].path);
    inotify_rm_watch(fw->fd, fw->dirs.data[i].wd);
  }
  free(fw->dirs.data);
  close(fw->fd);
  free(fw);
}

static void preprocess_event(filewatch *fw, struct inotify_event *ie) {
  if (ie->mask & (IN_CREATE | IN_MOVED_TO)) {
    if (ie->mask & IN_ISDIR) {
      if (!create_watch(fw, find_dir(fw, ie->wd, NULL)->path, ie->name)) {
        log_warn("unable to create watch for newly created directory: '%s'",
                 ie->name);
      }
    }
  }
}

bool filewatch_poll(filewatch *fw, filewatch_event *e) {
  if (fw->bufsize == 0) {
    i32 read_ret = read(fw->fd, fw->buf, sizeof fw->buf);
    if (read_ret < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return false;
      }

      char msg[100];
      strerror_r(errno, msg, sizeof msg);
      log_error("unable to read inotify event for filewatch: %s", msg);
      return false;
    }

    if (read_ret == 0) {
      return false;
    }

    fw->bufsize = read_ret;
  }

  struct inotify_event *ie = (struct inotify_event *)fw->buf;
  const char *dir_path = find_dir(fw, ie->wd, NULL)->path;
  e->isdir = ie->mask & IN_ISDIR;
  e->name = path_concat(dir_path, ie->name, e->isdir);
  e->name_len = (i32)ie->len - 1;
  e->created = ie->mask & IN_CREATE;
  e->deleted = ie->mask & IN_DELETE;
  e->movedto = ie->mask & IN_MOVED_TO;
  e->movedfrom = ie->mask & IN_MOVED_FROM;
  e->modified = ie->mask & IN_MODIFY;
  preprocess_event(fw, ie);
  i32 in_event_size = sizeof(*ie) + ie->len;
  memmove(fw->buf, &fw->buf[in_event_size], fw->bufsize - in_event_size);
  fw->bufsize -= in_event_size;

  return true;
}

void filewatch_free_event(filewatch *fw, filewatch_event *e) {
  (void)fw;
  free(e->name);
}
