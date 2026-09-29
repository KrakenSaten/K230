/*
 * The FFmpeg decoding backend. See video_backend.h.
 *
 * What is used, all of it FFmpeg 4.4 as the image already ships it (the
 * vendor build carries Canaan's patches for the K230's video processing unit):
 *
 *   libavformat   reads the MP4 container (v0.1 opens the MP4 family only)
 *   h264_v4l2m2m  the hardware H.264 decoder, /dev/video_vpu ("mvx", Arm's
 *                 Linlon video processor) behind FFmpeg's V4L2 memory-to-
 *                 memory wrapper; its dsl_width/dsl_height options (a vendor
 *                 patch) make the decoder scale the picture down on the way
 *                 out, so the CPU never sees a full-size picture. Measured on
 *                 unit B 2026-09-29: 1280x720 at about 120 pictures a second
 *                 scaled to 640x360, 0.6 s of CPU for 18 s of video.
 *   libswscale    the decoder's YUV 4:2:0 to RGB565 at the same size (about
 *                 3 ms a 640x360 picture on the C908)
 *   libavcodec    the sound track (AAC in the files tested)
 *   libswresample the sound to mono signed 16-bit 48 kHz
 *
 * Nothing falls back to software H.264 decoding: on this single core it
 * would take half the CPU at 720p (measured) and the picture would stutter,
 * so a file the hardware decoder refuses is reported as such.
 *
 * SEEKING reopens the hardware decoder rather than flushing it: a close and
 * open is the one sequence the driver certainly supports, and the Camera's
 * experience with a streaming stop and restart on the same open node of
 * another vendor driver (docs/apps/CAMERA.md) is not repeated here.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "video_backend.h"

#include "video_proto.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define HW_DECODER "h264_v4l2m2m"
/* Packets of the other stream set aside while looking for one of this. */
#define PKTQ_MAX 1024
#define PKTQ_MAX_BYTES (8u << 20)
/* This many packets in a row the decoder refuses is a damaged file. */
#define BAD_PACKETS_MAX 50

struct pktq {
    AVPacket *pk[PKTQ_MAX];
    int head;
    int count;
    size_t bytes;
};

struct ff {
    AVFormatContext *fmt;
    int vi;
    int ai;
    AVStream *vs;
    AVStream *as;
    AVCodecContext *vdec;
    AVCodecContext *adec;
    struct SwsContext *sws;
    SwrContext *swr;
    int swr_rate;
    int swr_fmt;
    uint64_t swr_layout;
    AVFrame *vframe;
    AVFrame *aframe;
    AVPacket *pkt;
    AVPacket *vpending;   /* refused with EAGAIN: sent again first */
    struct pktq vq;
    struct pktq aq;
    int64_t start_ms;
    int demux_eof;
    int vdrain;
    int adrain;
    int video_done;
    int audio_done;
    int audio_on;
    int held;
    int bad_packets;
    uint32_t out_w;
    uint32_t out_h;
    int16_t abuf[VIDEO_AUDIO_CHUNK_MAX];
};

static void text_err(char *out, size_t n, const char *what, int err)
{
    char e[AV_ERROR_MAX_STRING_SIZE];

    av_strerror(err, e, sizeof(e));
    snprintf(out, n, "%s: %s", what, e);
}

/* ---- packet queues ------------------------------------------------------------------ */

static int q_full(const struct pktq *q)
{
    return q->count == PKTQ_MAX || q->bytes >= PKTQ_MAX_BYTES;
}

static void q_push(struct pktq *q, AVPacket *p)
{
    q->pk[(q->head + q->count) % PKTQ_MAX] = p;
    q->count++;
    q->bytes += (size_t)p->size;
}

static AVPacket *q_pop(struct pktq *q)
{
    AVPacket *p = q->pk[q->head];

    q->head = (q->head + 1) % PKTQ_MAX;
    q->count--;
    q->bytes -= (size_t)p->size;
    return p;
}

