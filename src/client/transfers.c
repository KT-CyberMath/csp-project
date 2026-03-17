#include "transfers.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>
#include "endian_compat.h"

#include "utils.h"

/*
transfers.c
Purpose: shared upload and download logic for the client.
Protocol is text command + binary payload:
Upload:
1) client -> "upload <client_path> <server_path>\n"
2) server -> "OK: ready\n"
3) client -> uint64_t size (big endian)
4) client -> <size> bytes
5) server -> "OK: upload done\n"

Download:
1) client -> "download <server_path> <client_path>\n"
2) server -> "OK: ready\n"
3) server -> uint64_t size (big endian)
4) server -> <size> bytes
5) server -> "OK: download done\n"
*/

int uploadFile_simple(int sock, const char *client_path, const char *server_path) {
    // 1) send upload command line
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "upload %s %s\n", client_path, server_path);
    if (writen(sock, cmd, strlen(cmd)) < 0) {
        perror("write");
        return -1;
    }

    // 2) wait for server readiness
    char reply[1024];
    int rn = read_line(sock, reply, sizeof(reply));
    if (rn <= 0) {
        printf("Server disconnected\n");
        return -1;
    }
    if (strncmp(reply, "OK: ready", 8) != 0) {
        printf("Server: %s", reply);
        return -1;
    }

    // 3) open local file and send its size
    int fd = open(client_path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return -1;
    }

    uint64_t size_net = htobe64((uint64_t)st.st_size);
    if (writen(sock, &size_net, sizeof(size_net)) != (ssize_t)sizeof(size_net)) {
        close(fd);
        return -1;
    }

    // 4) send file bytes
    char filebuf[4096];
    ssize_t r;
    while ((r = read(fd, filebuf, sizeof(filebuf))) > 0) {
        if (writen(sock, filebuf, (size_t)r) < 0) {
            close(fd);
            return -1;
        }
    }
    close(fd);
    if (r < 0) {
        return -1;
    }

    // 5) read final confirmation
    rn = read_line(sock, reply, sizeof(reply));
    if (rn <= 0) {
        printf("Server disconnected\n");
        return -1;
    }
    printf("Server: %s", reply);

    if (strncmp(reply, "OK", 2) != 0) {
        return -1;
    }

    return 0;
}

int downloadFile_simple(int sock, const char *server_path, const char *client_path) {
    // 1) send download command line
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "download %s %s\n", server_path, client_path);
    if (writen(sock, cmd, strlen(cmd)) < 0) {
        perror("write");
        return -1;
    }

    // 2) wait for server readiness
    char reply[1024];
    int rn = read_line(sock, reply, sizeof(reply));
    if (rn <= 0) {
        printf("Server disconnected\n");
        return -1;
    }
    if (strncmp(reply, "OK: ready", 9) != 0) {
        printf("Server: %s", reply);
        return -1;
    }

    // 3) read incoming size (big endian)
    uint64_t size_net;
    if (readn(sock, &size_net, sizeof(size_net)) != (ssize_t)sizeof(size_net)) {
        printf("ERR: failed to read size\n");
        return -1;
    }
    uint64_t size = be64toh(size_net);

    // 4) receive bytes and write them to local file
    int fd = open(client_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        printf("ERR: cannot create local file\n");
        return -1;
    }

    char chunk[4096];
    uint64_t left = size;
    while (left > 0) {
        size_t want = left > sizeof(chunk) ? sizeof(chunk) : (size_t)left;
        ssize_t r = readn(sock, chunk, want);
        if (r <= 0) {
            close(fd);
            printf("ERR: download failed\n");
            return -1;
        }
        if (writen(fd, chunk, (size_t)r) < 0) {
            close(fd);
            printf("ERR: local write failed\n");
            return -1;
        }
        left -= (uint64_t)r;
    }
    close(fd);

    // 5) read final confirmation
    rn = read_line(sock, reply, sizeof(reply));
    if (rn <= 0) {
        printf("Server disconnected\n");
        return -1;
    }
    printf("Server: %s", reply);

    if (strncmp(reply, "OK", 2) != 0) {
        return -1;
    }

    return 0;
}
