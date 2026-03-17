#include "path_utils.h"

#include <string.h>
#include <stdio.h>

void build_path(const char *base, const char *arg, char *out, size_t outsz) {
    if (!arg || !*arg) {
        snprintf(out, outsz, "%s", base);
        return;
    }
    if (arg[0] == '/') {
        snprintf(out, outsz, "%s", arg);
        return;
    }
    if (strcmp(base, "/") == 0) snprintf(out, outsz, "/%s", arg);
    else snprintf(out, outsz, "%s/%s", base, arg);
}

int in_home(const char *home_dir, const char *resolved) {
    size_t hlen = strlen(home_dir);
    if (strncmp(resolved, home_dir, hlen) != 0) return 0;
    if (resolved[hlen] == '\0') return 1;
    if (resolved[hlen] == '/') return 1;
    return 0;
}
