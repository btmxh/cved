#pragma once

#include "types.h"

bool is_pathsep(char c);
char pathsep();
char *path_concat(const char *parent, const char *child, bool isdir);
char *path_absolute(const char *path);
