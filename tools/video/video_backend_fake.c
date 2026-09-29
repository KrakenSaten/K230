/*
 * The fake decoding backend. See video_backend.h.
 *
 * A fake video is a small text file whose first line is
 *
 *   DOORS-FAKE-VIDEO key=value ...
 *
 *   w=, h=         the stored picture size (default 320 x 180)
 *   fps=           frames per second, 1..120 (default 25)
 *   ms=            length (default 4000)
 *   gop=           frames from one key picture to the next (default fps)
 *   audio=0|1|bad  no sound, a quiet tone, or a sound track this build
 *                  "cannot decode" (default 0)
 *   codec=         the codec it claims (default h264)
 *   fail=          refuse to open: missing, unsupported, corrupt, device, io
 *   error_at=N     picture N is an unrecoverable decode error
 *   hang_at=N      decoding picture N never returns (a stuck decoder)
 *   crash_at=N     the helper aborts at picture N
 *   slow_ms=N      every picture takes N ms to decode
 *   output_fail=1  the decoder cannot be set to a new size
 *
 * Anything else in the file, or an unknown key, is a damaged file ("corrupt"),
 * so an invalid file is tested through the same path as a real one.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "video_backend.h"

#include "video_proto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FAKE_MAGIC "DOORS-FAKE-VIDEO"
/* The tone: a square wave far below the ceiling, 32 samples a half period. */
#define FAKE_TONE_LEVEL 1200
#define FAKE_TONE_HALF 32
#define FAKE_AUDIO_CHUNK 1024

struct fake {
    uint32_t w, h;
    uint32_t fps;
    int64_t ms;
    uint32_t gop;
    int audio;          /* 0 none, 1 tone, 2 undecodable */
    char codec[VIDEO_BACKEND_WORD_MAX];
    int64_t error_at, hang_at, crash_at;
    int slow_ms;
    int output_fail;

    uint32_t out_w, out_h;
    int64_t frames;     /* pictures in the file */
    int64_t next_pic;   /* index of the next picture to decode */
    int64_t next_sample;/* 48 kHz index of the next sound sample */
    int64_t samples;    /* sound samples in the file */
    int64_t held;       /* the picture next() returned, -1 when none */
    int16_t chunk[FAKE_AUDIO_CHUNK];
};

static void say(char *out, size_t n, const char *fmt, const char *arg)
{
    if (out && n) {
        snprintf(out, n, fmt, arg ? arg : "");
    }
}

static int parse_int(const char *v, int64_t lo, int64_t hi, int64_t *out)
{
    char *end;
    long long x;

    errno = 0;
    x = strtoll(v, &end, 10);
    if (errno || end == v || *end || x < lo || x > hi) {
        return -1;
    }
    *out = x;
    return 0;
}

/* One key=value. 0, 1 (a fail= word into fail), or -1 when unknown/bad. */
static int apply(struct fake *f, const char *key, const char *val, char *fail, size_t faillen)
{
    int64_t v;

    if (strcmp(key, "fail") == 0) {
        static const char *const ok[] = { VIDEO_OPENFAIL_MISSING, VIDEO_OPENFAIL_UNSUPPORTED,
                                          VIDEO_OPENFAIL_CORRUPT, VIDEO_OPENFAIL_DEVICE,
                                          VIDEO_OPENFAIL_IO };
        size_t i;

        for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
            if (strcmp(val, ok[i]) == 0) {
                snprintf(fail, faillen, "%s", val);
                return 1;
            }
        }
        return -1;
    }
    if (strcmp(key, "codec") == 0) {
        if (!*val || strlen(val) >= sizeof(f->codec)) {
            return -1;
        }
        snprintf(f->codec, sizeof(f->codec), "%s", val);
        return 0;
    }
    if (strcmp(key, "audio") == 0) {
        if (strcmp(val, "bad") == 0) {
            f->audio = 2;
            return 0;
        }
        if (parse_int(val, 0, 1, &v) != 0) {
            return -1;
        }
        f->audio = (int)v;
        return 0;
    }
    if (strcmp(key, "w") == 0 || strcmp(key, "h") == 0) {
        if (parse_int(val, 2, 8192, &v) != 0) {
            return -1;
        }
        if (key[0] == 'w') {
            f->w = (uint32_t)v;
        } else {
            f->h = (uint32_t)v;
        }
        return 0;
    }
    if (strcmp(key, "fps") == 0) {
        if (parse_int(val, 1, 120, &v) != 0) {
            return -1;
        }
        f->fps = (uint32_t)v;
        return 0;
    }
    if (strcmp(key, "ms") == 0) {
        if (parse_int(val, 1, VIDEO_DURATION_MAX_MS, &v) != 0) {
            return -1;
        }
        f->ms = v;
        return 0;
    }
    if (strcmp(key, "gop") == 0) {
        if (parse_int(val, 1, 1000, &v) != 0) {
            return -1;
        }
        f->gop = (uint32_t)v;
        return 0;
    }
    if (strcmp(key, "slow_ms") == 0) {
        if (parse_int(val, 0, 10000, &v) != 0) {
            return -1;
        }
        f->slow_ms = (int)v;
        return 0;
    }
    if (strcmp(key, "output_fail") == 0) {
        if (parse_int(val, 0, 1, &v) != 0) {
            return -1;
        }
        f->output_fail = (int)v;
        return 0;
    }
    if (strcmp(key, "error_at") == 0 || strcmp(key, "hang_at") == 0 ||
        strcmp(key, "crash_at") == 0) {
        if (parse_int(val, 0, INT32_MAX, &v) != 0) {
            return -1;
        }
        if (key[0] == 'e') {
            f->error_at = v;
        } else if (key[0] == 'h') {
            f->hang_at = v;
        } else {
            f->crash_at = v;
        }
        return 0;
    }
    return -1;
}

