/*
 * pocketaudio: the smallest audio layer PocketOS needs.
 *
 * One mono, signed 16-bit, 48 kHz stream at a time, either played to the
 * built-in speaker or captured from the built-in microphone. Everything that
 * is particular to a board - the ALSA device, how many channels are on the
 * wire, which of them carries the microphone, the mixer switch that routes
 * the SoC's I2S to the codec or to the amplifier, and the amplifier's enable
 * line - is described by a struct pocketaudio_board and handled here, so no
 * caller ever names a mixer control or a GPIO.
 *
 * What it guarantees, and how:
 *
 *   One owner.        An exclusive, non-blocking flock on
 *                     $POCKETOS_RUNTIME_DIR/audio.lock taken before anything
 *                     is touched. A second stream, in either direction, gets
 *                     POCKETAUDIO_E_BUSY. Playback and capture need opposite
 *                     routes on the K230, so the lock is shared by both.
 *   Bounded waits.    Every read or write transfers at most one period and
 *                     waits at most POCKETAUDIO_MAX_WAIT_MS; the PCM is opened
 *                     non-blocking. A caller that checks its own stop flag
 *                     between calls stops within one wait.
 *   Bounded buffers.  One period of scratch inside the stream, allocated at
 *                     open. Nothing grows afterwards.
 *   Safe level.       Every played sample is clamped to the stream's peak
 *                     limit, which can never exceed POCKETAUDIO_PEAK_CEILING.
 *   Cleanup.          pocketaudio_close() turns the amplifier off first, then
 *                     drops and closes the PCM, restores the route it found,
 *                     and releases the lock - in that order, on every path
 *                     including a failed open.
 *   Recovery.         Cleanup cannot run in a process that is SIGKILLed. The
 *                     kernel still closes the PCM (the sound stops) and the
 *                     lock, but the route and the amplifier enable line keep
 *                     their last values. So before open changes either, it
 *                     writes what would undo the change to
 *                     <lock dir>/audio.recovery (write-ahead, replaced
 *                     atomically), and removes the record only once close has
 *                     really undone it. Whoever takes the lock next - the
 *                     next open, or pocketaudio_recover() run by the process
 *                     that saw the owner die - finds the record and restores
 *                     from it first: amplifier off, then the route. The lock
 *                     guarantees the writer is gone. The record lives in the
 *                     runtime directory, which a reboot clears together with
 *                     the hardware state it describes.
 *   Hardware gate.    A board path that has not been validated on hardware
 *                     is refused (POCKETAUDIO_E_DISABLED) unless the caller
 *                     passes allow_unverified. On the K230 both paths are
 *                     unverified until the controlled audio test.
 *
 * No threads, no LVGL, no allocation after open. The ALSA and GPIO calls live
 * behind struct pocketaudio_backend, so the policy above is tested on a host
 * against a fake (tests/pocketaudio_test.c); pocketaudio_alsa.c is the real
 * backend.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_POCKETAUDIO_H
#define POCKETOS_POCKETAUDIO_H

#include <stddef.h>
#include <stdint.h>

#define POCKETAUDIO_RATE 48000
/* 20 ms periods. The kernel build is PREEMPT_NONE with HZ=250 on one hart,
 * so periods shorter than a few scheduler ticks buy nothing but xruns. */
#define POCKETAUDIO_PERIOD_FRAMES 960
/* Playback keeps 80 ms queued, capture 500 ms. A capture that falls behind
 * while the decoder works loses audio, a playback one only delays it; and
 * ggwave's worst analysis step (17.9 ms on a desktop core with every protocol
 * enabled) is unmeasured on the C908 and could be many times that. The kernel
 * preallocates 512 KiB per stream, 2.7 s of stereo S16, so this costs
 * nothing. */
