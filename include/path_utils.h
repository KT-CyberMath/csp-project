#ifndef PATH_UTILS_H
#define PATH_UTILS_H

#include <stddef.h>

void build_path(const char *base, const char *arg, char *out, size_t outsz);
int in_home(const char *home_dir, const char *resolved);

#endif