static void q_clear(struct pktq *q)
{
    while (q->count) {
        AVPacket *p = q_pop(q);

        av_packet_free(&p);
    }
    q->head = 0;
    q->bytes = 0;
}

/* The next packet of one stream: 0 with *out, 1 at the end of the file, 2
 * when the other stream's queue is at its bound, -1 on a read error. */
static int demux_for(struct ff *f, int video, AVPacket **out)
{
    struct pktq *mine = video ? &f->vq : &f->aq;
    struct pktq *other = video ? &f->aq : &f->vq;

    if (mine->count) {
        *out = q_pop(mine);
        return 0;
    }
    for (;;) {
        int r;
        int idx;
        AVPacket *p;

        if (f->demux_eof) {
            return 1;
        }
        if (q_full(other)) {
            return 2;
        }
        r = av_read_frame(f->fmt, f->pkt);
        if (r == AVERROR(EAGAIN)) {
            continue;
        }
        if (r == AVERROR_EOF || (r < 0 && avio_feof(f->fmt->pb))) {
            f->demux_eof = 1;
            return 1;
        }
        if (r < 0) {
            return -1;
        }
        idx = f->pkt->stream_index;
        if (idx != f->vi && !(f->audio_on && idx == f->ai)) {
            av_packet_unref(f->pkt);
            continue;
        }
        p = av_packet_alloc();
        if (!p) {
            av_packet_unref(f->pkt);
            return -1;
        }
        av_packet_move_ref(p, f->pkt);
        if ((idx == f->vi) == !!video) {
            *out = p;
            return 0;
        }
        q_push(idx == f->vi ? &f->vq : &f->aq, p);
    }
}

/* ---- decoders ------------------------------------------------------------------------ */

static void close_vdec(struct ff *f)
{
    if (f->held) {
        av_frame_unref(f->vframe);
        f->held = 0;
    }
    av_packet_free(&f->vpending);
    avcodec_free_context(&f->vdec);
}

static int open_vdec(struct ff *f, uint32_t w, uint32_t h, char *text, size_t textlen)
{
    const AVCodec *c = avcodec_find_decoder_by_name(HW_DECODER);
    AVDictionary *opts = NULL;
    AVCodecContext *d;
    int r;

    if (!c) {
        snprintf(text, textlen, "this build has no hardware H.264 decoder");
        return -1;
    }
    d = avcodec_alloc_context3(c);
    if (!d) {
        snprintf(text, textlen, "out of memory");
        return -1;
    }
    r = avcodec_parameters_to_context(d, f->vs->codecpar);
    if (r < 0) {
        avcodec_free_context(&d);
        text_err(text, textlen, "decoder parameters", r);
        return -1;
    }
    d->pkt_timebase = f->vs->time_base;
    if (w != (uint32_t)f->vs->codecpar->width || h != (uint32_t)f->vs->codecpar->height) {
        av_dict_set_int(&opts, "dsl_width", w, 0);
        av_dict_set_int(&opts, "dsl_height", h, 0);
    }
    r = avcodec_open2(d, c, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        avcodec_free_context(&d);
        text_err(text, textlen, "hardware decoder", r);
        return -1;
    }
    f->vdec = d;
    f->vdrain = 0;
    f->video_done = 0;
    f->bad_packets = 0;
    return 0;
}

static int open_adec(struct ff *f)
{
    const AVCodec *c = avcodec_find_decoder(f->as->codecpar->codec_id);
    AVCodecContext *d;

    if (!c) {
        return -1;
    }
    d = avcodec_alloc_context3(c);
    if (!d) {
        return -1;
    }
    if (avcodec_parameters_to_context(d, f->as->codecpar) < 0 || avcodec_open2(d, c, NULL) < 0) {
        avcodec_free_context(&d);
        return -1;
    }
    d->pkt_timebase = f->as->time_base;
    f->adec = d;
    return 0;
}

