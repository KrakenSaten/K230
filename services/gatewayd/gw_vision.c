/*
 * gatewayd: Vision for a paired client (gw_vision.h).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_vision.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The modes the unit may say it is in. The first GW_VISION_REMOTE of them
 * are the ones a client may choose; the rest are never chosen from a phone
 * and never shared. */
static const char *const MODES[] = {"detect", "track", "traffic", "segment", "instances", "color",
                                    "edge",   "trace", "face",    "recognize", "read",    NULL};
#define GW_VISION_REMOTE 8
static const char *const OWNERS[] = {"vision", "camera", "none", NULL};
static const char *const SHARES[] = {"off", "live", "stopped", NULL};
static const char *const STATS[] = {"fps_x10", "infer_ms", "pre_ms",   "post_ms", "cpu_pct",
                                    "rss_kb",  "dropped",  "count_ab", "count_ba", NULL};

static const char *const V_LINE[] = {"off", "across", "down", NULL};
static const char *const V_RANGE[] = {"near", "normal", "far", NULL};
static const char *const V_SPEED[] = {"off", "narrow", "wide", NULL};
static const char *const V_TOL[] = {"low", "med", "high", NULL};
static const char *const V_FLAG[] = {"0", "1", NULL};
static const char *const V_SEG[] = {"original", "overlay", "mask", NULL};
static const struct {
    const char *name;
    const char *const *values; /* what a client may set it to */
} SETTINGS[] = {
    {"track.line", V_LINE},     {"traffic.range", V_RANGE}, {"traffic.line", V_LINE}, {"traffic.speed", V_SPEED},
    {"color.tol", V_TOL},       {"edge.hard", V_FLAG},      {"trace.dark", V_FLAG},   {"seg.view", V_SEG},
};
#define SETTING_COUNT (sizeof(SETTINGS) / sizeof(SETTINGS[0]))

/* The word of `list` that v is, or `otherwise`. */
static const char *word(const cJSON *v, const char *const *list, const char *otherwise)
{
    size_t i;

    for (i = 0; cJSON_IsString(v) && list[i] != NULL; i++) {
        if (strcmp(v->valuestring, list[i]) == 0) {
            return list[i];
        }
    }
    return otherwise;
}

static bool whole(const cJSON *v, double lo, double hi)
{
    return cJSON_IsNumber(v) && v->valuedouble >= lo && v->valuedouble <= hi && v->valuedouble == floor(v->valuedouble);
}

/* A figure: a whole number from 0 to `max`, or 0. */
static double figure(const cJSON *v, double max)
{
    return whole(v, 0, max) ? v->valuedouble : 0;
}

/* Text a screen can show as it is: at most `max` bytes, each a printable
 * ASCII character. */
static bool printable(const cJSON *v, size_t max)
{
    const char *p;

    if (!cJSON_IsString(v) || strlen(v->valuestring) > max) {
        return false;
    }
    for (p = v->valuestring; *p != '\0'; p++) {
        if (*p < 0x20 || *p > 0x7e) {
            return false;
        }
    }
    return true;
}

/* A setting's value as the unit may report it: one to sixteen of a-z, 0-9. */
static bool value_plain(const cJSON *v)
{
    const char *p;

    if (!cJSON_IsString(v) || v->valuestring[0] == '\0' || strlen(v->valuestring) > GW_VISION_VALUE_MAX) {
        return false;
    }
    for (p = v->valuestring; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) {
            return false;
        }
    }
    return true;
}

static bool only(const cJSON *params, const char *const *names)
{
    const cJSON *el;
    size_t i;

    if (!cJSON_IsObject(params)) {
        return false;
    }
    cJSON_ArrayForEach(el, params) {
        for (i = 0; names[i] != NULL; i++) {
            if (el->string != NULL && strcmp(el->string, names[i]) == 0) {
                break;
            }
        }
        if (names[i] == NULL) {
            return false;
        }
    }
    return true;
}

/* ---- the status ------------------------------------------------------------- */

/* What a status says of the things vision.state is about. */
struct said {
    bool available;
    const char *owner, *mode, *share;
    unsigned modes; /* bit i: MODES[i] */
    const char *set[SETTING_COUNT]; /* into the answer read, or NULL */
};

