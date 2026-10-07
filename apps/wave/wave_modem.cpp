/*
 * Wave's modem over ggwave's C++ interface. See wave_modem.h for why each
 * rule below exists.
 *
 * Built with -fno-exceptions -fno-rtti: allocation uses nothrow new, and
 * nothing here can throw across the C boundary.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_modem.h"

#include "ggwave/ggwave.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace {

const ggwave_ProtocolId kProtocol[WAVE_PROFILE_COUNT] = {
    GGWAVE_PROTOCOL_AUDIBLE_NORMAL,
    GGWAVE_PROTOCOL_AUDIBLE_FAST,
    GGWAVE_PROTOCOL_AUDIBLE_FASTEST,
};
const int kFramesPerTx[WAVE_PROFILE_COUNT] = { 9, 6, 3 };
const char *const kNames[WAVE_PROFILE_COUNT] = WAVE_PROFILE_NAMES;

void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        std::snprintf(err, errlen, "%s", what);
    }
}

/* Only the audible protocols, in both directions. Receiving fewer protocols
 * is also what keeps ggwave's analysis spike small (it analyses every enabled
 * protocol that shares the marker's start frequency). */
void select_protocols()
{
    int i;

    GGWave::setLogFile(nullptr);
    GGWave::Protocols::rx().disableAll();
    GGWave::Protocols::tx().disableAll();
    for (i = 0; i < WAVE_PROFILE_COUNT; i++) {
        GGWave::Protocols::rx().toggle(kProtocol[i], true);
        GGWave::Protocols::tx().toggle(kProtocol[i], true);
    }
}

GGWave::Parameters parameters(int mode)
{
    GGWave::Parameters p = GGWave::getDefaultParameters();

    p.payloadLength = -1; /* variable length, with start and end markers */
    p.sampleRateInp = WAVE_MODEM_RATE;
    p.sampleRateOut = WAVE_MODEM_RATE;
    p.sampleRate = WAVE_MODEM_RATE;
    p.samplesPerFrame = WAVE_MODEM_FRAME;
    p.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I16;
    p.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I16;
    p.operatingMode = mode;
    return p;
}

GGWave *build(int mode, char *err, size_t errlen)
{
    GGWave *g;

    select_protocols();
    g = new (std::nothrow) GGWave();
    if (!g) {
        say(err, errlen, "out of memory");
        return nullptr;
    }
    if (!g->prepare(parameters(mode))) {
        delete g;
        say(err, errlen, "ggwave rejected its parameters or could not allocate");
        return nullptr;
    }
    return g;
}

size_t ecc_bytes(size_t len)
{
    /* ggwave.cpp getECCBytesForLength */
    if (len < 4) {
        return 2;
    }
    return 2 * (len / 5) > 4 ? 2 * (len / 5) : 4;
}

} // namespace

struct wave_encoder {
    GGWave *g;
};

struct wave_decoder {
    GGWave *g;
    int16_t frame[WAVE_MODEM_FRAME];
    size_t fill;
    long receiving_frames;
    unsigned watchdog_stops;
    int broken;
};

size_t wave_modem_frames_for(enum wave_profile p, size_t len)
{
    size_t bytes;

    if ((unsigned)p >= WAVE_PROFILE_COUNT) {
        return 0;
    }
    bytes = 3 + len + ecc_bytes(len);
    return 2 * 16 + ((bytes + 2) / 3) * (size_t)kFramesPerTx[p];
}

size_t wave_modem_samples_for(enum wave_profile p, size_t len)
{
    return wave_modem_frames_for(p, len) * WAVE_MODEM_FRAME;
}

int wave_modem_parse_profile(const char *name, enum wave_profile *out)
{
    int i;

    for (i = 0; name && i < WAVE_PROFILE_COUNT; i++) {
        if (std::strcmp(name, kNames[i]) == 0) {
            *out = (enum wave_profile)i;
            return 0;
        }
    }
    return -1;
}

struct wave_encoder *wave_encoder_new(char *err, size_t errlen)
{
    wave_encoder *e = new (std::nothrow) wave_encoder();

    if (!e) {
        say(err, errlen, "out of memory");
        return nullptr;
    }
    e->g = build(GGWAVE_OPERATING_MODE_TX, err, errlen);
    if (!e->g) {
        delete e;
        return nullptr;
    }
    return e;
}

void wave_encoder_free(struct wave_encoder *e)
{
    if (e) {
        delete e->g;
        delete e;
    }
}

