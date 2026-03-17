#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

#include <sys/types.h>

#define MAX_BG 64

typedef struct {
    pid_t pid;
    int is_upload;
    char server_path[512];
    char client_path[512];
} bg_job_t;

extern bg_job_t bg_jobs[MAX_BG];
extern char last_user[64];

void bg_add(pid_t pid, int is_upload, const char *server_path, const char *client_path);
void bg_poll_and_print(void);
int connect_to_server(const char *ip, int port);

#endif
