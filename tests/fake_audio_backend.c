/*
 * File-backed pocketaudio backend for process-level tests. See
 * fake_audio_backend.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake_audio_backend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char base[256];
static int pcm_handle;
static FILE *feed;                 /* capture.raw, when the test provides one */
static unsigned long long captured; /* frames delivered since the capture opened */

static void file_path(const char *name, char *out, size_t len)
{
    snprintf(out, len, "%s/%s", base, name);
}

static int put(const char *name, const char *value)
{
    char path[320];
    FILE *f;

    file_path(name, path, sizeof(path));
    f = fopen(path, "w");
    if (!f) {
        return -errno;
    }
    fprintf(f, "%s\n", value);
    return fclose(f) == 0 ? 0 : -EIO;
}

static int get(const char *name, char *value, size_t len)
{
    char path[320];
    FILE *f;

    file_path(name, path, sizeof(path));
    f = fopen(path, "r");
    if (!f) {
        return -ENODEV;
    }
    if (!fgets(value, (int)len, f)) {
        value[0] = '\0';
    }
    fclose(f);
    value[strcspn(value, "\n")] = '\0';
    return 0;
}

static void note(const char *what, int v)
{
    char path[320];
    FILE *f;

    file_path("log", path, sizeof(path));
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s %d\n", what, v);
        fclose(f);
    }
}

static void pace(size_t frames)
{
    long ns = (long)(frames * 1000000000ULL / POCKETAUDIO_RATE);
    struct timespec d = { ns / 1000000000L, ns % 1000000000L };

    nanosleep(&d, NULL);
}

static void *f_open(const char *name, int capture, unsigned rate, unsigned channels, unsigned period,
                    unsigned buffer, int *err, char *msg, size_t msglen)
{
    (void)name;
    (void)rate;
    (void)channels;
    (void)period;
    (void)buffer;
    (void)msg;
    (void)msglen;
    if (put("pcm", capture ? "capture" : "playback") < 0) {
        *err = -ENODEV;
        return NULL;
    }
    note(capture ? "pcm capture" : "pcm playback", 1);
    if (capture) {
        char path[320];

        captured = 0;
        file_path("capture.raw", path, sizeof(path));
        feed = fopen(path, "rb");
    }
    return &pcm_handle;
}

static long f_write(void *pcm, const int16_t *buf, size_t frames, int timeout_ms)
{
    (void)pcm;
    (void)buf;
    (void)timeout_ms;
    pace(frames);
    return (long)frames;
}

static long f_read(void *pcm, int16_t *buf, size_t frames, int timeout_ms)
{
    char v[32];
    size_t i;

    (void)pcm;
    (void)timeout_ms;
    pace(frames);
    if (get("capture_fail_after", v, sizeof(v)) == 0 && captured + frames > strtoull(v, NULL, 10)) {
        note("pcm capture failed", -EIO);
        return -EIO;
    }
    memset(buf, 0, frames * POCKETAUDIO_MAX_CHANNELS * sizeof(int16_t));
    for (i = 0; feed && i < frames; i++) {
        int16_t x;

        /* The feed is the microphone: the right slot, as on the K230. */
        if (fread(&x, sizeof(x), 1, feed) == 1) {
            buf[i * 2 + 1] = x;
        }
    }
    captured += frames;
    return (long)frames;
}

static int f_drain(void *pcm, int timeout_ms)
{
    (void)pcm;
    (void)timeout_ms;
    return 0;
}

static void f_close(void *pcm)
{
    (void)pcm;
    if (feed) {
        fclose(feed);
        feed = NULL;
    }
    put("pcm", "closed");
    note("pcm closed", 0);
}

static int f_get(const char *ctl, const char *name, int *value)
{
    char v[16];
    int e;

    (void)ctl;
    (void)name;
    e = get("route", v, sizeof(v));
    if (e < 0) {
        return e;
    }
    *value = strcmp(v, "1") == 0;
    return 0;
}

static int f_set(const char *ctl, const char *name, int value)
{
    (void)ctl;
    (void)name;
    note("route", value);
    return put("route", value ? "1" : "0");
}

static int f_gpio_request(const char *chip, unsigned line, int value)
{
    (void)chip;
    (void)line;
    note("amp request", value);
    return put("amp", value ? "1" : "0") < 0 ? -EIO : 7;
}

static int f_gpio_set(int h, int value)
{
    (void)h;
    note("amp set", value);
    return put("amp", value ? "1" : "0");
}

static void f_gpio_release(int h)
{
    (void)h;
    note("amp release", 0);
}

static const struct pocketaudio_backend fake = {
    .pcm_open = f_open,
    .pcm_write = f_write,
    .pcm_read = f_read,
    .pcm_drain = f_drain,
    .pcm_close = f_close,
    .ctl_get_bool = f_get,
    .ctl_set_bool = f_set,
    .gpio_request_output = f_gpio_request,
    .gpio_set = f_gpio_set,
    .gpio_release = f_gpio_release,
};

static const struct pocketaudio_board board = {
    .name = "fake-k230",
    .pcm = "fake",
    .ctl = "fake",
    .channels = 2,
    .capture_channel = 1,
    .capture_settle_frames = POCKETAUDIO_RATE / 2,
    .route_control = "External I2S Output Switch",
    .route_playback = 1,
    .route_capture = 0,
    .amp_chip = "/dev/gpiochip1",
    .amp_line = 2,
    .amp_active_high = 1,
    .playback_verified = 0,
    .capture_verified = 0,
};

const struct pocketaudio_backend *fake_audio_backend(const char *dir)
{
    snprintf(base, sizeof(base), "%s", dir);
    return &fake;
}

const struct pocketaudio_board *fake_audio_board(void)
{
    return &board;
}
