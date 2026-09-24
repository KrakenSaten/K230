/*
 * pos-camera: the only process that ever opens the camera.
 *
 *   pos-camera session [--backend NAME] [--fake SCRIPT] [--dir DIR]
 *       The Camera app's helper (docs/decisions/ADR-006-camera-ownership.md).
 *       stdin and stdout are a socketpair to the shell, descriptor 3 is the
 *       shared memory; the protocol is core/pocketcam/pocketcam_proto.h. It
 *       lives exactly as long as the Camera screen: it leaves on `quit`, when
 *       the shell closes its end, on SIGTERM, and - through PR_SET_PDEATHSIG,
 *       set by the session before exec - when the shell dies.
 *   pos-camera probe [--backend NAME] [--fake SCRIPT]
 *       Open the camera, say what it is, close it. Exit 0, or 3 when there is
 *       no camera.
 *   pos-camera snap FILE [--backend NAME] [--fake SCRIPT] [--portrait]
 *       One still, turned for the orientation, written to FILE with this
 *       build's encoder (pocketcam_codec.h). For the bench: the first frame
 *       from a real backend on unit A should come from here, not the app.
 *
 * The backend is --backend, else $POCKETOS_CAMERA_BACKEND, else "v4l2" - the
 * real one, which on this branch is not implemented and answers "no camera".
 * The fake is only ever used when asked for.
 *
 * Nothing here blocks for longer than the backend call it makes, and every
 * backend call is given a bound: 50 ms for a preview frame, so a command is
 * never more than that away from being read, and POCKETCAM_STILL_TIMEOUT_MS
 * for a still. A backend that ignores its bound - a driver stuck in the
 * kernel - is what the session's watchdog and SIGKILL are for.
 *
 * Exit codes: 0 done, 2 usage or setup, 3 no camera or it could not be
 * opened, 4 the camera went away while in use.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketcam/pocketcam.h"
#include "pocketcam/pocketcam_codec.h"
#include "pocketcam/pocketcam_convert.h"
#include "pocketcam/pocketcam_proto.h"
#include "pocketcam/pocketcam_store.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EXIT_USAGE 2
#define EXIT_NOCAMERA 3
#define EXIT_LOST 4
/* How long one preview wait may take: the longest a command waits. */
#define FRAME_WAIT_MS 50

static volatile sig_atomic_t stop_requested;

static void on_term(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- events ------------------------------------------------------------------ */

static int out_broken;

/* One event line to stdout, whole or not at all. A shell that has gone
 * leaves nobody to talk to: the loop ends at its next turn. */
static void say(const char *fmt, ...)
{
    char line[POCKETCAM_LINE_MAX];
    va_list ap;
    int n;
    size_t off = 0;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof(line) - 2) {
        n = (int)sizeof(line) - 2;
    }
    line[n++] = '\n';
    while (off < (size_t)n && !out_broken) {
        ssize_t w = write(1, line + off, (size_t)n - off);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            out_broken = 1;
            break;
        }
        off += (size_t)w;
    }
}

/* Text for an event: one line, printable. */
static const char *clean(const char *s, char *buf, size_t len)
{
    size_t i;

    snprintf(buf, len, "%s", s ? s : "");
    for (i = 0; buf[i]; i++) {
        if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] == 0x7f) {
            buf[i] = ' ';
        }
    }
    return buf;
}

/* ---- the session -------------------------------------------------------------- */

struct session {
    struct pocketcam_backend cam;
    struct pocketcam_info info;
    struct pocketcam_store store;
    bool store_ok;
    char store_err[96];
    uint8_t *shm;
    bool held[POCKETCAM_SLOTS];
    uint32_t view_w;
    uint32_t view_h;
    int display_rotation;
    bool streaming;
    int64_t last_sent_ms;
    int64_t last_frame_ms;
    int64_t last_stall_ms;
    int malformed_run;
    char in[POCKETCAM_LINE_MAX];
    size_t in_len;
    bool in_overlong;
    bool in_eof;
    bool quit;
    int exit_code;
};

