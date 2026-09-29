/*
 * pos-video: the Video app's helper, and the only program that opens a video
 * file, the hardware video decoder or - for a video's sound - the sound card
 * for it (docs/decisions/ADR-012-video-playback.md).
 *
 *   pos-video session [--backend ffmpeg|fake] [--volume-percent L]
 *                     [--allow-unverified]
 *       The app's player: commands on stdin, events on stdout
 *       (apps/video/video_proto.h), pictures into the sealed memfd on
 *       descriptor 3. L is the system volume, 0 (muted: no sound is opened)
 *       to 100.
 *   pos-video probe [--backend NAME] FILE
 *       What the player would say about FILE, and the time to its first
 *       picture. Opens the decoder, never the sound card.
 *   pos-video bench [--backend NAME] [--size WxH] FILE
 *       Decode every picture as fast as possible at WxH (default 640x360)
 *       into memory: pictures per second and CPU. No sound card.
 *   pos-video recover
 *       Undo what a player killed outright left switched on the sound card
 *       (pocketaudio_recover). The app starts this, detached, after it had
 *       to kill a player that had sound.
 *
 * STOPPING. SIGTERM, SIGINT, `quit` and a closed stdin all end the session
 * loop: the sound thread closes the card (amplifier off, route back, lock
 * released), the file and the decoder are closed, `bye` is said if anyone is
 * listening. A helper killed outright leaves its audio route to the next
 * audio open or `recover`, as pocketaudio.h describes, and its decoder to the
 * kernel.
 *
 * The backend is `ffmpeg` in a build with POCKETVIDEO_FFMPEG=1 (the device),
 * else `fake` (tools/video/video_backend_fake.c). Nothing is logged: no file
 * names, no pictures.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketaudio/pocketaudio.h"
#include "video_backend.h"
#include "video_player.h"
#include "video_proto.h"
#ifdef POS_VIDEO_TEST_HOOKS
/* tests/pos-video-testhooks only: POS_VIDEO_FAKE_AUDIO=<dir> replaces the
 * sound card with files that pace like a device (tests/fake_audio_backend.c).
 * The shipped helper has no such hook. */
#include "fake_audio_backend.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef POCKETVIDEO_HAVE_FFMPEG
#define BACKEND_DEFAULT "ffmpeg"
#else
#define BACKEND_DEFAULT "fake"
#endif

static volatile sig_atomic_t stop_requested;
static int out_broken;

static void on_signal(int sig)
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

static void emit(void *user, const char *line)
{
    char buf[VIDEO_LINE_MAX + 2];
    size_t n = strlen(line);
    size_t off = 0;

    (void)user;
    if (out_broken) {
        return;
    }
    if (n > VIDEO_LINE_MAX - 1) {
        n = VIDEO_LINE_MAX - 1;
    }
    memcpy(buf, line, n);
    buf[n++] = '\n';
    while (off < n) {
        ssize_t w = write(1, buf + off, n - off);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            out_broken = 1; /* the app is gone: the loop ends */
            return;
        }
        off += (size_t)w;
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: pos-video session [--backend NAME] [--volume-percent L] "
                    "[--allow-unverified]\n"
                    "       pos-video probe [--backend NAME] FILE\n"
                    "       pos-video bench [--backend NAME] [--size WxH] FILE\n"
                    "       pos-video recover\n");
}

static const struct pocketaudio_backend *audio_backend(const struct pocketaudio_board **board,
                                                       int *allow_unverified)
{
    *board = NULL;
#ifdef POS_VIDEO_TEST_HOOKS
    {
        const char *dir = getenv("POS_VIDEO_FAKE_AUDIO");

        if (dir && *dir) {
            *board = fake_audio_board();
            *allow_unverified = 1;
            return fake_audio_backend(dir);
        }
        if (getenv("POS_VIDEO_NO_AUDIO")) {
            return NULL;
        }
    }
#else
    (void)allow_unverified;
#endif
    return pocketaudio_alsa_backend();
}

/* ---- session -------------------------------------------------------------------- */

