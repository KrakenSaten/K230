/*
 * Camera's helper process client. See camera_session.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "camera_session.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HELPER_DEFAULT "/usr/bin/pos-camera"
#ifndef CAMERA_BACKEND_DEFAULT
#define CAMERA_BACKEND_DEFAULT "v4l2"
#endif
/* Descriptors above stderr the shell may hold without close-on-exec (the
 * display, input devices, IPC sockets). The child closes them all but the
 * shared memory before exec; this is the highest it looks at. */
#define CHILD_FD_SCAN_MAX 1024
/* The largest still size a ready line may name. */
#define CAMERA_DIM_MAX 8192

void camera_session_init(struct camera_session *s)
{
    memset(s, 0, sizeof(*s));
    s->pid = -1;
    s->fd = -1;
    s->shm_fd = -1;
    s->frame_slot = -1;
    s->review_slot = -1;
}

const char *camera_session_helper_path(void)
{
    const char *p = getenv("POCKETOS_CAMERA_HELPER");

    return p && *p ? p : HELPER_DEFAULT;
}

const char *camera_session_backend(void)
{
    const char *p = getenv("POCKETOS_CAMERA_BACKEND");

    return p && *p ? p : CAMERA_BACKEND_DEFAULT;
}

bool camera_session_active(const struct camera_session *s)
{
    return s && s->running;
}

/* ---- the event queue ---------------------------------------------------------- */

static void push(struct camera_session *s, const struct camera_event *ev)
{
    if (s->q_count == CAMERA_EVENT_QUEUE) {
        /* Only stalls and damaged-frame counts come often enough to fill it;
         * the oldest event goes, and the count says so. */
        s->q_head = (s->q_head + 1) % CAMERA_EVENT_QUEUE;
        s->q_count--;
        s->dropped++;
    }
    s->queue[(s->q_head + s->q_count) % CAMERA_EVENT_QUEUE] = *ev;
    s->q_count++;
}

static int pop(struct camera_session *s, struct camera_event *ev)
{
    if (s->q_count == 0) {
        return 0;
    }
    *ev = s->queue[s->q_head];
    s->q_head = (s->q_head + 1) % CAMERA_EVENT_QUEUE;
    s->q_count--;
    if (ev->kind == CAMERA_EV_FRAME) {
        s->frame_queued = false;
    }
    return 1;
}

/* ---- parsing ------------------------------------------------------------------ */

static const char *after(const char *line, const char *word)
{
    size_t n = strlen(word);

    if (strncmp(line, word, n) != 0) {
        return NULL;
    }
    if (line[n] == '\0') {
        return line + n;
    }
    return line[n] == ' ' ? line + n + 1 : NULL;
}

/* The next space-separated word of *p into out; false when there is none or
 * it does not fit. */