static int map_shm(struct session *s)
{
    struct stat st;
    const struct pocketcam_shm_header *h;

    if (fstat(POCKETCAM_SHM_FD, &st) != 0 || st.st_size < (off_t)POCKETCAM_SHM_BYTES) {
        return -1;
    }
    s->shm = mmap(NULL, POCKETCAM_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, POCKETCAM_SHM_FD,
                  0);
    if (s->shm == MAP_FAILED) {
        s->shm = NULL;
        return -1;
    }
    h = (const struct pocketcam_shm_header *)s->shm;
    if (h->magic != POCKETCAM_SHM_MAGIC || h->version != POCKETCAM_PROTO_VERSION ||
        h->slots != POCKETCAM_SLOTS || h->slot_bytes != POCKETCAM_SLOT_BYTES) {
        munmap(s->shm, POCKETCAM_SHM_BYTES);
        s->shm = NULL;
        return -1;
    }
    return 0;
}

static uint16_t *slot_pixels(struct session *s, int slot)
{
    return (uint16_t *)(void *)(s->shm + pocketcam_slot_offset((uint32_t)slot));
}

/* A display rotation: 0, 90, 180 or 270. */
static int parse_rotation(const char *w, int *deg)
{
    char *end;
    long v = strtol(w, &end, 10);

    if (end == w || *end || (v != 0 && v != 90 && v != 180 && v != 270)) {
        return -1;
    }
    *deg = (int)v;
    return 0;
}

static void lose(struct session *s, int err)
{
    char t[96];

    say("lost %s", clean(pocketcam_strerror(err), t, sizeof(t)));
    s->exit_code = EXIT_LOST;
    s->quit = true;
}

struct encode_job {
    const struct pocketcam_frame *f;
    int rotation;
    bool mirror;
};

static int write_photo(FILE *fp, void *user)
{
    struct encode_job *j = user;

    return pocketcam_encode(fp, j->f, j->rotation, j->mirror);
}

static void capture(struct session *s, int display_rotation)
{
    struct pocketcam_frame f;
    struct encode_job job;
    char name[POCKETCAM_STORE_NAME_MAX];
    char t[96];
    uint64_t bytes = 0;
    uint32_t rw = 0;
    uint32_t rh = 0;
    int r;

    if (!s->store_ok) {
        say("capfail io %s", clean(s->store_err, t, sizeof(t)));
        return;
    }
    pocketcam_store_scan(&s->store);
    r = pocketcam_store_room(&s->store,
                             pocketcam_codec_estimate(s->info.still_w, s->info.still_h));
    if (r == -EDQUOT) {
        say("capfail quota the photo limit is reached");
        return;
    }
    if (r == -ENOSPC) {
        say("capfail nospace storage is full");
        return;
    }
    if (r != 0) {
        say("capfail io %s", clean(strerror(-r), t, sizeof(t)));
        return;
    }
    s->streaming = false;
    r = pocketcam_still(&s->cam, POCKETCAM_STILL_TIMEOUT_MS, &f);
    if (r == -ENODEV) {
        lose(s, r);
        return;
    }
    if (r != 0) {
        say("capfail device %s", clean(pocketcam_strerror(r), t, sizeof(t)));
        return;
    }
    say("saving");
    job.f = &f;
    job.rotation = pocketcam_view_rotation(s->info.mount_rotation, display_rotation);
    job.mirror = s->info.mount_mirror;
    /* The review picture first: it is what the screen shows next. */
    if (s->view_w && s->view_h &&
        pocketcam_to_rgb565(&f, job.rotation, job.mirror, POCKETCAM_FIT_CONTAIN,
                            slot_pixels(s, POCKETCAM_REVIEW_SLOT), s->view_w, s->view_h,
                            s->view_w) == 0) {
        rw = s->view_w;
        rh = s->view_h;
    }
    pocketcam_store_next_name(&s->store, (int64_t)time(NULL), pocketcam_codec_ext(), name,
                              sizeof(name));
    r = pocketcam_store_write(&s->store, name, write_photo, &job, &bytes);
    pocketcam_release(&s->cam, &f);
    if (r == -ENOSPC) {
        say("capfail nospace storage is full");
    } else if (r != 0) {
        say("capfail io %s", clean(strerror(-r), t, sizeof(t)));
    } else {
        s->held[POCKETCAM_REVIEW_SLOT] = rw != 0;
        say("captured %d %u %u %llu %s %u", POCKETCAM_REVIEW_SLOT, rw, rh,
            (unsigned long long)bytes, name, s->store.files);
    }
}