static uint8_t *map_shm(void)
{
    struct stat st;
    struct video_shm_header h;
    void *map;

    if (fstat(VIDEO_SHM_FD, &st) != 0 || st.st_size < (off_t)VIDEO_SHM_BYTES ||
        pread(VIDEO_SHM_FD, &h, sizeof(h), 0) != (ssize_t)sizeof(h) || h.magic != VIDEO_SHM_MAGIC ||
        h.version != VIDEO_PROTO_VERSION || h.slots != VIDEO_SLOTS ||
        h.slot_bytes != VIDEO_SLOT_BYTES || h.max_pixels != VIDEO_VIEW_MAX_PIXELS) {
        return NULL;
    }
    map = mmap(NULL, VIDEO_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, VIDEO_SHM_FD, 0);
    close(VIDEO_SHM_FD);
    return map == MAP_FAILED ? NULL : map;
}

static int session(const char *backend_name, int volume, int allow_unverified)
{
    const struct video_backend *backend = video_backend_by_name(backend_name);
    struct video_player_config cfg;
    struct video_player *player = NULL;
    char line[VIDEO_LINE_MAX];
    size_t len = 0;
    int overlong = 0;
    int quit = 0;
    int wait = 0;
    uint8_t *shm;

    memset(&cfg, 0, sizeof(cfg));
    {
        char hello[64];

        snprintf(hello, sizeof(hello), "hello %d %s", VIDEO_PROTO_VERSION,
                 backend ? backend->name : "none");
        emit(NULL, hello);
    }
    if (!backend) {
        emit(NULL, "error device no such backend");
        return 2;
    }
    shm = map_shm();
    if (!shm) {
        emit(NULL, "error device no shared memory on descriptor 3");
        return 2;
    }
    cfg.backend = backend;
    cfg.allow_unverified = allow_unverified;
    cfg.audio_backend = audio_backend(&cfg.audio_board, &cfg.allow_unverified);
    cfg.volume_percent = volume;
    cfg.shm = shm;
    cfg.emit = emit;
    cfg.now_ms = mono_ms;
    if (video_player_create(&player, &cfg) != 0) {
        emit(NULL, "error device the player could not start");
        munmap(shm, VIDEO_SHM_BYTES);
        return 2;
    }
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    while (!quit && !stop_requested && !out_broken) {
        struct pollfd pfd = { .fd = 0, .events = POLLIN };
        int r = poll(&pfd, 1, wait);

        if (r < 0 && errno != EINTR) {
            break;
        }
        if (r > 0) {
            char buf[512];
            ssize_t n = read(0, buf, sizeof(buf));
            ssize_t i;

            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                break; /* the app is gone */
            }
            for (i = 0; i < n && !quit; i++) {
                if (buf[i] == '\n') {
                    if (!overlong) {
                        line[len] = '\0';
                        quit = video_player_command(player, line);
                    }
                    len = 0;
                    overlong = 0;
                } else if (overlong) {
                    continue;
                } else if (len + 1 >= sizeof(line)) {
                    overlong = 1; /* ignored whole: a newer app may say more */
                    len = 0;
                } else {
                    line[len++] = buf[i];
                }
            }
        }
        if (!quit) {
            wait = video_player_step(player);
        }
    }
    video_player_destroy(player);
    munmap(shm, VIDEO_SHM_BYTES);
    emit(NULL, "bye");
    return 0;
}

/* ---- bench tools ---------------------------------------------------------------- */

static double cpu_s(void)
{
    struct rusage ru;

    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + (double)ru.ru_stime.tv_sec +
           ru.ru_stime.tv_usec / 1e6;
}