static bool word(const char **p, char *out, size_t len)
{
    const char *s = *p;
    size_t n = 0;

    while (*s == ' ') {
        s++;
    }
    while (s[n] && s[n] != ' ') {
        n++;
    }
    if (n == 0 || n >= len) {
        return false;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    *p = s + n;
    return true;
}

static bool number(const char **p, uint64_t max, uint64_t *out)
{
    char w[24];
    char *end;
    unsigned long long v;

    if (!word(p, w, sizeof(w)) || w[0] < '0' || w[0] > '9') {
        return false;
    }
    errno = 0;
    v = strtoull(w, &end, 10);
    if (errno || *end || v > max) {
        return false;
    }
    *out = v;
    return true;
}

static void rest(const char *p, char *out, size_t len)
{
    while (*p == ' ') {
        p++;
    }
    snprintf(out, len, "%s", p);
}

/* A photo name as the helper can send one: a word of letters, digits, '_'
 * and '.', never a path. */
static bool name_ok(const char *name)
{
    size_t i;

    if (!name || !name[0] || name[0] == '.' || strlen(name) >= CAMERA_NAME_MAX) {
        return false;
    }
    for (i = 0; name[i]; i++) {
        char c = name[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

static int parse_library(const char *line, struct camera_event *ev)
{
    const char *a;
    uint64_t v[8];
    char w[CAMERA_NAME_MAX];
    int i;

    if ((a = after(line, "listed")) != NULL) {
        ev->kind = CAMERA_EV_LISTED;
        if (!number(&a, POCKETCAM_SLOTS - 1, &v[0]) || v[0] != POCKETCAM_LIST_SLOT ||
            !number(&a, CAMERA_LIBRARY_MAX, &v[1]) || !number(&a, 100000000, &v[2])) {
            return 0;
        }
        ev->value = (int)v[1];
        ev->bytes = v[2];
        return 1;
    }
    if ((a = after(line, "listfail")) != NULL) {
        ev->kind = CAMERA_EV_LISTFAIL;
        rest(a, ev->text, sizeof(ev->text));
        return 1;
    }
    if ((a = after(line, "image")) != NULL) {
        char taken[CAMERA_TAKEN_MAX + 4];
        char kind[8];

        ev->kind = CAMERA_EV_IMAGE;
        if (!number(&a, CAMERA_PICTURE_SLOTS - 1, &v[0]) ||
            !number(&a, POCKETCAM_VIEW_MAX_W, &v[1]) || !number(&a, POCKETCAM_VIEW_MAX_H, &v[2]) ||
            v[1] == 0 || v[2] == 0 || !number(&a, CAMERA_DIM_MAX, &v[3]) ||
            !number(&a, CAMERA_DIM_MAX, &v[4]) || !number(&a, UINT64_MAX / 2, &v[5]) ||
            !number(&a, INT64_MAX, &v[6]) || !number(&a, 1, &v[7]) ||
            !word(&a, kind, sizeof(kind)) || !word(&a, taken, sizeof(taken)) ||
            !word(&a, ev->name, sizeof(ev->name)) || !name_ok(ev->name)) {
            return 0;
        }
        ev->value = (int)v[0];
        ev->w = (uint32_t)v[1];
        ev->h = (uint32_t)v[2];
        ev->image.shown_w = (uint32_t)v[3];
        ev->image.shown_h = (uint32_t)v[4];
        ev->image.bytes = v[5];
        ev->image.mtime = (int64_t)v[6];
        ev->image.damaged = v[7] == 1;
        ev->image.jpeg = strcmp(kind, "jpg") == 0;
        if (strlen(taken) == CAMERA_TAKEN_MAX - 1 && taken[10] == 'T') {
            taken[10] = ' ';
            memcpy(ev->image.taken, taken, CAMERA_TAKEN_MAX);
        }
        rest(a, ev->text, sizeof(ev->text));
        return 1;
    }
    if ((a = after(line, "imgfail")) != NULL) {
        static const char *const words[] = { "missing", "corrupt", "unsupported", "toolarge" };

        ev->kind = CAMERA_EV_IMGFAIL;
        if (!number(&a, CAMERA_PICTURE_SLOTS - 1, &v[0]) ||
            !word(&a, ev->name, sizeof(ev->name)) || !word(&a, w, sizeof(w))) {
            return 0;
        }
        ev->value = (int)v[0];
        ev->reason = CAMERA_IMGFAIL_IO;
        for (i = 0; i < 4; i++) {
            if (strcmp(w, words[i]) == 0) {
                ev->reason = i;
            }
        }
        rest(a, ev->text, sizeof(ev->text));
        return 1;
    }
    if ((a = after(line, "exported")) != NULL) {
        ev->kind = CAMERA_EV_EXPORTED;
        if (!word(&a, ev->name, sizeof(ev->name)) || !number(&a, 1, &v[0])) {
            return 0;
        }
        ev->value = (int)v[0];
        rest(a, ev->path, sizeof(ev->path));
        return 1;
    }
    if ((a = after(line, "expfail")) != NULL) {
        ev->kind = CAMERA_EV_EXPFAIL;
        if (!word(&a, ev->name, sizeof(ev->name)) || !word(&a, w, sizeof(w))) {
            return 0;
        }
        ev->reason = strcmp(w, "exists") == 0    ? CAMERA_EXPFAIL_EXISTS
                     : strcmp(w, "nospace") == 0 ? CAMERA_EXPFAIL_NOSPACE
                     : strcmp(w, "missing") == 0 ? CAMERA_EXPFAIL_MISSING
                                                 : CAMERA_EXPFAIL_IO;
        rest(a, ev->text, sizeof(ev->text));
        return 1;
    }
    return 0;
}

int camera_session_parse_line(const char *line, struct camera_event *ev)
{
    const char *a;
    uint64_t v1;
    uint64_t v2;
    uint64_t v3;
    uint64_t v4;
    char w[CAMERA_NAME_MAX];

    memset(ev, 0, sizeof(*ev));
    if ((a = after(line, "ready")) != NULL) {
        ev->kind = CAMERA_EV_READY;
        if (!word(&a, ev->name, sizeof(ev->name)) || !number(&a, CAMERA_DIM_MAX, &v1) ||
            !number(&a, CAMERA_DIM_MAX, &v2) || !number(&a, 1, &v3) ||
            !number(&a, 1000000, &v4) || !word(&a, w, sizeof(w))) {
            return 0;
        }
        ev->w = (uint32_t)v1;
        ev->h = (uint32_t)v2;
        ev->simulated = v3 == 1;
        ev->value = (int)v4;
        snprintf(ev->text, sizeof(ev->text), "%s", strcmp(w, "-") == 0 ? "" : w);
        return 1;
    }
    if ((a = after(line, "nodevice")) != NULL) {
        ev->kind = CAMERA_EV_NODEVICE;
        rest(a, ev->text, sizeof(ev->text));
        return 1;
    }
    if ((a = after(line, "error")) != NULL && *a) {
        ev->kind = CAMERA_EV_ERROR;
        rest(a, ev->text, sizeof(ev->text));
        return 1;
    }
    if ((a = after(line, "frame")) != NULL) {
        ev->kind = CAMERA_EV_FRAME;
        if (!number(&a, POCKETCAM_SLOTS - 1, &v1) || !number(&a, UINT32_MAX, &v2) ||
            !number(&a, POCKETCAM_VIEW_MAX_W, &v3) || !number(&a, POCKETCAM_VIEW_MAX_H, &v4) ||
            v1 >= POCKETCAM_PREVIEW_SLOTS || v3 == 0 || v4 == 0) {
            return 0;
        }
        ev->value = (int)v1;
        ev->bytes = v2; /* the frame's sequence number */
        ev->w = (uint32_t)v3;
        ev->h = (uint32_t)v4;
        return 1;
    }
    if ((a = after(line, "stall")) != NULL) {
        ev->kind = CAMERA_EV_STALL;
        if (!number(&a, 100000000, &v1)) {
            return 0;
        }
        ev->value = (int)v1;
        return 1;
    }
    if ((a = after(line, "malformed")) != NULL) {
        ev->kind = CAMERA_EV_MALFORMED;
        if (!number(&a, 1000000, &v1)) {
            return 0;
        }
        ev->value = (int)v1;
        return 1;
    }
    if ((a = after(line, "stopped")) != NULL && *a == '\0') {
        ev->kind = CAMERA_EV_STOPPED;
        return 1;
    }
    if ((a = after(line, "saving")) != NULL && *a == '\0') {
        ev->kind = CAMERA_EV_SAVING;
        return 1;
    }
    if ((a = after(line, "captured")) != NULL) {
        uint64_t slot;
        uint64_t photos;

        ev->kind = CAMERA_EV_CAPTURED;
        if (!number(&a, POCKETCAM_SLOTS - 1, &slot) || slot != POCKETCAM_REVIEW_SLOT ||
            !number(&a, POCKETCAM_VIEW_MAX_W, &v1) || !number(&a, POCKETCAM_VIEW_MAX_H, &v2) ||
            (v1 == 0) != (v2 == 0) || !number(&a, UINT64_MAX / 2, &v3) ||
            !word(&a, ev->name, sizeof(ev->name)) || !number(&a, 1000000, &photos)) {
            return 0;
        }
        ev->w = (uint32_t)v1;
        ev->h = (uint32_t)v2;
        ev->bytes = v3;
        ev->value = (int)photos;
        return 1;
    }
    if ((a = after(line, "capfail")) != NULL) {
        ev->kind = CAMERA_EV_CAPFAIL;
        if (!word(&a, w, sizeof(w))) {
            return 0;
        }
        if (strcmp(w, "nospace") == 0) {
            ev->reason = CAMERA_CAPFAIL_NOSPACE;
        } else if (strcmp(w, "quota") == 0) {
            ev->reason = CAMERA_CAPFAIL_QUOTA;
        } else if (strcmp(w, "device") == 0) {
            ev->reason = CAMERA_CAPFAIL_DEVICE;
        } else {
            ev->reason = CAMERA_CAPFAIL_IO;
        }
        rest(a, ev->text, sizeof(ev->text));
        return 1;
    }
    if ((a = after(line, "deleted")) != NULL) {
        ev->kind = CAMERA_EV_DELETED;
        if (!word(&a, ev->name, sizeof(ev->name)) || !number(&a, 1000000, &v1)) {
            return 0;
        }
        ev->value = (int)v1;
        return 1;
    }
    if ((a = after(line, "delfail")) != NULL) {
        ev->kind = CAMERA_EV_DELFAIL;
        rest(a, ev->text, sizeof(ev->text));
        return 1;
    }
    if ((a = after(line, "lost")) != NULL) {
        ev->kind = CAMERA_EV_LOST;
        rest(a, ev->text, sizeof(ev->text));
        return 1;
    }
    return parse_library(line, ev);
}

/* ---- sending ------------------------------------------------------------------ */

static int send_line(struct camera_session *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static int send_line(struct camera_session *s, const char *fmt, ...)
{
    char line[POCKETCAM_LINE_MAX];
    va_list ap;
    int n;

    if (!s->running || s->fd < 0) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(line) - 1) {
        return -1;
    }
    line[n++] = '\n';
    /* A few dozen bytes into a socket the helper drains every 50 ms: this
     * never waits. MSG_NOSIGNAL, so a helper that has just died cannot take
     * the shell down with SIGPIPE; that death is reported by poll(). */
    return send(s->fd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) == n ? 0 : -1;
}

/* ---- the child ---------------------------------------------------------------- */

static void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s: %s", what, strerror(errno));
    }
}

static int make_shm(struct camera_session *s)
{
    struct pocketcam_shm_header h = {
        .magic = POCKETCAM_SHM_MAGIC,
        .version = POCKETCAM_PROTO_VERSION,
        .slots = POCKETCAM_SLOTS,
        .slot_bytes = POCKETCAM_SLOT_BYTES,
        .max_w = POCKETCAM_VIEW_MAX_W,
        .max_h = POCKETCAM_VIEW_MAX_H,
    };
    void *map;

    s->shm_fd = memfd_create("doors-camera", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (s->shm_fd < 0) {
        return -1;
    }
    /* Sealed at its size: the helper maps it writable, and a shrink under
     * the shell's read-only mapping would be a SIGBUS in the shell. */
    if (ftruncate(s->shm_fd, POCKETCAM_SHM_BYTES) != 0 ||
        pwrite(s->shm_fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h) ||
        fcntl(s->shm_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
        return -1;
    }
    map = mmap(NULL, POCKETCAM_SHM_BYTES, PROT_READ, MAP_SHARED, s->shm_fd, 0);
    if (map == MAP_FAILED) {
        return -1;
    }
    s->shm = map;
    return 0;
}

static void drop_shm(struct camera_session *s)
{
    if (s->shm) {
        munmap((void *)s->shm, POCKETCAM_SHM_BYTES);
        s->shm = NULL;
    }
    if (s->shm_fd >= 0) {
        close(s->shm_fd);
        s->shm_fd = -1;
    }
}

int camera_session_start(struct camera_session *s, const struct camera_session_config *cfg,
                         int64_t now_ms, char *err, size_t errlen)
{
    const char *helper = cfg && cfg->helper ? cfg->helper : camera_session_helper_path();
    const char *backend = cfg && cfg->backend ? cfg->backend : camera_session_backend();
    char *argv[10];
    int argc = 0;
    int sv[2];
    pid_t parent = getpid();
    pid_t pid;

    if (s->running) {
        if (err && errlen) {
            snprintf(err, errlen, "the camera is already running");
        }
        return -1;
    }
    camera_session_init(s);
    if (strlen(helper) >= CAMERA_HELPER_PATH_MAX || strlen(backend) >= CAMERA_ARG_MAX ||
        (cfg && cfg->fake && strlen(cfg->fake) >= CAMERA_ARG_MAX) ||
        (cfg && cfg->dir && strlen(cfg->dir) >= CAMERA_ARG_MAX) ||
        (cfg && cfg->export_dir && strlen(cfg->export_dir) >= CAMERA_ARG_MAX)) {
        if (err && errlen) {
            snprintf(err, errlen, "camera helper arguments too long");
        }
        return -1;
    }
    argv[argc++] = (char *)helper;
    if (cfg && cfg->library) {
        /* The gallery: no camera, so no backend and no fake script. */
        argv[argc++] = "library";
        if (cfg->export_dir && *cfg->export_dir) {
            argv[argc++] = "--export";
            argv[argc++] = (char *)cfg->export_dir;
        }
    } else {
        argv[argc++] = "session";
        argv[argc++] = "--backend";
        argv[argc++] = (char *)backend;
    }
    if (cfg && !cfg->library && cfg->fake && *cfg->fake) {
        argv[argc++] = "--fake";
        argv[argc++] = (char *)cfg->fake;
    }
    if (cfg && cfg->dir && *cfg->dir) {
        argv[argc++] = "--dir";
        argv[argc++] = (char *)cfg->dir;
    }
    argv[argc] = NULL;

    if (make_shm(s) != 0) {
        say(err, errlen, "shared memory");
        drop_shm(s);
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        say(err, errlen, "socketpair");
        drop_shm(s);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        say(err, errlen, "fork");
        close(sv[0]);
        close(sv[1]);
        drop_shm(s);
        return -1;
    }
    if (pid == 0) {
        sigset_t none;
        int null;
        int fd;

        /* Leave with the shell: if it dies - or execs itself to rotate -
         * this gets SIGTERM and releases the camera on its way out. The check
         * after closes the race where the shell died before the prctl. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) {
            _exit(0);
        }
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (dup2(sv[1], 0) < 0 || dup2(sv[1], 1) < 0) {
            _exit(127);
        }
        /* dup2 onto itself keeps close-on-exec, so clear it explicitly. */
        if (s->shm_fd == POCKETCAM_SHM_FD) {
            fcntl(POCKETCAM_SHM_FD, F_SETFD, 0);
        } else if (dup2(s->shm_fd, POCKETCAM_SHM_FD) < 0) {
            _exit(127);
        }
        null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, 2);
        }
        for (fd = POCKETCAM_SHM_FD + 1; fd < CHILD_FD_SCAN_MAX; fd++) {
            close(fd);
        }
        execv(helper, argv);
        {
            char line[160];
            int n = snprintf(line, sizeof(line), "error exec %s: %s\n", helper, strerror(errno));

            if (n > 0 && write(1, line, (size_t)n) < 0) {
                /* nothing more can be said */
            }
        }
        _exit(127);
    }
    close(sv[1]);
    s->fd = sv[0];
    fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
    s->pid = pid;
    s->running = true;
    s->library = cfg && cfg->library;
    s->hello_by = now_ms + CAMERA_HELLO_MS;
    s->open_by = now_ms + CAMERA_OPEN_MS;
    return 0;
}

/* ---- running ------------------------------------------------------------------ */

static void release_slot(struct camera_session *s, int slot)
{
    if (slot >= 0) {
        send_line(s, "release %d", slot);
    }
}

static void kill_helper(struct camera_session *s, enum camera_exit why)
{
    if (s->running && s->pid > 0 && !s->killed) {
        kill(s->pid, SIGKILL);
        s->killed = true;
        s->exit_reason = why;
    }
}

/* A well-formed event: bookkeeping, then into the queue. */
static void handle(struct camera_session *s, struct camera_event *ev, int64_t now)
{
    int slot;

    /* Anything at all from a streaming helper says it is alive. */
    if (s->streaming) {
        s->silence_by = now + CAMERA_SILENCE_MS;
    }
    /* The library helper answers one request after another. An answer to one
     * says it is alive and starts the window of what waits behind it, so a
     * reply queued behind picture decodes is not taken for a hung helper, nor
     * a picture queued behind an export. */
    if (ev->kind == CAMERA_EV_IMAGE || ev->kind == CAMERA_EV_IMGFAIL) {
        if (s->reply_by != 0 && s->reply_by < now + s->reply_window) {
            s->reply_by = now + s->reply_window;
        }
    } else if (s->decode_by != 0 &&
               (ev->kind == CAMERA_EV_LISTED || ev->kind == CAMERA_EV_LISTFAIL ||
                ev->kind == CAMERA_EV_EXPORTED || ev->kind == CAMERA_EV_EXPFAIL ||
                ev->kind == CAMERA_EV_DELETED || ev->kind == CAMERA_EV_DELFAIL)) {
        s->decode_by = now + CAMERA_DECODE_MS;
    }
    switch (ev->kind) {
    case CAMERA_EV_READY:
    case CAMERA_EV_NODEVICE:
    case CAMERA_EV_ERROR:
        s->open_by = 0;
        break;
    case CAMERA_EV_FRAME:
        if (!s->streaming) {
            /* Sent before a stop it had not read yet: give it back. */
            release_slot(s, ev->value);
            return;
        }
        /* Only the newest picture matters; an older one not taken yet goes
         * straight back to the helper. */
        if (s->frame_slot >= 0 && s->frame_slot != ev->value) {
            release_slot(s, s->frame_slot);
        }
        s->frame_slot = ev->value;
        s->frame_w = ev->w;
        s->frame_h = ev->h;
        if (s->frame_queued) {
            return;
        }
        s->frame_queued = true;
        break;
    case CAMERA_EV_STOPPED:
        s->reply_by = 0;
        break;
    case CAMERA_EV_CAPTURED:
        s->capture_by = 0;
        s->review_slot = ev->w ? POCKETCAM_REVIEW_SLOT : -1;
        s->review_w = ev->w;
        s->review_h = ev->h;
        break;
    case CAMERA_EV_CAPFAIL:
        s->capture_by = 0;
        break;
    case CAMERA_EV_DELETED:
    case CAMERA_EV_DELFAIL:
        s->reply_by = 0;
        break;
    case CAMERA_EV_LOST:
        s->streaming = false;
        s->silence_by = 0;
        s->capture_by = 0;
        break;
    case CAMERA_EV_LISTED:
        s->reply_by = 0;
        s->list_ready = true;
        s->list_count = ev->value;
        break;
    case CAMERA_EV_LISTFAIL:
    case CAMERA_EV_EXPORTED:
    case CAMERA_EV_EXPFAIL:
        s->reply_by = 0;
        break;
    case CAMERA_EV_IMAGE:
    case CAMERA_EV_IMGFAIL:
        if (!s->picture_asked[ev->value]) {
            /* Nothing was asked of that slot: give back what came, keep quiet. */
            if (ev->kind == CAMERA_EV_IMAGE) {
                release_slot(s, ev->value);
            }
            return;
        }
        s->picture_asked[ev->value] = false;
        if (ev->kind == CAMERA_EV_IMAGE) {
            s->picture_ready[ev->value] = true;
            s->picture_w[ev->value] = ev->w;
            s->picture_h[ev->value] = ev->h;
        }
        /* The next one in line gets its own window. */
        s->decode_by = 0;
        for (slot = 0; slot < CAMERA_PICTURE_SLOTS; slot++) {
            if (s->picture_asked[slot]) {
                s->decode_by = now + CAMERA_DECODE_MS;
            }
        }
        break;
    default:
        break;
    }
    push(s, ev);
}

static void feed(struct camera_session *s, const char *buf, size_t n, int64_t now)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char c = buf[i];

        if (c == '\n') {
            if (s->overlong) {
                kill_helper(s, CAMERA_EXIT_PROTOCOL);
            } else {
                struct camera_event ev;

                s->line[s->line_len] = '\0';
                if (strncmp(s->line, "hello", 5) == 0) {
                    unsigned version = 0;

                    s->hello_by = 0;
                    if (sscanf(s->line, "hello %u", &version) != 1 ||
                        version != POCKETCAM_PROTO_VERSION) {
                        kill_helper(s, CAMERA_EXIT_PROTOCOL);
                    }
                } else if (camera_session_parse_line(s->line, &ev)) {
                    handle(s, &ev, now);
                } else if (strncmp(s->line, "frame", 5) == 0 ||
                           strncmp(s->line, "captured", 8) == 0 ||
                           strncmp(s->line, "ready", 5) == 0 ||
                           strncmp(s->line, "image", 5) == 0 ||
                           strncmp(s->line, "listed", 6) == 0) {
                    /* A picture event that does not add up could make the
                     * shell read the wrong memory: that is not a helper to
                     * keep talking to. */
                    kill_helper(s, CAMERA_EXIT_PROTOCOL);
                }
                /* Any other unknown line is ignored: a newer helper may say
                 * more. */
            }
            s->line_len = 0;
            s->overlong = false;
        } else if (s->overlong) {
            continue;
        } else if (s->line_len + 1 >= sizeof(s->line)) {
            s->overlong = true;
            s->line_len = 0;
        } else {
            s->line[s->line_len++] = c;
        }
    }
}