static int64_t to_ms(const struct ff *f, int64_t ts, AVRational tb)
{
    if (ts == AV_NOPTS_VALUE) {
        return -1;
    }
    return av_rescale_q(ts, tb, (AVRational){ 1, 1000 }) - f->start_ms;
}

/* ---- the backend ---------------------------------------------------------------------- */

static void ff_close(void *ctx)
{
    struct ff *f = ctx;

    if (!f) {
        return;
    }
    close_vdec(f);
    avcodec_free_context(&f->adec);
    q_clear(&f->vq);
    q_clear(&f->aq);
    sws_freeContext(f->sws);
    swr_free(&f->swr);
    av_frame_free(&f->vframe);
    av_frame_free(&f->aframe);
    av_packet_free(&f->pkt);
    avformat_close_input(&f->fmt);
    free(f);
}

static int ff_open(void **ctx, const char *path, int want_audio, struct video_media_info *info,
                   char *reason, char *text, size_t textlen)
{
    struct ff *f;
    struct stat st;
    AVRational fr;
    int r;

    *ctx = NULL;
    av_log_set_level(AV_LOG_ERROR);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_MISSING);
        snprintf(text, textlen, "no such file");
        return -1;
    }
    f = calloc(1, sizeof(*f));
    if (!f) {
        snprintf(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_IO);
        snprintf(text, textlen, "out of memory");
        return -1;
    }
    f->vi = f->ai = -1;
    f->vframe = av_frame_alloc();
    f->aframe = av_frame_alloc();
    f->pkt = av_packet_alloc();
    if (!f->vframe || !f->aframe || !f->pkt) {
        ff_close(f);
        snprintf(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_IO);
        snprintf(text, textlen, "out of memory");
        return -1;
    }
    r = avformat_open_input(&f->fmt, path, NULL, NULL);
    if (r < 0) {
        snprintf(reason, VIDEO_BACKEND_WORD_MAX, "%s",
                 r == AVERROR(ENOENT) ? VIDEO_OPENFAIL_MISSING
                 : r == AVERROR(EIO) || r == AVERROR(EACCES) ? VIDEO_OPENFAIL_IO
                                                             : VIDEO_OPENFAIL_CORRUPT);
        text_err(text, textlen, "not a readable video", r);
        ff_close(f);
        return -1;
    }
    if (!strstr(f->fmt->iformat->name, "mp4")) {
        snprintf(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_UNSUPPORTED);
        snprintf(text, textlen, "%s files are not supported", f->fmt->iformat->name);
        ff_close(f);
        return -1;
    }
    f->vi = av_find_best_stream(f->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (f->vi >= 0 && (f->fmt->streams[f->vi]->codecpar->width <= 0 ||
                       f->fmt->streams[f->vi]->codecpar->height <= 0)) {
        /* MP4 names the size in its header; one that does not is read. */
        if (avformat_find_stream_info(f->fmt, NULL) < 0) {
            snprintf(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_CORRUPT);
            snprintf(text, textlen, "damaged video");
            ff_close(f);
            return -1;
        }
    }
    if (f->vi < 0) {
        snprintf(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_UNSUPPORTED);
        snprintf(text, textlen, "no pictures in this file");
        ff_close(f);
        return -1;
    }
    f->vs = f->fmt->streams[f->vi];
    if (f->vs->codecpar->codec_id != AV_CODEC_ID_H264) {
        snprintf(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_UNSUPPORTED);
        snprintf(text, textlen, "%s pictures are not supported",
                 avcodec_get_name(f->vs->codecpar->codec_id));
        ff_close(f);
        return -1;
    }
    if (f->vs->codecpar->width <= 0 || f->vs->codecpar->height <= 0) {
        snprintf(reason, VIDEO_BACKEND_WORD_MAX, "%s", VIDEO_OPENFAIL_CORRUPT);
        snprintf(text, textlen, "no picture size");
        ff_close(f);
        return -1;
    }
    f->ai = av_find_best_stream(f->fmt, AVMEDIA_TYPE_AUDIO, -1, f->vi, NULL, 0);
    if (f->ai >= 0) {
        f->as = f->fmt->streams[f->ai];
    }
    memset(info, 0, sizeof(*info));
    info->has_audio = f->ai >= 0;
    info->audio_decodable = f->ai >= 0 && avcodec_find_decoder(f->as->codecpar->codec_id) != NULL;
    if (want_audio && info->audio_decodable) {
        if (open_adec(f) == 0) {
            f->audio_on = 1;
        } else {
            info->audio_decodable = 0;
        }
    }
    {
        unsigned i;

        for (i = 0; i < f->fmt->nb_streams; i++) {
            if ((int)i != f->vi && !(f->audio_on && (int)i == f->ai)) {
                f->fmt->streams[i]->discard = AVDISCARD_ALL;
            }
        }
    }
    f->start_ms = f->fmt->start_time != AV_NOPTS_VALUE && f->fmt->start_time > 0
                      ? f->fmt->start_time / 1000
                      : 0;
    if (f->fmt->duration != AV_NOPTS_VALUE && f->fmt->duration > 0) {
        info->duration_ms = f->fmt->duration / 1000;
    } else if (f->vs->duration != AV_NOPTS_VALUE && f->vs->duration > 0) {
        info->duration_ms = av_rescale_q(f->vs->duration, f->vs->time_base, (AVRational){ 1, 1000 });
    }
    info->src_w = (uint32_t)f->vs->codecpar->width;
    info->src_h = (uint32_t)f->vs->codecpar->height;
    fr = av_guess_frame_rate(f->fmt, f->vs, NULL);
    if (fr.num > 0 && fr.den > 0) {
        info->fps_x100 = (uint32_t)av_rescale(fr.num, 100, fr.den);
    }
    snprintf(info->codec, sizeof(info->codec), "h264");
    f->audio_done = !f->audio_on;
    *ctx = f;
    return 0;
}

