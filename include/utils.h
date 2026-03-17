#ifndef SERVER_UTILS_H
#define SERVER_UTILS_H

#include <sys/types.h>
#include <sys/stat.h>

// Basic helpers
void send_msg(int fd, const char *msg);      // write string to fd
void chomp_newline(char *s);                // remove trailing \r/\n
void ltrim(char *s);                        // remove leading spaces/tabs

// Process
void reap_children(int sig);                               // SIGCHLD handler

// Validation
int  parse_octal_perm(const char *s, mode_t *out);        // "755" -> mode
int  valid_username(const char *u);                       // allowed chars

// Robust I/O
ssize_t writen(int fd, const void *buf, size_t n);        // write exactly n
ssize_t readn(int fd, void *buf, size_t n);               // read up to n
int     read_line(int fd, char *buf, size_t max);         // read until '\n'

#endif
