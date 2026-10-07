/*
 * pos-mp3's decoder on the image: FFmpeg 4.4 (libavformat, libavcodec,
 * libswresample), which the image already carries (Buildroot selects it for
 * OpenCV; docs/apps/MP3.md, "Backend"). See mp3_decoder.h.
 *
 * Only local files: the input is opened as "file:<path>" with a protocol
 * whitelist of "file", so no path can make FFmpeg reach a network or a
 * device. FFmpeg's own log is silenced; the helper reports errors itself.
 *
 * One damaged frame is skipped, the way players do; MP3_FF_BAD_MAX damaged
 * packets in a row end the file with MP3_DEC_E_DECODE. A read error from
 * the storage ends it with MP3_DEC_E_IO.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mp3_decoder.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libswresample/swresample.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Damaged packets in a row before the file is given up. */
#define MP3_FF_BAD_MAX 32
/* The most output samples one decoded frame may give (FLAC's largest block
 * at 8 kHz resampled to 48 kHz is under this). */
#define MP3_FF_BUF_MAX (1u << 19)

struct mp3_decoder {
    AVFormatContext *fmt;
    AVCodecContext *cc;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frame;
    int stream;
    AVRational tb;
    /* What swr was set up for; a frame that differs sets it up again. */
    int swr_rate;
    int swr_format;
    uint64_t swr_layout;
    int16_t *buf;
    size_t cap;
    size_t len;
    size_t off;
    int input_done;   /* the demuxer is at its end and the decoder was told */
    int ended;        /* nothing more will come */
    int bad;          /* damaged packets in a row */
    int64_t skip_ms;  /* after a seek: output before this is dropped; -1 none */
};

static void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s", what);
    }
}

static int is_io(int averr)
{
    return averr == AVERROR(EIO) || averr == AVERROR(ENXIO) || averr == AVERROR(ENODEV) ||
           averr == AVERROR(ESTALE) || averr == AVERROR(EACCES) || averr == AVERROR(EPERM);
}

static uint64_t layout_of(uint64_t layout, int channels)
{
    return layout ? layout : (uint64_t)av_get_default_channel_layout(channels);
}

static int setup_swr(struct mp3_decoder *d, uint64_t layout, int format, int rate)
{
    swr_free(&d->swr);
    d->swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_MONO, AV_SAMPLE_FMT_S16, MP3_DEC_RATE, (int64_t)layout,
                                (enum AVSampleFormat)format, rate, 0, NULL);
    if (!d->swr || swr_init(d->swr) < 0) {
        swr_free(&d->swr);
        return -1;
    }
    d->swr_layout = layout;
    d->swr_format = format;
    d->swr_rate = rate;
    return 0;
}

static void lower_word(char *dst, size_t n, const char *src)
{
    size_t w = 0;

    for (; src && *src && w + 1 < n; src++) {
        char c = (char)tolower((unsigned char)*src);

        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            dst[w++] = c;
        }
    }
    if (w == 0 && n > 1) {
        dst[w++] = '?';
    }
    dst[w] = '\0';
}

static void tag(const struct mp3_decoder *d, const char *key, char *dst, size_t n)
{
    const AVDictionaryEntry *e = av_dict_get(d->fmt->metadata, key, NULL, 0);

    if (!e && d->stream >= 0) {
        e = av_dict_get(d->fmt->streams[d->stream]->metadata, key, NULL, 0);
    }
    mp3_dec_clean_text(dst, n, e ? e->value : "");
}

void mp3_decoder_close(struct mp3_decoder *d)
{
    if (!d) {
        return;
    }
    swr_free(&d->swr);
    av_frame_free(&d->frame);
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->cc);
    avformat_close_input(&d->fmt);
    free(d->buf);
    free(d);
}