static void command(struct session *s, char *line)
{
    char *save = NULL;
    char *verb = strtok_r(line, " ", &save);
    char *a1 = verb ? strtok_r(NULL, " ", &save) : NULL;
    char *a2 = a1 ? strtok_r(NULL, " ", &save) : NULL;
    char *a3 = a2 ? strtok_r(NULL, " ", &save) : NULL;
    char t[96];

    if (!verb) {
        return;
    }
    if (strcmp(verb, "view") == 0 && a3) {
        long w = strtol(a1, NULL, 10);
        long h = strtol(a2, NULL, 10);
        int deg;

        if (w > 0 && h > 0 && w <= POCKETCAM_VIEW_MAX_W && h <= POCKETCAM_VIEW_MAX_H &&
            parse_rotation(a3, &deg) == 0) {
            s->view_w = (uint32_t)w;
            s->view_h = (uint32_t)h;
            s->display_rotation = deg;
        }
    } else if (strcmp(verb, "start") == 0) {
        int r = pocketcam_start(&s->cam);

        if (r != 0) {
            lose(s, r);
            return;
        }
        s->streaming = true;
        s->last_frame_ms = s->last_stall_ms = mono_ms();
        s->last_sent_ms = 0;
        s->malformed_run = 0;
    } else if (strcmp(verb, "stop") == 0) {
        pocketcam_stop(&s->cam);
        s->streaming = false;
        say("stopped");
    } else if (strcmp(verb, "release") == 0 && a1) {
        long slot = strtol(a1, NULL, 10);

        if (slot >= 0 && slot < POCKETCAM_SLOTS) {
            s->held[slot] = false;
        }
    } else if (strcmp(verb, "capture") == 0 && a1) {
        int deg;

        if (parse_rotation(a1, &deg) == 0) {
            capture(s, deg);
        }
    } else if (strcmp(verb, "delete") == 0 && a1) {
        int r = s->store_ok ? pocketcam_store_delete(&s->store, a1) : -EIO;

        if (r == 0) {
            say("deleted %s %u", a1, s->store.files);
        } else {
            say("delfail %s", clean(strerror(-r), t, sizeof(t)));
        }
    } else if (strcmp(verb, "quit") == 0) {
        s->quit = true;
    }
}

/* Read what the shell has sent; handle every whole line. */
static void read_commands(struct session *s, int timeout_ms)
{
    struct pollfd p = { .fd = 0, .events = POLLIN };
    char buf[512];
    ssize_t n;
    ssize_t i;

    if (poll(&p, 1, timeout_ms) <= 0) {
        return;
    }
    n = read(0, buf, sizeof(buf));
    if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
        s->in_eof = true; /* the shell closed its end: nobody to serve */
        return;
    }
    for (i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            if (!s->in_overlong) {
                s->in[s->in_len] = '\0';
                command(s, s->in);
            }
            s->in_len = 0;
            s->in_overlong = false;
            if (s->quit) {
                return;
            }
        } else if (s->in_overlong) {
            continue;
        } else if (s->in_len + 1 >= sizeof(s->in)) {
            s->in_overlong = true;
            s->in_len = 0;
        } else {
            s->in[s->in_len++] = buf[i];
        }
    }
}

static int free_preview_slot(const struct session *s)
{
    int i;

    for (i = 0; i < POCKETCAM_PREVIEW_SLOTS; i++) {
        if (!s->held[i]) {
            return i;
        }
    }
    return -1;
}