static int probe(const char *backend_name, const char *path, int bench, uint32_t bw, uint32_t bh)
{
    const struct video_backend *b = video_backend_by_name(backend_name);
    struct video_media_info info;
    char reason[VIDEO_BACKEND_WORD_MAX] = "";
    char text[VIDEO_BACKEND_TEXT_MAX] = "";
    uint16_t *px;
    uint32_t w;
    uint32_t h;
    void *ctx;
    int64_t t0 = mono_ms();
    double c0 = cpu_s();
    uint64_t pictures = 0;
    uint64_t samples = 0;
    int video_done = 0;
    int audio_done = 0;
    int rc = 0;

    if (!b) {
        fprintf(stderr, "pos-video: no backend %s\n", backend_name);
        return 2;
    }
    if (b->open(&ctx, path, bench, &info, reason, text, sizeof(text)) != 0) {
        printf("openfail %s %s\n", reason, text);
        return 1;
    }
    printf("backend %s\ncodec %s\nsize %ux%u\nfps_x100 %u\nduration_ms %lld\naudio %s\n", b->name,
           info.codec, info.src_w, info.src_h, info.fps_x100, (long long)info.duration_ms,
           !info.has_audio ? "none" : info.audio_decodable ? "decodable" : "unsupported");
    if (video_fit(info.src_w, info.src_h, bw, bh, &w, &h) != 0 ||
        b->output(ctx, w, h, text, sizeof(text)) != 0) {
        printf("output_error %s\n", text);
        b->close(ctx);
        return 1;
    }
    px = malloc((size_t)w * h * 2);
    if (!px) {
        b->close(ctx);
        return 1;
    }
    printf("output %ux%u\n", w, h);
    while (!video_done || (bench && !audio_done)) {
        struct video_item it;

        if (!video_done) {
            b->next_picture(ctx, &it);
            if (it.kind == VIDEO_ITEM_PICTURE) {
                if (b->picture(ctx, px, w, h) != 0) {
                    printf("picture_error\n");
                    rc = 1;
                    break;
                }
                if (pictures++ == 0) {
                    printf("first_picture_ms %lld pts %lld\n", (long long)(mono_ms() - t0),
                           (long long)it.pts_ms);
                    if (!bench) {
                        break;
                    }
                }
            } else if (it.kind == VIDEO_ITEM_EOF) {
                video_done = 1;
            } else if (it.kind == VIDEO_ITEM_ERROR) {
                printf("decode_error %s\n", it.text);
                rc = 1;
                break;
            }
        }
        if (bench && !audio_done) {
            b->next_audio(ctx, &it);
            if (it.kind == VIDEO_ITEM_AUDIO) {
                samples += it.count;
            } else if (it.kind == VIDEO_ITEM_EOF || it.kind == VIDEO_ITEM_ERROR) {
                audio_done = 1;
            }
        }
        if (stop_requested) {
            break;
        }
    }
    if (bench) {
        int64_t ms = mono_ms() - t0;
        double cpu = cpu_s() - c0;

        printf("pictures %llu\nsamples %llu\nwall_ms %lld\ncpu_s %.2f\nfps %.1f\n",
               (unsigned long long)pictures, (unsigned long long)samples, (long long)ms, cpu,
               ms > 0 ? pictures * 1000.0 / ms : 0.0);
        if (info.duration_ms > 0) {
            printf("cpu_pct_of_realtime %.1f\n", cpu * 100000.0 / (double)info.duration_ms);
        }
    }
    free(px);
    b->close(ctx);
    return rc;
}

static int recover(void)
{
    const struct pocketaudio_board *board = NULL;
    struct pocketaudio_options o;
    char report[160] = "";
    int allow = 0;
    int r;

    memset(&o, 0, sizeof(o));
    o.backend = audio_backend(&board, &allow);
    if (!o.backend) {
        return 0;
    }
    o.board = board;
    r = pocketaudio_recover(&o, report, sizeof(report));
    printf("recover %d %s\n", r, report);
    return r < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *backend = BACKEND_DEFAULT;
    const char *file = NULL;
    uint32_t bw = 640;
    uint32_t bh = 360;
    int volume = 100;
    int allow_unverified = 0;
    struct sigaction sa;
    int i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) {
        usage();
        return 2;
    }
    for (i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;

        if (strcmp(a, "--backend") == 0 && v) {
            backend = v;
            i++;
        } else if (strcmp(a, "--volume-percent") == 0 && v) {
            char *end;
            long n = strtol(v, &end, 10);

            if (*end || n < 0 || n > 100) {
                usage();
                return 2;
            }
            volume = (int)n;
            i++;
        } else if (strcmp(a, "--size") == 0 && v) {
            if (sscanf(v, "%ux%u", &bw, &bh) != 2 || !bw || !bh) {
                usage();
                return 2;
            }
            i++;
        } else if (strcmp(a, "--allow-unverified") == 0) {
            allow_unverified = 1;
        } else if (a[0] != '-' && !file) {
            file = a;
        } else {
            usage();
            return 2;
        }
    }
    if (strcmp(argv[1], "session") == 0 && !file) {
        return session(backend, volume, allow_unverified);
    }
    if ((strcmp(argv[1], "probe") == 0 || strcmp(argv[1], "bench") == 0) && file) {
        return probe(backend, file, strcmp(argv[1], "bench") == 0, bw, bh);
    }
    if (strcmp(argv[1], "recover") == 0 && !file) {
        return recover();
    }
    usage();
    return 2;
}