static void finish(struct camera_session *s, int status, int64_t now)
{
    struct camera_event ev;

    if (s->fd >= 0) {
        char buf[512];
        ssize_t n;

        /* Anything the helper wrote just before it left. */
        while ((n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
            feed(s, buf, (size_t)n, now);
        }
        close(s->fd);
        s->fd = -1;
    }
    drop_shm(s);
    s->frame_slot = -1;
    s->review_slot = -1;
    s->streaming = false;
    s->hello_by = s->open_by = s->silence_by = s->capture_by = s->reply_by = 0;
    s->decode_by = 0;
    memset(s->picture_asked, 0, sizeof(s->picture_asked));
    memset(s->picture_ready, 0, sizeof(s->picture_ready));
    s->list_ready = false;
    memset(&ev, 0, sizeof(ev));
    ev.kind = CAMERA_EV_EXITED;
    if (WIFEXITED(status)) {
        ev.value = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        ev.value = 128 + WTERMSIG(status);
    } else {
        ev.value = 255;
    }
    if (s->killed) {
        ev.reason = s->exit_reason;
    } else if (WIFSIGNALED(status)) {
        ev.reason = CAMERA_EXIT_CRASHED;
    } else {
        ev.reason = CAMERA_EXIT_NORMAL;
    }
    push(s, &ev);
    s->pid = -1;
    s->running = false;
}

static bool passed(int64_t deadline, int64_t now)
{
    return deadline != 0 && now >= deadline;
}

int camera_session_poll(struct camera_session *s, struct camera_event *ev, int64_t now_ms)
{
    if (s->running) {
        char buf[512];
        int status;
        pid_t r;

        while (s->fd >= 0 && !s->eof) {
            ssize_t n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT);

            if (n > 0) {
                feed(s, buf, (size_t)n, now_ms);
            } else if (n == 0) {
                s->eof = true;
            } else if (errno == EINTR) {
                continue;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    s->eof = true;
                }
                break;
            }
        }
        if (passed(s->hello_by, now_ms) || passed(s->open_by, now_ms) ||
            passed(s->silence_by, now_ms) || passed(s->capture_by, now_ms) ||
            passed(s->reply_by, now_ms) || passed(s->decode_by, now_ms)) {
            kill_helper(s, CAMERA_EXIT_HUNG);
        }
        r = waitpid(s->pid, &status, WNOHANG);
        if (r == s->pid) {
            finish(s, status, now_ms);
        } else if (r < 0 && errno == ECHILD) {
            finish(s, 255 << 8, now_ms);
        }
    }
    return pop(s, ev);
}