static void stream_once(struct session *s)
{
    struct pocketcam_frame f;
    int64_t now;
    int r = pocketcam_next(&s->cam, FRAME_WAIT_MS, &f);

    now = mono_ms();
    if (r == -ETIMEDOUT) {
        if (now - s->last_frame_ms >= POCKETCAM_STALL_MS &&
            now - s->last_stall_ms >= POCKETCAM_STALL_MS) {
            say("stall %lld", (long long)(now - s->last_frame_ms));
            s->last_stall_ms = now;
        }
        return;
    }
    if (r == -EPROTO) {
        say("malformed %d", ++s->malformed_run);
        if (s->malformed_run >= POCKETCAM_MALFORMED_LIMIT) {
            lose(s, -EPROTO);
        }
        return;
    }
    if (r != 0) {
        lose(s, r);
        return;
    }
    s->malformed_run = 0;
    s->last_frame_ms = now;
    if (s->view_w && now - s->last_sent_ms >= POCKETCAM_PREVIEW_MIN_INTERVAL_MS) {
        int slot = free_preview_slot(s);

        if (slot >= 0 &&
            pocketcam_to_rgb565(&f, pocketcam_view_rotation(s->info.mount_rotation, s->display_rotation),
                                s->info.mount_mirror, POCKETCAM_FIT_COVER, slot_pixels(s, slot),
                                s->view_w, s->view_h, s->view_w) == 0) {
            s->held[slot] = true;
            s->last_sent_ms = now;
            say("frame %d %u %u %u", slot, f.seq, s->view_w, s->view_h);
        }
    }
    pocketcam_release(&s->cam, &f);
}

static const char *pick_backend(const char *arg)
{
    const char *env = getenv("POCKETOS_CAMERA_BACKEND");

    if (arg && *arg) {
        return arg;
    }
    return env && *env ? env : "v4l2";
}

static int open_camera(struct pocketcam_backend *cam, struct pocketcam_info *info,
                       const char *backend, const char *script, bool events)
{
    char t[96];
    int r = pocketcam_open(cam, backend, script, info);

    if (r == 0) {
        return 0;
    }
    if (events) {
        if (r == -ENODEV || r == -ENOTSUP) {
            say("nodevice %s", clean(pocketcam_strerror(r), t, sizeof(t)));
        } else if (r == -EBUSY) {
            say("error busy %s", clean(pocketcam_strerror(r), t, sizeof(t)));
        } else {
            say("error open %s", clean(pocketcam_strerror(r), t, sizeof(t)));
        }
    } else {
        fprintf(stderr, "pos-camera: %s: %s\n", backend, pocketcam_strerror(r));
    }
    return r;
}

static int run_session(const char *backend, const char *script, const char *dir)
{
    struct session *s = calloc(1, sizeof(*s));
    char dir_buf[PATH_MAX];
    int code;
    int r;

    if (!s) {
        say("error memory out of memory");
        return EXIT_USAGE;
    }
    say("hello %d %s", POCKETCAM_PROTO_VERSION, backend);
    if (map_shm(s) != 0) {
        say("error shm no usable shared memory on descriptor %d", POCKETCAM_SHM_FD);
        free(s);
        return EXIT_USAGE;
    }
    if (open_camera(&s->cam, &s->info, backend, script, true) != 0) {
        munmap(s->shm, POCKETCAM_SHM_BYTES);
        free(s);
        return EXIT_NOCAMERA;
    }
    if (!dir || !*dir) {
        pocketcam_store_default_dir(dir_buf, sizeof(dir_buf));
        dir = dir_buf;
    }
    r = pocketcam_store_open(&s->store, dir);
    s->store_ok = r == 0;
    if (!s->store_ok) {
        snprintf(s->store_err, sizeof(s->store_err), "photo folder: %s", strerror(-r));
    }
    say("ready %s %u %u %d %u %s", s->info.name, s->info.still_w, s->info.still_h,
        s->info.simulated ? 1 : 0, s->store_ok ? s->store.files : 0,
        s->store_ok && s->store.last[0] ? s->store.last : "-");
    while (!s->quit && !s->in_eof && !stop_requested && !out_broken) {
        read_commands(s, s->streaming ? 0 : 250);
        if (s->streaming && !s->quit) {
            stream_once(s);
        }
    }
    pocketcam_close(&s->cam);
    if (s->quit && s->exit_code == 0) {
        say("bye");
    }
    munmap(s->shm, POCKETCAM_SHM_BYTES);
    code = s->exit_code;
    free(s);
    return code;
}

