#include "client_commands.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdint.h>
#include "endian_compat.h"
#include <errno.h>

#include "utils.h"
#include "transfers.h"
#include "client_state.h"

static int fetch_remote_pwd(int sock, char *out, size_t out_sz) {
    char reply[512];
    if (writen(sock, "pwd\n", 4) < 0) return -1;
    if (read_line(sock, reply, sizeof(reply)) <= 0) return -1;
    if (strncmp(reply, "PWD: ", 5) != 0) return -1;

    char *p = reply + 5;
    p[strcspn(p, "\r\n")] = '\0';
    if (p[0] == '\0') return -1;
    snprintf(out, out_sz, "%s", p);
    return 0;
}

static int extract_write_path(const char *line, char *out_path, size_t out_sz) {
    if (!line || strncmp(line, "write", 5) != 0) return -1;

    const char *p = line + 5;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return -1;

    if (strncmp(p, "-offset=", 8) == 0) {
        while (*p && *p != ' ' && *p != '\t') p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') return -1;
    }

    snprintf(out_path, out_sz, "%s", p);
    out_path[strcspn(out_path, "\r\n")] = '\0';
    for (int i = (int)strlen(out_path) - 1; i >= 0; i--) {
        if (out_path[i] == ' ' || out_path[i] == '\t') out_path[i] = '\0';
        else break;
    }
    if (out_path[0] == '\0') return -1;
    return 0;
}

static int preview_existing_for_write(int sock, const char *path) {
    char read_cmd[1024];
    snprintf(read_cmd, sizeof(read_cmd), "read %s\n", path);

    if (writen(sock, read_cmd, strlen(read_cmd)) < 0) return -1;

    char reply[256];
    if (read_line(sock, reply, sizeof(reply)) <= 0) return -1;

    if (strncmp(reply, "OK: ready", 9) != 0) {
        if (strncmp(reply, "ERR: file not found", 19) == 0) {
            printf("Current content: <new file>\n");
            return 0;
        }
        printf("Current content unavailable: %s", reply);
        return 0;
    }

    uint64_t net_size;
    if (readn(sock, &net_size, sizeof(net_size)) != (ssize_t)sizeof(net_size)) return -1;
    uint64_t size = be64toh(net_size);

    printf("Current content (%llu bytes):\n", (unsigned long long)size);
    if (size == 0) {
        printf("<empty>\n");
    } else {
        char chunk[4096];
        uint64_t left = size;
        while (left > 0) {
            size_t want = left > sizeof(chunk) ? sizeof(chunk) : (size_t)left;
            ssize_t r = readn(sock, chunk, want);
            if (r <= 0) return -1;
            write(STDOUT_FILENO, chunk, (size_t)r);
            left -= (uint64_t)r;
        }
        if (size > 0) printf("\n");
    }

    if (read_line(sock, reply, sizeof(reply)) <= 0) return -1;
    return 0;
}

 static int read_text_until_eof(char **out_buf, size_t *out_len) {
        size_t cap = 4096;
        size_t len = 0;
        char *buf = malloc(cap);
        if (!buf) return -1;

        char tmp[1024];
        while (1) {
            size_t n = fread(tmp, 1, sizeof(tmp), stdin);
            if (n > 0) {
                if (len + n > cap) {
                    while (len + n > cap) cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (!nb) { free(buf); return -1; }
                    buf = nb;
                }
                memcpy(buf + len, tmp, n);
                len += n;
            }
            if (n < sizeof(tmp)) {
                if (ferror(stdin)) { free(buf); return -1; }
                if (feof(stdin)) break;
            }
        }

        clearerr(stdin);
        *out_buf = buf;
        *out_len = len;
        return 0;
    }

    static int drain_and_print_reply(int sock) {
        char buf[1024];

        while (1) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);

            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 150000;  // 150ms

            int rv = select(sock + 1, &rfds, NULL, NULL, &tv);
            if (rv < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (rv == 0) {
                return 0; // nothing more to read
            }

            ssize_t n = read(sock, buf, sizeof(buf) - 1);
            if (n <= 0) return -1;

            buf[n] = '\0';
            printf("%s", buf);
            fflush(stdout);
        }
    }