static int ff_output(void *ctx, uint32_t w, uint32_t h, char *text, size_t textlen)
{
    struct ff *f = ctx;

    if (f->vdec && w == f->out_w && h == f->out_h) {
        return 0;
    }
    close_vdec(f);
    if (open_vdec(f, w, h, text, textlen) != 0) {
        return -1;
    }
    f->out_w = w;
    f->out_h = h;
    return 0;
}

static void reset_swr(struct ff *f)
{
    swr_free(&f->swr);
    f->swr_rate = 0;
}

static int ff_seek(void *ctx, int64_t ms, char *text, size_t textlen)
{
    struct ff *f = ctx;
    int64_t ts = (ms + f->start_ms) * 1000; /* AV_TIME_BASE is microseconds */
    int r;

    r = av_seek_frame(f->fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (r < 0) {
        /* No key picture at or before the target: the start of a file whose
         * first picture is stamped a little after 0 (an MP4 edit list; the
         * unit B test clip starts at 1/15360 s, and STOP and PLAY after the
         * end both seek to 0). The next key picture is the right one. */
        r = av_seek_frame(f->fmt, -1, ts, 0);
    }
    if (r < 0) {
        text_err(text, textlen, "seek", r);
        return -1;
    }
    q_clear(&f->vq);
    q_clear(&f->aq);
    f->demux_eof = 0;
    close_vdec(f);
    if (open_vdec(f, f->out_w, f->out_h, text, textlen) != 0) {
        return -1;
    }
    if (f->adec) {
        avcodec_flush_buffers(f->adec);
        f->adrain = 0;
        f->audio_done = 0;
        reset_swr(f);
    }
    return 0;
}

static void ff_next_picture(void *ctx, struct video_item *it)
{
    struct ff *f = ctx;

    memset(it, 0, sizeof(*it));
    if (f->held) {
        av_frame_unref(f->vframe);
        f->held = 0;
    }
    for (;;) {
        AVPacket *p = NULL;
        int r;

        if (f->video_done || !f->vdec) {
            it->kind = VIDEO_ITEM_EOF;
            return;
        }
        r = avcodec_receive_frame(f->vdec, f->vframe);
        if (r == 0) {
            int64_t ts = f->vframe->best_effort_timestamp;

            if (ts == AV_NOPTS_VALUE) {
                ts = f->vframe->pts;
            }
            it->kind = VIDEO_ITEM_PICTURE;
            it->pts_ms = to_ms(f, ts, f->vs->time_base);
            if (it->pts_ms < 0) {
                it->pts_ms = 0;
            }
            f->held = 1;
            return;
        }
        if (r == AVERROR_EOF) {
            f->video_done = 1;
            continue;
        }
        if (r != AVERROR(EAGAIN)) {
            it->kind = VIDEO_ITEM_ERROR;
            text_err(it->text, sizeof(it->text), "picture decoder", r);
            return;
        }
        if (f->vpending) {
            p = f->vpending;
            f->vpending = NULL;
        } else if (!f->vdrain) {
            r = demux_for(f, 1, &p);
            if (r == 2) {
                it->kind = VIDEO_ITEM_AGAIN;
                return;
            }
            if (r < 0) {
                it->kind = VIDEO_ITEM_ERROR;
                snprintf(it->text, sizeof(it->text), "the file could not be read");
                return;
            }
            if (r == 1) {
                /* The end: let the decoder give back what it holds. */
                f->vdrain = 1;
                avcodec_send_packet(f->vdec, NULL);
                continue;
            }
        } else {
            /* Draining, and it said EAGAIN: it has nothing more. */
            f->video_done = 1;
            continue;
        }
        r = avcodec_send_packet(f->vdec, p);
        if (r == AVERROR(EAGAIN)) {
            f->vpending = p;
            continue;
        }
        av_packet_free(&p);
        if (r < 0 && r != AVERROR_EOF) {
            if (++f->bad_packets >= BAD_PACKETS_MAX) {
                it->kind = VIDEO_ITEM_ERROR;
                text_err(it->text, sizeof(it->text), "damaged pictures", r);
                return;
            }
        } else {
            f->bad_packets = 0;
        }
    }
}

static int convert_audio(struct ff *f, struct video_item *it)
{
    AVFrame *fr = f->aframe;
    uint64_t layout = fr->channel_layout ? fr->channel_layout
                                         : (uint64_t)av_get_default_channel_layout(fr->channels);
    uint8_t *out[1] = { (uint8_t *)f->abuf };
    int n;

    if (!f->swr || f->swr_rate != fr->sample_rate || f->swr_fmt != fr->format ||
        f->swr_layout != layout) {
        swr_free(&f->swr);
        f->swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_MONO, AV_SAMPLE_FMT_S16, VIDEO_AUDIO_RATE,
                                    (int64_t)layout, fr->format, fr->sample_rate, 0, NULL);
        if (!f->swr || swr_init(f->swr) < 0) {
            swr_free(&f->swr);
            return -1;
        }
        f->swr_rate = fr->sample_rate;
        f->swr_fmt = fr->format;
        f->swr_layout = layout;
    }
    n = swr_convert(f->swr, out, VIDEO_AUDIO_CHUNK_MAX, (const uint8_t **)fr->extended_data,
                    fr->nb_samples);
    if (n < 0) {
        return -1;
    }
    {
        int64_t ts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;

        it->pts_ms = to_ms(f, ts, f->as->time_base);
        if (it->pts_ms < 0) {
            it->pts_ms = 0;
        }
    }
    it->kind = VIDEO_ITEM_AUDIO;
    it->samples = f->abuf;
    it->count = (size_t)n;
    return 0;
}