static void read_said(const cJSON *result, struct said *s)
{
    const cJSON *modes, *settings, *el;
    size_t i;
    int n = 0;

    memset(s, 0, sizeof(*s));
    s->owner = "none";
    s->mode = "";
    s->share = "off";
    if (!cJSON_IsObject(result) || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "available"))) {
        return;
    }
    s->available = true;
    s->owner = word(cJSON_GetObjectItemCaseSensitive(result, "owner"), OWNERS, "none");
    s->mode = word(cJSON_GetObjectItemCaseSensitive(result, "mode"), MODES, "");
    s->share = word(cJSON_GetObjectItemCaseSensitive(result, "share"), SHARES, "off");
    modes = cJSON_GetObjectItemCaseSensitive(result, "modes");
    if (cJSON_IsArray(modes)) {
        cJSON_ArrayForEach(el, modes) {
            if (n++ >= GW_VISION_MODES_MAX) {
                break;
            }
            for (i = 0; cJSON_IsString(el) && MODES[i] != NULL; i++) {
                if (strcmp(el->valuestring, MODES[i]) == 0) {
                    s->modes |= 1u << i;
                }
            }
        }
    }
    settings = cJSON_GetObjectItemCaseSensitive(result, "settings");
    for (i = 0; cJSON_IsObject(settings) && i < SETTING_COUNT; i++) {
        el = cJSON_GetObjectItemCaseSensitive(settings, SETTINGS[i].name);
        if (value_plain(el)) {
            s->set[i] = el->valuestring;
        }
    }
}