/* ---- commands ------------------------------------------------------------------ */

int camera_session_view(struct camera_session *s, uint32_t w, uint32_t h, int display_rotation)
{
    if (w == 0 || h == 0 || w > POCKETCAM_VIEW_MAX_W || h > POCKETCAM_VIEW_MAX_H) {
        return -1;
    }
    /* A picture of the old size still waiting is now of no use. */
    if (s->frame_slot >= 0 && (s->frame_w != w || s->frame_h != h)) {
        release_slot(s, s->frame_slot);
        s->frame_slot = -1;
    }
    return send_line(s, "view %u %u %d", w, h, display_rotation);
}

int camera_session_preview(struct camera_session *s, bool on, int64_t now_ms)
{
    int r;

    if (on) {
        r = send_line(s, "start");
        if (r == 0) {
            s->streaming = true;
            s->silence_by = now_ms + CAMERA_SILENCE_MS;
        }
        return r;
    }
    s->streaming = false;
    s->silence_by = 0;
    if (s->frame_slot >= 0) {
        release_slot(s, s->frame_slot);
        s->frame_slot = -1;
    }
    r = send_line(s, "stop");
    if (r == 0) {
        s->reply_window = CAMERA_REPLY_MS;
        s->reply_by = now_ms + s->reply_window;
    }
    return r;
}

