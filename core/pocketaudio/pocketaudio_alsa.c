/*
 * pocketaudio's real backend: alsa-lib for the PCM and the mixer switch, the
 * GPIO character device (uAPI v2, no library) for the amplifier enable line.
 *
 * Every PCM is opened SND_PCM_NONBLOCK and every wait is snd_pcm_wait() with
 * the caller's bound, so nothing here can hold a caller longer than it asked
 * for - including a drain, which in non-blocking mode only starts draining
 * and is then watched against the clock.
 *
 * The GPIO line is requested with its output value in the request itself, so
 * there is no instant at which it is an output at an unknown level. It is
 * requested only for playback on a board that names an amplifier, and never
 * read: a read request would make the line an input.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketaudio.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define CONSUMER "pocketaudio-amp"

/* alsa-lib prints its own diagnostics to stderr; every one of them is also
 * a return code here, which is where pocketaudio reports it. */
static void quiet_alsa(const char *file, int line, const char *function, int err,
                       const char *fmt, ...)
{
    (void)file;
    (void)line;
    (void)function;
    (void)err;
    (void)fmt;
}

static void install_quiet(void)
{
    static int done;

    if (!done) {
        snd_lib_error_set_handler(quiet_alsa);
        done = 1;
    }
}

static void msgf(char *msg, size_t len, const char *what, int e)
{
    if (msg && len) {
        snprintf(msg, len, "%s: %s", what, snd_strerror(e));
    }
}

