#include "instance.h"
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)

int  lumen_instance_forward(const char *const *p, int n, int e) { (void)p; (void)n; (void)e; return 0; }
void lumen_instance_poll(lumen_playback_state_t *st) { (void)st; }
void lumen_instance_shutdown(void) {}

#else

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int g_listen_fd = -1;

static void socket_path(char *out, size_t n) {
    const char *rt = getenv("XDG_RUNTIME_DIR");       /* per-user, mode 0700 */
    if (rt && *rt) snprintf(out, n, "%s/qqvideo.sock", rt);
    else           snprintf(out, n, "/tmp/qqvideo-%u.sock", (unsigned)getuid());
}

static int make_addr(struct sockaddr_un *a) {
    char path[sizeof(a->sun_path) + 64];
    socket_path(path, sizeof(path));
    if (strlen(path) >= sizeof(a->sun_path)) return -1;
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    strcpy(a->sun_path, path);
    return 0;
}

int lumen_instance_forward(const char *const *paths, int n, int enqueue) {
    struct sockaddr_un a;
    if (make_addr(&a) != 0) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {   /* nobody listening */
        close(fd);
        return 0;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return 0; }
    for (int i = 0; i < n; i++) {
        char abs[PATH_MAX];
        const char *p = realpath(paths[i], abs) ? abs : paths[i];   /* our cwd isn't theirs */
        /* First file: "play now" unless the user prefers queuing; the rest queue. */
        fprintf(f, "%s %s\n", (i == 0 && !enqueue) ? "OPEN" : "ADD", p);
    }
    fprintf(f, "RAISE\n");
    fclose(f);
    printf("qqvideo: already running -- handed %d file(s) to it\n", n);
    return 1;
}

static void start_listening(void) {
    struct sockaddr_un a;
    if (make_addr(&a) != 0) return;
    /* A socket file with no listener behind it is left over from a crash:
     * probe it, and only remove it if nobody answers. */
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
        int alive = connect(probe, (struct sockaddr *)&a, sizeof(a)) == 0;
        close(probe);
        if (alive) return;              /* another instance owns it */
        unlink(a.sun_path);
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    mode_t old = umask(0077);           /* socket file: owner only */
    int ok = bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(fd, 8) == 0;
    umask(old);
    if (!ok) { close(fd); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    g_listen_fd = fd;
}

static void stop_listening(void) {
    if (g_listen_fd < 0) return;
    struct sockaddr_un a;
    close(g_listen_fd);
    g_listen_fd = -1;
    if (make_addr(&a) == 0) unlink(a.sun_path);
}

static void handle_line(lumen_playback_state_t *st, char *line) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    if (!strcmp(line, "RAISE")) {
        st->raise_window_requested = 1;
    } else if (!strncmp(line, "OPEN ", 5) && line[5]) {
        snprintf(st->open_file_pending_path, sizeof(st->open_file_pending_path), "%s", line + 5);
        st->open_file_requested = 1;       /* exactly what File > Open does */
    } else if (!strncmp(line, "ADD ", 4) && line[4] && st->pending_add_count < LUMEN_PENDING_QUEUE_MAX) {
        snprintf(st->pending_add_paths[st->pending_add_count], sizeof(st->pending_add_paths[0]), "%s", line + 4);
        st->pending_add_count++;           /* exactly what File > Add to Queue does */
    }
}

void lumen_instance_poll(lumen_playback_state_t *st) {
    if (st->pref_single_instance && g_listen_fd < 0) start_listening();
    if (!st->pref_single_instance && g_listen_fd >= 0) stop_listening();
    if (g_listen_fd < 0) return;
    for (;;) {
        int c = accept(g_listen_fd, NULL, NULL);
        if (c < 0) return;                 /* EAGAIN: nothing waiting */
        /* The sender writes a few short lines and closes; don't let a
         * misbehaving client stall the UI for more than a moment. */
        struct timeval tv = { 0, 200000 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        FILE *f = fdopen(c, "r");
        if (!f) { close(c); continue; }
        char line[600];
        while (fgets(line, sizeof(line), f)) handle_line(st, line);
        fclose(f);
    }
}

void lumen_instance_shutdown(void) { stop_listening(); }

#endif