int camera_session_capture(struct camera_session *s, int display_rotation, int64_t now_ms)
{
    int r;

    /* The helper stops the preview for the still; so does the session, so a
     * frame sent in the meantime is given straight back. */
    s->streaming = false;
    s->silence_by = 0;
    if (s->frame_slot >= 0) {
        release_slot(s, s->frame_slot);
        s->frame_slot = -1;
    }
    r = send_line(s, "capture %d", display_rotation);
    if (r == 0) {
        s->capture_by = now_ms + CAMERA_CAPTURE_MS;
    }
    return r;
}

int camera_session_delete(struct camera_session *s, const char *name, int64_t now_ms)
{
    size_t i;
    int r;

    if (!name || !*name || strlen(name) >= CAMERA_NAME_MAX) {
        return -1;
    }
    for (i = 0; name[i]; i++) {
        if (name[i] <= ' ' || name[i] == '/' || name[i] == 0x7f) {
            return -1;
        }
    }
    r = send_line(s, "delete %s", name);
    if (r == 0) {
        s->reply_window = CAMERA_REPLY_MS;
        s->reply_by = now_ms + s->reply_window;
    }
    return r;
}

static int take(struct camera_session *s, int *slot, uint32_t sw, uint32_t sh, uint16_t *dst,
                uint32_t w, uint32_t h)
{
    int copied = 0;

    if (*slot < 0) {
        return 0;
    }
    if (s->shm && dst && sw == w && sh == h) {
        memcpy(dst, s->shm + pocketcam_slot_offset((uint32_t)*slot), (size_t)w * h * 2);
        copied = 1;
    }
    release_slot(s, *slot);
    *slot = -1;
    return copied;
}