static void ff_next_audio(void *ctx, struct video_item *it)
{
    struct ff *f = ctx;

    memset(it, 0, sizeof(*it));
    for (;;) {
        AVPacket *p = NULL;
        int r;

        if (f->audio_done || !f->adec) {
            it->kind = VIDEO_ITEM_EOF;
            return;
        }
        r = avcodec_receive_frame(f->adec, f->aframe);
        if (r == 0) {
            r = convert_audio(f, it);
            av_frame_unref(f->aframe);
            if (r != 0) {
                it->kind = VIDEO_ITEM_ERROR;
                snprintf(it->text, sizeof(it->text), "sound conversion failed");
                f->audio_done = 1;
                return;
            }
            if (it->count > 0) {
                return;
            }
            continue;
        }
        if (r == AVERROR_EOF) {
            f->audio_done = 1;
            continue;
        }
        if (r != AVERROR(EAGAIN)) {
            it->kind = VIDEO_ITEM_ERROR;
            text_err(it->text, sizeof(it->text), "sound decoder", r);
            f->audio_done = 1;
            return;
        }
        if (f->adrain) {
            f->audio_done = 1;
            continue;
        }
        r = demux_for(f, 0, &p);
        if (r == 2) {
            it->kind = VIDEO_ITEM_AGAIN;
            return;
        }
        if (r < 0) {
            it->kind = VIDEO_ITEM_ERROR;
            snprintf(it->text, sizeof(it->text), "the file could not be read");
            f->audio_done = 1;
            return;
        }
        if (r == 1) {
            f->adrain = 1;
            avcodec_send_packet(f->adec, NULL);
            continue;
        }
        /* A damaged sound packet is skipped: the decoder resynchronises. */
        avcodec_send_packet(f->adec, p);
        av_packet_free(&p);
    }
}

