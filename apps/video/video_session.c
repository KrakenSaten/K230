/*
 * The Video app's helper process client. See video_session.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "video_session.h"

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

#define HELPER_DEFAULT "/usr/bin/pos-video"
#ifndef VIDEO_BACKEND_DEFAULT
#define VIDEO_BACKEND_DEFAULT "ffmpeg"
#endif
/* Descriptors above stderr the shell may hold without close-on-exec (the
 * display, input devices, IPC sockets). The child closes them all but the
 * shared memory before exec; this is the highest it looks at. */
#define CHILD_FD_SCAN_MAX 1024
/* The largest stored picture size an opened line may name. */
#define VIDEO_DIM_MAX 16384

void video_session_init(struct video_session *s)
{
    memset(s, 0, sizeof(*s));
    s->pid = -1;
    s->fd = -1;
    s->shm_fd = -1;
    s->frame_slot = -1;
}

const char *video_session_helper_path(void)
{
    const char *p = getenv("POCKETOS_VIDEO_HELPER");

    return p && *p ? p : HELPER_DEFAULT;
}

const char *video_session_backend(void)
{
    const char *p = getenv("POCKETOS_VIDEO_BACKEND");

    return p && *p ? p : VIDEO_BACKEND_DEFAULT;
}

bool video_session_active(const struct video_session *s)
{
    return s && s->running;
}

/* ---- the event queue ---------------------------------------------------------- */

static void push(struct video_session *s, const struct video_event *ev)
{
    if (s->q_count == VIDEO_EVENT_QUEUE) {
        /* Only progress and statistics come often enough to fill it; the
         * oldest event goes, and the count says so. */
        s->q_head = (s->q_head + 1) % VIDEO_EVENT_QUEUE;
        s->q_count--;
        s->dropped++;
    }
    s->queue[(s->q_head + s->q_count) % VIDEO_EVENT_QUEUE] = *ev;
    s->q_count++;
}