#define POCKETAUDIO_PLAYBACK_BUFFER_FRAMES (4 * POCKETAUDIO_PERIOD_FRAMES)
#define POCKETAUDIO_CAPTURE_BUFFER_FRAMES (25 * POCKETAUDIO_PERIOD_FRAMES)
/* The longest a single read, write or open step may wait on the device. */
#define POCKETAUDIO_MAX_WAIT_MS 200
/* The most channels a board may put on the wire. */
#define POCKETAUDIO_MAX_CHANNELS 2
/* The loudest sample pocketaudio will ever play: -12 dBFS. Raising it is a
 * hardware decision, not a caller's (AUDIO_HARDWARE_MAP §8). */
#define POCKETAUDIO_PEAK_CEILING 8192

enum pocketaudio_dir {
    POCKETAUDIO_PLAYBACK,
    POCKETAUDIO_CAPTURE
};

enum pocketaudio_err {
    POCKETAUDIO_OK = 0,
    POCKETAUDIO_E_INVAL = -1,    /* bad argument */
    POCKETAUDIO_E_BUSY = -2,     /* another stream owns the audio */
    POCKETAUDIO_E_NODEV = -3,    /* the device is not there */
    POCKETAUDIO_E_FORMAT = -4,   /* the device refused 48 kHz S16 on its channels */
    POCKETAUDIO_E_ROUTE = -5,    /* the route or the amplifier could not be set */
    POCKETAUDIO_E_IO = -6,       /* the device failed and did not recover */
    POCKETAUDIO_E_TIMEOUT = -7,  /* a drain did not finish within its bound */
    POCKETAUDIO_E_DISABLED = -8, /* this board's path is not validated on hardware */
    POCKETAUDIO_E_LOCK = -9      /* the lock file could not be created */
};

struct pocketaudio_board {
    const char *name;           /* "k230-t-display", "generic" */
    const char *pcm;            /* ALSA PCM name */
    const char *ctl;            /* ALSA control device for route_control, or NULL */
    unsigned channels;          /* channels on the wire, 1..POCKETAUDIO_MAX_CHANNELS */
    unsigned capture_channel;   /* the wire channel that carries the built-in mic */
    /* A boolean mixer control that selects the route, or NULL when the board
     * has one fixed route. route_playback / route_capture are its values for
     * the speaker and for the microphone. */
    const char *route_control;
    int route_playback;
    int route_capture;
    /* The speaker amplifier's enable line (GPIO character device and line
     * offset), or amp_chip NULL when the board has none to switch. */
    const char *amp_chip;
    unsigned amp_line;
    int amp_active_high;
    /* Whether each path has passed its controlled hardware test. */
    int playback_verified;
    int capture_verified;
};

/* The ALSA and GPIO operations pocketaudio needs. Return conventions: a
 * negative errno on failure. Every call returns within the bound it is given
 * (timeout_ms) or at once. */
struct pocketaudio_backend {
    /* Open, configure (S16_LE, exact rate and channels, period and buffer as
     * given) and prepare a PCM. Returns an opaque handle, or NULL with *err set
     * and a reason in msg. */
    void *(*pcm_open)(const char *name, int capture, unsigned rate, unsigned channels,
                      unsigned period_frames, unsigned buffer_frames, int *err,
                      char *msg, size_t msglen);
    /* Transfer up to frames interleaved frames, waiting at most timeout_ms for
     * the device to become ready. Returns frames transferred (0 on timeout),
     * -EPIPE after an xrun it has already recovered from, or another -errno. */
    long (*pcm_write)(void *pcm, const int16_t *interleaved, size_t frames, int timeout_ms);
    long (*pcm_read)(void *pcm, int16_t *interleaved, size_t frames, int timeout_ms);
    /* Play out what is queued, then stop. 0, or -ETIMEDOUT after dropping it. */
    int (*pcm_drain)(void *pcm, int timeout_ms);
    /* Drop anything queued and close. */
    void (*pcm_close)(void *pcm);
    /* A boolean mixer element by name. */
    int (*ctl_get_bool)(const char *ctl, const char *name, int *value);
    int (*ctl_set_bool)(const char *ctl, const char *name, int value);
    /* Request one GPIO line as an output driven to value. Returns a handle
     * (>= 0) or -errno. Releasing the handle leaves the line as last set. */
    int (*gpio_request_output)(const char *chip, unsigned line, int value);
    int (*gpio_set)(int handle, int value);
    void (*gpio_release)(int handle);
};