static int fake_open(void **ctx, const char *path, int want_audio, struct video_media_info *info,
                     char *reason, char *text, size_t textlen)
{
    struct fake *f;
    struct stat st;
    FILE *fp;
    char line[512];
    char fail[VIDEO_BACKEND_WORD_MAX] = "";
    char *save = NULL;
    char *tok;

    *ctx = NULL;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        say(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_MISSING);
        say(text, textlen, "%s", "no such file");
        return -1;
    }
    fp = fopen(path, "r");
    if (!fp) {
        say(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_IO);
        say(text, textlen, "%s", strerror(errno));
        return -1;
    }
    if (!fgets(line, sizeof(line), fp)) {
        line[0] = '\0';
    }
    fclose(fp);
    line[strcspn(line, "\r\n")] = '\0';
    tok = strtok_r(line, " ", &save);
    if (!tok || strcmp(tok, FAKE_MAGIC) != 0) {
        say(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_CORRUPT);
        say(text, textlen, "%s", "not a video file");
        return -1;
    }
    f = calloc(1, sizeof(*f));
    if (!f) {
        say(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_IO);
        say(text, textlen, "%s", "out of memory");
        return -1;
    }
    f->w = 320;
    f->h = 180;
    f->fps = 25;
    f->ms = 4000;
    f->error_at = f->hang_at = f->crash_at = -1;
    snprintf(f->codec, sizeof(f->codec), "h264");
    while ((tok = strtok_r(NULL, " ", &save)) != NULL) {
        char *eq = strchr(tok, '=');
        int r;

        if (!eq) {
            r = -1;
        } else {
            *eq = '\0';
            r = apply(f, tok, eq + 1, fail, sizeof(fail));
        }
        if (r < 0) {
            free(f);
            say(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_CORRUPT);
            say(text, textlen, "%s", "damaged video header");
            return -1;
        }
    }
    if (fail[0]) {
        free(f);
        say(reason, VIDEO_BACKEND_WORD_MAX, "%s", fail);
        say(text, textlen, "simulated %s", fail);
        return -1;
    }
    if (strcmp(f->codec, "h264") != 0) {
        char c[VIDEO_BACKEND_WORD_MAX];

        snprintf(c, sizeof(c), "%s", f->codec);
        free(f);
        say(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_UNSUPPORTED);
        say(text, textlen, "%s pictures are not supported", c);
        return -1;
    }
    if (f->gop == 0) {
        f->gop = f->fps;
    }
    f->frames = f->ms * f->fps / 1000;
    if (f->frames < 1) {
        f->frames = 1;
    }
    f->samples = f->audio == 1 && want_audio ? f->ms * VIDEO_AUDIO_RATE / 1000 : 0;
    f->out_w = f->w;
    f->out_h = f->h;
    f->held = -1;

    memset(info, 0, sizeof(*info));
    info->duration_ms = f->ms;
    info->src_w = f->w;
    info->src_h = f->h;
    info->fps_x100 = f->fps * 100;
    info->has_audio = f->audio != 0;
    info->audio_decodable = f->audio == 1;
    snprintf(info->codec, sizeof(info->codec), "%s", f->codec);
    *ctx = f;
    return 0;
}

static int fake_output(void *ctx, uint32_t w, uint32_t h, char *text, size_t textlen)
{
    struct fake *f = ctx;

    if (f->output_fail) {
        say(text, textlen, "%s", "simulated decoder reconfiguration failure");
        return -1;
    }
    f->out_w = w;
    f->out_h = h;
    f->held = -1;
    return 0;
}

