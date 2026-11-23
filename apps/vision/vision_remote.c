/*
 * Vision's answers to a paired companion (vision_remote.h).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_remote.h"

#include "pocketipc/pocketipc.h"
#include "pocketvision/vision_share.h"

#include <stdlib.h>
#include <string.h>

static const char *const line_words[VISION_LINE_MODES] = { "off", "across", "down" };
static const char *const speed_words[VISION_SPEED_MODES] = { "off", "narrow", "wide" };
static const char *const tol_words[VISION_TOLS] = { "low", "med", "high" };
/* In enum vision_range's order (vision_range.h). */
static const char *const range_words[VISION_RANGES] = { "near", "normal", "far" };

bool vision_remote_private(enum vision_mode mode)
{
    return mode == VISION_MODE_FACE || mode == VISION_MODE_RECOGNIZE || mode == VISION_MODE_READ;
}

bool vision_remote_shareable(const struct vision_model *m, const struct vision_session *s)
{
    return m->state == VISION_LIVE && s->share_cap && s->streaming && !vision_remote_private(m->mode);
}

static int word_index(const char *w, const char *const *words, int n)
{
    int i;

    for (i = 0; w && i < n; i++) {
        if (strcmp(w, words[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static const char *word_or(const char *const *words, int n, int i)
{
    return i >= 0 && i < n ? words[i] : words[0];
}

cJSON *vision_remote_status(const struct vision_model *m, const struct vision_session *s,
                            enum vision_share_state share, const char *client)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *modes;
    cJSON *set;
    cJSON *labels;
    cJSON *pic;
    cJSON *stats;
    uint32_t pw = s->share_w;
    uint32_t ph = s->share_h;
    size_t label_bytes = 0;
    uint32_t i;
    int mode;

    if (!o) {
        return NULL;
    }
    cJSON_AddBoolToObject(o, "available", true);
    cJSON_AddStringToObject(o, "owner", m->state == VISION_LIVE ? "vision" : "none");
    cJSON_AddStringToObject(o, "mode", vision_mode_word(m->mode));
    modes = cJSON_AddArrayToObject(o, "modes");
    for (mode = 0; modes && mode < VISION_MODES; mode++) {
        if (vision_model_offered(m, (enum vision_mode)mode)) {
            cJSON_AddItemToArray(modes, cJSON_CreateString(vision_mode_word((enum vision_mode)mode)));
        }
    }
    set = cJSON_AddObjectToObject(o, "settings");
    if (set) {
        cJSON_AddStringToObject(set, "track.line", word_or(line_words, VISION_LINE_MODES, m->set.track.line));
        cJSON_AddStringToObject(set, "traffic.range", vision_model_range_word(m));
        cJSON_AddStringToObject(set, "traffic.line", word_or(line_words, VISION_LINE_MODES, m->set.traffic.line));
        cJSON_AddStringToObject(set, "traffic.speed", word_or(speed_words, VISION_SPEED_MODES, m->set.traffic.speed));
        cJSON_AddStringToObject(set, "color.tol", word_or(tol_words, VISION_TOLS, m->set.color.tol_idx));
        cJSON_AddStringToObject(set, "edge.hard", m->set.edge.hard ? "1" : "0");
        cJSON_AddStringToObject(set, "trace.dark", m->set.trace.dark ? "1" : "0");
        cJSON_AddStringToObject(set, "seg.view", vision_seg_view_word(m->set.seg.view));
    }
    cJSON_AddStringToObject(o, "model", m->model_name);
    /* The detector's class names, by index, so that a box's class can be
     * said on the phone. All of them or none: a list cut short would name
     * some boxes and not others. */
    labels = cJSON_AddArrayToObject(o, "labels");
    if (labels && m->classes > 0 && m->classes <= VISION_REMOTE_LABELS_MAX) {
        for (i = 0; i < m->classes; i++) {
            label_bytes += strlen(vision_model_class_label(m, i)) + 3;
        }
        if (label_bytes <= VISION_REMOTE_LABEL_BYTES) {
            for (i = 0; i < m->classes; i++) {
                cJSON_AddItemToArray(labels, cJSON_CreateString(vision_model_class_label(m, i)));
            }
        }
    }
    if (pw == 0 || ph == 0) {
        /* Before the first shared picture: the size it will have. */
        uint32_t div = vision_share_divisor(s->frame_w, s->frame_h, VISION_SHARE_DEFAULT_SIDE);

        pw = div ? s->frame_w / div : 0;
        ph = div ? s->frame_h / div : 0;
    }
    pic = cJSON_AddObjectToObject(o, "pic");
    if (pic) {
        cJSON_AddNumberToObject(pic, "w", pw);
        cJSON_AddNumberToObject(pic, "h", ph);
    }
    stats = cJSON_AddObjectToObject(o, "stats");
    if (stats) {
        cJSON_AddNumberToObject(stats, "fps_x10", m->stats_valid ? m->stats.fps_x10 : 0);
        cJSON_AddNumberToObject(stats, "infer_ms", m->stats_valid ? m->stats.infer_ms : 0);
        cJSON_AddNumberToObject(stats, "pre_ms", m->stats_valid ? m->stats.pre_ms : 0);
        cJSON_AddNumberToObject(stats, "post_ms", m->stats_valid ? m->stats.post_ms : 0);
        cJSON_AddNumberToObject(stats, "cpu_pct", m->stats_valid ? m->stats.cpu_pct : 0);
        cJSON_AddNumberToObject(stats, "rss_kb", m->stats_valid ? (double)m->stats.rss_kb : 0);
        cJSON_AddNumberToObject(stats, "dropped", m->stats_valid ? m->stats.dropped : 0);
        cJSON_AddNumberToObject(stats, "count_ab", m->count_a);
        cJSON_AddNumberToObject(stats, "count_ba", m->count_b);
        /* What sharing itself costs, for whoever measures it. */
        cJSON_AddNumberToObject(stats, "share_made", s->share_made);
        cJSON_AddNumberToObject(stats, "share_failed", s->share_failed);
        cJSON_AddNumberToObject(stats, "share_enc_ms", s->share_enc_ms);
        cJSON_AddNumberToObject(stats, "share_enc_max_ms", s->share_enc_max_ms);
        cJSON_AddNumberToObject(stats, "share_bytes", s->share_len);
    }
    cJSON_AddStringToObject(o, "share", share == VISION_SHARE_LIVE ? "live" : share == VISION_SHARE_STOPPED ? "stopped" : "off");
    if (share == VISION_SHARE_LIVE && client && client[0]) {
        cJSON_AddStringToObject(o, "client", client);
    }
    return o;
}

size_t vision_remote_base64(const uint8_t *in, size_t n, char *out)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    size_t o = 0;

    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];

        out[o++] = abc[v >> 18];
        out[o++] = abc[(v >> 12) & 63];
        out[o++] = abc[(v >> 6) & 63];
        out[o++] = abc[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;

        out[o++] = abc[v >> 18];
        out[o++] = abc[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);

        out[o++] = abc[v >> 18];
        out[o++] = abc[(v >> 12) & 63];
        out[o++] = abc[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

cJSON *vision_remote_picture(const struct vision_session *s, uint32_t seq, int64_t now_ms)
{
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t len = 0;
    int64_t at = 0;
    const uint8_t *jpeg = vision_session_shared(s, NULL, &w, &h, &len, &at);
    const struct vision_shown *b;
    cJSON *o;
    cJSON *boxes;
    cJSON *text;
    char *b64;
    int n = 0;
    int i;

    if (!jpeg || len == 0 || len > VISION_SHARE_MAX_BYTES) {
        return NULL;
    }
    b64 = malloc(4 * (((size_t)len + 2) / 3) + 1);
    if (!b64) {
        return NULL;
    }
    vision_remote_base64(jpeg, len, b64);
    o = cJSON_CreateObject();
    text = o ? cJSON_CreateString(b64) : NULL;
    free(b64);
    if (!o || !text) {
        cJSON_Delete(o);
        cJSON_Delete(text);
        return NULL;
    }
    cJSON_AddNumberToObject(o, "seq", seq);
    cJSON_AddNumberToObject(o, "w", w);
    cJSON_AddNumberToObject(o, "h", h);
    cJSON_AddNumberToObject(o, "age_ms", now_ms > at ? (double)(now_ms - at) : 0);
    cJSON_AddItemToObject(o, "jpeg", text);
    boxes = cJSON_AddArrayToObject(o, "boxes");
    b = vision_session_shared_boxes(s, &n);
    for (i = 0; boxes && i < n && i < VISION_MAX_SHOWN; i++) {
        const int v[9] = { (int)b[i].id, b[i].cls, b[i].conf, b[i].x, b[i].y, b[i].w, b[i].h, b[i].dir, (int)b[i].kmh10 };

        cJSON_AddItemToArray(boxes, cJSON_CreateIntArray(v, 9));
    }
    return o;
}

static const char *param_word(const cJSON *params, const char *key)
{
    const cJSON *v = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;

    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

int vision_remote_mode(struct vision_model *m, const cJSON *params, unsigned *acts, const char **message)
{
    const char *w = param_word(params, "mode");
    int mode = w ? vision_mode_parse(w) : -1;

    *acts = 0;
    if (mode < 0) {
        *message = "mode: one of Vision's modes";
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    if (vision_remote_private((enum vision_mode)mode)) {
        *message = "this mode is chosen on the unit only";
        return POCKETIPC_ERR_POLICY;
    }
    if (!vision_model_offered(m, (enum vision_mode)mode)) {
        *message = "this unit cannot run that mode";
        return POCKETIPC_ERR_POLICY;
    }
    *acts = vision_model_set_mode(m, (enum vision_mode)mode);
    return 0;
}

int vision_remote_set(struct vision_model *m, const cJSON *params, unsigned *acts, const char **message)
{
    const char *key = param_word(params, "key");
    const char *value = param_word(params, "value");
    int v;
    int i;

    *acts = 0;
    if (!key || !value) {
        *message = "key and value: strings";
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    /* Each setting belongs to a mode, as on Vision's own sheets: it is
     * changed while that mode is in force, through the same step a tap
     * takes, so the helper and the stored settings follow as they do there. */
    if (strcmp(key, "track.line") == 0 || strcmp(key, "traffic.line") == 0) {
        enum vision_mode need = strcmp(key, "track.line") == 0 ? VISION_MODE_TRACK : VISION_MODE_TRAFFIC;

        v = word_index(value, line_words, VISION_LINE_MODES);
        if (v < 0) {
            goto bad_value;
        }
        if (m->mode != need) {
            goto wrong_mode;
        }
        for (i = 0; i < VISION_LINE_MODES && (int)vision_model_line(m) != v; i++) {
            *acts |= vision_model_line_next(m);
        }
        return 0;
    }
    if (strcmp(key, "traffic.range") == 0) {
        v = word_index(value, range_words, VISION_RANGES);
        if (v < 0) {
            goto bad_value;
        }
        if (m->mode != VISION_MODE_TRAFFIC) {
            goto wrong_mode;
        }
        *acts = vision_model_set_range(m, (enum vision_range)v);
        return 0;
    }
    if (strcmp(key, "traffic.speed") == 0) {
        v = word_index(value, speed_words, VISION_SPEED_MODES);
        if (v < 0) {
            goto bad_value;
        }
        if (m->mode != VISION_MODE_TRAFFIC) {
            goto wrong_mode;
        }
        for (i = 0; i < VISION_SPEED_MODES && (int)m->set.traffic.speed != v; i++) {
            *acts |= vision_model_speed_next(m);
        }
        return 0;
    }
    if (strcmp(key, "color.tol") == 0) {
        v = word_index(value, tol_words, VISION_TOLS);
        if (v < 0) {
            goto bad_value;
        }
        if (m->mode != VISION_MODE_COLOR) {
            goto wrong_mode;
        }
        for (i = 0; i < VISION_TOLS && m->set.color.tol_idx != v; i++) {
            *acts |= vision_model_tol_next(m);
        }
        return 0;
    }
    if (strcmp(key, "edge.hard") == 0 || strcmp(key, "trace.dark") == 0) {
        bool edge = key[0] == 'e';

        if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
            goto bad_value;
        }
        if (m->mode != (edge ? VISION_MODE_EDGE : VISION_MODE_TRACE)) {
            goto wrong_mode;
        }
        if ((edge ? m->set.edge.hard : m->set.trace.dark) != (value[0] == '1')) {
            *acts = edge ? vision_model_edge_next(m) : vision_model_trace_next(m);
        }
        return 0;
    }
    if (strcmp(key, "seg.view") == 0) {
        for (v = 0; v < VISION_SEG_VIEWS; v++) {
            if (strcmp(value, vision_seg_view_word((enum vision_seg_view)v)) == 0) {
                break;
            }
        }
        if (v >= VISION_SEG_VIEWS) {
            goto bad_value;
        }
        if (m->mode != VISION_MODE_SEGMENT && m->mode != VISION_MODE_INSTANCES) {
            goto wrong_mode;
        }
        *acts = vision_model_set_view(m, (enum vision_seg_view)v);
        return 0;
    }
    *message = "key: not a setting a companion may change";
    return POCKETIPC_ERR_INVALID_PARAMS;
bad_value:
    *message = "value: not one this setting offers";
    return POCKETIPC_ERR_INVALID_PARAMS;
wrong_mode:
    *message = "this setting belongs to another mode";
    return POCKETIPC_ERR_POLICY;
}

int vision_remote_share_params(const cJSON *params, bool *on, char client[VISION_REMOTE_CLIENT_MAX],
                               char name[VISION_REMOTE_NAME_MAX], const char **message)
{
    const cJSON *v = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "on") : NULL;
    const char *c = param_word(params, "client");
    const char *n = param_word(params, "name");
    size_t i;

    client[0] = '\0';
    name[0] = '\0';
    if (!cJSON_IsBool(v)) {
        *message = "on: true or false";
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    *on = cJSON_IsTrue(v);
    if (!*on) {
        return 0;
    }
    /* Who is shown the picture is said on the screen: a share without a
     * viewer's key is not started. */
    if (!c || strlen(c) != VISION_REMOTE_CLIENT_MAX - 1) {
        *message = "client: the viewer's key, 16 hex digits";
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    for (i = 0; i < VISION_REMOTE_CLIENT_MAX - 1; i++) {
        if (!((c[i] >= '0' && c[i] <= '9') || (c[i] >= 'a' && c[i] <= 'f'))) {
            *message = "client: the viewer's key, 16 hex digits";
            return POCKETIPC_ERR_INVALID_PARAMS;
        }
        client[i] = c[i];
    }
    client[i] = '\0';
    /* The name goes on the screen: printable ASCII of it, the rest as '?'. */
    for (i = 0; n && n[i] && i < VISION_REMOTE_NAME_MAX - 1; i++) {
        name[i] = n[i] >= 0x20 && n[i] < 0x7f ? n[i] : '?';
    }
    name[i] = '\0';
    return 0;
}

uint32_t vision_remote_after(const cJSON *params)
{
    const cJSON *v = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "after_seq") : NULL;

    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 4294967295.0) {
        return 0;
    }
    return (uint32_t)v->valuedouble;
}
