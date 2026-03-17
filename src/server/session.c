#include "session.h"

#include <string.h>
#include <unistd.h>

void init_session(session_t *s) {
    if (!s) return;
    s->current_user[0] = '\0';
    s->home_dir[0] = '\0';
    s->current_dir[0] = '\0';
    s->uid = (uid_t)-1;
    s->gid = (gid_t)-1;
    s->root_uid = geteuid();
    s->root_gid = getegid();
}

//unused
int is_logged_in(const session_t *s) {
    return s && s->current_user[0] != '\0';
}
