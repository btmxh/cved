#include "fs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool is_pathsep(char c) { return c == '/'; }

char pathsep() { return '/'; }

char *path_concat(const char *parent, const char *child, bool isdir) {
  assert(!is_pathsep(child[0]) && "child must not be an absolute path");
  i32 parent_len = strlen(parent);
  assert(parent_len > 0 && "parent and child must be non-empty paths");
  char *ret;

  char fmt[8];
  char *ptr = fmt;
  *(ptr++) = '%';
  *(ptr++) = 's';
  if (parent[parent_len - 1] != '/') {
    *(ptr++) = '/';
  }
  *(ptr++) = '%';
  *(ptr++) = 's';
  if (isdir) {
    *(ptr++) = '/';
  }
  *ptr = '\0';
  assert(ptr <= fmt + sizeof fmt);

  return asprintf(&ret, fmt, parent, child) == -1 ? NULL : ret;
}

char *path_absolute(const char *path) { return realpath(path, NULL); }