static void *a_pcm_open(const char *name, int capture, unsigned rate, unsigned channels,
                        unsigned period_frames, unsigned buffer_frames, int *err, char *msg,
                        size_t msglen)
{
    snd_pcm_t *h = NULL;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_uframes_t period = period_frames;
    snd_pcm_uframes_t buffer = buffer_frames;
    unsigned got_rate = rate;
    int dir = 0;
    int e;

    install_quiet();
    e = snd_pcm_open(&h, name, capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK,
                     SND_PCM_NONBLOCK);
    if (e < 0) {
        msgf(msg, msglen, "open", e);
        *err = e;
        return NULL;
    }
    snd_pcm_hw_params_alloca(&hw);
    if ((e = snd_pcm_hw_params_any(h, hw)) < 0 ||
        (e = snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (e = snd_pcm_hw_params_set_format(h, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (e = snd_pcm_hw_params_set_channels(h, hw, channels)) < 0 ||
        (e = snd_pcm_hw_params_set_rate_near(h, hw, &got_rate, &dir)) < 0) {
        msgf(msg, msglen, "format", e);
        goto fail;
    }
    if (got_rate != rate) {
        /* Resampling is not wanted anywhere in this path: the modem's tones
         * are defined against the exact rate. */
        if (msg && msglen) {
            snprintf(msg, msglen, "rate %u refused (device offers %u)", rate, got_rate);
        }
        e = -EINVAL;
        goto fail;
    }
    if ((e = snd_pcm_hw_params_set_period_size_near(h, hw, &period, &dir)) < 0 ||
        (e = snd_pcm_hw_params_set_buffer_size_near(h, hw, &buffer)) < 0 ||
        (e = snd_pcm_hw_params(h, hw)) < 0) {
        msgf(msg, msglen, "buffer", e);
        goto fail;
    }
    snd_pcm_sw_params_alloca(&sw);
    if ((e = snd_pcm_sw_params_current(h, sw)) < 0 ||
        (e = snd_pcm_sw_params_set_avail_min(h, sw, period)) < 0 ||
        (e = snd_pcm_sw_params_set_start_threshold(h, sw, capture ? 1 : period)) < 0 ||
        (e = snd_pcm_sw_params(h, sw)) < 0) {
        msgf(msg, msglen, "sw params", e);
        goto fail;
    }
    if ((e = snd_pcm_prepare(h)) < 0) {
        msgf(msg, msglen, "prepare", e);
        goto fail;
    }
    if (capture && (e = snd_pcm_start(h)) < 0) {
        msgf(msg, msglen, "start", e);
        goto fail;
    }
    return h;

fail:
    snd_pcm_close(h);
    *err = e;
    return NULL;
}

/* After an xrun or a suspend, bring the stream back. */
static int recover(snd_pcm_t *h, int capture, long e)
{
    int r = snd_pcm_recover(h, (int)e, 1);

    if (r == 0 && capture) {
        r = snd_pcm_start(h);
    }
    return r;
}

static snd_pcm_sframes_t io_once(snd_pcm_t *h, int capture, int16_t *rbuf, const int16_t *wbuf,
                                 size_t frames)
{
    return capture ? snd_pcm_readi(h, rbuf, frames) : snd_pcm_writei(h, wbuf, frames);
}

/* One attempt, at most one wait of timeout_ms, then one more attempt. */
static long transfer(void *pcm, int capture, int16_t *rbuf, const int16_t *wbuf, size_t frames,
                     int timeout_ms)
{
    snd_pcm_t *h = pcm;
    snd_pcm_sframes_t r = io_once(h, capture, rbuf, wbuf, frames);

    if (r == -EAGAIN) {
        int w = snd_pcm_wait(h, timeout_ms);

        if (w == 0) {
            return 0; /* the device stayed busy for the whole bound */
        }
        r = w > 0 ? io_once(h, capture, rbuf, wbuf, frames) : w;
        if (r == -EAGAIN) {
            return 0;
        }
    }
    if (r == -EPIPE || r == -ESTRPIPE) {
        int rr = recover(h, capture, r);

        return rr < 0 ? rr : -EPIPE;
    }
    return (long)r;
}

static long a_pcm_write(void *pcm, const int16_t *interleaved, size_t frames, int timeout_ms)
{
    return transfer(pcm, 0, NULL, interleaved, frames, timeout_ms);
}

static long a_pcm_read(void *pcm, int16_t *interleaved, size_t frames, int timeout_ms)
{
    return transfer(pcm, 1, interleaved, NULL, frames, timeout_ms);
}

static long elapsed_ms(const struct timespec *t0)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1000L + (t.tv_nsec - t0->tv_nsec) / 1000000L;
}

static int a_pcm_drain(void *pcm, int timeout_ms)
{
    snd_pcm_t *h = pcm;
    struct timespec t0;
    int e;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    e = snd_pcm_drain(h);
    if (e < 0 && e != -EAGAIN) {
        snd_pcm_drop(h);
        return e;
    }
    while (snd_pcm_state(h) == SND_PCM_STATE_DRAINING) {
        if (elapsed_ms(&t0) >= timeout_ms) {
            snd_pcm_drop(h);
            return -ETIMEDOUT;
        }
        usleep(5000);
    }
    return 0;
}

static void a_pcm_close(void *pcm)
{
    snd_pcm_t *h = pcm;

    snd_pcm_drop(h);
    snd_pcm_close(h);
}

static int ctl_elem(const char *ctl, const char *name, int write, int *value)
{
    snd_ctl_t *c = NULL;
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_value_t *v;
    int e;

    install_quiet();
    e = snd_ctl_open(&c, ctl, 0);
    if (e < 0) {
        return e;
    }
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_alloca(&v);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, name);
    snd_ctl_elem_value_set_id(v, id);
    if (write) {
        snd_ctl_elem_value_set_boolean(v, 0, *value ? 1 : 0);
        e = snd_ctl_elem_write(c, v);
    } else {
        e = snd_ctl_elem_read(c, v);
        if (e >= 0) {
            *value = snd_ctl_elem_value_get_boolean(v, 0);
        }
    }
    snd_ctl_close(c);
    return e < 0 ? e : 0;
}

static int a_ctl_get_bool(const char *ctl, const char *name, int *value)
{
    return ctl_elem(ctl, name, 0, value);
}

static int a_ctl_set_bool(const char *ctl, const char *name, int value)
{
    return ctl_elem(ctl, name, 1, &value);
}

static int a_gpio_request_output(const char *chip, unsigned line, int value)
{
    struct gpio_v2_line_request req;
    int fd = open(chip, O_RDWR | O_CLOEXEC);
    int e;

    if (fd < 0) {
        return -errno;
    }
    memset(&req, 0, sizeof(req));
    req.offsets[0] = line;
    req.num_lines = 1;
    snprintf(req.consumer, sizeof(req.consumer), "%s", CONSUMER);
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = value ? 1 : 0;
    req.config.attrs[0].mask = 1;
    e = ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req);
    e = e < 0 ? -errno : 0;
    close(fd);
    if (e < 0) {
        return e;
    }
    return req.fd;
}

static int a_gpio_set(int handle, int value)
{
    struct gpio_v2_line_values v;

    memset(&v, 0, sizeof(v));
    v.bits = value ? 1 : 0;
    v.mask = 1;
    return ioctl(handle, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0 ? -errno : 0;
}

static void a_gpio_release(int handle)
{
    if (handle >= 0) {
        close(handle);
    }
}

static const struct pocketaudio_backend alsa_backend = {
    .pcm_open = a_pcm_open,
    .pcm_write = a_pcm_write,
    .pcm_read = a_pcm_read,
    .pcm_drain = a_pcm_drain,
    .pcm_close = a_pcm_close,
    .ctl_get_bool = a_ctl_get_bool,
    .ctl_set_bool = a_ctl_set_bool,
    .gpio_request_output = a_gpio_request_output,
    .gpio_set = a_gpio_set,
    .gpio_release = a_gpio_release,
};

const struct pocketaudio_backend *pocketaudio_alsa_backend(void)
{
    return &alsa_backend;
}