struct pocketaudio_options {
    /* Required: pocketaudio_alsa_backend() in use, a fake in tests. Passed
     * in rather than defaulted so this file links without alsa-lib. */
    const struct pocketaudio_backend *backend;
    const struct pocketaudio_board *board;     /* NULL: pocketaudio_board_detect() */
    /* Open a path whose board entry says it is not yet validated. Bench use. */
    int allow_unverified;
    /* Playback only: the largest sample magnitude that will be played, 1 to
     * POCKETAUDIO_PEAK_CEILING. 0 means the ceiling. */
    int peak_limit;
    /* Where the lock lives; NULL means pocketos_runtime_dir(). */
    const char *lock_dir;
};

struct pocketaudio_stream;

/* The board this machine is, from <proc_root>/asound/card0/id ("/proc" in
 * use). The K230's codec card gives the K230 entry, anything else the generic
 * entry (ALSA "default", mono, no route, no amplifier, both paths allowed).
 * POCKETOS_AUDIO_BOARD=generic forces the generic entry and POCKETOS_AUDIO_PCM
 * replaces the PCM name of whichever entry is chosen (tests use "null").
 * The result is copied into *board; its strings are static or from getenv. */
void pocketaudio_board_detect(struct pocketaudio_board *board, const char *proc_root);
const struct pocketaudio_board *pocketaudio_board_k230(void);
const struct pocketaudio_board *pocketaudio_board_generic(void);

/* Open a stream. On failure *out is NULL, nothing is left changed, and err
 * (if not NULL) holds a one-line reason. */
int pocketaudio_open(struct pocketaudio_stream **out, enum pocketaudio_dir dir,
                     const struct pocketaudio_options *options, char *err, size_t errlen);

/* Play up to POCKETAUDIO_PERIOD_FRAMES mono samples (more are left for the
 * next call). Returns samples accepted (0 when the device stayed busy for
 * POCKETAUDIO_MAX_WAIT_MS) or a negative enum pocketaudio_err. */
long pocketaudio_write(struct pocketaudio_stream *s, const int16_t *mono, size_t frames);

/* Capture up to POCKETAUDIO_PERIOD_FRAMES mono samples from the board's
 * microphone channel. Same return convention as pocketaudio_write(). */
long pocketaudio_read(struct pocketaudio_stream *s, int16_t *mono, size_t frames);

/* Playback only: wait for what has been written to be heard, at most
 * timeout_ms (capped at 10 s). */
int pocketaudio_drain(struct pocketaudio_stream *s, int timeout_ms);

/* Amplifier off, PCM closed, route restored, lock released. NULL is fine. */
void pocketaudio_close(struct pocketaudio_stream *s);

/* Undo what an owner that died without closing left behind, without opening a
 * stream: takes the lock (POCKETAUDIO_E_BUSY when a live owner holds it),
 * restores from the recovery record if there is one, and releases the lock.
 * Returns 0 when there was nothing to do, 1 when something was restored, or a
 * negative error; report says which in words. options needs only backend and
 * lock_dir. Idempotent. */
int pocketaudio_recover(const struct pocketaudio_options *options, char *report, size_t len);

/* Whether this stream's open found and undid a previous owner's leftovers. */
int pocketaudio_recovered(const struct pocketaudio_stream *s);

/* xruns recovered from so far, and the last error in words. */
unsigned pocketaudio_xruns(const struct pocketaudio_stream *s);
const char *pocketaudio_last_error(const struct pocketaudio_stream *s);

const char *pocketaudio_strerror(int err);

/* The largest sample magnitude in a buffer (32768 for -32768). */
int pocketaudio_peak(const int16_t *samples, size_t n);

/* The real backend, from pocketaudio_alsa.c. */
const struct pocketaudio_backend *pocketaudio_alsa_backend(void);

#endif
