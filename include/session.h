#ifndef SESSION_H
#define SESSION_H

#include <sys/types.h>

typedef struct {
    char current_user[64];
    char home_dir[256];
    char current_dir[256];
    uid_t uid;
    gid_t gid;
    uid_t root_uid;
    gid_t root_gid;
} session_t;

void init_session(session_t *s);
int is_logged_in(const session_t *s);

#endif