int mp3_decoder_open(struct mp3_decoder **out, const char *path, struct mp3_dec_info *info,
                     char *err, size_t errlen)
{
    struct mp3_decoder *d;
    AVDictionary *opts = NULL;
    AVCodec *codec = NULL;
    AVStream *st;
    struct stat sb;
    char url[1100];
    int r;

    *out = NULL;
    memset(info, 0, sizeof(*info));
    av_log_set_level(AV_LOG_QUIET);
    if (stat(path, &sb) != 0) {
        int missing = errno == ENOENT || errno == ENOTDIR;

        say(err, errlen, missing ? "the file is not there" : strerror(errno));
        return missing ? MP3_DEC_E_MISSING : MP3_DEC_E_IO;
    }
    if (!S_ISREG(sb.st_mode)) {
        say(err, errlen, "not a file");
        return MP3_DEC_E_FORMAT;
    }
    if (sb.st_size == 0) {
        say(err, errlen, "the file is empty");
        return MP3_DEC_E_FORMAT;
    }
    if (snprintf(url, sizeof(url), "file:%s", path) >= (int)sizeof(url)) {
        say(err, errlen, "the path is too long");
        return MP3_DEC_E_MISSING;
    }
    d = calloc(1, sizeof(*d));
    if (!d) {
        say(err, errlen, "out of memory");
        return MP3_DEC_E_NOMEM;
    }
    d->stream = -1;
    d->skip_ms = -1;
    av_dict_set(&opts, "protocol_whitelist", "file", 0);
    r = avformat_open_input(&d->fmt, url, NULL, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        say(err, errlen, is_io(r) ? "the file could not be read" : "not an audio file");
        free(d);
        return is_io(r) ? MP3_DEC_E_IO : MP3_DEC_E_FORMAT;
    }
    r = avformat_find_stream_info(d->fmt, NULL);
    if (r < 0) {
        say(err, errlen, is_io(r) ? "the file could not be read" : "the file is damaged");
        mp3_decoder_close(d);
        return is_io(r) ? MP3_DEC_E_IO : MP3_DEC_E_FORMAT;
    }
    d->stream = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (d->stream < 0 || !codec) {
        say(err, errlen, d->stream == AVERROR_DECODER_NOT_FOUND ? "no decoder for this audio"
                                                                 : "the file holds no audio");
        d->stream = -1;
        mp3_decoder_close(d);
        return MP3_DEC_E_FORMAT;
    }
    st = d->fmt->streams[d->stream];
    d->tb = st->time_base;
    d->cc = avcodec_alloc_context3(codec);
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->cc || !d->pkt || !d->frame) {
        say(err, errlen, "out of memory");
        mp3_decoder_close(d);
        return MP3_DEC_E_NOMEM;
    }
    if (avcodec_parameters_to_context(d->cc, st->codecpar) < 0 ||
        (d->cc->pkt_timebase = st->time_base, avcodec_open2(d->cc, codec, NULL)) < 0) {
        say(err, errlen, "the decoder could not start");
        mp3_decoder_close(d);
        return MP3_DEC_E_FORMAT;
    }
    if (d->cc->sample_rate <= 0 || d->cc->channels <= 0 ||
        setup_swr(d, layout_of(d->cc->channel_layout, d->cc->channels), d->cc->sample_fmt,
                  d->cc->sample_rate) != 0) {
        say(err, errlen, "this audio format cannot be converted");
        mp3_decoder_close(d);
        return MP3_DEC_E_FORMAT;
    }
    /* Only the chosen stream is demuxed further: cover art and the like are
     * skipped inside FFmpeg rather than read and dropped here. */
    {
        unsigned i;

        for (i = 0; i < d->fmt->nb_streams; i++) {
            if ((int)i != d->stream) {
                d->fmt->streams[i]->discard = AVDISCARD_ALL;
            }
        }
    }
    if (d->fmt->duration != AV_NOPTS_VALUE && d->fmt->duration > 0) {
        info->total_ms = d->fmt->duration / (AV_TIME_BASE / 1000);
    } else if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
        info->total_ms = av_rescale_q(st->duration, st->time_base, (AVRational){ 1, 1000 });
    }
    info->seekable = info->total_ms > 0 && d->fmt->pb && (d->fmt->pb->seekable & AVIO_SEEKABLE_NORMAL);
    info->rate = (unsigned)d->cc->sample_rate;
    info->channels = (unsigned)d->cc->channels;
    lower_word(info->codec, sizeof(info->codec), codec->name);
    tag(d, "title", info->title, sizeof(info->title));
    tag(d, "artist", info->artist, sizeof(info->artist));
    if (!info->artist[0]) {
        tag(d, "album_artist", info->artist, sizeof(info->artist));
    }
    *out = d;
    return MP3_DEC_OK;
}

/* Convert the frame in d->frame into d->buf. 0 (maybe with nothing to give
 * after a seek's trim), or a negative enum mp3_dec_result. */
static int convert(struct mp3_decoder *d)
{
    AVFrame *f = d->frame;
    uint64_t layout = layout_of(f->channel_layout, f->channels);
    uint8_t *outp[1];
    int want;
    int n;

    if ((int)f->sample_rate != d->swr_rate || f->format != d->swr_format || layout != d->swr_layout) {
        if (setup_swr(d, layout, f->format, f->sample_rate) != 0) {
            return MP3_DEC_E_DECODE;
        }
    }
    want = swr_get_out_samples(d->swr, f->nb_samples);
    if (want < 0 || (unsigned)want > MP3_FF_BUF_MAX) {
        return MP3_DEC_E_DECODE;
    }
    if ((size_t)want > d->cap) {
        int16_t *nb = realloc(d->buf, (size_t)want * sizeof(int16_t));

        if (!nb) {
            return MP3_DEC_E_NOMEM;
        }
        d->buf = nb;
        d->cap = (size_t)want;
    }
    outp[0] = (uint8_t *)d->buf;
    n = swr_convert(d->swr, outp, (int)d->cap, (const uint8_t **)f->extended_data, f->nb_samples);
    if (n < 0) {
        return MP3_DEC_E_DECODE;
    }
    d->len = (size_t)n;
    d->off = 0;
    if (d->skip_ms >= 0) {
        int64_t pts = f->best_effort_timestamp;

        if (pts == AV_NOPTS_VALUE) {
            d->skip_ms = -1;
        } else {
            int64_t start = av_rescale_q(pts, d->tb, (AVRational){ 1, 1000 });
            int64_t drop = (d->skip_ms - start) * (MP3_DEC_RATE / 1000);

            if (drop >= (int64_t)d->len) {
                d->len = 0; /* all of it before the target */
            } else {
                d->off = drop > 0 ? (size_t)drop : 0;
                d->skip_ms = -1;
            }
        }
    }
    return 0;
}