int camera_session_take_frame(struct camera_session *s, uint16_t *dst, uint32_t w, uint32_t h)
{
    return take(s, &s->frame_slot, s->frame_w, s->frame_h, dst, w, h);
}

int camera_session_take_review(struct camera_session *s, uint16_t *dst, uint32_t w, uint32_t h)
{
    return take(s, &s->review_slot, s->review_w, s->review_h, dst, w, h);
}

/* ---- the library ----------------------------------------------------------------- */

int camera_session_list(struct camera_session *s, int64_t now_ms)
{
    int r;

    if (!s->library) {
        return -1;
    }
    if (s->list_ready) {
        /* An old list nobody took: the helper needs its slot back. */
        s->list_ready = false;
        release_slot(s, POCKETCAM_LIST_SLOT);
    }
    r = send_line(s, "list");
    if (r == 0) {
        s->reply_window = CAMERA_LIST_MS;
        s->reply_by = now_ms + s->reply_window;
    }
    return r;
}

int camera_session_take_list(struct camera_session *s, char (*names)[CAMERA_NAME_MAX], int max)
{
    const char *p;
    const char *end;
    int n = 0;

    if (!s->list_ready) {
        return 0;
    }
    s->list_ready = false;
    if (s->shm && names) {
        p = (const char *)(s->shm + pocketcam_slot_offset(POCKETCAM_LIST_SLOT));
        end = p + POCKETCAM_SLOT_BYTES;
        while (n < max && n < s->list_count && p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t len = nl ? (size_t)(nl - p) : 0;

            if (!nl || len == 0 || len >= CAMERA_NAME_MAX) {
                break;
            }
            memcpy(names[n], p, len);
            names[n][len] = '\0';
            if (!name_ok(names[n])) {
                break;
            }
            n++;
            p = nl + 1;
        }
    }
    release_slot(s, POCKETCAM_LIST_SLOT);
    return n;
}

