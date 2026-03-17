#include "server_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <stdint.h>
#include "endian_compat.h"

#include "utils.h"
#include "privilege.h"
#include "path_utils.h"

static int lock_file_fd(int fd, short lock_type, int wait) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = lock_type;   // F_RDLCK or F_WRLCK
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;            // 0 means whole file
    if (wait) return fcntl(fd, F_SETLKW, &fl);
    return fcntl(fd, F_SETLK, &fl);
}

static void unlock_file_fd(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fcntl(fd, F_SETLK, &fl);
}

static int notif_find_slot_locked(const char *user, int create) {
    int empty = -1;
    for (int i = 0; i < MAX_USERS; i++) {
        if (g_shared->notifs[i].user[0] == '\0') {
            if (empty == -1) empty = i;
            continue;
        }
        if (strncmp(g_shared->notifs[i].user, user, sizeof(g_shared->notifs[i].user)) == 0) {
            return i;
        }
    }
    if (create && empty != -1) {
        strncpy(g_shared->notifs[empty].user, user, sizeof(g_shared->notifs[empty].user) - 1);
        g_shared->notifs[empty].user[sizeof(g_shared->notifs[empty].user) - 1] = '\0';
        g_shared->notifs[empty].head = 0;
        g_shared->notifs[empty].tail = 0;
        return empty;
    }
    return -1;
}

static int online_idx_locked(const char *user, int create) {
    return notif_find_slot_locked(user, create);
}

void online_dec_user(const char *user) {
    if (!user || user[0] == '\0') return;
    pthread_mutex_lock(&g_shared->mu);
    int idx = online_idx_locked(user, 0);
    if (idx >= 0 && g_shared->online_count[idx] > 0) {
        g_shared->online_count[idx]--;
        pthread_cond_broadcast(&g_shared->user_online_cv[idx]);
    }
    pthread_mutex_unlock(&g_shared->mu);
}

static void notif_push(const char *user, const char *msg) {
    if (!user || user[0] == '\0' || !msg) return;
    pthread_mutex_lock(&g_shared->mu);
    int idx = notif_find_slot_locked(user, 1);
    if (idx < 0) {
        pthread_mutex_unlock(&g_shared->mu);
        return;
    }
    int next = (g_shared->notifs[idx].tail + 1) % MAX_NOTIF;
    if (next == g_shared->notifs[idx].head) {
        g_shared->notifs[idx].head = (g_shared->notifs[idx].head + 1) % MAX_NOTIF;
    }
    strncpy(g_shared->notifs[idx].msgs[g_shared->notifs[idx].tail], msg, MAX_MSG - 1);
    g_shared->notifs[idx].msgs[g_shared->notifs[idx].tail][MAX_MSG - 1] = '\0';
    g_shared->notifs[idx].tail = next;
    pthread_mutex_unlock(&g_shared->mu);
}

void flush_notifications(int client_fd, const char *username) {
    if (!username || username[0] == '\0') return;
    char local[MAX_NOTIF][MAX_MSG];
    int count = 0;

    pthread_mutex_lock(&g_shared->mu);
    int idx = notif_find_slot_locked(username, 0);
    if (idx < 0) {
        pthread_mutex_unlock(&g_shared->mu);
        return;
    }
    while (g_shared->notifs[idx].head != g_shared->notifs[idx].tail && count < MAX_NOTIF) {
        strncpy(local[count], g_shared->notifs[idx].msgs[g_shared->notifs[idx].head], MAX_MSG - 1);
        local[count][MAX_MSG - 1] = '\0';
        g_shared->notifs[idx].head = (g_shared->notifs[idx].head + 1) % MAX_NOTIF;
        count++;
    }
    pthread_mutex_unlock(&g_shared->mu);

    for (int i = 0; i < count; i++) {
        send_msg(client_fd, local[i]);
        send_msg(client_fd, "\n");
    }
}

static int req_create_locked(const char *src_user, const char *dst_user,
                             const char *src_abs, const char *src_name, int *out_id) {
    for (int i = 0; i < MAX_REQ; i++) {
        if (!g_shared->reqs[i].pending) {
            g_shared->reqs[i].id = g_shared->next_id++;
            strncpy(g_shared->reqs[i].src_user, src_user, sizeof(g_shared->reqs[i].src_user) - 1);
            g_shared->reqs[i].src_user[sizeof(g_shared->reqs[i].src_user) - 1] = '\0';
            strncpy(g_shared->reqs[i].dst_user, dst_user, sizeof(g_shared->reqs[i].dst_user) - 1);
            g_shared->reqs[i].dst_user[sizeof(g_shared->reqs[i].dst_user) - 1] = '\0';
            strncpy(g_shared->reqs[i].src_abs, src_abs, sizeof(g_shared->reqs[i].src_abs) - 1);
            g_shared->reqs[i].src_abs[sizeof(g_shared->reqs[i].src_abs) - 1] = '\0';
            strncpy(g_shared->reqs[i].src_name, src_name, sizeof(g_shared->reqs[i].src_name) - 1);
            g_shared->reqs[i].src_name[sizeof(g_shared->reqs[i].src_name) - 1] = '\0';
            g_shared->reqs[i].pending = 1;
            *out_id = g_shared->reqs[i].id;
            return 0;
        }
    }
    return -1;
}

