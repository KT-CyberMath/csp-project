#include "privilege.h"

#include <unistd.h>

int drop_to_user(session_t *s) {
    if (!s || s->uid == (uid_t)-1 || s->gid == (gid_t)-1) return -1;
    if (setegid(s->gid) < 0) return -1;
    if (seteuid(s->uid) < 0) return -1;
    return 0;
}

void restore_root(session_t *s) {
    if (!s) return;
    seteuid(s->root_uid);
    setegid(s->root_gid);
}