int camera_session_pictures_free(const struct camera_session *s)
{
    int i;
    int n = 0;

    for (i = 0; i < CAMERA_PICTURE_SLOTS; i++) {
        n += !s->picture_asked[i] && !s->picture_ready[i];
    }
    return s->running && s->library ? n : 0;
}

int camera_session_request_picture(struct camera_session *s, const char *name, uint32_t w,
                                   uint32_t h, bool cover, int64_t now_ms)
{
    int slot;

    if (!s->library || !name_ok(name) || w == 0 || h == 0 || w > POCKETCAM_VIEW_MAX_W ||
        h > POCKETCAM_VIEW_MAX_H) {
        return -1;
    }
    for (slot = 0; slot < CAMERA_PICTURE_SLOTS; slot++) {
        if (!s->picture_asked[slot] && !s->picture_ready[slot]) {
            break;
        }
    }
    if (slot == CAMERA_PICTURE_SLOTS ||
        send_line(s, "picture %d %u %u %s %s", slot, w, h, cover ? "cover" : "contain", name) != 0) {
        return -1;
    }
    s->picture_asked[slot] = true;
    if (s->decode_by == 0) {
        s->decode_by = now_ms + CAMERA_DECODE_MS;
        /* Behind a list, export or delete still being worked on: its window
         * first (handle() starts this one's when that answer comes). */
        if (s->reply_by != 0 && s->decode_by < s->reply_by + CAMERA_DECODE_MS) {
            s->decode_by = s->reply_by + CAMERA_DECODE_MS;
        }
    }
    return slot;
}