static int pop(struct video_session *s, struct video_event *ev)
{
    if (s->q_count == 0) {
        return 0;
    }
    *ev = s->queue[s->q_head];
    s->q_head = (s->q_head + 1) % VIDEO_EVENT_QUEUE;
    s->q_count--;
    if (ev->kind == VIDEO_EV_FRAME) {
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

static bool one_of(const char *w, const char *const *list, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (strcmp(w, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *const audio_words[] = { VIDEO_AUDIO_NONE, VIDEO_AUDIO_ON, VIDEO_AUDIO_MUTED,
                                           VIDEO_AUDIO_BUSY, VIDEO_AUDIO_UNSUPPORTED,
                                           VIDEO_AUDIO_ERROR };

int video_session_parse_line(const char *line, struct video_event *ev)
{
    const char *a;
    uint64_t v[8];

    memset(ev, 0, sizeof(*ev));
    if ((a = after(line, "opened")) != NULL) {
        ev->kind = VIDEO_EV_OPENED;
        if (!number(&a, (uint64_t)VIDEO_DURATION_MAX_MS, &v[0]) || !number(&a, VIDEO_DIM_MAX, &v[1]) ||
            !number(&a, VIDEO_DIM_MAX, &v[2]) || v[1] == 0 || v[2] == 0 ||
            !number(&a, 100000, &v[3]) || !word(&a, ev->word, sizeof(ev->word)) ||
            !one_of(ev->word, audio_words, sizeof(audio_words) / sizeof(audio_words[0])) ||
            !word(&a, ev->codec, sizeof(ev->codec))) {
            return 0;
        }
        ev->ms = (int64_t)v[0];
        ev->w = (uint32_t)v[1];
        ev->h = (uint32_t)v[2];
        ev->fps_x100 = (uint32_t)v[3];
        return 1;
    }
    if ((a = after(line, "openfail")) != NULL) {
        static const char *const reasons[] = { VIDEO_OPENFAIL_MISSING, VIDEO_OPENFAIL_UNSUPPORTED,
                                               VIDEO_OPENFAIL_CORRUPT, VIDEO_OPENFAIL_DEVICE,
                                               VIDEO_OPENFAIL_IO };

        ev->kind = VIDEO_EV_OPENFAIL;
        if (!word(&a, ev->word, sizeof(ev->word))) {
            return 0;
        }
        if (!one_of(ev->word, reasons, sizeof(reasons) / sizeof(reasons[0]))) {
            snprintf(ev->word, sizeof(ev->word), "%s", VIDEO_OPENFAIL_IO);
        }
        rest(a, ev->text, sizeof(ev->text));
        return 1;
    }
    if ((a = after(line, "frame")) != NULL) {
        ev->kind = VIDEO_EV_FRAME;
        if (!number(&a, VIDEO_SLOTS - 1, &v[0]) || !number(&a, UINT32_MAX, &v[1]) ||
            !number(&a, VIDEO_VIEW_MAX_W, &v[2]) || !number(&a, VIDEO_VIEW_MAX_H, &v[3]) ||
            v[2] == 0 || v[3] == 0 || v[2] * v[3] > VIDEO_VIEW_MAX_PIXELS ||
            !number(&a, (uint64_t)VIDEO_DURATION_MAX_MS, &v[4])) {
            return 0;
        }
        ev->value = (int)v[0];
        ev->w = (uint32_t)v[2];
        ev->h = (uint32_t)v[3];
        ev->ms = (int64_t)v[4];
        return 1;
    }
    if ((a = after(line, "state")) != NULL) {
        static const char *const states[] = { "playing", "paused", "stopped", "ended" };
        char w[VIDEO_WORD_MAX];
        int i;

        ev->kind = VIDEO_EV_STATE;
        if (!word(&a, w, sizeof(w)) || !number(&a, (uint64_t)VIDEO_DURATION_MAX_MS, &v[0])) {
            return 0;
        }
        for (i = 0; i < 4; i++) {
            if (strcmp(w, states[i]) == 0) {
                ev->value = i;
                ev->ms = (int64_t)v[0];
                snprintf(ev->word, sizeof(ev->word), "%s", w);
                return 1;
            }
        }
        return 0;
    }
    if ((a = after(line, "pos")) != NULL) {
        ev->kind = VIDEO_EV_POS;
        if (!number(&a, (uint64_t)VIDEO_DURATION_MAX_MS, &v[0])) {
            return 0;
        }
        ev->ms = (int64_t)v[0];
        return 1;
    }
    if ((a = after(line, "seeked")) != NULL) {
        ev->kind = VIDEO_EV_SEEKED;
        if (!number(&a, (uint64_t)VIDEO_DURATION_MAX_MS, &v[0])) {
            return 0;
        }
        ev->ms = (int64_t)v[0];
        return 1;
    }
    if ((a = after(line, "audio")) != NULL) {
        ev->kind = VIDEO_EV_AUDIO;
        if (!word(&a, ev->word, sizeof(ev->word)) ||
            !one_of(ev->word, audio_words, sizeof(audio_words) / sizeof(audio_words[0]))) {
            return 0;
        }
        return 1;
    }
    if ((a = after(line, "stats")) != NULL) {
        int i;

        ev->kind = VIDEO_EV_STATS;
        for (i = 0; i < 7; i++) {
            if (!number(&a, UINT32_MAX, &v[i])) {
                return 0;
            }
        }
        ev->stats.fps_x10 = (uint32_t)v[0];
        ev->stats.shown = v[1];
        ev->stats.dropped = v[2];
        ev->stats.late = v[3];
        ev->stats.cpu_pct = (uint32_t)v[4];
        ev->stats.rss_kb = (uint32_t)v[5];
        ev->stats.xruns = (uint32_t)v[6];
        return 1;
    }
    if ((a = after(line, "error")) != NULL && *a) {
        ev->kind = VIDEO_EV_ERROR;
        if (!word(&a, ev->word, sizeof(ev->word))) {
            return 0;
        }
        rest(a, ev->text, sizeof(ev->text));
        return 1;
    }
    return 0;
}

/* ---- sending ------------------------------------------------------------------ */

static int send_line(struct video_session *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static int send_line(struct video_session *s, const char *fmt, ...)
{
    char line[VIDEO_LINE_MAX + 2];
    va_list ap;
    int n;

    if (!s->running || s->fd < 0) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(line) - 1 || n >= VIDEO_LINE_MAX) {
        return -1;
    }
    line[n++] = '\n';
    /* A few bytes into a socket the helper drains every few milliseconds:
     * this never waits. MSG_NOSIGNAL, so a helper that has just died cannot
     * take the shell down with SIGPIPE; that death is reported by poll(). */
    return send(s->fd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) == n ? 0 : -1;
}

/* ---- the child ---------------------------------------------------------------- */

static void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s: %s", what, strerror(errno));
    }
}

static int make_shm(struct video_session *s)
{
    struct video_shm_header h = {
        .magic = VIDEO_SHM_MAGIC,
        .version = VIDEO_PROTO_VERSION,
        .slots = VIDEO_SLOTS,
        .slot_bytes = VIDEO_SLOT_BYTES,
        .max_w = VIDEO_VIEW_MAX_W,
        .max_h = VIDEO_VIEW_MAX_H,
        .max_pixels = VIDEO_VIEW_MAX_PIXELS,
    };
    void *map;

    s->shm_fd = memfd_create("doors-video", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (s->shm_fd < 0) {
        return -1;
    }
    /* Sealed at its size: the helper maps it writable, and a shrink under
     * the shell's read-only mapping would be a SIGBUS in the shell. */
    if (ftruncate(s->shm_fd, VIDEO_SHM_BYTES) != 0 ||
        pwrite(s->shm_fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h) ||
        fcntl(s->shm_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
        return -1;
    }
    map = mmap(NULL, VIDEO_SHM_BYTES, PROT_READ, MAP_SHARED, s->shm_fd, 0);
    if (map == MAP_FAILED) {
        return -1;
    }
    s->shm = map;
    return 0;
}

static void drop_shm(struct video_session *s)
{
    if (s->shm) {
        munmap((void *)s->shm, VIDEO_SHM_BYTES);
        s->shm = NULL;
    }
    if (s->shm_fd >= 0) {
        close(s->shm_fd);
        s->shm_fd = -1;
    }
}

int video_session_start(struct video_session *s, const struct video_session_config *cfg,
                        int64_t now_ms, char *err, size_t errlen)
{
    const char *helper = cfg && cfg->helper ? cfg->helper : video_session_helper_path();
    const char *backend = cfg && cfg->backend ? cfg->backend : video_session_backend();
    int volume = cfg ? cfg->volume_percent : 0;
    char vol[8];
    char *argv[8];
    int sv[2];
    pid_t parent = getpid();
    pid_t pid;

    if (s->running) {
        if (err && errlen) {
            snprintf(err, errlen, "the player is already running");
        }
        return -1;
    }
    video_session_init(s);
    if (strlen(helper) >= VIDEO_HELPER_PATH_MAX || strlen(backend) >= VIDEO_WORD_MAX) {
        if (err && errlen) {
            snprintf(err, errlen, "video helper arguments too long");
        }
        return -1;
    }
    snprintf(s->helper, sizeof(s->helper), "%s", helper);
    snprintf(vol, sizeof(vol), "%d", volume < 0 ? 0 : volume > 100 ? 100 : volume);
    argv[0] = s->helper;
    argv[1] = "session";
    argv[2] = "--backend";
    argv[3] = (char *)backend;
    argv[4] = "--volume-percent";
    argv[5] = vol;
    argv[6] = NULL;

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
         * this gets SIGTERM and closes the sound card on its way out. The
         * check after closes the race where the shell died before the prctl. */
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
        if (s->shm_fd == VIDEO_SHM_FD) {
            fcntl(VIDEO_SHM_FD, F_SETFD, 0);
        } else if (dup2(s->shm_fd, VIDEO_SHM_FD) < 0) {
            _exit(127);
        }
        null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, 2);
        }
        for (fd = VIDEO_SHM_FD + 1; fd < CHILD_FD_SCAN_MAX; fd++) {
            close(fd);
        }
        execv(s->helper, argv);
        {
            char line[160];
            int n = snprintf(line, sizeof(line), "error device exec %s: %s\n", s->helper,
                             strerror(errno));

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
    s->hello_by = now_ms + VIDEO_HELLO_MS;
    return 0;
}

/* `helper recover`, detached (double fork, own session, no death signal): it
 * completes even while the shell exits, and init reaps it. */
static void recover_detached(struct video_session *s)
{
    pid_t mid;
    int status;

    if (!s->helper[0]) {
        return;
    }
    mid = fork();
    if (mid < 0) {
        return;
    }
    if (mid == 0) {
        pid_t pid = fork();

        if (pid == 0) {
            char *argv[] = { s->helper, "recover", NULL };
            sigset_t none;
            int null = open("/dev/null", O_RDWR);
            int fd;

            setsid();
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, NULL);
            signal(SIGPIPE, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            if (null >= 0) {
                dup2(null, 0);
                dup2(null, 1);
                dup2(null, 2);
            }
            for (fd = 3; fd < CHILD_FD_SCAN_MAX; fd++) {
                close(fd);
            }
            execv(s->helper, argv);
            _exit(127);
        }
        _exit(0);
    }
    while (waitpid(mid, &status, 0) < 0 && errno == EINTR) {
    }
    s->recoveries++;
}

/* ---- running ------------------------------------------------------------------ */

static void release_slot(struct video_session *s, int slot)
{
    if (slot >= 0) {
        send_line(s, "release %d", slot);
    }
}

static void kill_helper(struct video_session *s, enum video_exit why)
{
    if (s->running && s->pid > 0 && !s->killed) {
        kill(s->pid, SIGKILL);
        s->killed = true;
        s->exit_reason = why;
    }
}

static void handle(struct video_session *s, struct video_event *ev, int64_t now)
{
    /* Anything at all from a playing helper says it is alive. */
    if (s->playing) {
        s->silence_by = now + VIDEO_SILENCE_MS;
    }
    switch (ev->kind) {
    case VIDEO_EV_OPENED:
        s->open_by = 0;
        s->picture_by = now + VIDEO_OPEN_MS;
        break;
    case VIDEO_EV_OPENFAIL:
        s->open_by = 0;
        break;
    case VIDEO_EV_FRAME:
        s->picture_by = 0;
        /* Only the newest picture matters; an older one not taken yet goes
         * straight back to the helper. */
        if (s->frame_slot >= 0 && s->frame_slot != ev->value) {
            release_slot(s, s->frame_slot);
        }
        s->frame_slot = ev->value;
        s->frame_w = ev->w;
        s->frame_h = ev->h;
        s->frame_ms = ev->ms;
        if (s->frame_queued) {
            return;
        }
        s->frame_queued = true;
        break;
    case VIDEO_EV_STATE:
        s->reply_by = 0;
        s->playing = ev->value == VIDEO_PLAY_PLAYING;
        s->silence_by = s->playing ? now + VIDEO_SILENCE_MS : 0;
        break;
    case VIDEO_EV_SEEKED:
        s->reply_by = 0;
        break;
    case VIDEO_EV_ERROR:
        s->open_by = s->picture_by = s->reply_by = s->silence_by = 0;
        s->playing = false;
        break;
    default:
        break;
    }
    push(s, ev);
}

static void feed(struct video_session *s, const char *buf, size_t n, int64_t now)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char c = buf[i];

        if (c == '\n') {
            if (s->overlong) {
                kill_helper(s, VIDEO_EXIT_PROTOCOL);
            } else {
                struct video_event ev;

                s->line[s->line_len] = '\0';
                if (strncmp(s->line, "hello", 5) == 0) {
                    unsigned version = 0;

                    s->hello_by = 0;
                    if (sscanf(s->line, "hello %u", &version) != 1 ||
                        version != VIDEO_PROTO_VERSION) {
                        kill_helper(s, VIDEO_EXIT_PROTOCOL);
                    }
                } else if (video_session_parse_line(s->line, &ev)) {
                    handle(s, &ev, now);
                } else if (strncmp(s->line, "frame", 5) == 0) {
                    /* A picture event that does not add up could make the
                     * shell read the wrong memory: that is not a helper to
                     * keep talking to. */
                    kill_helper(s, VIDEO_EXIT_PROTOCOL);
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

static void finish(struct video_session *s, int status, int64_t now)
{
    struct video_event ev;

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
    s->playing = false;
    s->hello_by = s->open_by = s->picture_by = s->reply_by = s->silence_by = 0;
    memset(&ev, 0, sizeof(ev));
    ev.kind = VIDEO_EV_EXITED;
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
        ev.reason = VIDEO_EXIT_CRASHED;
    } else {
        ev.reason = VIDEO_EXIT_NORMAL;
    }
    if (WIFSIGNALED(status)) {
        /* It could not close the sound card itself. */
        recover_detached(s);
    }
    push(s, &ev);
    s->pid = -1;
    s->running = false;
}

static bool passed(int64_t deadline, int64_t now)
{
    return deadline != 0 && now >= deadline;
}

int video_session_poll(struct video_session *s, struct video_event *ev, int64_t now_ms)
{
    if (s->running) {
        char buf[1024];
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
            passed(s->picture_by, now_ms) || passed(s->reply_by, now_ms) ||
            passed(s->silence_by, now_ms)) {
            kill_helper(s, VIDEO_EXIT_HUNG);
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

int video_session_view(struct video_session *s, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0 || w > VIDEO_VIEW_MAX_W || h > VIDEO_VIEW_MAX_H) {
        return -1;
    }
    return send_line(s, "view %u %u", w, h);
}

int video_session_open(struct video_session *s, const char *path, int64_t now_ms)
{
    size_t i;

    if (!path || path[0] != '/' || strlen(path) >= VIDEO_PATH_MAX) {
        return -1;
    }
    for (i = 0; path[i]; i++) {
        if ((unsigned char)path[i] < 0x20 || path[i] == 0x7f) {
            return -1;
        }
    }
    if (send_line(s, "open %s", path) != 0) {
        return -1;
    }
    s->open_by = now_ms + VIDEO_OPEN_MS;
    return 0;
}

static int command(struct video_session *s, const char *cmd, int64_t now_ms)
{
    if (send_line(s, "%s", cmd) != 0) {
        return -1;
    }
    s->reply_by = now_ms + VIDEO_REPLY_MS;
    return 0;
}

int video_session_play(struct video_session *s, int64_t now_ms)
{
    return command(s, "play", now_ms);
}

int video_session_pause(struct video_session *s, int64_t now_ms)
{
    return command(s, "pause", now_ms);
}

int video_session_stop(struct video_session *s, int64_t now_ms)
{
    return command(s, "stop", now_ms);
}

int video_session_seek(struct video_session *s, int64_t ms, int64_t now_ms)
{
    if (ms < 0 || ms > VIDEO_DURATION_MAX_MS) {
        return -1;
    }
    if (send_line(s, "seek %lld", (long long)ms) != 0) {
        return -1;
    }
    s->reply_by = now_ms + VIDEO_REPLY_MS;
    return 0;
}

int video_session_take_frame(struct video_session *s, uint16_t *dst, size_t max_pixels, uint32_t *w,
                             uint32_t *h)
{
    int copied = 0;

    if (s->frame_slot < 0) {
        return 0;
    }
    if (s->shm && dst && (size_t)s->frame_w * s->frame_h <= max_pixels) {
        memcpy(dst, s->shm + video_slot_offset((uint32_t)s->frame_slot),
               (size_t)s->frame_w * s->frame_h * 2);
        if (w) {
            *w = s->frame_w;
        }
        if (h) {
            *h = s->frame_h;
        }
        copied = 1;
    }
    release_slot(s, s->frame_slot);
    s->frame_slot = -1;
    return copied;
}

/* ---- leaving ------------------------------------------------------------------- */

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* True when the helper is gone within ms; *signaled says whether a signal
 * ended it. */
static bool reap_within(struct video_session *s, int ms, bool *signaled)
{
    int64_t end = mono_ms() + ms;
    int status;

    for (;;) {
        pid_t r = waitpid(s->pid, &status, WNOHANG);

        if (r == s->pid) {
            *signaled = WIFSIGNALED(status);
            return true;
        }
        if (r < 0 && errno == ECHILD) {
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

void video_session_abandon(struct video_session *s, int grace_ms)
{
    if (s->running && s->pid > 0) {
        bool signaled = false;

        /* quit lets it close the sound card properly; SIGTERM ends the loop
         * even if it has stopped reading its socket. */
        send_line(s, "quit");
        if (!s->killed) {
            kill(s->pid, SIGTERM);
        }
        if (!reap_within(s, grace_ms < 0 ? 0 : grace_ms, &signaled)) {
            kill(s->pid, SIGKILL);
            /* If even this is not enough the helper is stuck in the kernel;
             * it is left as a zombie rather than holding the LVGL thread. */
            reap_within(s, VIDEO_KILL_REAP_MS, &signaled);
            signaled = true;
        }
        if (signaled) {
            recover_detached(s);
        }
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
    drop_shm(s);
    {
        unsigned recoveries = s->recoveries;
        char helper[VIDEO_HELPER_PATH_MAX];

        memcpy(helper, s->helper, sizeof(helper));
        video_session_init(s);
        s->recoveries = recoveries;
        memcpy(s->helper, helper, sizeof(helper));
    }
}