static int64_t pic_pts(const struct fake *f, int64_t i)
{
    return i * 1000 / f->fps;
}

static int fake_seek(void *ctx, int64_t ms, char *text, size_t textlen)
{
    struct fake *f = ctx;
    int64_t i;

    (void)text;
    (void)textlen;
    if (ms < 0) {
        ms = 0;
    }
    i = ms * f->fps / 1000;
    if (i >= f->frames) {
        i = f->frames - 1;
    }
    i -= i % f->gop;
    f->next_pic = i;
    f->next_sample = pic_pts(f, i) * VIDEO_AUDIO_RATE / 1000;
    if (f->next_sample > f->samples) {
        f->next_sample = f->samples;
    }
    f->held = -1;
    return 0;
}

static void sleep_ms(int ms)
{
    struct timespec d = { ms / 1000, (long)(ms % 1000) * 1000000L };

    while (nanosleep(&d, &d) != 0 && errno == EINTR) {
    }
}

static void fake_next_audio(void *ctx, struct video_item *it)
{
    struct fake *f = ctx;
    size_t n = FAKE_AUDIO_CHUNK;
    size_t i;

    memset(it, 0, sizeof(*it));
    if (f->next_sample >= f->samples) {
        it->kind = VIDEO_ITEM_EOF;
        return;
    }
    if ((int64_t)n > f->samples - f->next_sample) {
        n = (size_t)(f->samples - f->next_sample);
    }
    for (i = 0; i < n; i++) {
        int64_t k = f->next_sample + (int64_t)i;

        f->chunk[i] = (int16_t)((k / FAKE_TONE_HALF) % 2 ? FAKE_TONE_LEVEL : -FAKE_TONE_LEVEL);
    }
    it->kind = VIDEO_ITEM_AUDIO;
    it->pts_ms = f->next_sample * 1000 / VIDEO_AUDIO_RATE;
    it->samples = f->chunk;
    it->count = n;
    f->next_sample += (int64_t)n;
}

static void fake_next_picture(void *ctx, struct video_item *it)
{
    struct fake *f = ctx;
    int64_t pic_ms;

    memset(it, 0, sizeof(*it));
    f->held = -1;
    if (f->next_pic >= f->frames) {
        it->kind = VIDEO_ITEM_EOF;
        return;
    }
    pic_ms = pic_pts(f, f->next_pic);
    if (f->next_pic == f->crash_at) {
        abort();
    }
    if (f->next_pic == f->hang_at) {
        for (;;) {
            sleep_ms(1000);
        }
    }
    if (f->next_pic == f->error_at) {
        it->kind = VIDEO_ITEM_ERROR;
        snprintf(it->text, sizeof(it->text), "simulated decode error at picture %lld",
                 (long long)f->next_pic);
        return;
    }
    if (f->slow_ms > 0) {
        sleep_ms(f->slow_ms);
    }
    it->kind = VIDEO_ITEM_PICTURE;
    it->pts_ms = pic_ms;
    f->held = f->next_pic++;
}

/* The pattern: a diagonal gradient that moves one step a frame, and a bar
 * whose height is the frame number, so a test can read which picture it got. */
static int fake_picture(void *ctx, uint16_t *dst, uint32_t w, uint32_t h)
{
    struct fake *f = ctx;
    uint32_t x;
    uint32_t y;

    if (f->held < 0 || w != f->out_w || h != f->out_h) {
        return -1;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t v = (x + y + (uint32_t)f->held) & 0x1f;

            dst[(size_t)y * w + x] = (uint16_t)((v << 11) | ((y * 63 / h) << 5) | (x * 31 / w));
        }
    }
    /* The first pixel carries the picture number, low 16 bits. */
    dst[0] = (uint16_t)(f->held & 0xffff);
    f->held = -1;
    return 0;
}

static void fake_close(void *ctx)
{
    free(ctx);
}

static const struct video_backend fake_backend = {
    .name = "fake",
    .open = fake_open,
    .output = fake_output,
    .seek = fake_seek,
    .next_picture = fake_next_picture,
    .next_audio = fake_next_audio,
    .picture = fake_picture,
    .close = fake_close,
};

const struct video_backend *video_backend_fake(void)
{
    return &fake_backend;
}

const struct video_backend *video_backend_by_name(const char *name)
{
    if (!name) {
        return NULL;
    }
    if (strcmp(name, "fake") == 0) {
        return &fake_backend;
    }
#ifdef POCKETVIDEO_HAVE_FFMPEG
    if (strcmp(name, "ffmpeg") == 0) {
        return video_backend_ffmpeg();
    }
#endif
    return NULL;
}