/* Fill d->buf with the next output. 1 when there is some, 0 at the end, or a
 * negative enum mp3_dec_result. */
static int produce(struct mp3_decoder *d)
{
    for (;;) {
        int r;

        if (d->ended) {
            return 0;
        }
        r = avcodec_receive_frame(d->cc, d->frame);
        if (r == 0) {
            r = convert(d);
            av_frame_unref(d->frame);
            if (r < 0) {
                return r;
            }
            d->bad = 0;
            if (d->off < d->len) {
                return 1;
            }
            continue;
        }
        if (r == AVERROR_EOF) {
            uint8_t *outp[1];
            int n;

            /* What the resampler still holds. */
            d->ended = 1;
            if (d->skip_ms >= 0 || d->cap == 0) {
                return 0;
            }
            outp[0] = (uint8_t *)d->buf;
            n = swr_convert(d->swr, outp, (int)d->cap, NULL, 0);
            d->len = n > 0 ? (size_t)n : 0;
            d->off = 0;
            return d->len > 0 ? 1 : 0;
        }
        if (r != AVERROR(EAGAIN)) {
            if (++d->bad > MP3_FF_BAD_MAX) {
                return MP3_DEC_E_DECODE;
            }
            continue;
        }
        if (d->input_done) {
            /* The decoder wants input after being told there is none. */
            d->ended = 1;
            return 0;
        }
        r = av_read_frame(d->fmt, d->pkt);
        if (r == AVERROR_EOF) {
            d->input_done = 1;
            avcodec_send_packet(d->cc, NULL);
            continue;
        }
        if (r < 0) {
            if (is_io(r)) {
                return MP3_DEC_E_IO;
            }
            if (++d->bad > MP3_FF_BAD_MAX) {
                return MP3_DEC_E_DECODE;
            }
            continue;
        }
        if (d->pkt->stream_index != d->stream) {
            av_packet_unref(d->pkt);
            continue;
        }
        r = avcodec_send_packet(d->cc, d->pkt);
        av_packet_unref(d->pkt);
        if (r < 0 && r != AVERROR(EAGAIN)) {
            if (++d->bad > MP3_FF_BAD_MAX) {
                return MP3_DEC_E_DECODE;
            }
        }
    }
}

long mp3_decoder_read(struct mp3_decoder *d, int16_t *out, size_t max)
{
    size_t n;

    if (max > MP3_DEC_MAX_READ) {
        max = MP3_DEC_MAX_READ;
    }
    if (d->off >= d->len) {
        int r = produce(d);

        if (r <= 0) {
            return r;
        }
    }
    n = d->len - d->off;
    if (n > max) {
        n = max;
    }
    memcpy(out, d->buf + d->off, n * sizeof(int16_t));
    d->off += n;
    return (long)n;
}

int mp3_decoder_seek(struct mp3_decoder *d, int64_t ms)
{
    int64_t total = d->fmt->duration != AV_NOPTS_VALUE ? d->fmt->duration / (AV_TIME_BASE / 1000) : 0;
    int r;

    if (ms < 0) {
        ms = 0;
    }
    if (total > 0 && ms > total) {
        ms = total;
    }
    r = av_seek_frame(d->fmt, -1, ms * (AV_TIME_BASE / 1000), AVSEEK_FLAG_BACKWARD);
    if (r < 0) {
        return is_io(r) ? MP3_DEC_E_IO : MP3_DEC_E_FORMAT;
    }
    avcodec_flush_buffers(d->cc);
    /* Drop what the resampler held from before the seek. */
    if (setup_swr(d, d->swr_layout, d->swr_format, d->swr_rate) != 0) {
        return MP3_DEC_E_DECODE;
    }
    d->len = 0;
    d->off = 0;
    d->input_done = 0;
    d->ended = 0;
    d->bad = 0;
    d->skip_ms = ms;
    return MP3_DEC_OK;
}