static int run_probe(const char *backend, const char *script)
{
    struct pocketcam_backend cam;
    struct pocketcam_info info;

    if (open_camera(&cam, &info, backend, script, false) != 0) {
        return EXIT_NOCAMERA;
    }
    printf("backend=%s\nname=%s\npreview=%ux%u\nstill=%ux%u\nmount=%d%s\nsimulated=%d\n", backend,
           info.name, info.preview_w, info.preview_h, info.still_w, info.still_h,
           info.mount_rotation, info.mount_mirror ? "m" : "", info.simulated ? 1 : 0);
    pocketcam_close(&cam);
    return 0;
}

static int run_snap(const char *backend, const char *script, const char *path, int display_rotation)
{
    struct pocketcam_backend cam;
    struct pocketcam_info info;
    struct pocketcam_frame f;
    FILE *fp;
    int r;

    if (open_camera(&cam, &info, backend, script, false) != 0) {
        return EXIT_NOCAMERA;
    }
    r = pocketcam_still(&cam, POCKETCAM_STILL_TIMEOUT_MS, &f);
    if (r != 0) {
        fprintf(stderr, "pos-camera: still: %s\n", pocketcam_strerror(r));
        pocketcam_close(&cam);
        return r == -ENODEV ? EXIT_LOST : EXIT_NOCAMERA;
    }
    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "pos-camera: %s: %s\n", path, strerror(errno));
        pocketcam_release(&cam, &f);
        pocketcam_close(&cam);
        return EXIT_USAGE;
    }
    r = pocketcam_encode(fp, &f, pocketcam_view_rotation(info.mount_rotation, display_rotation),
                         info.mount_mirror);
    if (fclose(fp) != 0 && r == 0) {
        r = -errno;
    }
    printf("snap %s %ux%u %s %s\n", path, f.width, f.height, pocketcam_codec_ext(),
           r == 0 ? "ok" : strerror(-r));
    pocketcam_release(&cam, &f);
    pocketcam_close(&cam);
    return r == 0 ? 0 : EXIT_USAGE;
}

/* For the bench (docs/hardware/CAMERA_GATE.md): what the camera costs on this
 * board. Timings are this process's own, on the wall clock. */
