/*
 * pos-video's player engine. See video_player.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "video_player.h"

#include "video_proto.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Sound decoded ahead of the speaker, at most (the ring holds more, so a
 * burst of sound packets from the file always fits). */
#define AUDIO_AHEAD_SAMPLES (2 * VIDEO_AUDIO_RATE)
/* What the device holds queued once it is full: pocketaudio's buffer. */
#define AUDIO_DEVICE_QUEUE ((int64_t)POCKETAUDIO_PLAYBACK_BUFFER_FRAMES)
/* The thread's wait for more samples when the ring is empty. */
#define AUDIO_STARVE_WAIT_MS 10
#define AUDIO_DRAIN_MS 1000

enum pstate { PS_IDLE, PS_PAUSED, PS_PLAYING, PS_STOPPED, PS_ENDED };
enum slot_owner { SLOT_FREE, SLOT_PENDING, SLOT_SESSION };

/* ---- the sound thread ----------------------------------------------------------- */

struct audio {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    int started;
    int quit;

    /* main thread -> sound thread */
    int want_run;          /* the card should play */
    unsigned run_gen;      /* bumped by every start; a failed open is tried once per gen */
    int16_t *ring;
    size_t head;
    size_t count;
    int64_t head_pos;      /* the file position of ring[head], in samples */
    unsigned flush_gen;    /* bumped whenever the main thread empties the ring */
    int eof;               /* nothing more will be pushed */

    /* sound thread -> main thread */
    int have_clock;
    int64_t clock_samples; /* the file position being heard */
    int64_t clock_at;      /* when that was worked out */
    int drained;           /* eof, and all of it played */
    int status;            /* a failed open or write (enum pocketaudio_err), not yet reported */
    unsigned fail_gen;
    unsigned xruns;

    /* the sound thread's own */
    struct pocketaudio_stream *stream;
    int64_t written;       /* samples accepted since the stream was opened */
    struct pocketaudio_options opts;
    int64_t (*now)(void);
};

static void audio_close_locked(struct audio *a)
{
    struct pocketaudio_stream *s = a->stream;

    if (!s) {
        return;
    }
    a->stream = NULL;
    a->have_clock = 0;
    pthread_mutex_unlock(&a->mu);
    /* Amplifier off, queue dropped, route back, lock released. */
    pocketaudio_close(s);
    pthread_mutex_lock(&a->mu);
}

static void timed_wait(struct audio *a, int ms)
{
    struct timespec t;

    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += (long)ms * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&a->cv, &a->mu, &t);
}

static void *audio_main(void *arg)
{
    struct audio *a = arg;
    int16_t buf[POCKETAUDIO_PERIOD_FRAMES];

    pthread_mutex_lock(&a->mu);
    while (!a->quit) {
        struct pocketaudio_stream *s;
        unsigned fg;
        size_t n;
        size_t i;
        long w;

        if (!a->want_run) {
            audio_close_locked(a);
            if (!a->want_run && !a->quit) {
                pthread_cond_wait(&a->cv, &a->mu);
            }
            continue;
        }
        if (!a->stream) {
            unsigned gen = a->run_gen;
            struct pocketaudio_stream *opened = NULL;
            int r;

            if (a->fail_gen == gen) {
                pthread_cond_wait(&a->cv, &a->mu);
                continue;
            }
            pthread_mutex_unlock(&a->mu);
            r = pocketaudio_open(&opened, POCKETAUDIO_PLAYBACK, &a->opts, NULL, 0);
            pthread_mutex_lock(&a->mu);
            if (r != 0) {
                a->fail_gen = gen;
                a->status = r;
                continue;
            }
            a->stream = opened;
            a->written = 0;
            if (!a->want_run || a->quit || a->run_gen != gen) {
                audio_close_locked(a);
            }
            continue;
        }
        if (a->count == 0) {
            if (a->eof && !a->drained) {
                s = a->stream;
                fg = a->flush_gen;
                pthread_mutex_unlock(&a->mu);
                pocketaudio_drain(s, AUDIO_DRAIN_MS);
                pthread_mutex_lock(&a->mu);
                if (fg == a->flush_gen) {
                    a->drained = 1;
                    a->have_clock = 0;
                }
                continue;
            }
            timed_wait(a, AUDIO_STARVE_WAIT_MS);
            continue;
        }
        n = a->count < POCKETAUDIO_PERIOD_FRAMES ? a->count : POCKETAUDIO_PERIOD_FRAMES;
        for (i = 0; i < n; i++) {
            buf[i] = a->ring[(a->head + i) % VIDEO_AUDIO_RING_SAMPLES];
        }
        s = a->stream;
        fg = a->flush_gen;
        pthread_mutex_unlock(&a->mu);
        w = pocketaudio_write(s, buf, n);
        pthread_mutex_lock(&a->mu);
        a->xruns = pocketaudio_xruns(s);
        if (w < 0) {
            a->status = (int)w;
            a->fail_gen = a->run_gen;
            audio_close_locked(a);
            continue;
        }
        if (fg != a->flush_gen || w == 0) {
            continue; /* emptied meanwhile (a seek), or the device stayed busy */
        }
        a->head = (a->head + (size_t)w) % VIDEO_AUDIO_RING_SAMPLES;
        a->count -= (size_t)w;
        a->head_pos += w;
        a->written += w;
        a->clock_samples = a->head_pos - (a->written < AUDIO_DEVICE_QUEUE ? a->written
                                                                          : AUDIO_DEVICE_QUEUE);
        a->clock_at = a->now();
        a->have_clock = 1;
    }
    audio_close_locked(a);
    pthread_mutex_unlock(&a->mu);
    return NULL;
}