static bool mode_biometric(const char *mode)
{
    size_t i;

    for (i = GW_VISION_REMOTE; MODES[i] != NULL; i++) {
        if (strcmp(mode, MODES[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool add_string_item(cJSON *arr, const char *text)
{
    cJSON *s = cJSON_CreateString(text);

    if (s == NULL || !cJSON_AddItemToArray(arr, s)) {
        cJSON_Delete(s);
        return false;
    }
    return true;
}

static bool add_stats(cJSON *o, const cJSON *stats)
{
    size_t i;

    for (i = 0; STATS[i] != NULL; i++) {
        double v = figure(cJSON_IsObject(stats) ? cJSON_GetObjectItemCaseSensitive(stats, STATS[i]) : NULL,
                          GW_VISION_FIGURE_MAX);

        if (strcmp(STATS[i], "cpu_pct") == 0 && v > 100) {
            v = 100;
        }
        if (cJSON_AddNumberToObject(o, STATS[i], v) == NULL) {
            return false;
        }
    }
    return true;
}

cJSON *gw_vision_status(const cJSON *result, enum gw_vision_view view)
{
    const cJSON *model, *labels, *pic, *el;
    const char *share;
    struct said s;
    cJSON *o, *sub;
    size_t i;
    bool ok;
    int n = 0;

    if (!cJSON_IsObject(result)) {
        return NULL;
    }
    o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    read_said(result, &s);
    if (!s.available) {
        /* Vision is not in front, or an answer that does not say: nothing else. */
        if (cJSON_AddBoolToObject(o, "available", false) == NULL) {
            cJSON_Delete(o);
            return NULL;
        }
        return o;
    }
    /* The unit's "live" is somebody's preview. Whose, the gateway knows:
     * the viewer is told live, every other client busy. A "live" the gateway
     * has no viewer for is being ended and is nobody's. */
    share = s.share;
    if (strcmp(share, "live") == 0) {
        share = view == GW_VV_SELF ? "live" : view == GW_VV_OTHER ? "busy" : "off";
    }
    ok = cJSON_AddBoolToObject(o, "available", true) != NULL && cJSON_AddStringToObject(o, "owner", s.owner) != NULL &&
         cJSON_AddStringToObject(o, "mode", s.mode) != NULL;
    sub = ok ? cJSON_AddArrayToObject(o, "modes") : NULL;
    for (i = 0; ok && sub != NULL && MODES[i] != NULL; i++) {
        if ((s.modes & (1u << i)) != 0) {
            ok = add_string_item(sub, MODES[i]);
        }
    }
    ok = ok && sub != NULL;
    sub = ok ? cJSON_AddObjectToObject(o, "settings") : NULL;
    for (i = 0; ok && sub != NULL && i < SETTING_COUNT; i++) {
        if (s.set[i] != NULL) {
            ok = cJSON_AddStringToObject(sub, SETTINGS[i].name, s.set[i]) != NULL;
        }
    }
    ok = ok && sub != NULL;
    model = cJSON_GetObjectItemCaseSensitive(result, "model");
    ok = ok && cJSON_AddStringToObject(o, "model", printable(model, GW_VISION_MODEL_MAX) ? model->valuestring : "") != NULL;
    /* The detector's class names. A box names its class by its place here,
     * so a name that cannot be shown keeps its place, empty. */
    labels = cJSON_GetObjectItemCaseSensitive(result, "labels");
    sub = ok ? cJSON_AddArrayToObject(o, "labels") : NULL;
    if (ok && sub != NULL && cJSON_IsArray(labels)) {
        cJSON_ArrayForEach(el, labels) {
            if (!ok || n++ >= GW_VISION_LABELS_MAX) {
                break;
            }
            ok = add_string_item(sub, printable(el, GW_VISION_LABEL_MAX) ? el->valuestring : "");
        }
    }
    ok = ok && sub != NULL;
    pic = cJSON_GetObjectItemCaseSensitive(result, "pic");
    sub = ok ? cJSON_AddObjectToObject(o, "pic") : NULL;
    ok = ok && sub != NULL &&
         cJSON_AddNumberToObject(sub, "w",
                                 figure(cJSON_IsObject(pic) ? cJSON_GetObjectItemCaseSensitive(pic, "w") : NULL,
                                        GW_VISION_SIDE_MAX)) != NULL &&
         cJSON_AddNumberToObject(sub, "h",
                                 figure(cJSON_IsObject(pic) ? cJSON_GetObjectItemCaseSensitive(pic, "h") : NULL,
                                        GW_VISION_SIDE_MAX)) != NULL;
    sub = ok ? cJSON_AddObjectToObject(o, "stats") : NULL;
    ok = ok && sub != NULL && add_stats(sub, cJSON_GetObjectItemCaseSensitive(result, "stats")) &&
         cJSON_AddStringToObject(o, "share", share) != NULL;
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

cJSON *gw_vision_status_row(const cJSON *result)
{
    return gw_vision_status(result, GW_VV_NONE);
}

cJSON *gw_vision_shared(const cJSON *result)
{
    const char *state = cJSON_IsObject(result) ? word(cJSON_GetObjectItemCaseSensitive(result, "state"), SHARES, NULL)
                                               : NULL;
    cJSON *o;

    if (state == NULL) {
        return NULL;
    }
    o = cJSON_CreateObject();
    if (o == NULL || cJSON_AddStringToObject(o, "state", state) == NULL) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

cJSON *gw_vision_done(const cJSON *result)
{
    return cJSON_IsObject(result) ? cJSON_CreateObject() : NULL;
}

/* ---- parameters ------------------------------------------------------------- */

int gw_vision_params_share(const cJSON *in, cJSON **out)
{
    static const char *const names[] = {"on", NULL};
    const cJSON *on = cJSON_IsObject(in) ? cJSON_GetObjectItemCaseSensitive(in, "on") : NULL;

    *out = NULL;
    if (!only(in, names) || !cJSON_IsBool(on)) {
        return -1;
    }
    *out = cJSON_CreateObject();
    if (*out == NULL || cJSON_AddBoolToObject(*out, "on", cJSON_IsTrue(on)) == NULL) {
        cJSON_Delete(*out);
        *out = NULL;
        return -1;
    }
    return 0;
}

int gw_vision_params_ack(const cJSON *in, cJSON **out)
{
    static const char *const names[] = {"seq", NULL};
    const cJSON *seq = cJSON_IsObject(in) ? cJSON_GetObjectItemCaseSensitive(in, "seq") : NULL;

    *out = NULL;
    if (!only(in, names) || !whole(seq, 0, GW_VISION_SEQ_MAX)) {
        return -1;
    }
    *out = cJSON_CreateObject();
    if (*out == NULL || cJSON_AddNumberToObject(*out, "seq", seq->valuedouble) == NULL) {
        cJSON_Delete(*out);
        *out = NULL;
        return -1;
    }
    return 0;
}

int gw_vision_params_mode(const cJSON *in, cJSON **out)
{
    static const char *const names[] = {"mode", NULL};
    const cJSON *mode = cJSON_IsObject(in) ? cJSON_GetObjectItemCaseSensitive(in, "mode") : NULL;
    const char *w = NULL;
    size_t i;

    *out = NULL;
    /* Only the modes a phone may choose: the list ends before the others. */
    for (i = 0; cJSON_IsString(mode) && i < GW_VISION_REMOTE; i++) {
        if (strcmp(mode->valuestring, MODES[i]) == 0) {
            w = MODES[i];
        }
    }
    if (!only(in, names) || w == NULL) {
        return -1;
    }
    *out = cJSON_CreateObject();
    if (*out == NULL || cJSON_AddStringToObject(*out, "mode", w) == NULL) {
        cJSON_Delete(*out);
        *out = NULL;
        return -1;
    }
    return 0;
}

int gw_vision_params_set(const cJSON *in, cJSON **out)
{
    static const char *const names[] = {"key", "value", NULL};
    const cJSON *key = cJSON_IsObject(in) ? cJSON_GetObjectItemCaseSensitive(in, "key") : NULL;
    const cJSON *value = cJSON_IsObject(in) ? cJSON_GetObjectItemCaseSensitive(in, "value") : NULL;
    const char *k = NULL, *v = NULL;
    size_t i;

    *out = NULL;
    for (i = 0; cJSON_IsString(key) && i < SETTING_COUNT; i++) {
        if (strcmp(key->valuestring, SETTINGS[i].name) == 0) {
            k = SETTINGS[i].name;
            v = word(value, SETTINGS[i].values, NULL);
        }
    }
    if (!only(in, names) || k == NULL || v == NULL) {
        return -1;
    }
    *out = cJSON_CreateObject();
    if (*out == NULL || cJSON_AddStringToObject(*out, "key", k) == NULL ||
        cJSON_AddStringToObject(*out, "value", v) == NULL) {
        cJSON_Delete(*out);
        *out = NULL;
        return -1;
    }
    return 0;
}

/* ---- a picture -------------------------------------------------------------- */

/* Base64 as RFC 4648 section 4 writes it, with its padding, of 1 to
 * GW_VISION_JPEG_MAX bytes that begin FF D8 FF, as every JPEG does. */
static bool jpeg_text(const cJSON *v)
{
    const char *p;
    size_t n, i, pad = 0;

    if (!cJSON_IsString(v)) {
        return false;
    }
    p = v->valuestring;
    n = strlen(p);
    if (n < 4 || n > GW_VISION_B64_MAX || n % 4 != 0 || strncmp(p, "/9j/", 4) != 0) {
        return false;
    }
    if (p[n - 1] == '=') {
        pad = p[n - 2] == '=' ? 2 : 1;
    }
    for (i = 0; i < n - pad; i++) {
        char c = p[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')) {
            return false;
        }
    }
    return n / 4 * 3 - pad <= GW_VISION_JPEG_MAX;
}

enum gw_vision_pulled gw_vision_frame(const cJSON *result, double after_seq, cJSON **data, double *seq)
{
    const cJSON *vseq, *w, *h, *age, *jpeg, *boxes, *row, *el;
    cJSON *o, *arr, *out_row;
    bool ok;
    int n;

    *data = NULL;
    *seq = 0;
    if (!cJSON_IsObject(result)) {
        return GW_VF_BAD;
    }
    vseq = cJSON_GetObjectItemCaseSensitive(result, "seq");
    jpeg = cJSON_GetObjectItemCaseSensitive(result, "jpeg");
    if (!whole(vseq, 0, GW_VISION_SEQ_MAX)) {
        return GW_VF_BAD;
    }
    if (jpeg == NULL) {
        /* No picture in it. Anything else it holds is not looked at. */
        return GW_VF_NONE;
    }
    w = cJSON_GetObjectItemCaseSensitive(result, "w");
    h = cJSON_GetObjectItemCaseSensitive(result, "h");
    age = cJSON_GetObjectItemCaseSensitive(result, "age_ms");
    boxes = cJSON_GetObjectItemCaseSensitive(result, "boxes");
    if (!(vseq->valuedouble > after_seq) || !whole(w, 1, GW_VISION_SIDE_MAX) || !whole(h, 1, GW_VISION_SIDE_MAX) ||
        !whole(age, 0, GW_VISION_AGE_MAX) || !jpeg_text(jpeg) || !cJSON_IsArray(boxes) ||
        cJSON_GetArraySize(boxes) > GW_VISION_BOXES_MAX) {
        return GW_VF_BAD;
    }
    cJSON_ArrayForEach(row, boxes) {
        n = 0;
        if (!cJSON_IsArray(row)) {
            return GW_VF_BAD;
        }
        cJSON_ArrayForEach(el, row) {
            if (!whole(el, -GW_VISION_BOX_ABS_MAX, GW_VISION_BOX_ABS_MAX)) {
                return GW_VF_BAD;
            }
            n++;
        }
        if (n != GW_VISION_BOX_FIELDS) {
            return GW_VF_BAD;
        }
    }
    /* Checked; now written again. */
    o = cJSON_CreateObject();
    ok = o != NULL && cJSON_AddNumberToObject(o, "seq", vseq->valuedouble) != NULL &&
         cJSON_AddNumberToObject(o, "w", w->valuedouble) != NULL &&
         cJSON_AddNumberToObject(o, "h", h->valuedouble) != NULL &&
         cJSON_AddNumberToObject(o, "age_ms", age->valuedouble) != NULL &&
         cJSON_AddStringToObject(o, "jpeg", jpeg->valuestring) != NULL;
    arr = ok ? cJSON_AddArrayToObject(o, "boxes") : NULL;
    ok = ok && arr != NULL;
    cJSON_ArrayForEach(row, boxes) {
        if (!ok) {
            break;
        }
        out_row = cJSON_CreateArray();
        ok = out_row != NULL && cJSON_AddItemToArray(arr, out_row);
        if (!ok) {
            cJSON_Delete(out_row);
            break;
        }
        cJSON_ArrayForEach(el, row) {
            cJSON *num = cJSON_CreateNumber(el->valuedouble);

            if (num == NULL || !cJSON_AddItemToArray(out_row, num)) {
                cJSON_Delete(num);
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        cJSON_Delete(o);
        return GW_VF_BAD;
    }
    *data = o;
    *seq = vseq->valuedouble;
    return GW_VF_PICTURE;
}

/* ---- what clients were told, and the one viewer ----------------------------- */

void gw_vision_init(struct gw_vision *v)
{
    memset(v, 0, sizeof(*v));
}

void gw_vision_step(struct gw_vision *v, const cJSON *raw, bool *changed, cJSON **stats, struct gw_vision_seen *seen)
{
    const cJSON *client;
    char sig[GW_VISION_SIG];
    struct said s;
    size_t i, at;
    int w;

    *changed = false;
    *stats = NULL;
    memset(seen, 0, sizeof(*seen));
    if (!cJSON_IsObject(raw)) {
        return;
    }
    read_said(raw, &s);
    /* Everything vision.state is about, as one line to compare. */
    w = snprintf(sig, sizeof(sig), "%d;%s;%s;%x;%s", s.available ? 1 : 0, s.owner, s.mode, s.modes, s.share);
    at = w > 0 && (size_t)w < sizeof(sig) ? (size_t)w : sizeof(sig) - 1;
    for (i = 0; i < SETTING_COUNT; i++) {
        w = snprintf(sig + at, sizeof(sig) - at, ";%s", s.set[i] != NULL ? s.set[i] : "");
        at = w > 0 && at + (size_t)w < sizeof(sig) ? at + (size_t)w : sizeof(sig) - 1;
    }
    if (!v->told || strcmp(v->sig, sig) != 0) {
        memcpy(v->sig, sig, sizeof(sig));
        v->told = true;
        *changed = true;
    }
    seen->available = s.available;
    if (!s.available) {
        return;
    }
    seen->live = strcmp(s.share, "live") == 0;
    seen->biometric = mode_biometric(s.mode);
    client = cJSON_GetObjectItemCaseSensitive(raw, "client");
    if (seen->live && cJSON_IsString(client) && strlen(client->valuestring) == GW_VISION_CLIENT - 1 &&
        strspn(client->valuestring, "0123456789abcdef") == GW_VISION_CLIENT - 1) {
        memcpy(seen->client, client->valuestring, GW_VISION_CLIENT);
    }
    *stats = cJSON_CreateObject();
    if (*stats != NULL && !add_stats(*stats, cJSON_GetObjectItemCaseSensitive(raw, "stats"))) {
        cJSON_Delete(*stats);
        *stats = NULL;
    }
}

bool gw_vision_lost(struct gw_vision *v)
{
    bool owed = v->told && v->sig[0] == '1';

    v->told = false;
    v->sig[0] = '\0';
    return owed;
}

void gw_vision_viewer(struct gw_vision *v, const uint8_t *fp, uint64_t sid, uint64_t order, int64_t now_ms)
{
    v->viewing = true;
    v->sid = sid;
    memcpy(v->fp, fp, GW_FP_LEN);
    v->began = order;
    v->last_seq = 0;
    v->sent = false;
    v->waiting = false;
    v->sent_seq = 0;
    v->sent_ms = 0;
    v->next_ms = now_ms; /* the first picture needs no acknowledgement and no wait */
}

void gw_vision_viewer_end(struct gw_vision *v)
{
    v->viewing = false;
    v->sid = 0;
    memset(v->fp, 0, sizeof(v->fp));
    v->sent = false;
    v->waiting = false;
}

enum gw_vision_view gw_vision_view_of(const struct gw_vision *v, uint64_t sid)
{
    return !v->viewing ? GW_VV_NONE : v->sid == sid ? GW_VV_SELF : GW_VV_OTHER;
}

bool gw_vision_pull_due(const struct gw_vision *v, int64_t now_ms)
{
    return v->viewing && now_ms >= v->next_ms && !(v->sent && v->waiting);
}

void gw_vision_none(struct gw_vision *v, int64_t now_ms)
{
    v->next_ms = now_ms + GW_VISION_RETRY_MS;
}

void gw_vision_sent(struct gw_vision *v, double seq, int64_t now_ms)
{
    v->last_seq = seq;
    v->sent = true;
    v->waiting = true;
    v->sent_seq = seq;
    v->sent_ms = now_ms;
    v->next_ms = now_ms + GW_VISION_INTERVAL_MS;
}

void gw_vision_skipped(struct gw_vision *v, double seq, int64_t now_ms)
{
    if (seq > v->last_seq) {
        v->last_seq = seq;
    }
    v->waiting = false;
    v->next_ms = now_ms + GW_VISION_INTERVAL_MS;
}

bool gw_vision_ack(struct gw_vision *v, uint64_t sid, double seq)
{
    if (!v->viewing || v->sid != sid || !v->sent || !v->waiting || seq != v->sent_seq) {
        return false;
    }
    v->waiting = false;
    return true;
}

bool gw_vision_ack_late(const struct gw_vision *v, int64_t now_ms)
{
    return v->viewing && v->sent && v->waiting && now_ms - v->sent_ms >= GW_VISION_ACK_MS;
}

bool gw_vision_rate(struct gw_vision *v, const uint8_t *fp, int64_t now_ms)
{
    size_t i, at = 0;
    bool found = false;
    int oldest;

    /* Its own slot, a free one, or the one that was used longest ago. */
    for (i = 0; i < GW_MAX_CLIENTS && !found; i++) {
        if (v->rate[i].used && memcmp(v->rate[i].fp, fp, GW_FP_LEN) == 0) {
            at = i;
            found = true;
        }
    }
    for (i = 0; i < GW_MAX_CLIENTS && !found; i++) {
        int last_i = (v->rate[i].at + GW_VISION_PER_MIN - 1) % GW_VISION_PER_MIN;
        int last_at = (v->rate[at].at + GW_VISION_PER_MIN - 1) % GW_VISION_PER_MIN;

        if (!v->rate[i].used) {
            at = i;
            break;
        }
        if (v->rate[at].used && v->rate[i].ms[last_i] < v->rate[at].ms[last_at]) {
            at = i;
        }
    }
    if (!found) {
        memset(&v->rate[at], 0, sizeof(v->rate[at]));
        v->rate[at].used = true;
        memcpy(v->rate[at].fp, fp, GW_FP_LEN);
    }
    /* A ring of the last GW_VISION_PER_MIN times: full, and its oldest
     * younger than a minute, is the limit. */
    if (v->rate[at].n >= GW_VISION_PER_MIN) {
        oldest = v->rate[at].at;
        if (now_ms >= v->rate[at].ms[oldest] && now_ms - v->rate[at].ms[oldest] < 60000) {
            return false;
        }
    } else {
        v->rate[at].n++;
    }
    v->rate[at].ms[v->rate[at].at] = now_ms;
    v->rate[at].at = (v->rate[at].at + 1) % GW_VISION_PER_MIN;
    return true;
}
