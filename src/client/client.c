#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>
#include "endian_compat.h"
#include "utils.h"
#include "transfers.h"
#include "client_state.h"
#include "client_commands.h"
#include <sys/wait.h>
#include <signal.h>

#define PORT 8080

/*
Client:
- Interactive command loop
- Foreground transfers: blocking protocol
- Background transfers (-b): fork + new socket
*/
// bg_jobs array

bg_job_t bg_jobs[MAX_BG];
char last_user[64] = "";

// Save info so we can print the required message when the child finishes
void bg_add(pid_t pid, int is_upload, const char *server_path, const char *client_path) {
    for (int i = 0; i < MAX_BG; i++) {
        if (bg_jobs[i].pid == 0) {
            bg_jobs[i].pid = pid;
            bg_jobs[i].is_upload = is_upload;

            strncpy(bg_jobs[i].server_path, server_path, sizeof(bg_jobs[i].server_path) - 1);
            bg_jobs[i].server_path[sizeof(bg_jobs[i].server_path) - 1] = '\0';

            strncpy(bg_jobs[i].client_path, client_path, sizeof(bg_jobs[i].client_path) - 1);
            bg_jobs[i].client_path[sizeof(bg_jobs[i].client_path) - 1] = '\0';
            return;
        }
    }
}

/*
Check if any background child finished.
We do polling with waitpid(WNOHANG) so we do NOT print from a signal handler.
This is simple + safe.
*/
void bg_poll_and_print(void) {
    int status = 0;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_BG; i++) {
            if (bg_jobs[i].pid == pid) {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    if (bg_jobs[i].is_upload) {
                        printf("\n[Background] Command: upload %s %s concluded\n",
                            bg_jobs[i].server_path, bg_jobs[i].client_path);
                    } else {
                        printf("\n[Background] Command: download %s %s concluded\n",
                            bg_jobs[i].server_path, bg_jobs[i].client_path);
                    }
                    fflush(stdout);
                } else if (WIFEXITED(status)) {
                    if (bg_jobs[i].is_upload) {
                        printf("\n[Background] Command: upload %s %s failed\n",
                            bg_jobs[i].server_path, bg_jobs[i].client_path);
                    } else {
                        printf("\n[Background] Command: download %s %s failed\n",
                            bg_jobs[i].server_path, bg_jobs[i].client_path);
                    }
                    fflush(stdout);
                }
                bg_jobs[i].pid = 0;     // mark slot free
                break;
            }
        }
    }
}

static int poll_server_notifications(int sock) {
    int printed = 0;
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        int rv = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0) return printed;

        char buf[1024];
        ssize_t n = read(sock, buf, sizeof(buf) - 1);
        if (n <= 0) return printed;
        buf[n] = '\0';
        printf("\n%s\n", buf);
        fflush(stdout);
        printed = 1;
    }
}

static int bg_has_active(void) {
    for (int i = 0; i < MAX_BG; i++) {
        if (bg_jobs[i].pid != 0) return 1;
    }
    return 0;
}

/*
Background transfers must NOT reuse the interactive socket,
otherwise file bytes can mix with normal command replies.
So the child opens its own socket to the same server.
*/
int connect_to_server(const char *ip, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(s);
        return -1;
    }

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }

    return s;
}

// ===== Main starts here ========
int main(int argc, char *argv[]) {
    // Simple client: send command, print reply
    int sock;
    struct sockaddr_in addr;
    char buffer[1024];

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <IP> <port>\n", argv[0]);
        exit(1);
    }

    const char *ip_str = argv[1];
    int port = atoi(argv[2]);

    // Create TCP socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip_str, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        exit(1);
    }

    // Connect to local server
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        exit(1);
    }

    // Simple request/response loop
    while (1) {
        /* Print any finished background jobs */
        bg_poll_and_print();
        poll_server_notifications(sock);
        printf("> ");
        fflush(stdout);

        if (!fgets(buffer, sizeof(buffer), stdin)) {
            break;
        }

        if (strncmp(buffer, "exit", 4) == 0) {
            // remove newline
            buffer[strcspn(buffer, "\r\n")] = '\0';

            if (strcmp(buffer, "exit") == 0) {
                if (bg_has_active()) {
                    printf("ERR: background operations still running\n");
                    continue; // do not exit, return to prompt
                }
                break; // exit client
            }
        }

        if (handle_input_line(sock, ip_str, port, buffer)) {
            break;
        }
    }

    close(sock);
    return 0;
}