int handle_input_line(int sock, const char *ip_str, int port, char *buffer) {
    
    // WRITE <path> or write -offset=N <path>
    if (strncmp(buffer, "write", 5) == 0) {
        printf("Edit content. Finish with Ctrl + D.\n");
        char target_path[512];
        if (extract_write_path(buffer, target_path, sizeof(target_path)) == 0) {
            if (preview_existing_for_write(sock, target_path) < 0) return 0;
        }

        // 1) send command line (exactly one newline)
        chomp_newline(buffer);
        if (writen(sock, buffer, strlen(buffer)) < 0) return 0;
        if (writen(sock, "\n", 1) < 0) return 0;

        // 2) expect "OK: ready"
        char reply[256];
        if (read_line(sock, reply, sizeof(reply)) <= 0) return 0;
        if (strncmp(reply, "OK: ready", 9) != 0) {
            printf("Server: %s", reply);
            return 0;
        }

        // 3) read stdin until "."
        char *payload = NULL;
        size_t payload_len = 0;

        if (read_text_until_eof(&payload, &payload_len) < 0) {
            printf("ERR: cannot read input\n");
            return 0;
        }

        // 4) send size
        uint64_t net_size = htobe64((uint64_t)payload_len);
        if (writen(sock, &net_size, sizeof(net_size)) != sizeof(net_size)) {
            free(payload);
            return 0;
        }

        // 5) send data
        if (payload_len > 0) {
            if (writen(sock, payload, payload_len) < 0) {
                free(payload);
                return 0;
            }
        }
        free(payload);

        // 6) final OK
        if (read_line(sock, reply, sizeof(reply)) <= 0) return 0;
        printf("\n\nServer: %s", reply);

        return 0;
    }

    // READ <path> or read -offset=N <path>
    if (strncmp(buffer, "read", 4) == 0) {
        // send command line
        chomp_newline(buffer);
        if (writen(sock, buffer, strlen(buffer)) < 0) return 1;
        if (writen(sock, "\n", 1) < 0) return 1;

        // expect OK: ready
        char reply[256];
        if (read_line(sock, reply, sizeof(reply)) <= 0) return 1;
        if (strncmp(reply, "OK: ready", 9) != 0) {
            printf("Server: %s", reply);
            return 0;
        }

        // read size (uint64)
        uint64_t net_size;
        if (readn(sock, &net_size, sizeof(net_size)) != (ssize_t)sizeof(net_size)) return 1;
        uint64_t size = be64toh(net_size);

        // read <size> bytes and print to stdout
        char chunk[4096];
        uint64_t left = size;
        while (left > 0) {
            size_t want = left > sizeof(chunk) ? sizeof(chunk) : (size_t)left;
            ssize_t r = readn(sock, chunk, want);
            if (r <= 0) return 1;
            write(STDOUT_FILENO, chunk, (size_t)r);
            left -= (uint64_t)r;
        }

        // final OK line
        if (read_line(sock, reply, sizeof(reply)) <= 0) return 1;
        printf("\n\nServer: %s", reply);

        return 0;
    }

    /*
    upload -b <client_path> <server_path>
    Parent continues interactive session.
    Child opens a new connection and sends file in background.
    */
    if (strncmp(buffer, "upload -b ", 10) == 0) {
        char client_path[512], server_path[512];
        char remote_pwd[512];
        int have_remote_pwd = 0;

        if (sscanf(buffer + 10, "%511s %511s", client_path, server_path) != 2) {
            printf("Usage: upload -b <client_path> <server_path>\n");
            return 0;
        }
        if (fetch_remote_pwd(sock, remote_pwd, sizeof(remote_pwd)) == 0) {
            have_remote_pwd = 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 0;
        }

        if (pid == 0) {
            // CHILD: do the transfer on its own socket
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) {
                dup2(dn, STDOUT_FILENO);
                dup2(dn, STDERR_FILENO);
                close(dn);
            }
            int s2 = connect_to_server(ip_str, port);
            if (s2 < 0) _exit(1);

            // Background connection is a NEW session -> must login again
            if (last_user[0] == '\0') _exit(1);

            char login_cmd[256];
            snprintf(login_cmd, sizeof(login_cmd), "login %s\n", last_user);
            if (writen(s2, login_cmd, strlen(login_cmd)) < 0) _exit(1);

            char reply[256];
            if (read_line(s2, reply, sizeof(reply)) <= 0) _exit(1);
            if (strncmp(reply, "OK", 2) != 0) _exit(1);

            if (have_remote_pwd) {
                char cd_cmd[1024];
                snprintf(cd_cmd, sizeof(cd_cmd), "cd %s\n", remote_pwd);
                if (writen(s2, cd_cmd, strlen(cd_cmd)) < 0) _exit(1);
                if (read_line(s2, reply, sizeof(reply)) <= 0) _exit(1);
                if (strncmp(reply, "OK: cd ", 7) != 0 && strncmp(reply, "OK: cd home", 11) != 0) _exit(1);
            }

            int rc = uploadFile_simple(s2, client_path, server_path);
            close(s2);
            _exit(rc == 0 ? 0 : 1);
        }

        // PARENT: remember this job so we print the concluded message later
        bg_add(pid, 1, server_path, client_path);
        return 0;
    }

   // ===== UPLOAD handling (client-side protocol) ===== 
    if (strncmp(buffer, "upload ", 7) == 0) {

        char client_path[512], server_path[512];

        if (sscanf(buffer + 7, "%511s %511s", client_path, server_path) != 2) {
            printf("Usage: upload <client_path> <server_path>\n");
            return 0;
        }

        if (uploadFile_simple(sock, client_path, server_path) < 0) {
            printf("ERR: upload failed\n");
        }

        return 0;
    }
    // ===== END upload handling =====

    /*
    download -b <server_path> <client_path>
    Child opens new connection and receives file in background.
    */
    if (strncmp(buffer, "download -b ", 12) == 0) {
        char server_path[512], client_path[512];
        char remote_pwd[512];
        int have_remote_pwd = 0;

        if (sscanf(buffer + 12, "%511s %511s", server_path, client_path) != 2) {
            printf("Usage: download -b <server_path> <client_path>\n");
            return 0;
        }
        if (fetch_remote_pwd(sock, remote_pwd, sizeof(remote_pwd)) == 0) {
            have_remote_pwd = 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 0;
        }

        if (pid == 0) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) {
                dup2(dn, STDOUT_FILENO);
                dup2(dn, STDERR_FILENO);
                close(dn);
            }

            int s2 = connect_to_server(ip_str, port);
            if (s2 < 0) _exit(1);

            // Background connection is a NEW session -> must login again
            if (last_user[0] == '\0') _exit(1);

            char login_cmd[256];
            snprintf(login_cmd, sizeof(login_cmd), "login %s\n", last_user);
            if (writen(s2, login_cmd, strlen(login_cmd)) < 0) _exit(1);

            char reply[256];
            if (read_line(s2, reply, sizeof(reply)) <= 0) _exit(1);
            if (strncmp(reply, "OK", 2) != 0) _exit(1);

            if (have_remote_pwd) {
                char cd_cmd[1024];
                snprintf(cd_cmd, sizeof(cd_cmd), "cd %s\n", remote_pwd);
                if (writen(s2, cd_cmd, strlen(cd_cmd)) < 0) _exit(1);
                if (read_line(s2, reply, sizeof(reply)) <= 0) _exit(1);
                if (strncmp(reply, "OK: cd ", 7) != 0 && strncmp(reply, "OK: cd home", 11) != 0) _exit(1);
            }

            int rc = downloadFile_simple(s2, server_path, client_path);
            close(s2);
            _exit(rc == 0 ? 0 : 1);
        }

        bg_add(pid, 0, server_path, client_path);
        return 0;
    }

    /* 
    ===== DOWNLOAD <server_path> <client_path> =============
    Foreground download (lecture-style).
    Protocol:
    1) client -> "download <server_path> <client_path>\n"
    2) server -> "OK: ready\n"
    3) server -> uint64_t size
    4) server -> bytes
    5) server -> "OK: download done\n"
    */
    if (strncmp(buffer, "download ", 9) == 0) {

        char cmdline[1024];
        strncpy(cmdline, buffer, sizeof(cmdline) - 1);
        cmdline[sizeof(cmdline) - 1] = '\0';
        chomp_newline(cmdline);

        char *arg = cmdline + 9;
        while (*arg == ' ') arg++;

        // Parse server_path
        char *server_path = arg;
        while (*arg && *arg != ' ') arg++;
        if (*arg == '\0') {
            printf("Usage: download <server_path> <client_path>\n");
            return 0;
        }
        *arg = '\0';
        arg++;
        while (*arg == ' ') arg++;

        // Parse client_path
        char *client_path = arg;
        if (*client_path == '\0') {
            printf("Usage: download <server_path> <client_path>\n");
            return 0;
        }

        if (downloadFile_simple(sock, server_path, client_path) < 0) {
            printf("ERR: download failed\n");
        }

        return 0;
    }
    // ===== END download handling ========

    int is_login = 0;
    char login_user[64] = "";
    if (strncmp(buffer, "login ", 6) == 0) {
        is_login = 1;
        sscanf(buffer + 6, "%63s", login_user);
    }

    chomp_newline(buffer);
    if (writen(sock, buffer, strlen(buffer)) < 0) {
        return 1;
    }
    if (writen(sock, "\n", 1) < 0) {
        return 1;
    }

    char reply[1024];
    if (read_line(sock, reply, sizeof(reply)) <= 0) {
        return 1;
    }

    printf("Server: %s", reply);

    // Remember last logged-in user
    if (is_login && login_user[0] != '\0' && strncmp(reply, "OK", 2) == 0) {
        strncpy(last_user, login_user, sizeof(last_user) - 1);
        last_user[sizeof(last_user) - 1] = '\0';
    }

   if (strncmp(reply, "OK: Ciao", 8) == 0) {
        // "quit" should only log out server-side; client keeps running.
        // Only "exit" in client.c terminates the client.
        return 0;
    }

    // print any extra lines (help/list)
    if (drain_and_print_reply(sock) < 0) {
        return 1;

    }
    return 0;
}