static int run_bench(const char *backend, const char *script, int display_rotation)
{
    bool portrait = display_rotation % 180 == 0;
    struct pocketcam_backend cam;
    struct pocketcam_info info;
    struct pocketcam_frame f;
    uint32_t vw = portrait ? 528 : 802;
    uint32_t vh = portrait ? 938 : 452;
    uint16_t *view = malloc((size_t)vw * vh * 2);
    int64_t t0 = mono_ms();
    int64_t t_open;
    int64_t t_first = -1;
    int64_t t_end;
    int64_t conv_ms = 0;
    int frames = 0;
    int timeouts = 0;
    int rot;
    FILE *fp;
    int r;

    if (!view || open_camera(&cam, &info, backend, script, false) != 0) {
        free(view);
        return EXIT_NOCAMERA;
    }
    rot = pocketcam_view_rotation(info.mount_rotation, display_rotation);
    t_open = mono_ms() - t0;
    r = pocketcam_start(&cam);
    if (r != 0) {
        fprintf(stderr, "pos-camera: start: %s\n", pocketcam_strerror(r));
        pocketcam_close(&cam);
        free(view);
        return EXIT_NOCAMERA;
    }
    t0 = mono_ms();
    while (mono_ms() - t0 < 4000) {
        int64_t c0;

        r = pocketcam_next(&cam, 500, &f);
        if (r == -ETIMEDOUT) {
            timeouts++;
            continue;
        }
        if (r != 0) {
            fprintf(stderr, "pos-camera: frame: %s\n", pocketcam_strerror(r));
            break;
        }
        if (t_first < 0) {
            t_first = mono_ms() - t0;
        }
        c0 = mono_ms();
        pocketcam_to_rgb565(&f, rot, info.mount_mirror, POCKETCAM_FIT_COVER, view, vw, vh, vw);
        conv_ms += mono_ms() - c0;
        frames++;
        pocketcam_release(&cam, &f);
    }
    t_end = mono_ms() - t0;
    printf("open %lld ms, first frame %lld ms after start, %d frames in %lld ms (%.1f fps), "
           "%d timeouts\n",
           (long long)t_open, (long long)t_first, frames, (long long)t_end,
           t_first >= 0 && frames > 1 ? (frames - 1) * 1000.0 / (double)(t_end - t_first) : 0.0,
           timeouts);
    printf("convert %ux%u turn %d: %.1f ms a frame\n", vw, vh, rot,
           frames ? (double)conv_ms / frames : 0.0);
    t0 = mono_ms();
    r = pocketcam_still(&cam, POCKETCAM_STILL_TIMEOUT_MS, &f);
    if (r != 0) {
        printf("still: %s\n", pocketcam_strerror(r));
    } else {
        int64_t t_still = mono_ms() - t0;

        fp = fopen("/tmp/pos-camera-bench.out", "wb");
        t0 = mono_ms();
        r = fp ? pocketcam_encode(fp, &f, rot, info.mount_mirror) : -errno;
        if (fp) {
            fseek(fp, 0, SEEK_END);
            printf("still %ux%u in %lld ms, %s encode %lld ms, %ld bytes (%s)\n", f.width, f.height,
                   (long long)t_still, pocketcam_codec_ext(), (long long)(mono_ms() - t0),
                   ftell(fp), r == 0 ? "ok" : strerror(-r));
            fclose(fp);
        }
        pocketcam_release(&cam, &f);
    }
    pocketcam_close(&cam);
    free(view);
    return 0;
}

/* The app's sequence, repeated: preview for a second, a still while it runs,
 * the still encoded, and the preview started again - the path that locked
 * unit A up once (docs/hardware/CAMERA_GATE.md). One line a cycle, flushed,
 * so a hang shows where it stopped. */
static int run_soak(const char *backend, const char *script, int cycles)
{
    struct pocketcam_backend cam;
    struct pocketcam_info info;
    struct pocketcam_frame f;
    int i;
    int r = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (open_camera(&cam, &info, backend, script, false) != 0) {
        return EXIT_NOCAMERA;
    }
    for (i = 1; i <= cycles && !stop_requested; i++) {
        int64_t t0 = mono_ms();
        int frames = 0;
        FILE *fp;

        r = pocketcam_start(&cam);
        if (r != 0) {
            printf("cycle %d: start: %s\n", i, pocketcam_strerror(r));
            break;
        }
        while (mono_ms() - t0 < 1000) {
            r = pocketcam_next(&cam, 500, &f);
            if (r == 0) {
                frames++;
                pocketcam_release(&cam, &f);
            } else if (r != -ETIMEDOUT) {
                break;
            }
        }
        if (r != 0 && r != -ETIMEDOUT) {
            printf("cycle %d: frame: %s\n", i, pocketcam_strerror(r));
            break;
        }
        printf("cycle %d: %d frames;", i, frames);
        t0 = mono_ms();
        r = pocketcam_still(&cam, POCKETCAM_STILL_TIMEOUT_MS, &f);
        if (r != 0) {
            printf(" still: %s\n", pocketcam_strerror(r));
            break;
        }
        printf(" still %lld ms;", (long long)(mono_ms() - t0));
        fp = fopen("/tmp/pos-camera-soak.out", "wb");
        if (fp) {
            r = pocketcam_encode(fp, &f, 0, false);
            fclose(fp);
        }
        pocketcam_release(&cam, &f);
        printf(" encoded %s\n", r == 0 ? "ok" : strerror(-r));
        if (i % 3 == 0) {
            /* Sometimes stopped from the preview rather than by a still. */
            pocketcam_start(&cam);
            pocketcam_stop(&cam);
        }
    }
    remove("/tmp/pos-camera-soak.out");
    pocketcam_close(&cam);
    printf("soak: %d of %d cycles\n", i - 1, cycles);
    return i - 1 == cycles ? 0 : EXIT_LOST;
}

