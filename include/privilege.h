#ifndef PRIVILEGE_H
#define PRIVILEGE_H

#include "session.h"

int drop_to_user(session_t *s);
void restore_root(session_t *s);

#endif
