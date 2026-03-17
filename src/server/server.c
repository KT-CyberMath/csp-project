#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <grp.h>

#include "utils.h"
#include "session.h"
#include "server_commands.h"

shared_t *g_shared = NULL;
static volatile sig_atomic_t g_shutdown = 0;

static void handle_sigterm(int sig) {
    (void)sig;
    g_shutdown = 1;
}


/*
Server overview
1. Create TCP socket, bind, listen.
2. Loop: accept new client.
3. fork() for concurrency.
   Parent: closes client socket and continues accept.
   Child: closes listening socket, serves commands for that client only, then exits.
4. Each client keeps its own state: current_user, home_dir, current_dir.
5. Filesystem sandbox: all paths must stay inside home_dir.
*/


int main(int argc, char *argv[]) {

    // A) argument parsing & validation 
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "Usage: %s <root_directory> [IP] [port]\n", argv[0]);
        exit(1);
    }

    char root_dir[512];
    const char *ip_str = (argc >= 3) ? argv[2] : "127.0.0.1";
    int port = (argc >= 4) ? atoi(argv[3]) : 8080;

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port\n");
        exit(1);
    }

    int server_fd, client_fd;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    char buffer[1024];
    
    // B) setup server socket 
    if (setpgid(0, 0) < 0) {
        perror("setpgid");
    }

    signal(SIGCHLD, reap_children);
    signal(SIGTERM, handle_sigterm);

    // Normalize root_dir to absolute path
    char root_abs[512];
    char tmp[512];
    const char *root_in = argv[1];

    if (root_in[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", root_in);
    } else {
        char cwd[512];
        if (!getcwd(cwd, sizeof(cwd))) {
            perror("getcwd");
            exit(1);
        }
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, root_in);
    }

    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
        perror("mkdir root_dir");
        exit(1);
    }

    if (!realpath(tmp, root_abs)) {
        perror("realpath root_dir");
        exit(1);
    }

    strncpy(root_dir, root_abs, sizeof(root_dir) - 1);
    root_dir[sizeof(root_dir) - 1] = '\0';

    struct group *grp = getgrnam("csapgroup");
    if (!grp) {
        pid_t p = fork();
        if (p == 0) {
            execlp("addgroup", "addgroup", "csapgroup", (char*)NULL);
            _exit(1);
        }
        int st = 0;
        waitpid(p, &st, 0);
        grp = getgrnam("csapgroup");
        if (!grp) {
            perror("addgroup csapgroup");
            exit(1);
        }
    }

    g_shared = mmap(NULL, sizeof(shared_t),
                    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (g_shared == MAP_FAILED) {
        perror("mmap shared");
        exit(1);
    }
    memset(g_shared, 0, sizeof(shared_t));
    g_shared->next_id = 1;

    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0 ||
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0 ||
        pthread_mutex_init(&g_shared->mu, &attr) != 0) {
        perror("pthread_mutex_init");
        exit(1);
    }
    pthread_mutexattr_destroy(&attr);

    pthread_condattr_t cattr;
    if (pthread_condattr_init(&cattr) != 0 ||
        pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED) != 0) {
        perror("pthread_condattr_init");
        exit(1);
    }
    for (int i = 0; i < MAX_USERS; i++) {
        g_shared->online_count[i] = 0;
        if (pthread_cond_init(&g_shared->user_online_cv[i], &cattr) != 0) {
            perror("pthread_cond_init");
            exit(1);
        }
    }
    pthread_condattr_destroy(&cattr);

    // Create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip_str, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(server_fd);
        exit(1);
    }

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(1);
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        exit(1);
    }

    printf("Server listening on port %d...\n", port);

    // Main accept loop: fork a child per client
    while (!g_shutdown) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = (server_fd > STDIN_FILENO) ? server_fd : STDIN_FILENO;

        int rv = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rv < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char line[256];
            if (fgets(line, sizeof(line), stdin)) {
                line[strcspn(line, "\r\n")] = '\0';
                if (strcmp(line, "exit") == 0) {
                    printf("Server exiting on console command.\n");
                    g_shutdown = 1;
                    killpg(getpgrp(), SIGTERM);
                    break;
                }
            }
        }

        if (g_shutdown) {
            break;
        }

        if (!FD_ISSET(server_fd, &rfds)) {
            continue;
        }

        client_fd = accept(server_fd, (struct sockaddr *)&addr, &addrlen);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(client_fd);
            continue;

        }
        if (pid == 0) {
            // Child: handle this client connection; parent continues accepting
            close(server_fd);
            signal(SIGTERM, handle_sigterm);

            session_t session;
            init_session(&session);

            // Per-client command loop
            while (!g_shutdown) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(client_fd, &rfds);
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 200000; // 200ms

                int rv = select(client_fd + 1, &rfds, NULL, NULL, &tv);
                if (rv < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (rv == 0) {
                    if (session.current_user[0] != '\0') {
                        flush_notifications(client_fd, session.current_user);
                    }
                    continue;
                }

                int n = read_line(client_fd, buffer, sizeof(buffer));
                if (n <= 0) {
                    if (session.current_user[0] != '\0') {
                        online_dec_user(session.current_user);
                        session.current_user[0] = '\0';
                    }
                    break;  // client disconnected or error
                }
                ltrim(buffer);
                buffer[strcspn(buffer, "\r\n")] = '\0';

                if (dispatch_command(client_fd, &session, root_dir, buffer)) {
                    break;
                }

                if (session.current_user[0] != '\0') {
                    flush_notifications(client_fd, session.current_user);
                }
            }
             
        // Client disconnected; clean up and exit child
        if (session.current_user[0] != '\0') {
            online_dec_user(session.current_user);
        }
        close(client_fd);
        exit(0);
        }

    // Parent: close client socket (handled by child)
    close(client_fd);
    }
    
// only reached if you ever break out of while(1)
close(server_fd);
while (waitpid(-1, NULL, 0) > 0) {
}
return 0;
}
