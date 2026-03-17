#ifndef SERVER_COMMANDS_H
#define SERVER_COMMANDS_H

#include <pthread.h>
#include "session.h"

#define MAX_REQ 128
#define MAX_USERS 128
#define MAX_NOTIF 64
#define MAX_MSG 256

typedef struct {
    int id;
    char src_user[64];
    char dst_user[64];
    char src_abs[512];
    char src_name[256];
    int pending;
} transfer_req_t;

typedef struct {
    char user[64];
    int head;
    int tail;
    char msgs[MAX_NOTIF][MAX_MSG];
} notif_queue_t;

typedef struct {
    pthread_mutex_t mu;
    int online_count[MAX_USERS];
    pthread_cond_t user_online_cv[MAX_USERS];
    int next_id;
    transfer_req_t reqs[MAX_REQ];
    notif_queue_t notifs[MAX_USERS];
} shared_t;

extern shared_t *g_shared;

int dispatch_command(int client_fd, session_t *session, const char *root_dir, char *buffer);

void flush_notifications(int client_fd, const char *username);
void online_dec_user(const char *user);

#endif