int camera_session_take_picture(struct camera_session *s, int slot, uint16_t *dst, uint32_t max_w,
                                uint32_t max_h, uint32_t *w, uint32_t *h)
{
    int copied = 0;

    if (slot < 0 || slot >= CAMERA_PICTURE_SLOTS || !s->picture_ready[slot]) {
        return 0;
    }
    if (s->shm && dst && s->picture_w[slot] <= max_w && s->picture_h[slot] <= max_h) {
        memcpy(dst, s->shm + pocketcam_slot_offset((uint32_t)slot),
               (size_t)s->picture_w[slot] * s->picture_h[slot] * 2);
        if (w) {
            *w = s->picture_w[slot];
        }
        if (h) {
            *h = s->picture_h[slot];
        }
        copied = 1;
    }
    s->picture_ready[slot] = false;
    release_slot(s, slot);
    return copied;
}

int camera_session_export(struct camera_session *s, const char *name, int64_t now_ms)
{
    int r;

    if (!s->library || !name_ok(name)) {
        return -1;
    }
    r = send_line(s, "export %s", name);
    if (r == 0) {
        s->reply_window = CAMERA_EXPORT_MS;
        s->reply_by = now_ms + s->reply_window;
    }
    return r;
}

/* ---- leaving ------------------------------------------------------------------- */

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* True when the helper is gone within ms. */
static bool reap_within(struct camera_session *s, int ms)
{
    int64_t end = mono_ms() + ms;
    int status;

    for (;;) {
        pid_t r = waitpid(s->pid, &status, WNOHANG);

        if (r == s->pid || (r < 0 && errno == ECHILD)) {
            return true;
        }
        if (mono_ms() >= end) {
            return false;
        }
        {
            struct timespec d = { 0, 5 * 1000000L };

            nanosleep(&d, NULL);
        }
    }
}

void camera_session_abandon(struct camera_session *s, int grace_ms)
{
    if (s->running && s->pid > 0) {
        /* quit lets it close the camera properly; SIGTERM ends the loop even
         * if it has stopped reading its socket. */
        send_line(s, "quit");
        if (!s->killed) {
            kill(s->pid, SIGTERM);
        }
        if (!reap_within(s, grace_ms < 0 ? 0 : grace_ms)) {
            kill(s->pid, SIGKILL);
            /* If even this is not enough the helper is stuck in the kernel;
             * it is left as a zombie rather than holding the LVGL thread. */
            reap_within(s, CAMERA_KILL_REAP_MS);
        }
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
    drop_shm(s);
    camera_session_init(s);
}