static void audio_run(struct audio *a, int on)
{
    pthread_mutex_lock(&a->mu);
    if (on && !a->want_run) {
        a->run_gen++;
    }
    a->want_run = on;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
}

static void audio_flush(struct audio *a)
{
    pthread_mutex_lock(&a->mu);
    a->head = 0;
    a->count = 0;
    a->flush_gen++;
    a->eof = 0;
    a->drained = 0;
    a->have_clock = 0;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
}

static size_t audio_count(struct audio *a)
{
    size_t n;

    pthread_mutex_lock(&a->mu);
    n = a->count;
    pthread_mutex_unlock(&a->mu);
    return n;
}

/* Append samples that start at file position pos (samples). Returns how many
 * fitted. */
static size_t audio_push(struct audio *a, const int16_t *s, size_t n, int64_t pos)
{
    size_t room;
    size_t i;

    pthread_mutex_lock(&a->mu);
    if (a->count == 0) {
        a->head_pos = pos;
    }
    room = VIDEO_AUDIO_RING_SAMPLES - a->count;
    if (n > room) {
        n = room;
    }
    for (i = 0; i < n; i++) {
        a->ring[(a->head + a->count + i) % VIDEO_AUDIO_RING_SAMPLES] = s[i];
    }
    a->count += n;
    a->drained = 0;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
    return n;
}

static void audio_set_eof(struct audio *a)
{
    pthread_mutex_lock(&a->mu);
    a->eof = 1;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
}

/* ---- the player ------------------------------------------------------------------ */

struct pend {
    int slot;
    int64_t pts;
};

struct video_player {
    struct video_player_config cfg;
    int64_t (*now)(void);
    struct audio a;

    /* the file */
    void *ctx;
    int open;
    struct video_media_info info;
    uint32_t view_w;
    uint32_t view_h;
    uint32_t out_w;
    uint32_t out_h;
    enum pstate state;
    int want_audio;        /* the file's sound is decoded for the card */
    int sink;              /* ... and the card takes it (not failed) */
    int audio_eof;
    int video_eof;
    int64_t frame_ms;
    int64_t last_pts;
    int64_t skip_until;    /* pictures before this are not shown (a seek), -1: none */
    int64_t audio_skip;    /* samples before this are not played, -1: none */
    int preview;           /* show the next picture at once */
    int seek_reply;        /* and answer `seeked` when it is */
    /* A decode error, reported once the pictures decoded before it are shown. */
    int video_error;
    char error_text[VIDEO_BACKEND_TEXT_MAX];

    /* slots */
    enum slot_owner owner[VIDEO_SLOTS];
    struct pend pend[VIDEO_SLOTS];
    int npend;
    uint32_t seq;

    /* the clock */
    int64_t pos_ms;        /* where playback is while not playing */
    int clock_running;
    int64_t base_media;
    int64_t base_wall;
    int64_t play_at;
    int64_t last_show_at;