#ifdef POS_CAMERA_TEST_HOOKS
/* pos-camera-testhooks only: a disk of a given size. */
static int64_t test_free_bytes(const char *dir)
{
    (void)dir;
    return strtoll(getenv("POCKETCAM_TEST_FREE_BYTES"), NULL, 10);
}

static void install_test_hooks(void)
{
    const char *free_env = getenv("POCKETCAM_TEST_FREE_BYTES");
    const char *fail_env = getenv("POCKETCAM_TEST_FAIL_AFTER");

    if (free_env && *free_env) {
        pocketcam_store_free_hook = test_free_bytes;
    }
    if (fail_env && *fail_env) {
        pocketcam_store_fail_after = strtoll(fail_env, NULL, 10);
    }
}
#endif

static void usage(void)
{
    fprintf(stderr,
            "usage: pos-camera session [--backend NAME] [--config CONFIG] [--dir DIR]\n"
            "       pos-camera probe [--backend NAME] [--config CONFIG]\n"
            "       pos-camera snap FILE [--backend NAME] [--config CONFIG] [--rotation DEG]\n"
            "       pos-camera bench [--backend NAME] [--config CONFIG] [--rotation DEG]\n"
            "       pos-camera soak CYCLES [--backend NAME] [--config CONFIG]\n"
            "DEG is the display rotation the picture is turned for: 0 (portrait,\n"
            "the default) or the shell's landscape, 270.\n"
            "CONFIG is the fake's script (--fake is the same) or the v4l2 backend's\n"
            "settings; the default comes from $POCKETOS_CAMERA_FAKE or\n"
            "$POCKETOS_CAMERA_CONFIG, whichever belongs to the backend.\n");
}

int main(int argc, char **argv)
{
    const char *backend = NULL;
    const char *script = NULL;
    const char *dir = NULL;
    const char *file = NULL;
    int display_rotation = 0;
    struct sigaction sa;
    int i;

    if (argc < 2) {
        usage();
        return EXIT_USAGE;
    }
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if ((strcmp(argv[i], "--fake") == 0 || strcmp(argv[i], "--config") == 0) &&
                   i + 1 < argc) {
            script = argv[++i];
        } else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--rotation") == 0 && i + 1 < argc &&
                   parse_rotation(argv[i + 1], &display_rotation) == 0) {
            i++;
        } else if (argv[i][0] != '-' && !file) {
            file = argv[i];
        } else {
            usage();
            return EXIT_USAGE;
        }
    }
    backend = pick_backend(backend);
    /* Each backend reads its own settings: a fake's fault script must never
     * reach the real camera, nor the real camera's settings the fake. */
    if (!script) {
        script = getenv(strcmp(backend, "fake") == 0 ? "POCKETOS_CAMERA_FAKE"
                                                     : "POCKETOS_CAMERA_CONFIG");
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
#ifdef POS_CAMERA_TEST_HOOKS
    install_test_hooks();
#endif
    if (strcmp(argv[1], "session") == 0 && !file) {
        return run_session(backend, script, dir);
    }
    if (strcmp(argv[1], "probe") == 0 && !file) {
        return run_probe(backend, script);
    }
    if (strcmp(argv[1], "snap") == 0 && file) {
        return run_snap(backend, script, file, display_rotation);
    }
    if (strcmp(argv[1], "soak") == 0 && file) {
        int n = atoi(file);

        return n > 0 && n <= 10000 ? run_soak(backend, script, n) : EXIT_USAGE;
    }
    if (strcmp(argv[1], "bench") == 0 && !file) {
        return run_bench(backend, script, display_rotation);
    }
    usage();
    return EXIT_USAGE;
}
