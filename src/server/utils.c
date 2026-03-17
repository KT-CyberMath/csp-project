#include "utils.h"

#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <sys/stat.h>   // mode_t
#include <sys/types.h>

#include <ctype.h>      // isalnum
#include <stdlib.h>     // strtol

#include <errno.h>

void send_msg(int fd, const char *msg) {
    write(fd, msg, strlen(msg));
}

void chomp_newline(char *s) {
    s[strcspn(s, "\r\n")] = '\0';
}

void ltrim(char *s) {
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

void reap_children(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

/*
Parse permissions in octal (example: "700", "755").
Returns 1 on success, 0 on failure.
*/
int parse_octal_perm(const char *s, mode_t *out) {
    if (!s || !*s) return 0;

    char *end = NULL;
    long v = strtol(s, &end, 8);     // base 8 (octal)

    if (end == s || *end != '\0') return 0;
    if (v < 0 || v > 0777) return 0;

    *out = (mode_t)v;
    return 1;
}

/*
Simple username validator:
allowed: letters, digits, underscore, dash
*/
int valid_username(const char *u) {
    if (!u || !*u) return 0;

    for (const char *p = u; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_' || c == '-')) return 0;
    }
    return 1;
}

ssize_t writen(int fd, const void *buf, size_t n) {
    size_t left = n;
    const char *p = buf;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        left -= (size_t)w;
        p += w;
    }
    return (ssize_t)n;
}

ssize_t readn(int fd, void *buf, size_t n) {
    size_t left = n;
    char *p = buf;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        left -= (size_t)r;
        p += r;
    }
    return (ssize_t)(n - left);
}

/* reads until '\n' or max-1 chars, stores '\0'. returns length, 0 on EOF, -1 on error */
int read_line(int fd, char *buf, size_t max) {
    size_t i = 0;
    while (i + 1 < max) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            if (i == 0) return 0;
            break;
        }
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return (int)i;
}