    /* progress and statistics */
    int64_t pos_emit_at;
    int64_t stats_at;
    uint64_t stats_shown;
    long long cpu_ticks;
    struct video_player_counters c;
};

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void emitf(struct video_player *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void emitf(struct video_player *p, const char *fmt, ...)
{
    char line[VIDEO_LINE_MAX];
    va_list ap;
    int n;
    int i;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    /* One line, whatever a file name or a decoder message contained. */
    for (i = 0; line[i]; i++) {
        if ((unsigned char)line[i] < 0x20) {
            line[i] = ' ';
        }
    }
    p->cfg.emit(p->cfg.user, line);
}

int video_fit(uint32_t src_w, uint32_t src_h, uint32_t view_w, uint32_t view_h, uint32_t *w,
              uint32_t *h)
{
    uint64_t tw;
    uint64_t th;

    if (!src_w || !src_h || !view_w || !view_h) {
        return -1;
    }
    if (view_w > VIDEO_VIEW_MAX_W) {
        view_w = VIDEO_VIEW_MAX_W;
    }
    if (view_h > VIDEO_VIEW_MAX_H) {
        view_h = VIDEO_VIEW_MAX_H;
    }
    if (src_w <= view_w && src_h <= view_h) {
        tw = src_w;
        th = src_h;
    } else if ((uint64_t)view_w * src_h <= (uint64_t)view_h * src_w) {
        tw = view_w;
        th = (uint64_t)src_h * view_w / src_w;
    } else {
        th = view_h;
        tw = (uint64_t)src_w * view_h / src_h;
    }
    while (tw * th > VIDEO_VIEW_MAX_PIXELS) {
        tw = tw * 15 / 16;
        th = th * 15 / 16;
    }
    tw -= tw % VIDEO_ALIGN_W;
    th -= th % VIDEO_ALIGN_H;
    if (tw < VIDEO_ALIGN_W || th < VIDEO_ALIGN_H) {
        return -1;
    }
    *w = (uint32_t)tw;
    *h = (uint32_t)th;
    return 0;
}

static uint16_t *slot_px(struct video_player *p, int slot)
{
    return (uint16_t *)(p->cfg.shm + video_slot_offset((uint32_t)slot));
}

static int free_slot(const struct video_player *p)
{
    int i;

    for (i = 0; i < VIDEO_SLOTS; i++) {
        if (p->owner[i] == SLOT_FREE) {
            return i;
        }
    }
    return -1;
}

static void drop_pending(struct video_player *p)
{
    int i;

    for (i = 0; i < p->npend; i++) {
        p->owner[p->pend[i].slot] = SLOT_FREE;
    }
    p->npend = 0;
}

static void pop_pending(struct video_player *p, int keep_slot)
{
    if (!keep_slot) {
        p->owner[p->pend[0].slot] = SLOT_FREE;
    }
    memmove(&p->pend[0], &p->pend[1], (size_t)(p->npend - 1) * sizeof(p->pend[0]));
    p->npend--;
}

static const char *state_word(enum pstate s)
{
    switch (s) {
    case PS_PLAYING:
        return "playing";
    case PS_STOPPED:
        return "stopped";
    case PS_ENDED:
        return "ended";
    default:
        return "paused";
    }
}

static int64_t clamp_pos(const struct video_player *p, int64_t ms)
{
    if (ms < 0) {
        return 0;
    }
    if (p->info.duration_ms > 0 && ms > p->info.duration_ms) {
        return p->info.duration_ms;
    }
    return ms;
}

/* The clock, in file milliseconds. See the header. */
static int64_t clock_now(struct video_player *p, int64_t now)
{
    int have = 0;
    int64_t cs = 0;
    int64_t at = 0;
    int waiting_ok = 0;

    if (p->state != PS_PLAYING) {
        return p->pos_ms;
    }
    if (p->sink) {
        pthread_mutex_lock(&p->a.mu);
        have = p->a.have_clock;
        cs = p->a.clock_samples;
        at = p->a.clock_at;
        /* Sound still to come: worth waiting for at the start. */
        waiting_ok = !p->a.drained && !(p->a.eof && p->a.count == 0);
        pthread_mutex_unlock(&p->a.mu);
    }
    if (!p->clock_running) {
        if (!have && p->sink && waiting_ok && now - p->play_at < VIDEO_AUDIO_START_MS) {
            return p->pos_ms;
        }
        p->clock_running = 1;
        p->base_media = p->pos_ms;
        p->base_wall = now;
    }
    if (have && now - at < VIDEO_AUDIO_FRESH_MS) {
        int64_t d = now - at;
        int64_t c = cs * 1000 / VIDEO_AUDIO_RATE + (d < VIDEO_AUDIO_EXTRAPOLATE_MS ? d : VIDEO_AUDIO_EXTRAPOLATE_MS);

        p->base_media = c;
        p->base_wall = now;
        return c;
    }
    return p->base_media + (now - p->base_wall);
}

static void start_clock(struct video_player *p, int64_t now)
{
    p->clock_running = 0;
    p->play_at = now;
    p->stats_at = 0;
    if (p->sink) {
        audio_run(&p->a, 1);
    }
}

static void close_file(struct video_player *p)
{
    audio_run(&p->a, 0);
    audio_flush(&p->a);
    drop_pending(p);
    if (p->open) {
        p->cfg.backend->close(p->ctx);
    }
    p->ctx = NULL;
    p->open = 0;
    p->state = PS_IDLE;
    p->preview = 0;
}

static void fail(struct video_player *p, const char *what, const char *text)
{
    close_file(p);
    emitf(p, "error %s %s", what, text && *text ? text : "failed");
}

static void do_seek(struct video_player *p, int64_t ms, int reply)
{
    char text[VIDEO_BACKEND_TEXT_MAX] = "";

    ms = clamp_pos(p, ms);
    if (p->info.duration_ms > 0 && ms >= p->info.duration_ms) {
        ms = p->info.duration_ms > p->frame_ms ? p->info.duration_ms - p->frame_ms : 0;
    }
    audio_run(&p->a, 0);
    audio_flush(&p->a);
    drop_pending(p);
    if (p->cfg.backend->seek(p->ctx, ms, text, sizeof(text)) != 0) {
        fail(p, "decode", text);
        return;
    }
    p->video_eof = 0;
    p->video_error = 0;
    p->audio_eof = !p->want_audio;
    p->last_pts = -1;
    p->skip_until = ms;
    p->audio_skip = ms * VIDEO_AUDIO_RATE / 1000;
    p->preview = 1;
    p->seek_reply = reply;
    p->pos_ms = ms;
    p->clock_running = 0;
}

static void open_file(struct video_player *p, const char *path)
{
    char reason[VIDEO_BACKEND_WORD_MAX] = VIDEO_OPENFAIL_IO;
    char text[VIDEO_BACKEND_TEXT_MAX] = "";
    int want = p->cfg.audio_backend != NULL && p->cfg.volume_percent > 0;
    const char *audio;
    uint32_t vw = p->view_w ? p->view_w : VIDEO_VIEW_MAX_W;
    uint32_t vh = p->view_h ? p->view_h : VIDEO_VIEW_MAX_H;

    if (p->cfg.backend->open(&p->ctx, path, want, &p->info, reason, text, sizeof(text)) != 0) {
        p->ctx = NULL;
        emitf(p, "openfail %s %s", reason[0] ? reason : VIDEO_OPENFAIL_IO, text);
        return;
    }
    p->open = 1;
    if (p->info.duration_ms < 0 || p->info.duration_ms > VIDEO_DURATION_MAX_MS) {
        p->info.duration_ms = 0;
    }
    if (video_fit(p->info.src_w, p->info.src_h, vw, vh, &p->out_w, &p->out_h) != 0) {
        close_file(p);
        emitf(p, "openfail %s picture size %ux%u", VIDEO_OPENFAIL_UNSUPPORTED, p->info.src_w,
              p->info.src_h);
        return;
    }
    if (p->cfg.backend->output(p->ctx, p->out_w, p->out_h, text, sizeof(text)) != 0) {
        close_file(p);
        emitf(p, "openfail %s %s", VIDEO_OPENFAIL_DEVICE, text);
        return;
    }
    if (!p->info.has_audio) {
        audio = VIDEO_AUDIO_NONE;
    } else if (!p->info.audio_decodable) {
        audio = VIDEO_AUDIO_UNSUPPORTED;
    } else if (!want) {
        audio = VIDEO_AUDIO_MUTED;
    } else {
        audio = VIDEO_AUDIO_ON;
    }
    p->want_audio = strcmp(audio, VIDEO_AUDIO_ON) == 0;
    p->sink = p->want_audio;
    p->audio_eof = !p->want_audio;
    p->video_eof = 0;
    p->video_error = 0;
    p->frame_ms = p->info.fps_x100 ? 100000 / p->info.fps_x100 : 40;
    if (p->frame_ms < 1) {
        p->frame_ms = 1;
    }
    p->last_pts = -1;
    p->skip_until = -1;
    p->audio_skip = -1;
    p->pos_ms = 0;
    p->state = PS_PAUSED;
    p->preview = 1;
    p->seek_reply = 0;
    p->clock_running = 0;
    memset(&p->c, 0, sizeof(p->c));
    emitf(p, "opened %lld %u %u %u %s %s", (long long)p->info.duration_ms, p->info.src_w,
          p->info.src_h, p->info.fps_x100, audio, p->info.codec[0] ? p->info.codec : "-");
}

/* ---- commands --------------------------------------------------------------------- */

static int parse_u32(const char *s, uint32_t max, uint32_t *out, const char **end)
{
    char *e;
    unsigned long long v;

    if (*s < '0' || *s > '9') {
        return -1;
    }
    errno = 0;
    v = strtoull(s, &e, 10);
    if (errno || v > max || (*e != '\0' && *e != ' ')) {
        return -1;
    }
    *out = (uint32_t)v;
    if (end) {
        *end = *e == ' ' ? e + 1 : e;
    }
    return 0;
}

static const char *arg(const char *line, const char *word)
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

static void reconfigure(struct video_player *p)
{
    char text[VIDEO_BACKEND_TEXT_MAX] = "";
    uint32_t w;
    uint32_t h;
    int64_t at;

    if (video_fit(p->info.src_w, p->info.src_h, p->view_w, p->view_h, &w, &h) != 0 ||
        (w == p->out_w && h == p->out_h)) {
        return;
    }
    at = p->state == PS_PLAYING ? clock_now(p, p->now()) : p->pos_ms;
    if (p->state == PS_ENDED) {
        at = p->pos_ms;
    }
    if (p->cfg.backend->output(p->ctx, w, h, text, sizeof(text)) != 0) {
        fail(p, "device", text);
        return;
    }
    p->out_w = w;
    p->out_h = h;
    if (p->state == PS_ENDED) {
        /* Nothing plays; the last picture at the new size. */
        do_seek(p, at, 0);
        p->state = PS_ENDED;
        return;
    }
    do_seek(p, at, 0);
}

int video_player_command(struct video_player *p, const char *line)
{
    const char *a;
    uint32_t v1;
    uint32_t v2;
    int64_t now = p->now();

    if ((a = arg(line, "quit")) != NULL && *a == '\0') {
        return 1;
    }
    if ((a = arg(line, "release")) != NULL) {
        if (parse_u32(a, VIDEO_SLOTS - 1, &v1, NULL) == 0 && p->owner[v1] == SLOT_SESSION) {
            p->owner[v1] = SLOT_FREE;
        }
        return 0;
    }
    if ((a = arg(line, "view")) != NULL) {
        if (parse_u32(a, VIDEO_VIEW_MAX_W, &v1, &a) == 0 &&
            parse_u32(a, VIDEO_VIEW_MAX_H, &v2, NULL) == 0 && v1 && v2) {
            p->view_w = v1;
            p->view_h = v2;
            if (p->open) {
                reconfigure(p);
            }
        }
        return 0;
    }
    if ((a = arg(line, "open")) != NULL) {
        if (*a == '\0' || strlen(a) >= VIDEO_PATH_MAX) {
            emitf(p, "openfail %s bad path", VIDEO_OPENFAIL_MISSING);
            return 0;
        }
        if (p->open) {
            close_file(p);
        }
        open_file(p, a);
        return 0;
    }
    if (!p->open) {
        return 0; /* nothing to play, pause or seek */
    }
    if ((a = arg(line, "play")) != NULL && *a == '\0') {
        if (p->state == PS_ENDED) {
            do_seek(p, 0, 0);
            if (!p->open) {
                return 0;
            }
        }
        if (p->state != PS_PLAYING) {
            p->state = PS_PLAYING;
            if (!p->preview) {
                start_clock(p, now);
            }
        }
        emitf(p, "state playing %lld", (long long)p->pos_ms);
        return 0;
    }
    if ((a = arg(line, "pause")) != NULL && *a == '\0') {
        if (p->state == PS_PLAYING) {
            p->pos_ms = clamp_pos(p, clock_now(p, now));
            p->state = PS_PAUSED;
            audio_run(&p->a, 0);
        }
        emitf(p, "state %s %lld", state_word(p->state), (long long)p->pos_ms);
        return 0;
    }
    if ((a = arg(line, "stop")) != NULL && *a == '\0') {
        p->state = PS_STOPPED;
        do_seek(p, 0, 0);
        if (p->open) {
            emitf(p, "state stopped 0");
        }
        return 0;
    }
    if ((a = arg(line, "seek")) != NULL) {
        uint32_t ms;

        if (parse_u32(a, (uint32_t)(VIDEO_DURATION_MAX_MS < UINT32_MAX ? VIDEO_DURATION_MAX_MS
                                                                        : UINT32_MAX),
                      &ms, NULL) == 0) {
            if (p->state == PS_ENDED) {
                p->state = PS_PAUSED;
            }
            do_seek(p, ms, 1);
        }
        return 0;
    }
    return 0;
}

/* ---- stepping --------------------------------------------------------------------- */

static void announce(struct video_player *p, int64_t now)
{
    struct pend pd = p->pend[0];

    pop_pending(p, 1);
    p->owner[pd.slot] = SLOT_SESSION;
    p->seq++;
    p->c.shown++;
    p->last_show_at = now;
    emitf(p, "frame %d %u %u %u %lld", pd.slot, p->seq, p->out_w, p->out_h, (long long)pd.pts);
}

static void report_audio(struct video_player *p)
{
    int st;

    pthread_mutex_lock(&p->a.mu);
    st = p->a.status;
    p->a.status = 0;
    pthread_mutex_unlock(&p->a.mu);
    if (st == 0) {
        return;
    }
    /* The picture plays on without sound; decoded samples are dropped. */
    p->sink = 0;
    audio_run(&p->a, 0);
    audio_flush(&p->a);
    emitf(p, "audio %s", st == POCKETAUDIO_E_BUSY ? VIDEO_AUDIO_BUSY : VIDEO_AUDIO_ERROR);
}

/* Sound: decoded while the ring has room and is less than AUDIO_AHEAD
 * ahead. Returns backend calls made. */
static int decode_audio(struct video_player *p, int budget)
{
    int calls = 0;

    while (calls < budget && p->open && !p->audio_eof) {
        struct video_item it;
        const int16_t *s;
        size_t n;
        int64_t pos;

        if (p->sink && (audio_count(&p->a) + VIDEO_AUDIO_CHUNK_MAX > VIDEO_AUDIO_RING_SAMPLES ||
                        audio_count(&p->a) >= AUDIO_AHEAD_SAMPLES)) {
            break;
        }
        if (!p->sink && p->state != PS_PLAYING && !p->preview) {
            break; /* only drained to keep pictures coming */
        }
        p->cfg.backend->next_audio(p->ctx, &it);
        calls++;
        if (it.kind == VIDEO_ITEM_EOF) {
            p->audio_eof = 1;
            audio_set_eof(&p->a);
            break;
        }
        if (it.kind == VIDEO_ITEM_AGAIN) {
            break;
        }
        if (it.kind == VIDEO_ITEM_ERROR) {
            /* The sound is lost, not the picture. */
            p->audio_eof = 1;
            if (p->sink) {
                p->sink = 0;
                audio_run(&p->a, 0);
                audio_flush(&p->a);
                emitf(p, "audio %s", VIDEO_AUDIO_ERROR);
            }
            break;
        }
        if (it.kind != VIDEO_ITEM_AUDIO || !p->sink) {
            continue;
        }
        s = it.samples;
        n = it.count > VIDEO_AUDIO_CHUNK_MAX ? VIDEO_AUDIO_CHUNK_MAX : it.count;
        pos = it.pts_ms * VIDEO_AUDIO_RATE / 1000;
        if (p->audio_skip >= 0) {
            if (pos + (int64_t)n <= p->audio_skip) {
                continue;
            }
            if (pos < p->audio_skip) {
                size_t cut = (size_t)(p->audio_skip - pos);

                s += cut;
                n -= cut;
                pos = p->audio_skip;
            }
            p->audio_skip = -1;
        }
        audio_push(&p->a, s, n, pos);
    }
    return calls;
}

/* Pictures: decoded into free slots while playing, or for a preview. Returns
 * backend calls made. */
static int decode_pictures(struct video_player *p, int64_t now, int budget)
{
    int calls = 0;

    while (calls < budget && p->open && !p->video_eof && (p->state == PS_PLAYING || p->preview)) {
        struct video_item it;
        int slot = free_slot(p);
        int64_t pts;

        if (slot < 0 || (p->preview && p->npend > 0)) {
            break;
        }
        p->cfg.backend->next_picture(p->ctx, &it);
        calls++;
        if (it.kind == VIDEO_ITEM_EOF) {
            p->video_eof = 1;
            break;
        }
        if (it.kind == VIDEO_ITEM_AGAIN) {
            /* The sound has to be read first. */
            calls += decode_audio(p, 1);
            break;
        }
        if (it.kind == VIDEO_ITEM_ERROR) {
            /* What was decoded before it is still shown; step() reports it
             * once those pictures are on screen. */
            p->video_error = 1;
            p->video_eof = 1;
            snprintf(p->error_text, sizeof(p->error_text), "%s", it.text);
            break;
        }
        if (it.kind != VIDEO_ITEM_PICTURE) {
            continue;
        }
        p->c.decoded++;
        pts = it.pts_ms;
        if (p->last_pts >= 0 && pts <= p->last_pts) {
            pts = p->last_pts + p->frame_ms; /* a decoder that repeats a time */
        }
        p->last_pts = pts;
        if (p->skip_until >= 0) {
            if (pts + p->frame_ms / 2 < p->skip_until) {
                continue; /* before the seek target: never converted */
            }
            p->skip_until = -1;
        }
        if (!p->preview && p->clock_running) {
            int64_t c = clock_now(p, now);

            if (pts < c - VIDEO_LATE_DROP_MS && now - p->last_show_at < VIDEO_SHOW_ANYWAY_MS) {
                p->c.dropped++;
                continue;
            }
        }
        if (p->cfg.backend->picture(p->ctx, slot_px(p, slot), p->out_w, p->out_h) != 0) {
            fail(p, "decode", "a picture could not be converted");
            break;
        }
        p->owner[slot] = SLOT_PENDING;
        p->pend[p->npend].slot = slot;
        p->pend[p->npend].pts = pts;
        p->npend++;
    }
    return calls;
}

static void present(struct video_player *p, int64_t now)
{
    int64_t c;

    if (p->npend == 0) {
        return;
    }
    if (p->preview) {
        int64_t pts = p->pend[0].pts;

        announce(p, now);
        p->preview = 0;
        p->pos_ms = clamp_pos(p, pts);
        if (p->seek_reply) {
            p->seek_reply = 0;
            emitf(p, "seeked %lld", (long long)p->pos_ms);
        }
        if (p->state == PS_PLAYING) {
            start_clock(p, now);
        }
        return;
    }
    if (p->state != PS_PLAYING) {
        return;
    }
    c = clock_now(p, now);
    if (!p->clock_running) {
        return; /* waiting for the sound to start */
    }
    while (p->npend >= 2 && p->pend[1].pts <= c) {
        pop_pending(p, 0);
        p->c.dropped++;
    }
    if (p->pend[0].pts <= c) {
        if (c - p->pend[0].pts > p->frame_ms) {
            p->c.late++;
        }
        announce(p, now);
    }
}

static long long cpu_ticks(void)
{
    char buf[512];
    FILE *f = fopen("/proc/self/stat", "r");
    char *p;
    unsigned long ut = 0;
    unsigned long st = 0;
    size_t n;

    if (!f) {
        return -1;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    p = strrchr(buf, ')');
    if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) {
        return -1;
    }
    return (long long)(ut + st);
}

static long rss_kb(void)
{
    long pages = 0;
    long rss = 0;
    FILE *f = fopen("/proc/self/statm", "r");

    if (!f) {
        return 0;
    }
    if (fscanf(f, "%ld %ld", &pages, &rss) != 2) {
        rss = 0;
    }
    fclose(f);
    return rss * (sysconf(_SC_PAGESIZE) / 1024);
}

static void progress(struct video_player *p, int64_t now)
{
    if (p->state != PS_PLAYING || !p->clock_running) {
        return;
    }
    if (now - p->pos_emit_at >= VIDEO_POS_EVERY_MS) {
        p->pos_emit_at = now;
        emitf(p, "pos %lld", (long long)clamp_pos(p, clock_now(p, now)));
    }
    if (p->stats_at == 0) {
        p->stats_at = now;
        p->stats_shown = p->c.shown;
        p->cpu_ticks = cpu_ticks();
    } else if (now - p->stats_at >= VIDEO_STATS_EVERY_MS) {
        long long t = cpu_ticks();
        long hz = sysconf(_SC_CLK_TCK);
        int64_t dt = now - p->stats_at;
        long long cpu = t >= 0 && p->cpu_ticks >= 0 && hz > 0 && dt > 0
                            ? (t - p->cpu_ticks) * 100000LL / ((long long)hz * dt)
                            : 0;
        unsigned xr;

        pthread_mutex_lock(&p->a.mu);
        xr = p->a.xruns;
        pthread_mutex_unlock(&p->a.mu);
        p->c.xruns = xr;
        emitf(p, "stats %llu %llu %llu %llu %lld %ld %u",
              (unsigned long long)((p->c.shown - p->stats_shown) * 10000 / (uint64_t)dt),
              (unsigned long long)p->c.shown, (unsigned long long)p->c.dropped,
              (unsigned long long)p->c.late, cpu, rss_kb(), xr);
        p->stats_at = now;
        p->stats_shown = p->c.shown;
        p->cpu_ticks = t;
    }
}

static void check_end(struct video_player *p, int64_t now)
{
    int sound_done = 1;

    if (p->state != PS_PLAYING || !p->video_eof || p->npend > 0 || p->preview || p->video_error) {
        return;
    }
    if (p->sink && !p->audio_eof) {
        sound_done = 0;
    } else if (p->sink) {
        pthread_mutex_lock(&p->a.mu);
        sound_done = p->a.drained || !p->a.want_run || p->a.fail_gen == p->a.run_gen;
        pthread_mutex_unlock(&p->a.mu);
    }
    if (!sound_done) {
        return;
    }
    p->pos_ms = p->info.duration_ms > 0 ? p->info.duration_ms
                                         : (p->last_pts >= 0 ? p->last_pts + p->frame_ms
                                                             : clock_now(p, now));
    p->pos_ms = clamp_pos(p, p->pos_ms);
    p->state = PS_ENDED;
    audio_run(&p->a, 0);
    emitf(p, "state ended %lld", (long long)p->pos_ms);
}

int video_player_step(struct video_player *p)
{
    int64_t now = p->now();
    int calls;
    int wait = VIDEO_STEP_MAX_WAIT_MS;

    report_audio(p);
    if (!p->open) {
        return wait;
    }
    calls = decode_audio(p, VIDEO_STEP_DECODES / 2);
    if (p->open) {
        calls += decode_pictures(p, now, VIDEO_STEP_DECODES);
    }
    if (!p->open) {
        return wait;
    }
    if (p->video_error && (p->npend == 0 || p->state != PS_PLAYING)) {
        fail(p, "decode", p->error_text);
        return wait;
    }
    if (p->preview && p->video_eof && p->npend == 0) {
        /* The end came before a picture at or after the target. */
        if (p->c.decoded == 0) {
            fail(p, "decode", "no picture in this file could be decoded");
            return wait;
        }
        p->preview = 0;
        if (p->seek_reply) {
            p->seek_reply = 0;
            emitf(p, "seeked %lld", (long long)p->pos_ms);
        }
        if (p->state == PS_PLAYING) {
            start_clock(p, p->now());
        }
    }
    now = p->now();
    present(p, now);
    check_end(p, now);
    progress(p, now);
    if (calls >= VIDEO_STEP_DECODES / 2 || (p->preview && !p->video_eof)) {
        return 0;
    }
    if (p->state == PS_PLAYING && p->npend > 0 && p->clock_running) {
        int64_t d = p->pend[0].pts - clock_now(p, now);

        if (d < wait) {
            wait = d < 1 ? 0 : (int)d;
        }
    }
    if (p->state == PS_PLAYING && !p->clock_running) {
        wait = 5;
    }
    if (p->state == PS_PLAYING && free_slot(p) >= 0 && !p->video_eof && wait > 5) {
        wait = 5;
    }
    return wait;
}

void video_player_counters(const struct video_player *p, struct video_player_counters *out)
{
    *out = p->c;
}

/* ---- lifetime ------------------------------------------------------------------- */

int video_player_create(struct video_player **out, const struct video_player_config *cfg)
{
    struct video_player *p;

    *out = NULL;
    if (!cfg || !cfg->backend || !cfg->shm || !cfg->emit) {
        return -1;
    }
    p = calloc(1, sizeof(*p));
    if (!p) {
        return -1;
    }
    p->cfg = *cfg;
    p->now = cfg->now_ms ? cfg->now_ms : mono_ms;
    p->a.now = p->now;
    p->a.ring = calloc(VIDEO_AUDIO_RING_SAMPLES, sizeof(int16_t));
    if (!p->a.ring) {
        free(p);
        return -1;
    }
    pthread_mutex_init(&p->a.mu, NULL);
    pthread_cond_init(&p->a.cv, NULL);
    p->a.fail_gen = (unsigned)-1;
    if (cfg->audio_backend) {
        p->a.opts.backend = cfg->audio_backend;
        p->a.opts.board = cfg->audio_board;
        p->a.opts.lock_dir = cfg->audio_lock_dir;
        p->a.opts.allow_unverified = cfg->allow_unverified;
        p->a.opts.volume_percent = cfg->volume_percent;
        if (pthread_create(&p->a.thread, NULL, audio_main, &p->a) != 0) {
            pthread_cond_destroy(&p->a.cv);
            pthread_mutex_destroy(&p->a.mu);
            free(p->a.ring);
            free(p);
            return -1;
        }
        p->a.started = 1;
    }
    p->state = PS_IDLE;
    *out = p;
    return 0;
}

void video_player_destroy(struct video_player *p)
{
    if (!p) {
        return;
    }
    close_file(p);
    if (p->a.started) {
        pthread_mutex_lock(&p->a.mu);
        p->a.quit = 1;
        pthread_cond_broadcast(&p->a.cv);
        pthread_mutex_unlock(&p->a.mu);
        pthread_join(p->a.thread, NULL);
    }
    pthread_cond_destroy(&p->a.cv);
    pthread_mutex_destroy(&p->a.mu);
    free(p->a.ring);
    free(p);
}