static int ff_picture(void *ctx, uint16_t *dst, uint32_t w, uint32_t h)
{
    struct ff *f = ctx;
    AVFrame *fr = f->vframe;
    uint8_t *data[4] = { (uint8_t *)dst, NULL, NULL, NULL };
    int ls[4] = { (int)(w * 2), 0, 0, 0 };
    int full_range;
    int cs;

    if (!f->held || w != f->out_w || h != f->out_h || fr->width <= 0 || fr->height <= 0) {
        return -1;
    }
    /* The decoder's picture is normally exactly w x h (it scaled); if it did
     * not, this scales, which is correct and slower. */
    f->sws = sws_getCachedContext(f->sws, fr->width, fr->height, (enum AVPixelFormat)fr->format,
                                  (int)w, (int)h, AV_PIX_FMT_RGB565LE, SWS_BILINEAR, NULL, NULL,
                                  NULL);
    if (!f->sws) {
        av_frame_unref(fr);
        f->held = 0;
        return -1;
    }
    /* The range and matrix the file declares (the hardware decoder's frames
     * do not carry them reliably); unspecified HD is BT.709. */
    full_range = f->vs->codecpar->color_range == AVCOL_RANGE_JPEG;
    cs = f->vs->codecpar->color_space == AVCOL_SPC_BT709 ||
                 (f->vs->codecpar->color_space == AVCOL_SPC_UNSPECIFIED && f->vs->codecpar->height >= 720)
             ? SWS_CS_ITU709
             : SWS_CS_DEFAULT;
    sws_setColorspaceDetails(f->sws, sws_getCoefficients(cs), full_range, sws_getCoefficients(SWS_CS_DEFAULT),
                             1, 0, 1 << 16, 1 << 16);
    sws_scale(f->sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, data, ls);
    av_frame_unref(fr);
    f->held = 0;
    return 0;
}

static const struct video_backend ffmpeg_backend = {
    .name = "ffmpeg",
    .open = ff_open,
    .output = ff_output,
    .seek = ff_seek,
    .next_picture = ff_next_picture,
    .next_audio = ff_next_audio,
    .picture = ff_picture,
    .close = ff_close,
};

const struct video_backend *video_backend_ffmpeg(void)
{
    return &ffmpeg_backend;
}