long wave_encoder_encode(struct wave_encoder *e, const uint8_t *payload, size_t len,
                         enum wave_profile p, int volume, const int16_t **samples, char *err,
                         size_t errlen)
{
    uint32_t bytes;
    size_t n;

    if (samples) {
        *samples = nullptr;
    }
    if (!e || !samples || (unsigned)p >= WAVE_PROFILE_COUNT) {
        say(err, errlen, "invalid encode request");
        return -1;
    }
    if (len == 0 || !payload) {
        say(err, errlen, "empty message");
        return -1;
    }
    if (len > WAVE_MAX_MESSAGE_BYTES) {
        say(err, errlen, "message longer than the limit");
        return -1;
    }
    if (volume < 1 || volume > WAVE_MODEM_MAX_VOLUME) {
        say(err, errlen, "volume out of range");
        return -1;
    }
    if (!e->g->init((int)len, (const char *)payload, kProtocol[p], volume)) {
        say(err, errlen, "ggwave refused the message");
        return -1;
    }
    bytes = e->g->encode();
    n = bytes / sizeof(int16_t);
    if (bytes == 0 || bytes % sizeof(int16_t) != 0 || n != wave_modem_samples_for(p, len) ||
        n > WAVE_MODEM_MAX_SAMPLES) {
        /* The formula and ggwave disagree: better no sound than a waveform
         * of a size nothing here expected. */
        say(err, errlen, "ggwave produced an unexpected waveform size");
        return -1;
    }
    *samples = (const int16_t *)e->g->txWaveform();
    return (long)n;
}

long wave_encoder_heap_bytes(const struct wave_encoder *e)
{
    return e ? e->g->heapSize() : -1;
}

struct wave_decoder *wave_decoder_new(char *err, size_t errlen)
{
    wave_decoder *d = new (std::nothrow) wave_decoder();

    if (!d) {
        say(err, errlen, "out of memory");
        return nullptr;
    }
    d->g = build(GGWAVE_OPERATING_MODE_RX, err, errlen);
    if (!d->g) {
        delete d;
        return nullptr;
    }
    return d;
}

void wave_decoder_free(struct wave_decoder *d)
{
    if (d) {
        delete d->g;
        delete d;
    }
}

int wave_decoder_receiving(const struct wave_decoder *d)
{
    return d && d->g && d->g->rxReceiving();
}

unsigned wave_decoder_watchdog_stops(const struct wave_decoder *d)
{
    return d ? d->watchdog_stops : 0;
}

long wave_decoder_heap_bytes(const struct wave_decoder *d)
{
    return d && d->g ? d->g->heapSize() : -1;
}

static void one_frame(wave_decoder *d, wave_decode_fn fn, void *user)
{
    GGWave::TxRxData data;
    int r;

    d->g->decode(d->frame, sizeof(d->frame));
    r = d->g->rxTakeData(data);
    if (r > 0) {
        if ((size_t)r <= WAVE_MAX_MESSAGE_BYTES) {
            fn(user, WAVE_DECODED, data.data(), (size_t)r);
        } else {
            /* A longer message from another ggwave program: heard, not
             * accepted. */
            fn(user, WAVE_MISSED, nullptr, 0);
        }
    } else if (r < 0) {
        fn(user, WAVE_MISSED, nullptr, 0);
    }

    if (d->g->rxReceiving()) {
        if (++d->receiving_frames > WAVE_MODEM_WATCHDOG_FRAMES) {
            /* A start marker with no end in sight. ggwave would stay deaf for
             * up to 38.5 s; a fresh receiver is listening again at once. */
            char err[96];

            delete d->g;
            d->g = build(GGWAVE_OPERATING_MODE_RX, err, sizeof(err));
            d->receiving_frames = 0;
            d->watchdog_stops++;
            fn(user, WAVE_MISSED, nullptr, 0);
            if (!d->g) {
                d->broken = 1;
            }
        }
    } else {
        d->receiving_frames = 0;
    }
}

int wave_decoder_feed(struct wave_decoder *d, const int16_t *samples, size_t n, wave_decode_fn fn,
                      void *user)
{
    if (!d || (!samples && n) || !fn) {
        return -1;
    }
    while (n > 0 && !d->broken) {
        size_t take = WAVE_MODEM_FRAME - d->fill;

        if (take > n) {
            take = n;
        }
        std::memcpy(d->frame + d->fill, samples, take * sizeof(int16_t));
        d->fill += take;
        samples += take;
        n -= take;
        if (d->fill == WAVE_MODEM_FRAME) {
            d->fill = 0;
            one_frame(d, fn, user);
        }
    }
    return d->broken ? -1 : 0;
}