static int req_take(int id, const char *dst_user, transfer_req_t *out) {
    int rc = -1;
    pthread_mutex_lock(&g_shared->mu);
    for (int i = 0; i < MAX_REQ; i++) {
        if (g_shared->reqs[i].pending && g_shared->reqs[i].id == id) {
            if (strcmp(g_shared->reqs[i].dst_user, dst_user) != 0) {
                rc = -2;
                break;
            }
            *out = g_shared->reqs[i];
            g_shared->reqs[i].pending = 0;
            rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_shared->mu);
    return rc;
}

int dispatch_command(int client_fd, session_t *session, const char *root_dir, char *buffer) {
    if (strcmp(buffer, "exit") == 0) {
        send_msg(client_fd, "OK: client disconnecting\n");
        return 1;
    }

    // CD
    if (strcmp(buffer, "cd") == 0 || strncmp(buffer, "cd ", 3) == 0) {               
        if (session->current_user[0] == '\0') {
            const char *msg = "ERR: not logged in\n";
            write(client_fd, msg, strlen(msg));
            return 0;
        }

        char *arg = buffer + 2;
        while (*arg == ' ') arg++;
        arg[strcspn(arg, "\r\n")] = '\0';

        // cd with no arg goes home
        if (arg[0] == '\0') {
            strncpy(session->current_dir, session->home_dir, sizeof(session->current_dir) - 1);
            session->current_dir[sizeof(session->current_dir) - 1] = '\0';
            const char *msg = "OK: cd home\n";
            write(client_fd, msg, strlen(msg));
            return 0;
        }
        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        char candidate[512];
        build_path(session->current_dir, arg, candidate, sizeof(candidate));

        char resolved[512];
        if (realpath(candidate, resolved) == NULL) {
            const char *msg = "ERR: directory not found\n";
            write(client_fd, msg, strlen(msg));
            restore_root(session);
            return 0;
        }

        struct stat st;
        if (stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) {
            const char *msg = "ERR: not a directory\n";
            write(client_fd, msg, strlen(msg));
            restore_root(session);
            return 0;
        }

        // sandbox: must stay under home_dir
        size_t hlen = strlen(session->home_dir);
        if (strncmp(resolved, session->home_dir, hlen) != 0 || (resolved[hlen] != '\0' && resolved[hlen] != '/')) {
            const char *msg = "ERR: cannot leave home directory\n";
            write(client_fd, msg, strlen(msg));
            restore_root(session);
            return 0;
        }

        strncpy(session->current_dir, resolved, sizeof(session->current_dir) - 1);
        session->current_dir[sizeof(session->current_dir) - 1] = '\0';

        char reply[600];
        snprintf(reply, sizeof(reply), "OK: cd %s\n", session->current_dir);
        write(client_fd, reply, strlen(reply));
        if (dropped) restore_root(session);
        return 0;
    }

    // CREATE_USER: create_user <username> <perm-octal>
    if (strcmp(buffer, "create_user") == 0 || strncmp(buffer, "create_user ", 12) == 0) {
        char *arg = buffer + 12;
        while (*arg == ' ') arg++;

        char *username = arg;
        while (*arg && *arg != ' ') arg++;
        if (*arg == '\0') {
            send_msg(client_fd, "ERR: usage create_user <username> <perm>\n");
            return 0;
        }
        *arg = '\0';
        arg++;
        while (*arg == ' ') arg++;
        char *perm_s = arg;

        if (!valid_username(username)) {
            send_msg(client_fd, "ERR: invalid username\n");
            return 0;
        }

        mode_t perm;
        if (!parse_octal_perm(perm_s, &perm)) {
            send_msg(client_fd, "ERR: invalid permissions (octal, e.g. 700)\n");
            return 0;
        }
        perm &= 0770;

        struct group *grp = getgrnam("csapgroup");
        if (!grp) {
            send_msg(client_fd, "ERR: csapgroup missing\n");
            return 0;
        }

        char home_path[512];
        snprintf(home_path, sizeof(home_path), "%s/%s", root_dir, username);

        // 1) create directory if missing
        if (mkdir(home_path, 0755) < 0 && errno != EEXIST) {
            send_msg(client_fd, "ERR: cannot create home directory\n");
            return 0;
        }

        // 2) ensure OS user exists (slide hint)
        struct passwd *pw = getpwnam(username);
        if (!pw) {
        pid_t p = fork();
        if (p == 0) {
            execlp("adduser", "adduser",
                   "--disabled-password",
                   "--gecos", "",
                   "--ingroup", "csapgroup",
                   "--no-create-home",
                   "--shell", "/usr/sbin/nologin",
                   username, (char*)NULL);
            _exit(1);
        }
            int st = 0;
            waitpid(p, &st, 0);

            pw = getpwnam(username);
            if (!pw) {
                send_msg(client_fd, "ERR: cannot create system user (need sudo?)\n");
                return 0;
            }
        }

        // 3) set owner + group
        if (chown(home_path, pw->pw_uid, grp->gr_gid) < 0) {
            send_msg(client_fd, "ERR: chown failed (need sudo?)\n");
            return 0;
        }

        // 4) set permissions
        if (chmod(home_path, perm) < 0) {
            send_msg(client_fd, "ERR: chmod failed\n");
            return 0;
        }

        send_msg(client_fd, "OK: user created\n");
        return 0;
    }

    // LOGIN (existing users only)
    if (strncmp(buffer, "login ", 6) == 0) {
        char *username = buffer + 6;
        chomp_newline(username);

        if (session->current_user[0] != '\0') {
            send_msg(client_fd, "ERR: already logged in (use quit first)\n");
            return 0;
        }

        if (!valid_username(username)) {
            send_msg(client_fd, "ERR: invalid username\n");
            return 0;
        }

        char home_path[512];
        snprintf(home_path, sizeof(home_path), "%s/%s", root_dir, username);

        struct stat st;
        if (stat(home_path, &st) < 0 || !S_ISDIR(st.st_mode)) {
            send_msg(client_fd, "ERR: user not found (run create_user first)\n");
            return 0;
        }

        // optional: ensure OS user exists too
        struct passwd *pw = getpwnam(username);
        if (!pw) {
            send_msg(client_fd, "ERR: system user missing (run create_user)\n");
            return 0;
        }

        strncpy(session->current_user, username, sizeof(session->current_user) - 1);    
        session->current_user[sizeof(session->current_user) - 1] = '\0';

        strncpy(session->home_dir, home_path, sizeof(session->home_dir) - 1);
        session->home_dir[sizeof(session->home_dir) - 1] = '\0';

        strncpy(session->current_dir, home_path, sizeof(session->current_dir) - 1);
        session->current_dir[sizeof(session->current_dir) - 1] = '\0';

        session->uid = pw->pw_uid;
        session->gid = pw->pw_gid;

        pthread_mutex_lock(&g_shared->mu);
        int idx = online_idx_locked(session->current_user, 1);
        if (idx >= 0) {
            g_shared->online_count[idx]++;
            pthread_cond_broadcast(&g_shared->user_online_cv[idx]);
        }
        pthread_mutex_unlock(&g_shared->mu);

        char reply[512];
        snprintf(reply, sizeof(reply), "OK: logged in as %s, home=%s\n", session->current_user, session->home_dir);
        send_msg(client_fd, reply);
        flush_notifications(client_fd, session->current_user);
        return 0;
    }

    // PWD
    if (strncmp(buffer, "pwd", 3) == 0) {
        if (session->current_user[0] == '\0') {
            write(client_fd, "ERR: not logged in\n", 19);
        } else {
            char reply[300];
            snprintf(reply, sizeof(reply), "PWD: %s\n", session->current_dir);
            write(client_fd, reply, strlen(reply));
        }
        return 0;
    }

    // LIST
    if (strcmp(buffer, "list") == 0 || strncmp(buffer, "list ", 5) == 0) {
        if (session->current_user[0] == '\0') {
            write(client_fd, "ERR: not logged in\n", 19);
            return 0;
        }

        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        char *arg = buffer + 4;
        while (*arg == ' ') arg++;

        char target[512];
        if (*arg == '\0') {
            strncpy(target, session->current_dir, sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0';
        } else {
            build_path(session->current_dir, arg, target, sizeof(target));
        }

        char resolved_dir[512];
        if (realpath(target, resolved_dir) == NULL) {
            write(client_fd, "ERR: cannot open directory\n", 27);
            restore_root(session);
            return 0;
        }

        struct stat st_dir;
        if (stat(resolved_dir, &st_dir) < 0 || !S_ISDIR(st_dir.st_mode)) {
            write(client_fd, "ERR: cannot open directory\n", 27);
            restore_root(session);
            return 0;
        }

        if (!in_home(root_dir, resolved_dir)) {
            write(client_fd, "ERR: cannot open directory\n", 27);
            restore_root(session);
            return 0;
        }

        DIR *dir = opendir(resolved_dir);
        if (!dir) {
            write(client_fd, "ERR: cannot open directory\n", 27);
            restore_root(session);
            return 0;
        }

        struct dirent *entry;
        struct stat st;
        char path[512];
        char reply[4096];
        reply[0] = '\0';

        int count = 0;
        while ((entry = readdir(dir)) != NULL) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;

            snprintf(path, sizeof(path), "%s/%s", resolved_dir, entry->d_name);
            if (stat(path, &st) < 0) continue;

            char line[256];
            int perm = (int)(st.st_mode & 0777);
            if (S_ISDIR(st.st_mode))
                snprintf(line, sizeof(line), "DIR  %s (%03o) (%ld bytes)\n",
                        entry->d_name, perm, (long)st.st_size);
            else
                snprintf(line, sizeof(line), "FILE %s (%03o) (%ld bytes)\n",
                        entry->d_name, perm, (long)st.st_size);

            strncat(reply, line, sizeof(reply) - strlen(reply) - 1);
            count++;
        }

        closedir(dir);

        if (count == 0) strcpy(reply, "EMPTY\n");
        write(client_fd, reply, strlen(reply));
        if (dropped) restore_root(session);
        return 0;
    }

    // RMDIR
    if (strcmp(buffer, "rmdir") == 0 || strncmp(buffer, "rmdir ", 6) == 0) {
        if (session->current_user[0] == '\0') {
            write(client_fd, "ERR: not logged in\n", 19);
            return 0;
        }

        char *arg = buffer + 5;
        while (*arg == ' ') arg++;
        arg[strcspn(arg, "\r\n")] = '\0';

        if (arg[0] == '\0') {
            write(client_fd, "ERR: rmdir needs directory name\n", 32);
            return 0;
        }

        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        char candidate[512];
        build_path(session->current_dir, arg, candidate, sizeof(candidate));

        char resolved[512];
        if (realpath(candidate, resolved) == NULL) {
            write(client_fd, "ERR: directory not found\n", 25);
            restore_root(session);
            return 0;
        }

        // sandbox check
        size_t hlen = strlen(session->home_dir);
        if (strncmp(resolved, session->home_dir, hlen) != 0 ||
            (resolved[hlen] != '\0' && resolved[hlen] != '/')) {
            write(client_fd, "ERR: cannot leave home directory\n", 33);
            restore_root(session);
            return 0;
        }

        struct stat st;
        if (stat(resolved, &st) < 0 || !S_ISDIR(st.st_mode)) {
            write(client_fd, "ERR: not a directory\n", 22);
            restore_root(session);
            return 0;
        }

        if (rmdir(resolved) < 0) {
            write(client_fd, "ERR: directory not empty or failed\n", 36);
            restore_root(session);
            return 0;
        }

        write(client_fd, "OK: directory removed\n", 22);
        if (dropped) restore_root(session);
        return 0;
    }

    // MOVE
    if (strcmp(buffer, "move") == 0 || strncmp(buffer, "move ", 5) == 0) {
        if (session->current_user[0] == '\0') {
            write(client_fd, "ERR: not logged in\n", 19);
            return 0;
        }

        char *arg = buffer + 4;
        while (*arg == ' ') arg++;
        arg[strcspn(arg, "\r\n")] = '\0';
        if (*arg == '\0') {
            send_msg(client_fd, "ERR: usage read <path> or read -offset=N <path>\n");
            return 0;
        }

        if (arg[0] == '\0') {
            write(client_fd, "ERR: move needs <old> <new>\n", 28);
            return 0;
        }

        // split into two tokens
        char *oldname = arg;
        while (*arg && *arg != ' ') arg++;
        if (*arg == '\0') {
            write(client_fd, "ERR: move needs <old> <new>\n", 28);
            return 0;
        }
        *arg = '\0';
        arg++;
        while (*arg == ' ') arg++;
        char *newname = arg;

        if (newname[0] == '\0') {
            write(client_fd, "ERR: move needs <old> <new>\n", 28);
            return 0;
        }

        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        // resolve old path inside sandbox
        char oldcand[512], oldres[512];
        build_path(session->current_dir, oldname, oldcand, sizeof(oldcand));
        if (realpath(oldcand, oldres) == NULL) {
            write(client_fd, "ERR: source not found\n", 22);
            restore_root(session);
            return 0;
        }

        // resolve destination as: current_dir/newname (must stay in home)
        char newcand[512], newres[512];
        build_path(session->current_dir, newname, newcand, sizeof(newcand));
        if (realpath(newcand, newres) == NULL) {
            // destination might not exist yet: resolve parent directory instead allow rename as long as parent is in sandbox
            char parentcand[512], parentres[512];
            strncpy(parentcand, newcand, sizeof(parentcand) - 1);
            parentcand[sizeof(parentcand) - 1] = '\0';

            char *slash = strrchr(parentcand, '/');
            if (slash) *slash = '\0';

            if (realpath(parentcand, parentres) == NULL) {
                write(client_fd, "ERR: destination path invalid\n", 30);
                restore_root(session);
                return 0;
            }

            // build newres manually (parentres + "/" + basename)
            const char *base = strrchr(newcand, '/');
            base = base ? (base + 1) : newcand;
            snprintf(newres, sizeof(newres), "%s/%s", parentres, base);
        }

        // sandbox check for both old and new
        size_t hlen = strlen(session->home_dir);
        if (strncmp(oldres, session->home_dir, hlen) != 0 ||
            (oldres[hlen] != '\0' && oldres[hlen] != '/') ||
            strncmp(newres, session->home_dir, hlen) != 0 ||
            (newres[hlen] != '\0' && newres[hlen] != '/')) {
            write(client_fd, "ERR: cannot leave home directory\n", 33);
            restore_root(session);
            return 0;
        }

        if (rename(oldres, newres) < 0) {
            write(client_fd, "ERR: rename failed\n", 19);
            restore_root(session);
            return 0;
        }

        write(client_fd, "OK: renamed\n", 12);
        if (dropped) restore_root(session);
        return 0;
    }

    // STAT
    if (strcmp(buffer, "stat") == 0 || strncmp(buffer, "stat ", 5) == 0) {
        if (session->current_user[0] == '\0') {
            write(client_fd, "ERR: not logged in\n", 19);
            return 0;
        }

        char *arg = buffer + 4;
        while (*arg == ' ') arg++;
        arg[strcspn(arg, "\r\n")] = '\0';

        if (arg[0] == '\0') {
            write(client_fd, "ERR: stat needs filename\n", 25);
            return 0;
        }

        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        char candidate[512];
        build_path(session->current_dir, arg, candidate, sizeof(candidate));

        char resolved[512];
        if (realpath(candidate, resolved) == NULL) {
            write(client_fd, "ERR: file not found\n", 20);
            restore_root(session);
            return 0;
        }

        // sandbox check
        size_t hlen = strlen(session->home_dir);
        if (strncmp(resolved, session->home_dir, hlen) != 0 ||
            (resolved[hlen] != '\0' && resolved[hlen] != '/')) {
            write(client_fd, "ERR: cannot leave home directory\n", 33);
            restore_root(session);
            return 0;
        }

        struct stat st;
        if (stat(resolved, &st) < 0) {
            write(client_fd, "ERR: stat failed\n", 17);
            restore_root(session);
            return 0;
        }

        char reply[256];
        snprintf(reply, sizeof(reply),
                "OK: %s | size=%ld bytes | perms=%o\n",
                S_ISDIR(st.st_mode) ? "DIR" : "FILE",
                (long)st.st_size,
                st.st_mode & 0777);

        write(client_fd, reply, strlen(reply));
        if (dropped) restore_root(session);
        return 0;
    }

    // CHMOD: chmod <path> <perm-octal>
    if (strcmp(buffer, "chmod") == 0 || strncmp(buffer, "chmod ", 6) == 0) {
        if (session->current_user[0] == '\0') {
            send_msg(client_fd, "ERR: not logged in\n");
            return 0;
        }

        char *arg = buffer + 5;
        while (*arg == ' ') arg++;
        arg[strcspn(arg, "\r\n")] = '\0';

        if (arg[0] == '\0') {
            send_msg(client_fd, "ERR: usage chmod <path> <perm-octal>\n");
            return 0;
        }

        // split: <path> <perm>
        char *path_s = arg;
        while (*arg && *arg != ' ') arg++;
        if (*arg == '\0') {
            send_msg(client_fd, "ERR: usage chmod <path> <perm-octal>\n");
            return 0;
        }
        *arg = '\0';
        arg++;
        while (*arg == ' ') arg++;
        char *perm_s = arg;

        if (perm_s[0] == '\0') {
            send_msg(client_fd, "ERR: usage chmod <path> <perm-octal>\n");
            return 0;
        }

        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        mode_t perm;
        if (!parse_octal_perm(perm_s, &perm)) {
            send_msg(client_fd, "ERR: invalid permissions (octal, e.g. 700)\n");
            restore_root(session);
            return 0;
        }
        perm &= 0770;

        // build candidate path relative to current_dir
        char candidate[512];
        build_path(session->current_dir, path_s, candidate, sizeof(candidate));

        char resolved[512];
        if (realpath(candidate, resolved) == NULL) {
            send_msg(client_fd, "ERR: path not found\n");
            restore_root(session);
            return 0;
        }

        // sandbox check
        if (!in_home(session->home_dir, resolved)) {
            send_msg(client_fd, "ERR: cannot leave home directory\n");
            restore_root(session);
            return 0;
        }

        if (chmod(resolved, perm) < 0) {
            send_msg(client_fd, "ERR: chmod failed\n");
            restore_root(session);
            return 0;
        }

        send_msg(client_fd, "OK: chmod done\n");
        if (dropped) restore_root(session);
        return 0;
    }

    /* 
    UPLOAD <client_path> <server_path>
     Client sends a file to the server.
     Protocol (foreground):
     1) client -> "upload <client_path> <server_path>\n"
     2) server -> "OK: ready\n"
     3) client -> uint64_t size (network order)
     4) client -> <size> raw bytes
     5) server -> "OK: upload done\n" 
     */
    if (strcmp(buffer, "upload") == 0 || strncmp(buffer, "upload ", 7) == 0) {
        if (session->current_user[0] == '\0') {
            send_msg(client_fd, "ERR: not logged in\n");
            return 0;
        }

        // Parse: upload <client_path> <server_path>
        char *arg = buffer + 7;
        while (*arg == ' ') arg++;

        while (*arg && *arg != ' ') arg++;
        if (*arg == '\0') {
            send_msg(client_fd, "ERR: usage upload <client_path> <server_path>\n");
            return 0;
        }
        *arg = '\0';
        arg++;
        while (*arg == ' ') arg++;

        char *server_rel = arg;
        if (server_rel[0] == '\0') {
            send_msg(client_fd, "ERR: usage upload <client_path> <server_path>\n");
            return 0;
        }

        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        // Destination path is relative to current_dir
        char dest_path[512];
        build_path(session->current_dir, server_rel, dest_path, sizeof(dest_path));

        /* 
        Sandbox check:
         If file does not exist yet, realpath(dest_path) fails.
        So we realpath() the parent directory and ensure it is inside home_dir. 
        */
        char parent_path[512];
        strncpy(parent_path, dest_path, sizeof(parent_path) - 1);
        parent_path[sizeof(parent_path) - 1] = '\0';

        char *slash = strrchr(parent_path, '/');
        if (!slash) {
            send_msg(client_fd, "ERR: destination invalid\n");
            return 0;
        }
        *slash = '\0';

        char resolved_parent[512];
        if (realpath(parent_path, resolved_parent) == NULL) {
            send_msg(client_fd, "ERR: destination path invalid\n");
            restore_root(session);
            return 0;
        }
        if (!in_home(session->home_dir, resolved_parent)) {
            send_msg(client_fd, "ERR: cannot leave home directory\n");
            restore_root(session);
            return 0;
        }

        // Create/truncate the destination file (permissions can be adjusted later via chmod command)
        int out = open(dest_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (out < 0) {
            send_msg(client_fd, "ERR: cannot open destination\n");
            restore_root(session);
            return 0;
        }
        if (fchmod(out, 0660) < 0) {
            close(out);
            send_msg(client_fd, "ERR: chmod failed\n");
            restore_root(session);
            return 0;
        }
        if (lock_file_fd(out, F_WRLCK, 0) < 0) {
            close(out);
            if (errno == EACCES || errno == EAGAIN) {
                send_msg(client_fd, "ERR: file busy\n");
            } else {
                send_msg(client_fd, "ERR: lock failed\n");
            }
            restore_root(session);
            return 0;
        }

        // Handshake: tell client we are ready to receive the binary payload
        send_msg(client_fd, "OK: ready\n");

        // Read file size (uint64_t, network order)
        uint64_t net_size;
        if (readn(client_fd, &net_size, sizeof(net_size)) != (ssize_t)sizeof(net_size)) {
            unlock_file_fd(out);
            close(out);
            send_msg(client_fd, "ERR: failed to read size\n");
            return 0;
        }
        uint64_t size = be64toh(net_size);

        // Receive exactly <size> bytes and write them to disk
        char chunk[4096];
        uint64_t left = size;
        while (left > 0) {
            size_t want = left > sizeof(chunk) ? sizeof(chunk) : (size_t)left;

            ssize_t r = readn(client_fd, chunk, want);
            if (r <= 0) {
                unlock_file_fd(out);
                close(out);
                send_msg(client_fd, "ERR: upload failed\n");
                restore_root(session);
                goto upload_done;
            }

            if (writen(out, chunk, (size_t)r) < 0) {
                unlock_file_fd(out);
                close(out);
                send_msg(client_fd, "ERR: write failed\n");
                restore_root(session);
                goto upload_done;
            }

            left -= (uint64_t)r;
        }

        unlock_file_fd(out);
        close(out);
        send_msg(client_fd, "OK: upload done\n");
        if (dropped) restore_root(session);

        upload_done:
        return 0;
    }

    /*
    DOWNLOAD <server_path> <client_path>
    Server sends a file to the client.
    Protocol:
    1) client -> "download <server_path> <client_path>\n"
    2) server -> "OK: ready\n"
    3) server -> uint64_t size (network order)
    4) server -> <size> raw bytes
    5) server -> "OK: download done\n"
    */
    if (strcmp(buffer, "download") == 0 || strncmp(buffer, "download ", 9) == 0) {
        if (session->current_user[0] == '\0') {
            send_msg(client_fd, "ERR: not logged in\n");
            return 0;
        }

        // Parse: download <server_path> <client_path>
        char *arg = buffer + 9;
        while (*arg == ' ') arg++;

        char *server_rel = arg;
        while (*arg && *arg != ' ') arg++;
        if (*arg == '\0') {
            send_msg(client_fd, "ERR: usage download <server_path> <client_path>\n");
            return 0;
        }
        *arg = '\0';
        arg++;
        while (*arg == ' ') arg++;

        char *client_path = arg; // not used by server, but required by spec
        if (client_path[0] == '\0') {
            send_msg(client_fd, "ERR: usage download <server_path> <client_path>\n");
            return 0;
        }

        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        // Resolve path relative to current_dir
        char candidate[512];
        build_path(session->current_dir, server_rel, candidate, sizeof(candidate));

        char resolved[512];
        if (realpath(candidate, resolved) == NULL) {
            send_msg(client_fd, "ERR: file not found\n");
            restore_root(session);
            return 0;
        }

        // Sandbox check: must stay under home_dir
        if (!in_home(session->home_dir, resolved)) {
            send_msg(client_fd, "ERR: cannot leave home directory\n");
            restore_root(session);
            return 0;
        }

        struct stat st;
        if (stat(resolved, &st) < 0 || S_ISDIR(st.st_mode)) {
            send_msg(client_fd, "ERR: not a file\n");
            restore_root(session);
            return 0;
        }

        int fd = open(resolved, O_RDONLY);
        if (fd < 0) {
            send_msg(client_fd, "ERR: open failed\n");
            restore_root(session);
            return 0;
        }

        // Handshake
        send_msg(client_fd, "OK: ready\n");

        // Send size (uint64_t, network order)
        uint64_t size = (uint64_t)st.st_size;
        uint64_t net_size = htobe64(size);
        if (writen(client_fd, &net_size, sizeof(net_size)) < 0) {
            close(fd);
            send_msg(client_fd, "ERR: send size failed\n");
            restore_root(session);
            return 0;
        }

        // Send file bytes in chunks
        char chunk[4096];
        uint64_t left = size;
        while (left > 0) {
            ssize_t r = read(fd, chunk, left > sizeof(chunk) ? sizeof(chunk) : (size_t)left);
            if (r <= 0) {
                close(fd);
                send_msg(client_fd, "ERR: file read failed\n");
                restore_root(session);
                goto download_done;
            }
            if (writen(client_fd, chunk, (size_t)r) < 0) {
                close(fd);
                send_msg(client_fd, "ERR: send failed\n");
                restore_root(session);
                goto download_done;
            }
            left -= (uint64_t)r;
        }

        close(fd);
        send_msg(client_fd, "OK: download done\n");
        if (dropped) restore_root(session);

        download_done:
        return 0;
    }

    // TRANSFER_REQUEST <file> <dest_user>
    if (strcmp(buffer, "transfer_request") == 0 || strncmp(buffer, "transfer_request ", 17) == 0) {
        if (session->current_user[0] == '\0') {// must be logged in to send transfer request
            send_msg(client_fd, "ERR: not logged in\n");
            return 0;
        }

        char *arg = buffer + 17;
        while (*arg == ' ') arg++;

        char *file_arg = arg;
        while (*arg && *arg != ' ') arg++;
        if (*arg == '\0') {
            send_msg(client_fd, "ERR: usage transfer_request <file> <dest_user>\n");
            return 0;
        }
        *arg = '\0';
        arg++;
        while (*arg == ' ') arg++;

        char *dest_user = arg;
        if (dest_user[0] == '\0') {
            send_msg(client_fd, "ERR: usage transfer_request <file> <dest_user>\n");
            return 0;
        }

        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        if (file_arg[0] == '/') {
            send_msg(client_fd, "ERR: absolute paths not allowed\n");
            restore_root(session);
            return 0;
        }

        char candidate[512];
        snprintf(candidate, sizeof(candidate), "%s/%s", session->current_dir, file_arg);

        char resolved[512];
        if (realpath(candidate, resolved) == NULL) {
            send_msg(client_fd, "ERR: file not found\n");
            restore_root(session);
            return 0;
        }

        if (!in_home(session->home_dir, resolved)) {
            send_msg(client_fd, "ERR: cannot leave home directory\n");
            restore_root(session);
            return 0;
        }

        struct stat st;
        if (stat(resolved, &st) < 0 || S_ISDIR(st.st_mode)) {
            send_msg(client_fd, "ERR: not a file\n");
            restore_root(session);
            return 0;
        }

        char dest_home[512];  //destination user's home directory check if exists (must exist to send transfer request)
        snprintf(dest_home, sizeof(dest_home), "%s/%s", root_dir, dest_user);
        struct stat dstst;
        if (stat(dest_home, &dstst) < 0 || !S_ISDIR(dstst.st_mode)) {
            send_msg(client_fd, "ERR: destination user not found\n");
            restore_root(session);
            return 0;
        }

        const char *base = strrchr(resolved, '/');
        base = base ? base + 1 : resolved;
        int req_id = 0;

        pthread_mutex_lock(&g_shared->mu);
        int idx = online_idx_locked(dest_user, 1); //ensure destinations user is online before sending request, if not online, wait until they are online to send the request (so they can receive the notification)
        if (idx < 0) {
            pthread_mutex_unlock(&g_shared->mu);
            send_msg(client_fd, "ERR: destination user not found\n");
            restore_root(session);
            return 0;
        }
        while (g_shared->online_count[idx] == 0) {
            pthread_cond_wait(&g_shared->user_online_cv[idx], &g_shared->mu);
        }
        if (req_create_locked(session->current_user, dest_user, resolved, base, &req_id) < 0) {
            pthread_mutex_unlock(&g_shared->mu);
            send_msg(client_fd, "ERR: transfer request queue full\n");
            restore_root(session);
            return 0;
        }
        pthread_mutex_unlock(&g_shared->mu);

        char reply[128];
        snprintf(reply, sizeof(reply), "OK: transfer_request id=%d\n", req_id);
        send_msg(client_fd, reply);

        char notify[512];
        snprintf(notify, sizeof(notify),
                 "TRANSFER REQUEST from %s file %s (id=%d) | accept: accept <directory> %d | reject: reject %d",
                 session->current_user, base, req_id, req_id, req_id);
        notif_push(dest_user, notify);
        if (dropped) restore_root(session);
        return 0;
    }

    // ACCEPT <directory> <ID>
    if (strcmp(buffer, "accept") == 0 || strncmp(buffer, "accept ", 7) == 0) {
        if (session->current_user[0] == '\0') {
            send_msg(client_fd, "ERR: not logged in\n");
            return 0;
        }

        char *arg = buffer + 7;
        while (*arg == ' ') arg++;

        char *dir_arg = arg;
        while (*arg && *arg != ' ') arg++;
        if (*arg == '\0') {
            send_msg(client_fd, "ERR: usage accept <directory> <ID>\n");
            return 0;
        }
        *arg = '\0';
        arg++;
        while (*arg == ' ') arg++;

        char *id_s = arg;
        if (id_s[0] == '\0') {
            send_msg(client_fd, "ERR: usage accept <directory> <ID>\n");
            return 0;
        }

        char *end = NULL;
        long id = strtol(id_s, &end, 10);
        if (end == id_s || *end != '\0' || id <= 0) {
            send_msg(client_fd, "ERR: invalid id\n");
            return 0;
        }

        transfer_req_t req;
        int take_rc = req_take((int)id, session->current_user, &req);
        if (take_rc == -1) {
            send_msg(client_fd, "ERR: invalid transfer id\n");
            return 0;
        }

        if (take_rc == -2) {
            send_msg(client_fd, "ERR: not authorized\n");
            return 0;
        }

        char dest_dir[512];
        build_path(session->current_dir, dir_arg, dest_dir, sizeof(dest_dir));

        char resolved_dir[512];
        if (realpath(dest_dir, resolved_dir) == NULL) {
            send_msg(client_fd, "ERR: destination path invalid\n");
            restore_root(session);
            return 0;
        }

        struct stat dstst;
        if (stat(resolved_dir, &dstst) < 0 || !S_ISDIR(dstst.st_mode)) {
            send_msg(client_fd, "ERR: not a directory\n");
            restore_root(session);
            return 0;
        }

        if (!in_home(session->home_dir, resolved_dir)) {
            send_msg(client_fd, "ERR: cannot leave home directory\n");
            restore_root(session);
            return 0;
        }

        char dst_path[512];
        snprintf(dst_path, sizeof(dst_path), "%s/%s", resolved_dir, req.src_name);

        int src_fd = -1;
        int dst_fd = -1;
        int src_locked = 0;
        int dst_locked = 0;
        int dropped = 0;
        int ok = 1;

        restore_root(session);
        src_fd = open(req.src_abs, O_RDONLY);
        if (src_fd < 0) {
            if (errno == EACCES) send_msg(client_fd, "ERR: permission denied\n");
            else if (errno == ENOENT) send_msg(client_fd, "ERR: file not found\n");
            else {
                char msg[128];
                snprintf(msg, sizeof(msg), "ERR: source open failed: %s\n", strerror(errno));
                send_msg(client_fd, msg);
            }
            goto accept_cleanup;
        }
        if (lock_file_fd(src_fd, F_RDLCK, 0) < 0) {
            if (errno == EACCES || errno == EAGAIN) send_msg(client_fd, "ERR: file busy\n");
            else send_msg(client_fd, "ERR: lock failed\n");
            goto accept_cleanup;
        }
        src_locked = 1;

        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            goto accept_cleanup;
        }
        dropped = 1;

        dst_fd = open(dst_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (dst_fd < 0) {
            if (errno == EACCES) send_msg(client_fd, "ERR: permission denied\n");
            else if (errno == ENOENT) send_msg(client_fd, "ERR: file not found\n");
            else send_msg(client_fd, "ERR: open failed\n");
            goto accept_cleanup;
        }
        if (fchmod(dst_fd, 0660) < 0) {
            send_msg(client_fd, "ERR: chmod failed\n");
            goto accept_cleanup;
        }
        if (lock_file_fd(dst_fd, F_WRLCK, 0) < 0) {
            if (errno == EACCES || errno == EAGAIN) send_msg(client_fd, "ERR: file busy\n");
            else send_msg(client_fd, "ERR: lock failed\n");
            goto accept_cleanup;
        }
        dst_locked = 1;

        char buf[4096];
        ssize_t r;
        while ((r = read(src_fd, buf, sizeof(buf))) > 0) {
            if (writen(dst_fd, buf, (size_t)r) < 0) {
                send_msg(client_fd, "ERR: write failed\n");
                ok = 0;
                goto accept_cleanup;
            }
        }
        if (r < 0) {
            send_msg(client_fd, "ERR: read failed\n");
            ok = 0;
            goto accept_cleanup;
        }

        accept_cleanup:
        if (dst_locked) unlock_file_fd(dst_fd);
        if (src_locked) unlock_file_fd(src_fd);
        if (dst_fd >= 0) close(dst_fd);
        if (src_fd >= 0) close(src_fd);
        if (dropped) restore_root(session);
        if (!ok) return 0;
        if (src_fd < 0 || dst_fd < 0) return 0;

        send_msg(client_fd, "OK: transfer accepted\n");

        char notify[512];
        snprintf(notify, sizeof(notify), "TRANSFER ACCEPTED by %s (id=%d)",
                 req.dst_user, req.id);
        notif_push(req.src_user, notify);
        return 0;
    }

    // REJECT <ID>
    if (strcmp(buffer, "reject") == 0 || strncmp(buffer, "reject ", 7) == 0) {
        if (session->current_user[0] == '\0') {
            send_msg(client_fd, "ERR: not logged in\n");
            return 0;
        }

        char *id_s = buffer + 7;
        while (*id_s == ' ') id_s++;
        if (*id_s == '\0') {
            send_msg(client_fd, "ERR: usage reject <ID>\n");
            return 0;
        }

        char *end = NULL;
        long id = strtol(id_s, &end, 10);
        if (end == id_s || *end != '\0' || id <= 0) {
            send_msg(client_fd, "ERR: invalid id\n");
            return 0;
        }

        transfer_req_t req;
        int take_rc = req_take((int)id, session->current_user, &req);
        if (take_rc == -1) {
            send_msg(client_fd, "ERR: invalid transfer id\n");
            return 0;
        }

        if (take_rc == -2) {
            send_msg(client_fd, "ERR: not authorized\n");
            return 0;
        }

        send_msg(client_fd, "OK: transfer rejected\n");

        char notify[512];
        snprintf(notify, sizeof(notify), "TRANSFER REJECTED by %s (id=%d)",
                 req.dst_user, req.id);
        notif_push(req.src_user, notify);
        return 0;
    }

    // READ <path> or read -offset=N <path>
    if (strcmp(buffer, "read") == 0 || strncmp(buffer, "read ", 5) == 0) {
        if (session->current_user[0] == '\0') {
            send_msg(client_fd, "ERR: not logged in\n");
            return 0;
        }

        char *arg = buffer + 4;
        while (*arg == ' ') arg++;
        arg[strcspn(arg, "\r\n")] = '\0';

        uint64_t offset = 0;

        // optional: -offset=N
        if (strncmp(arg, "-offset=", 8) == 0) {
            char *space = strchr(arg, ' ');
            if (!space) {
                send_msg(client_fd, "ERR: usage read -offset=N <path>\n");
                return 0;
            }

            *space = '\0';
            char *num = arg + 8;
            char *end = NULL;
            unsigned long long v = strtoull(num, &end, 10);
            if (end == num || *end != '\0') {
                send_msg(client_fd, "ERR: invalid offset\n");
                return 0;
            }
            offset = (uint64_t)v;

            arg = space + 1;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                send_msg(client_fd, "ERR: usage read -offset=N <path>\n");
                return 0;
            }
        }

        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        char candidate[512];
        build_path(session->current_dir, arg, candidate, sizeof(candidate));

        char resolved[512];
        if (realpath(candidate, resolved) == NULL) {
            send_msg(client_fd, "ERR: file not found\n");
            restore_root(session);
            return 0;
        }

        if (!in_home(session->home_dir, resolved)) {
            send_msg(client_fd, "ERR: cannot leave home directory\n");
            restore_root(session);
            return 0;
        }

        struct stat st;
        if (stat(resolved, &st) < 0 || S_ISDIR(st.st_mode)) {
            send_msg(client_fd, "ERR: not a file\n");
            restore_root(session);
            return 0;
        }

        int fd = open(resolved, O_RDONLY);
        if (fd < 0) {
            send_msg(client_fd, "ERR: open failed\n");
            restore_root(session);
            return 0;
        }
        if (lock_file_fd(fd, F_RDLCK, 0) < 0) {
            close(fd);
            if (errno == EACCES || errno == EAGAIN) {
                send_msg(client_fd, "ERR: file busy\n");
            } else {
                send_msg(client_fd, "ERR: lock failed\n");
            }
            restore_root(session);
            return 0;
        }

        uint64_t fsize = (uint64_t)st.st_size;
        if (offset > fsize) offset = fsize;

        if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
            unlock_file_fd(fd);
            close(fd);
            send_msg(client_fd, "ERR: lseek failed\n");
            restore_root(session);
            return 0;
        }

        uint64_t remaining = fsize - offset;

        send_msg(client_fd, "OK: ready\n");

        uint64_t net_size = htobe64(remaining);
        if (writen(client_fd, &net_size, sizeof(net_size)) < 0) {
            unlock_file_fd(fd);
            close(fd);
            send_msg(client_fd, "ERR: send size failed\n");
            restore_root(session);
            return 0;
        }

        int ok = 1;
        char chunk[4096];
        uint64_t left = remaining;
        while (left > 0) {
            size_t want = left > sizeof(chunk) ? sizeof(chunk) : (size_t)left;
            ssize_t r = read(fd, chunk, want);
            if (r <= 0) { ok = 0; break; }
            if (writen(client_fd, chunk, (size_t)r) < 0) { ok = 0; break; }
            left -= (uint64_t)r;
        }

        unlock_file_fd(fd);
        close(fd);
        if (!ok) {
            send_msg(client_fd, "ERR: read failed\n");
            restore_root(session);
            return 0;
        }
        send_msg(client_fd, "OK: read done\n");
        if (dropped) restore_root(session);
        return 0;
    }

    // WRITE <path> or write -offset=N <path>
    if (strcmp(buffer, "write") == 0 || strncmp(buffer, "write ", 6) == 0) {
        if (session->current_user[0] == '\0') {
            send_msg(client_fd, "ERR: not logged in\n");
            return 0;
        }

        char *arg = buffer + 5;
        while (*arg == ' ') arg++;
        arg[strcspn(arg, "\r\n")] = '\0';

        uint64_t offset = 0;
        int has_offset = 0;

        // Optional -offset=N
        if (strncmp(arg, "-offset=", 8) == 0) {
            char *space = strchr(arg, ' ');
            if (!space) {
                send_msg(client_fd, "ERR: usage write -offset=N <path>\n");
                return 0;
            }

            char *num = arg + 8;
            *space = '\0';

            char *end = NULL;
            unsigned long long v = strtoull(num, &end, 10);
            if (end == num || *end != '\0') {
                send_msg(client_fd, "ERR: invalid offset\n");
                return 0;
            }
            offset = (uint64_t)v;
            has_offset = 1;

            arg = space + 1;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                send_msg(client_fd, "ERR: usage write -offset=N <path>\n");
                return 0;
            }
        }

        if (arg[0] == '\0') {
            send_msg(client_fd, "ERR: usage write <path>\n");
            return 0;
        }

        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        // Build destination path relative to current_dir
        char dest[512];
        build_path(session->current_dir, arg, dest, sizeof(dest));

        char resolved[512];
        if (realpath(dest, resolved) != NULL) {
            if (!in_home(session->home_dir, resolved)) {
                send_msg(client_fd, "ERR: cannot leave home directory\n");
                restore_root(session);
                return 0;
            }
            strncpy(dest, resolved, sizeof(dest) - 1);
            dest[sizeof(dest) - 1] = '\0';
        } else {
            // Sandbox check: if file doesn't exist, realpath(dest) fails, so validate parent
            char parent[512];
            strncpy(parent, dest, sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';

            char *slash = strrchr(parent, '/');
            if (!slash) {
                send_msg(client_fd, "ERR: invalid path\n");
                return 0;
            }
            *slash = '\0';

            char resolved_parent[512];
            if (realpath(parent, resolved_parent) == NULL || !in_home(session->home_dir, resolved_parent)) {
                send_msg(client_fd, "ERR: cannot leave home directory\n");
                restore_root(session);
                return 0;
            }
        }

        int fd = open(dest, O_WRONLY, 0700);
        if (fd < 0 && errno == ENOENT) {
            fd = open(dest, O_WRONLY | O_CREAT, 0700);
        }
        if (fd < 0) {
            send_msg(client_fd, "ERR: open failed\n");
            restore_root(session);
            return 0;
        }
        if (lock_file_fd(fd, F_WRLCK, 0) < 0) {
            close(fd);
            if (errno == EACCES || errno == EAGAIN) {
                send_msg(client_fd, "ERR: file busy\n");
            } else {
                send_msg(client_fd, "ERR: lock failed\n");
            }
            restore_root(session);
            return 0;
        }

        if (!has_offset) {
            struct stat st;
            if (fstat(fd, &st) < 0) {
                unlock_file_fd(fd);
                close(fd);
                send_msg(client_fd, "ERR: stat failed\n");
                restore_root(session);
                return 0;
            }
            offset = (uint64_t)st.st_size;
        }

        // Handshake
        send_msg(client_fd, "OK: ready\n");

        // Read size
        uint64_t net_size;
        if (readn(client_fd, &net_size, sizeof(net_size)) != (ssize_t)sizeof(net_size)) {
            unlock_file_fd(fd);
            close(fd);
            send_msg(client_fd, "ERR: failed to read size\n");
            restore_root(session);
            return 0;
        }
        uint64_t size = be64toh(net_size);

        if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
            unlock_file_fd(fd);
            close(fd);
            send_msg(client_fd, "ERR: lseek failed\n");
            restore_root(session);
            return 0;
        }

        // Receive exactly <size> bytes
        char chunk[4096];
        uint64_t left = size;
        while (left > 0) {
            size_t want = left > sizeof(chunk) ? sizeof(chunk) : (size_t)left;
            ssize_t r = readn(client_fd, chunk, want);
            if (r <= 0) {
                unlock_file_fd(fd);
                close(fd);
                send_msg(client_fd, "ERR: write failed\n");
                restore_root(session);
                return 0;
            }
            if (writen(fd, chunk, (size_t)r) < 0) {
                unlock_file_fd(fd);
                close(fd);
                send_msg(client_fd, "ERR: write failed\n");
                restore_root(session);
                return 0;
            }
            left -= (uint64_t)r;
        }

        fsync(fd);
        unlock_file_fd(fd);
        close(fd);
        send_msg(client_fd, "OK: write done\n");
        if (dropped) restore_root(session);
        return 0;
    }

    // DELETE
    if (strcmp(buffer, "delete") == 0 || strncmp(buffer, "delete ", 7) == 0) {        
        if (session->current_user[0] == '\0') {
            send_msg(client_fd, "ERR: not logged in\n");
            return 0;
        }

        char *arg = buffer + 7;
        while (*arg == ' ') arg++;
        arg[strcspn(arg, "\r\n")] = '\0';

        if (arg[0] == '\0') {
            send_msg(client_fd, "ERR: delete needs filename\n");
            return 0;
        }

        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        char candidate[512];
        build_path(session->current_dir, arg, candidate, sizeof(candidate));

        char resolved[512];
        if (realpath(candidate, resolved) == NULL) {
            send_msg(client_fd, "ERR: file not found\n");
            restore_root(session);
            return 0;
        }

        if (!in_home(session->home_dir, resolved)) {
            send_msg(client_fd, "ERR: cannot leave home directory\n");
            restore_root(session);
            return 0;
        }

        struct stat st;
        if (stat(resolved, &st) < 0) {
            send_msg(client_fd, "ERR: file not found\n");
            restore_root(session);
            return 0;
        }

        if (S_ISDIR(st.st_mode)) {
            send_msg(client_fd, "ERR: cannot delete directory\n");
            restore_root(session);
            return 0;
        }

        int fd = open(resolved, O_RDWR);
        if (fd < 0) {
            send_msg(client_fd, "ERR: unlink failed\n");
            restore_root(session);
            return 0;
        }
        if (lock_file_fd(fd, F_WRLCK, 0) < 0) {
            close(fd);
            if (errno == EACCES || errno == EAGAIN) {
                send_msg(client_fd, "ERR: file busy\n");
            } else {
                send_msg(client_fd, "ERR: lock failed\n");
            }
            restore_root(session);
            return 0;
        }

        if (unlink(resolved) < 0) {
            unlock_file_fd(fd);
            close(fd);
            send_msg(client_fd, "ERR: unlink failed\n");
            restore_root(session);
            return 0;
        }

        unlock_file_fd(fd);
        close(fd);
        send_msg(client_fd, "OK: file removed\n");
        if (dropped) restore_root(session);
        return 0;
    }

    // CREATE: create <path> <perm-octal>
    // CREATE dir: create -d <path> <perm-octal>
    if (strcmp(buffer, "create") == 0 || strncmp(buffer, "create ", 7) == 0) {
        char *arg = buffer + 6;
        while (*arg == ' ') arg++;

        if (*arg == '\0') {
            send_msg(client_fd,
                "ERR: usage create <path> <permissions> or create -d <path> <permissions>\n");
            return 0;
        }

        if (session->current_user[0] == '\0') {
            send_msg(client_fd, "ERR: not logged in\n");
            return 0;
        }

        // Make a writable copy for token parsing
        char tmp[1024];
        strncpy(tmp, buffer + 7, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        chomp_newline(tmp);

        // Tokens: [-d] path perm
        char *t1 = strtok(tmp, " \t");
        char *t2 = strtok(NULL, " \t");
        char *t3 = strtok(NULL, " \t");

        int is_dir = 0;
        char *path_s = NULL;
        char *perm_s = NULL;

        if (!t1) {
            send_msg(client_fd, "ERR: usage create [-d] <path> <perm-octal>\n");
            return 0;
        }

        if (strcmp(t1, "-d") == 0) {
            is_dir = 1;
            path_s = t2;
            perm_s = t3;
        } else {
            path_s = t1;
            perm_s = t2;
        }

        if (!path_s || !perm_s) {
            send_msg(client_fd, "ERR: usage create [-d] <path> <perm-octal>\n");
            return 0;
        }

        int dropped = 0;
        if (drop_to_user(session) < 0) {
            send_msg(client_fd, "ERR: permission denied\n");
            return 0;
        }
        dropped = 1;

        mode_t perm;
        if (!parse_octal_perm(perm_s, &perm)) {
            send_msg(client_fd, "ERR: invalid permissions (octal, e.g. 700)\n");
            restore_root(session);
            return 0;
        }

        // Build destination path relative to current_dir
        char dest[512];
        build_path(session->current_dir, path_s, dest, sizeof(dest));

        // Sandbox check: realpath(dest) fails if it does not exist, so validate parent dir
        char parent[512];
        strncpy(parent, dest, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';

        char *slash = strrchr(parent, '/');
        if (slash) {
            // keep parent directory
            if (slash == parent) *(slash + 1) = '\0'; // parent is "/"
            else *slash = '\0';
        } else {
            // fallback: parent is current_dir
            strncpy(parent, session->current_dir, sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';
        }

        char resolved_parent[512];
        if (realpath(parent, resolved_parent) == NULL || !in_home(session->home_dir, resolved_parent)) {
            send_msg(client_fd, "ERR: cannot leave home directory\n");
            restore_root(session);
            return 0;
        }

        if (is_dir) {
            if (mkdir(dest, 0755) < 0) {
                if (errno == EEXIST) send_msg(client_fd, "ERR: already exists\n");
                else send_msg(client_fd, "ERR: mkdir failed\n");
                restore_root(session);
                return 0;
            }
            if (chmod(dest, perm) < 0) {
                send_msg(client_fd, "ERR: chmod failed\n");
                restore_root(session);
                return 0;
            }
            send_msg(client_fd, "OK: directory created\n");
            if (dropped) restore_root(session);
            return 0;
        } else {
            int fd = open(dest, O_CREAT | O_EXCL | O_WRONLY, 0600);
            if (fd < 0) {
                if (errno == EEXIST) send_msg(client_fd, "ERR: already exists\n");
                else send_msg(client_fd, "ERR: create failed\n");
                restore_root(session);
                return 0;
            }
            if (fchmod(fd, perm) < 0) {
                close(fd);
                send_msg(client_fd, "ERR: chmod failed\n");
                restore_root(session);
                return 0;
            }
            close(fd);
            send_msg(client_fd, "OK: file created\n");
            if (dropped) restore_root(session);
            return 0;
        }
    }
    // QUIT: logout user only
    if (strcmp(buffer, "quit") == 0) {
        if (session->current_user[0] != '\0') {
            pthread_mutex_lock(&g_shared->mu);
            int idx = online_idx_locked(session->current_user, 0);
            if (idx >= 0 && g_shared->online_count[idx] > 0) {
                g_shared->online_count[idx]--;
                pthread_cond_broadcast(&g_shared->user_online_cv[idx]);
            }
            pthread_mutex_unlock(&g_shared->mu);
        }
        session->current_user[0] = '\0';
        session->home_dir[0] = '\0';
        session->current_dir[0] = '\0';
        session->uid = (uid_t)-1;
        session->gid = (gid_t)-1;

        send_msg(client_fd, "OK: logged out\n");
        return 0; // keep connection alive
    }

    // HELP
    if (strcmp(buffer, "help") == 0) {
        const char *msg =
            "OK: commands\n"
            "exit\n"
            "cd <dir>\n"
            "cd ..\n"
            "create_user <username> <perm-octal>\n"
            "login <user>\n"
            "pwd\n"
            "list [path]\n"
            "rmdir <dir>\n"
            "move <path1> <path2>\n"
            "stat <file>\n"
            "chmod <path> <perm-octal>\n"
            "upload <client_path> <server_path>\n"
            "download <server_path> <client_path>\n"
            "transfer_request <file> <dest_user>\n"
            "accept <directory> <id>\n"
            "reject <id>\n"
            "read <path>\n"
            "read -offset=N <path>\n"
            "write <path>\n"
            "write -offset=N <path>\n"
            "delete <path>\n"
            "create <path> <permissions>\n"
            "create -d <path> <permissions>\n"
            "quit\n"
            "help\n";
        write(client_fd, msg, strlen(msg));
        return 0;
    }

    // DEFAULT: unknown command
    {
        send_msg(client_fd, "ERR: unknown command\n");
    }
    
    return 0;
}
