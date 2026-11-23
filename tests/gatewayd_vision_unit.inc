/*
 * gatewayd unit suite, the Vision group (services/gatewayd/gw_vision.h;
 * ADR-019 amendment 1; ADR-006 amendment 1). Included by
 * gatewayd_unit_test.c.
 *
 * What is checked: the scope and the topic; the six rows and that
 * vision.pull is none; that an answer of the unit is rebuilt from the
 * contract's fields and nothing of it is passed on; that live is the
 * viewer's word alone and every other client reads busy; parameters, and
 * that FACE, RECOGNIZE and READ cannot be chosen; a picture: its bounds, its
 * base64, its boxes, and that anything else is dropped whole; the one
 * viewer: no second picture before the acknowledgement, the interval, the
 * time an acknowledgement may take; how often a client may ask.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */

static const char *const VIS_TOP[] = {"available", "owner", "mode",  "modes", "settings",
                                      "model",     "labels", "pic",  "stats", "share",  NULL};
static const char *const VIS_STATS[] = {"fps_x10", "infer_ms", "pre_ms",   "post_ms", "cpu_pct",
                                        "rss_kb",  "dropped",  "count_ab", "count_ba", NULL};
static const char *const VIS_FRAME[] = {"seq", "w", "h", "age_ms", "jpeg", "boxes", NULL};

static int vis_keys_are(const cJSON *o, const char *const *want)
{
    const cJSON *el;
    int i = 0;

    cJSON_ArrayForEach(el, o) {
        if (want[i] == NULL || el->string == NULL || strcmp(el->string, want[i]) != 0) {
            return 0;
        }
        i++;
    }
    return want[i] == NULL;
}

static const char *vis_s(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsString(v) ? v->valuestring : "(none)";
}

static double vis_n(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}

/* A status as the Vision app gives it, with more in it than any client may
 * see. */
static cJSON *vis_raw(const char *mode, const char *share, const char *client)
{
    char buf[1600];

    snprintf(buf, sizeof(buf),
             "{\"available\":true,\"owner\":\"vision\",\"mode\":\"%s\","
             "\"modes\":[\"detect\",\"track\",\"face\",\"LEAK\",7,\"detect\"],"
             "\"settings\":{\"track.line\":\"across\",\"color.tol\":\"med\",\"edge.hard\":\"1\","
             "\"seg.view\":\"Mask!\",\"face.owner\":\"LEAK\",\"traffic.range\":7},"
             "\"model\":\"yolov8n 320\",\"labels\":[\"person\",\"bicycle\",7,\"a label that is far too long to show\",\"car\"],"
             "\"pic\":{\"w\":264,\"h\":469,\"stride\":999},"
             "\"stats\":{\"fps_x10\":172,\"infer_ms\":34,\"pre_ms\":5,\"post_ms\":2,\"cpu_pct\":144,\"rss_kb\":7300,"
             "\"dropped\":0,\"count_ab\":3,\"count_ba\":1.5,\"faces\":2},"
             "\"share\":\"%s\",\"client\":\"%s\",\"name\":\"LEAK phone\",\"owner_name\":\"LEAK\","
             "\"embedding\":[1,2,3],\"helper_pid\":77}",
             mode, share, client);
    return parse(buf);
}

/* Base64, as the unit writes it, of `bytes` bytes that begin as a JPEG
 * does. To free. */
static char *vis_jpeg(size_t bytes)
{
    size_t len = (bytes + 2) / 3 * 4, pad = (3 - bytes % 3) % 3;
    char *s = malloc(len + 1);

    memset(s, 'A', len);
    memcpy(s, "/9j/", 4);
    if (pad > 0) {
        memset(s + len - pad, '=', pad);
    }
    s[len] = '\0';
    return s;
}

/* A vision.pull answer with a picture of `bytes` bytes and `boxes` boxes. */
static cJSON *vis_pull(double seq, double w, double h, size_t bytes, int boxes)
{
    cJSON *o = cJSON_CreateObject(), *arr, *row;
    char *jpeg = vis_jpeg(bytes);
    int i, k;

    cJSON_AddNumberToObject(o, "seq", seq);
    cJSON_AddNumberToObject(o, "w", w);
    cJSON_AddNumberToObject(o, "h", h);
    cJSON_AddNumberToObject(o, "age_ms", 41);
    cJSON_AddStringToObject(o, "jpeg", jpeg);
    free(jpeg);
    arr = cJSON_AddArrayToObject(o, "boxes");
    for (i = 0; i < boxes; i++) {
        row = cJSON_CreateArray();
        for (k = 0; k < 9; k++) {
            cJSON_AddItemToArray(row, cJSON_CreateNumber(k == 7 ? -1 : i * 10 + k));
        }
        cJSON_AddItemToArray(arr, row);
    }
    cJSON_AddStringToObject(o, "path", "/tmp/LEAK.jpg");
    return o;
}

static enum gw_vision_pulled vis_frame_of(cJSON *raw, double after)
{
    cJSON *data = NULL;
    double seq = -1;
    enum gw_vision_pulled r = gw_vision_frame(raw, after, &data, &seq);

    CHECK((r == GW_VF_PICTURE) == (data != NULL));
    cJSON_Delete(data);
    cJSON_Delete(raw);
    return r;
}

static int vis_params(int (*fn)(const cJSON *, cJSON **), const char *text, const char *want)
{
    cJSON *in = text != NULL ? parse(text) : NULL, *out = NULL;
    int rc = fn(in, &out), ok;
    char *got = out != NULL ? cJSON_PrintUnformatted(out) : NULL;

    ok = want == NULL ? (rc != 0 && out == NULL) : (rc == 0 && got != NULL && strcmp(got, want) == 0);
    free(got);
    cJSON_Delete(out);
    cJSON_Delete(in);
    return ok;
}

static void test_vision(void)
{
    static const char *const rows[] = {"vision.status", "vision.share", "vision.ack",
                                       "vision.mode",   "vision.set",   "vision.reset", NULL};
    static const char *const not_rows[] = {"vision.pull",  "vision.frame", "vision.state", "vision.stats",
                                           "vision.enrol", "vision.forget", "vision.open", "vision.start",
                                           "vision.save",  "gateway.scope", NULL};
    const struct gw_method *m;
    struct gw_vision_seen seen;
    struct gw_vision v;
    uint8_t fa[GW_FP_LEN], fb[GW_FP_LEN];
    cJSON *raw, *st, *a, *stats, *data;
    const cJSON *sub;
    enum gw_vision_pulled r;
    unsigned mask;
    bool changed;
    double seq;
    char *text;
    size_t i;
    int k;

    memset(fa, 0xa1, sizeof(fa));
    memset(fb, 0xb2, sizeof(fb));

    /* The scope: a name, a bit, valid alone, and not what a new pairing gets. */
    CHECK(gw_scope_parse("vision") == GW_S_VISION && strcmp(gw_scope_name(GW_S_VISION), "vision") == 0);
    CHECK(gw_scopes_valid(GW_S_VISION) && gw_scopes_valid(GW_S_VISION | GW_S_STATUS) && gw_scopes_valid(GW_S_ALL));
    CHECK((GW_S_DEFAULT & GW_S_VISION) == 0 && (GW_S_ALL & GW_S_VISION) != 0);
    CHECK(gw_scope_parse("vision_view") == 0 && gw_scope_parse("vision_control") == 0);
    a = parse("[\"vision\"]");
    CHECK(gw_scopes_from_json(a, &mask) == 0 && mask == GW_S_VISION);
    cJSON_Delete(a);
    a = gw_scopes_json(GW_S_ALL);
    CHECK(cJSON_GetArraySize(a) == 6);
    cJSON_Delete(a);
    /* The topic: held with the scope and with no other. */
    a = parse("{\"topics\":[\"vision\"]}");
    CHECK(gw_topics_parse(a, GW_S_VISION, &mask) == 0 && mask == GW_T_VISION);
    CHECK(gw_topics_parse(a, GW_S_ALL & ~GW_S_VISION, &mask) == 0 && mask == 0);
    cJSON_Delete(a);
    CHECK(gw_topics_allowed(GW_T_ALL, GW_S_VISION) == GW_T_VISION);
    CHECK((gw_topics_allowed(GW_T_ALL, GW_S_ALL & ~GW_S_VISION) & GW_T_VISION) == 0);

    /* The rows: six, of the Vision kind, by the one scope, for a paired
     * client; five reach the shell's socket and the acknowledgement reaches
     * nothing. vision.pull and the events are no method of a client. */
    for (i = 0; rows[i] != NULL; i++) {
        m = gw_method_find(rows[i]);
        CHECK(m != NULL && m->kind == GW_K_VISION && m->scope == GW_S_VISION && m->who == GW_W_PAIRED);
        CHECK(gw_policy_check(m, GW_W_PAIRED, GW_S_ALL & ~GW_S_VISION) == GW_V_FORBIDDEN);
        CHECK(gw_policy_check(m, GW_W_PAIRED, GW_S_VISION) == GW_V_OK);
        CHECK(gw_policy_check(m, GW_W_CANDIDATE, GW_S_ALL) == GW_V_UNKNOWN);
        if (m != NULL && strcmp(rows[i], "vision.ack") == 0) {
            CHECK(m->service == NULL && m->filter == NULL && m->params != NULL);
        } else if (m != NULL) {
            CHECK(m->service != NULL && strcmp(m->service, "shell") == 0 && m->filter != NULL);
        }
    }
    for (i = 0; not_rows[i] != NULL; i++) {
        CHECK(gw_method_find(not_rows[i]) == NULL);
        CHECK(gw_policy_check(gw_method_find(not_rows[i]), GW_W_PAIRED, GW_S_ALL) == GW_V_UNKNOWN);
    }
    m = gw_methods(&i);
    for (k = 0; i > 0; i--) {
        k += m[i - 1].kind == GW_K_VISION || m[i - 1].scope == GW_S_VISION || strncmp(m[i - 1].name, "vision.", 7) == 0;
    }
    CHECK(k == 6);

    /* The status: the contract's fields and no other, each checked. */
    raw = vis_raw("track", "off", "");
    st = gw_vision_status(raw, GW_VV_NONE);
    CHECK(st != NULL && vis_keys_are(st, VIS_TOP));
    CHECK(strcmp(vis_s(st, "owner"), "vision") == 0 && strcmp(vis_s(st, "mode"), "track") == 0);
    CHECK(strcmp(vis_s(st, "model"), "yolov8n 320") == 0 && strcmp(vis_s(st, "share"), "off") == 0);
    text = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(st, "modes"));
    CHECK(text != NULL && strcmp(text, "[\"detect\",\"track\",\"face\"]") == 0); /* known words, each once */
    free(text);
    text = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(st, "settings"));
    CHECK(text != NULL && strcmp(text, "{\"track.line\":\"across\",\"color.tol\":\"med\",\"edge.hard\":\"1\"}") == 0);
    free(text);
    text = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(st, "labels"));
    /* A label that cannot be shown keeps its place: a box names its class by it. */
    CHECK(text != NULL && strcmp(text, "[\"person\",\"bicycle\",\"\",\"\",\"car\"]") == 0);
    free(text);
    text = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(st, "pic"));
    CHECK(text != NULL && strcmp(text, "{\"w\":264,\"h\":469}") == 0);
    free(text);
    sub = cJSON_GetObjectItemCaseSensitive(st, "stats");
    CHECK(vis_keys_are(sub, VIS_STATS));
    CHECK(vis_n(sub, "fps_x10") == 172 && vis_n(sub, "rss_kb") == 7300 && vis_n(sub, "count_ab") == 3);
    CHECK(vis_n(sub, "cpu_pct") == 100); /* a share of one hart */
    CHECK(vis_n(sub, "count_ba") == 0);  /* not a whole number: not a count */
    text = cJSON_PrintUnformatted(st);
    CHECK(text != NULL && strstr(text, "LEAK") == NULL && strstr(text, "client") == NULL &&
          strstr(text, "embedding") == NULL && strstr(text, "helper") == NULL && strstr(text, "stride") == NULL &&
          strstr(text, "faces") == NULL && strstr(text, "name") == NULL);
    free(text);
    cJSON_Delete(st);
    cJSON_Delete(raw);

    /* Who views: live is the viewer's word alone, every other client reads
     * busy, and who it is, is passed to nobody. */
    raw = vis_raw("detect", "live", "a1a1a1a1a1a1a1a1");
    st = gw_vision_status(raw, GW_VV_SELF);
    CHECK(strcmp(vis_s(st, "share"), "live") == 0 && cJSON_GetObjectItemCaseSensitive(st, "client") == NULL);
    cJSON_Delete(st);
    st = gw_vision_status(raw, GW_VV_OTHER);
    CHECK(strcmp(vis_s(st, "share"), "busy") == 0 && cJSON_GetObjectItemCaseSensitive(st, "client") == NULL);
    cJSON_Delete(st);
    st = gw_vision_status(raw, GW_VV_NONE);
    CHECK(strcmp(vis_s(st, "share"), "off") == 0); /* a preview the gateway has no viewer for is nobody's */
    cJSON_Delete(st);
    st = gw_vision_status_row(raw);
    CHECK(strcmp(vis_s(st, "share"), "off") == 0);
    cJSON_Delete(st);
    cJSON_Delete(raw);
    raw = vis_raw("detect", "stopped", "");
    for (k = GW_VV_NONE; k <= GW_VV_OTHER; k++) {
        st = gw_vision_status(raw, (enum gw_vision_view)k);
        CHECK(strcmp(vis_s(st, "share"), "stopped") == 0);
        cJSON_Delete(st);
    }
    cJSON_Delete(raw);
    /* Words that are no words of the lists. */
    raw = parse("{\"available\":true,\"owner\":\"root\",\"mode\":\"spy\",\"share\":\"always\",\"model\":\"a\\nb\","
                "\"modes\":\"detect\",\"settings\":[1],\"labels\":{\"x\":1},\"pic\":{\"w\":4000,\"h\":-1}}");
    st = gw_vision_status(raw, GW_VV_SELF);
    CHECK(st != NULL && vis_keys_are(st, VIS_TOP));
    CHECK(strcmp(vis_s(st, "owner"), "none") == 0 && strcmp(vis_s(st, "mode"), "") == 0 &&
          strcmp(vis_s(st, "share"), "off") == 0 && strcmp(vis_s(st, "model"), "") == 0);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(st, "modes")) == 0 &&
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(st, "settings")) == 0 &&
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(st, "labels")) == 0);
    CHECK(vis_n(cJSON_GetObjectItemCaseSensitive(st, "pic"), "w") == 0 &&
          vis_n(cJSON_GetObjectItemCaseSensitive(st, "pic"), "h") == 0);
    cJSON_Delete(st);
    cJSON_Delete(raw);
    /* Not in front: that, and nothing else, whatever else the answer holds. */
    raw = parse("{\"available\":false,\"mode\":\"face\",\"share\":\"live\",\"client\":\"LEAK\"}");
    st = gw_vision_status(raw, GW_VV_SELF);
    text = cJSON_PrintUnformatted(st);
    CHECK(text != NULL && strcmp(text, "{\"available\":false}") == 0);
    free(text);
    cJSON_Delete(st);
    cJSON_Delete(raw);
    raw = parse("{\"mode\":\"detect\"}");
    st = gw_vision_status(raw, GW_VV_NONE);
    CHECK(st != NULL && cJSON_GetArraySize(st) == 1 && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(st, "available")));
    cJSON_Delete(st);
    cJSON_Delete(raw);
    raw = parse("[1]");
    CHECK(gw_vision_status(raw, GW_VV_NONE) == NULL && gw_vision_shared(raw) == NULL && gw_vision_done(raw) == NULL);
    cJSON_Delete(raw);
    CHECK(gw_vision_status(NULL, GW_VV_NONE) == NULL);
    /* More labels and modes than the bound: the first of them. */
    raw = parse("{\"available\":true}");
    a = cJSON_AddArrayToObject(raw, "labels");
    for (k = 0; k < GW_VISION_LABELS_MAX + 50; k++) {
        cJSON_AddItemToArray(a, cJSON_CreateString("x"));
    }
    st = gw_vision_status(raw, GW_VV_NONE);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(st, "labels")) == GW_VISION_LABELS_MAX);
    cJSON_Delete(st);
    cJSON_Delete(raw);

    /* The other answers. */
    raw = parse("{\"state\":\"live\",\"client\":\"LEAK\"}");
    st = gw_vision_shared(raw);
    text = cJSON_PrintUnformatted(st);
    CHECK(text != NULL && strcmp(text, "{\"state\":\"live\"}") == 0);
    free(text);
    cJSON_Delete(st);
    cJSON_Delete(raw);
    raw = parse("{\"state\":\"busy\"}"); /* not a word the unit has */
    CHECK(gw_vision_shared(raw) == NULL);
    cJSON_Delete(raw);
    raw = parse("{\"mode\":\"face\",\"note\":\"LEAK\"}");
    st = gw_vision_done(raw);
    CHECK(st != NULL && cJSON_IsObject(st) && cJSON_GetArraySize(st) == 0);
    cJSON_Delete(st);
    cJSON_Delete(raw);

    /* Parameters: checked and written again. */
    CHECK(vis_params(gw_vision_params_share, "{\"on\":true}", "{\"on\":true}"));
    CHECK(vis_params(gw_vision_params_share, "{\"on\":false}", "{\"on\":false}"));
    CHECK(vis_params(gw_vision_params_share, "{\"on\":1}", NULL));
    CHECK(vis_params(gw_vision_params_share, "{}", NULL));
    CHECK(vis_params(gw_vision_params_share, NULL, NULL));
    /* A client names neither who views nor what it is called. */
    CHECK(vis_params(gw_vision_params_share, "{\"on\":true,\"client\":\"b2b2b2b2b2b2b2b2\"}", NULL));
    CHECK(vis_params(gw_vision_params_share, "{\"on\":true,\"name\":\"x\"}", NULL));
    CHECK(vis_params(gw_vision_params_ack, "{\"seq\":7}", "{\"seq\":7}"));
    CHECK(vis_params(gw_vision_params_ack, "{\"seq\":7.5}", NULL));
    CHECK(vis_params(gw_vision_params_ack, "{\"seq\":-1}", NULL));
    CHECK(vis_params(gw_vision_params_ack, "{\"seq\":\"7\"}", NULL));
    CHECK(vis_params(gw_vision_params_ack, "{\"seq\":7,\"more\":1}", NULL));
    CHECK(vis_params(gw_vision_params_ack, "{}", NULL));
    {
        static const char *const may[] = {"detect", "track", "traffic", "segment", "instances", "color", "edge", "trace"};
        static const char *const never[] = {"face", "recognize", "read", "FACE", "Face", "face ", "enrol", "camera", ""};
        char buf[96], want[96];

        for (i = 0; i < sizeof(may) / sizeof(may[0]); i++) {
            snprintf(buf, sizeof(buf), "{\"mode\":\"%s\"}", may[i]);
            snprintf(want, sizeof(want), "{\"mode\":\"%s\"}", may[i]);
            CHECK(vis_params(gw_vision_params_mode, buf, want));
        }
        /* The modes of a face and of text cannot be chosen from a phone. */
        for (i = 0; i < sizeof(never) / sizeof(never[0]); i++) {
            snprintf(buf, sizeof(buf), "{\"mode\":\"%s\"}", never[i]);
            CHECK(vis_params(gw_vision_params_mode, buf, NULL));
        }
    }
    CHECK(vis_params(gw_vision_params_mode, "{\"mode\":\"detect\",\"force\":true}", NULL));
    CHECK(vis_params(gw_vision_params_mode, "{\"mode\":3}", NULL));
    CHECK(vis_params(gw_vision_params_mode, "{}", NULL));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"track.line\",\"value\":\"down\"}",
                     "{\"key\":\"track.line\",\"value\":\"down\"}"));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"traffic.line\",\"value\":\"off\"}",
                     "{\"key\":\"traffic.line\",\"value\":\"off\"}"));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"traffic.range\",\"value\":\"far\"}",
                     "{\"key\":\"traffic.range\",\"value\":\"far\"}"));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"traffic.speed\",\"value\":\"narrow\"}",
                     "{\"key\":\"traffic.speed\",\"value\":\"narrow\"}"));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"color.tol\",\"value\":\"high\"}",
                     "{\"key\":\"color.tol\",\"value\":\"high\"}"));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"edge.hard\",\"value\":\"1\"}",
                     "{\"key\":\"edge.hard\",\"value\":\"1\"}"));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"trace.dark\",\"value\":\"0\"}",
                     "{\"key\":\"trace.dark\",\"value\":\"0\"}"));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"seg.view\",\"value\":\"mask\"}",
                     "{\"key\":\"seg.view\",\"value\":\"mask\"}"));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"track.line\",\"value\":\"far\"}", NULL)); /* another key's value */
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"edge.hard\",\"value\":1}", NULL));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"edge.hard\",\"value\":\"2\"}", NULL));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"face.owner\",\"value\":\"x\"}", NULL));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"mode\",\"value\":\"face\"}", NULL));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"detector\",\"value\":\"yolo\"}", NULL));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"track.line\"}", NULL));
    CHECK(vis_params(gw_vision_params_set, "{\"key\":\"track.line\",\"value\":\"down\",\"x\":1}", NULL));

    /* A picture. */
    raw = vis_pull(5, 264, 469, 20000, 3);
    r = gw_vision_frame(raw, 4, &data, &seq);
    CHECK(r == GW_VF_PICTURE && seq == 5 && data != NULL && vis_keys_are(data, VIS_FRAME));
    CHECK(vis_n(data, "seq") == 5 && vis_n(data, "w") == 264 && vis_n(data, "h") == 469 && vis_n(data, "age_ms") == 41);
    CHECK(strcmp(vis_s(data, "jpeg"), vis_s(raw, "jpeg")) == 0 && strlen(vis_s(data, "jpeg")) == 26668);
    sub = cJSON_GetObjectItemCaseSensitive(data, "boxes");
    CHECK(cJSON_GetArraySize(sub) == 3 && cJSON_GetArraySize(cJSON_GetArrayItem(sub, 2)) == 9);
    CHECK(cJSON_GetArrayItem(cJSON_GetArrayItem(sub, 2), 8)->valuedouble == 28 &&
          cJSON_GetArrayItem(cJSON_GetArrayItem(sub, 2), 7)->valuedouble == -1);
    cJSON_Delete(data);
    cJSON_Delete(raw);
    /* No picture yet: {seq} alone. */
    CHECK(vis_frame_of(parse("{\"seq\":4}"), 4) == GW_VF_NONE);
    CHECK(vis_frame_of(parse("{\"seq\":0}"), 0) == GW_VF_NONE);
    /* The bounds, each at its edge. */
    CHECK(vis_frame_of(vis_pull(1, 640, 640, GW_VISION_JPEG_MAX, GW_VISION_BOXES_MAX), 0) == GW_VF_PICTURE);
    CHECK(vis_frame_of(vis_pull(1, 640, 640, GW_VISION_JPEG_MAX + 1, 0), 0) == GW_VF_BAD);
    CHECK(vis_frame_of(vis_pull(1, 640, 640, GW_VISION_JPEG_MAX + 3, 0), 0) == GW_VF_BAD);
    CHECK(vis_frame_of(vis_pull(1, 640, 640, 300000, 0), 0) == GW_VF_BAD);
    CHECK(vis_frame_of(vis_pull(1, 640, 640, 3, GW_VISION_BOXES_MAX + 1), 0) == GW_VF_BAD);
    CHECK(vis_frame_of(vis_pull(1, 641, 640, 3000, 0), 0) == GW_VF_BAD);
    CHECK(vis_frame_of(vis_pull(1, 640, 0, 3000, 0), 0) == GW_VF_BAD);
    CHECK(vis_frame_of(vis_pull(1, 640.5, 480, 3000, 0), 0) == GW_VF_BAD);
    /* Its number: whole, and newer than the last. */
    CHECK(vis_frame_of(vis_pull(4, 640, 480, 3000, 0), 4) == GW_VF_BAD);
    CHECK(vis_frame_of(vis_pull(3, 640, 480, 3000, 0), 4) == GW_VF_BAD);
    CHECK(vis_frame_of(vis_pull(5.5, 640, 480, 3000, 0), 4) == GW_VF_BAD);
    CHECK(vis_frame_of(vis_pull(-1, 640, 480, 3000, 0), -2) == GW_VF_BAD);
    CHECK(vis_frame_of(parse("{\"w\":1}"), 0) == GW_VF_BAD);
    CHECK(vis_frame_of(parse("[]"), 0) == GW_VF_BAD);
    CHECK(vis_frame_of(NULL, 0) == GW_VF_BAD);
    /* What is not base64 of a JPEG. */
    {
        static const char *const bad[] = {
            "\"\"",          "\"/9j\"",          "\"/9j/A\"",      "\"/9j/AAA\"",     "\"AAAA/9j/\"",
            "\"/9j/AA=A\"",  "\"/9j/A===\"",     "\"/9j/AA A\"",   "\"/9j/AA-_\"",    "\"/9j/AA\\nA\"",
            "\"/9j/<svg>\"", "\"data:image/jpeg;base64,/9j/AAAA\"", "7", "null", "[\"/9j/AAAA\"]", "{\"b64\":\"/9j/\"}",
        };
        static const char *const good[] = {"\"/9j/\"", "\"/9j/AAAA\"", "\"/9j/AA==\"", "\"/9j/AAA=\"", "\"/9j/+/09azAZ\""};
        char buf[200];

        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            snprintf(buf, sizeof(buf), "{\"seq\":1,\"w\":8,\"h\":8,\"age_ms\":0,\"jpeg\":%s,\"boxes\":[]}", bad[i]);
            CHECK(vis_frame_of(parse(buf), 0) == GW_VF_BAD);
        }
        for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
            snprintf(buf, sizeof(buf), "{\"seq\":1,\"w\":8,\"h\":8,\"age_ms\":0,\"jpeg\":%s,\"boxes\":[]}", good[i]);
            CHECK(vis_frame_of(parse(buf), 0) == GW_VF_PICTURE);
        }
    }
    /* Boxes: nine whole numbers each, and nothing else. */
    {
        static const char *const bad[] = {
            "[[1,2,3,4,5,6,7,8]]",        "[[1,2,3,4,5,6,7,8,9,10]]",       "[[1,2,3,4,5,6,7,8,9.5]]",
            "[[1,2,3,4,5,6,7,8,\"9\"]]",  "[[1,2,3,4,5,6,7,8,9],7]",        "[{\"id\":1}]",
            "[[1,2,3,4,5,6,7,8,1e9]]",    "{\"0\":[1,2,3,4,5,6,7,8,9]}",    "\"none\"",
            "[[1,2,3,4,5,6,7,8,[9]]]",    "[[1,2,3,4,5,6,7,8,null]]",
        };
        char buf[200];

        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            snprintf(buf, sizeof(buf), "{\"seq\":1,\"w\":8,\"h\":8,\"age_ms\":0,\"jpeg\":\"/9j/AAAA\",\"boxes\":%s}", bad[i]);
            CHECK(vis_frame_of(parse(buf), 0) == GW_VF_BAD);
        }
        CHECK(vis_frame_of(parse("{\"seq\":1,\"w\":8,\"h\":8,\"age_ms\":0,\"jpeg\":\"/9j/AAAA\"}"), 0) == GW_VF_BAD);
        CHECK(vis_frame_of(parse("{\"seq\":1,\"w\":8,\"h\":8,\"jpeg\":\"/9j/AAAA\",\"boxes\":[]}"), 0) == GW_VF_BAD);
        CHECK(vis_frame_of(parse("{\"seq\":1,\"w\":8,\"h\":8,\"age_ms\":-5,\"jpeg\":\"/9j/AAAA\",\"boxes\":[]}"), 0) ==
              GW_VF_BAD);
    }
    /* The largest picture the contract allows fits the gateway's own frame
     * to a client many times over; the unit's bound on its whole answer is
     * larger than the largest picture's text. */
    CHECK(GW_VISION_B64_MAX == (GW_VISION_JPEG_MAX + 2) / 3 * 4 && GW_VISION_B64_MAX < GW_VISION_ANSWER_MAX);
    CHECK(GW_VISION_ANSWER_MAX + 256 < GW_EVENT_SOFT_MAX && GW_EVENT_SOFT_MAX + GW_VISION_ANSWER_MAX + 256 < GW_OUTQ_MAX);

    /* The one viewer. */
    gw_vision_init(&v);
    CHECK(!gw_vision_pull_due(&v, 1000) && gw_vision_view_of(&v, 7) == GW_VV_NONE);
    CHECK(!gw_vision_ack(&v, 7, 0) && !gw_vision_ack_late(&v, 99999));
    gw_vision_viewer(&v, fa, 7, 3, 1000);
    CHECK(v.viewing && v.began == 3 && gw_vision_view_of(&v, 7) == GW_VV_SELF && gw_vision_view_of(&v, 8) == GW_VV_OTHER);
    CHECK(gw_vision_pull_due(&v, 1000)); /* the first picture needs no acknowledgement and no wait */
    gw_vision_none(&v, 1000);
    CHECK(!gw_vision_pull_due(&v, 1000 + GW_VISION_RETRY_MS - 1) && gw_vision_pull_due(&v, 1000 + GW_VISION_RETRY_MS));
    gw_vision_sent(&v, 11, 2000);
    CHECK(v.last_seq == 11);
    /* Not before the acknowledgement, however long. */
    CHECK(!gw_vision_pull_due(&v, 2000) && !gw_vision_pull_due(&v, 2000 + GW_VISION_INTERVAL_MS) &&
          !gw_vision_pull_due(&v, 2000 + 4000));
    CHECK(!gw_vision_ack(&v, 8, 11));  /* not the viewer's */
    CHECK(!gw_vision_ack(&v, 7, 10));  /* an old one */
    CHECK(!gw_vision_ack(&v, 7, 12));  /* one that was never sent */
    CHECK(!gw_vision_pull_due(&v, 2000 + 4000));
    CHECK(!gw_vision_ack_late(&v, 2000 + GW_VISION_ACK_MS - 1) && gw_vision_ack_late(&v, 2000 + GW_VISION_ACK_MS));
    CHECK(gw_vision_ack(&v, 7, 11));
    CHECK(!gw_vision_ack(&v, 7, 11)); /* once */
    CHECK(!gw_vision_ack_late(&v, 2000 + GW_VISION_ACK_MS + 60000));
    /* Acknowledged at once: still not before the interval. */
    CHECK(!gw_vision_pull_due(&v, 2000 + GW_VISION_INTERVAL_MS - 1) && gw_vision_pull_due(&v, 2000 + GW_VISION_INTERVAL_MS));
    CHECK(1000 / GW_VISION_INTERVAL_MS <= 5);
    /* A picture that was not sent (the viewer is behind): no
     * acknowledgement is owed, the interval is. */
    gw_vision_sent(&v, 12, 3000);
    CHECK(gw_vision_ack(&v, 7, 12));
    gw_vision_skipped(&v, 13, 3300);
    CHECK(v.last_seq == 13 && !gw_vision_ack_late(&v, 99999) && !gw_vision_pull_due(&v, 3300 + GW_VISION_INTERVAL_MS - 1) &&
          gw_vision_pull_due(&v, 3300 + GW_VISION_INTERVAL_MS));
    gw_vision_skipped(&v, 0, 3600); /* an answer that was dropped: the number stays */
    CHECK(v.last_seq == 13);
    gw_vision_sent(&v, 14, 4000);
    gw_vision_viewer_end(&v);
    CHECK(!v.viewing && !gw_vision_pull_due(&v, 99999) && !gw_vision_ack_late(&v, 99999) && !gw_vision_ack(&v, 7, 14) &&
          gw_vision_view_of(&v, 7) == GW_VV_NONE);
    /* The next preview starts at the beginning. */
    gw_vision_viewer(&v, fb, 9, 8, 5000);
    CHECK(v.last_seq == 0 && !v.sent && gw_vision_pull_due(&v, 5000) && memcmp(v.fp, fb, GW_FP_LEN) == 0);
    gw_vision_viewer_end(&v);

    /* How often: twenty in a minute for each client. */
    gw_vision_init(&v);
    for (k = 0; k < GW_VISION_PER_MIN; k++) {
        CHECK(gw_vision_rate(&v, fa, 10000 + k * 100));
    }
    CHECK(!gw_vision_rate(&v, fa, 10000 + 2000) && !gw_vision_rate(&v, fa, 10000 + 59999));
    CHECK(gw_vision_rate(&v, fb, 10000 + 2000)); /* another client's own */
    CHECK(gw_vision_rate(&v, fa, 10000 + 60000)); /* the oldest is a minute old */
    CHECK(!gw_vision_rate(&v, fa, 10000 + 60050));
    CHECK(gw_vision_rate(&v, fa, 10000 + 60100));
    {
        uint8_t fx[GW_FP_LEN];

        /* More clients than there are slots: the limit of one that still asks holds. */
        for (k = 0; k < GW_MAX_CLIENTS + 3; k++) {
            memset(fx, 0x10 + k, sizeof(fx));
            CHECK(gw_vision_rate(&v, fx, 200000 + k));
        }
    }

    /* What clients are told: vision.state once per change. */
    gw_vision_init(&v);
    raw = vis_raw("track", "off", "");
    gw_vision_step(&v, raw, &changed, &stats, &seen);
    CHECK(changed && stats != NULL && vis_keys_are(stats, VIS_STATS) && vis_n(stats, "fps_x10") == 172);
    CHECK(seen.available && !seen.live && !seen.biometric && seen.client[0] == '\0');
    cJSON_Delete(stats);
    gw_vision_step(&v, raw, &changed, &stats, &seen);
    CHECK(!changed && stats != NULL); /* figures every time, the state once */
    cJSON_Delete(stats);
    cJSON_Delete(raw);
    raw = vis_raw("traffic", "off", "");
    gw_vision_step(&v, raw, &changed, &stats, &seen);
    CHECK(changed);
    cJSON_Delete(stats);
    cJSON_Delete(raw);
    raw = vis_raw("traffic", "live", "a1a1a1a1a1a1a1a1");
    gw_vision_step(&v, raw, &changed, &stats, &seen);
    CHECK(changed && seen.live && strcmp(seen.client, "a1a1a1a1a1a1a1a1") == 0);
    cJSON_Delete(stats);
    cJSON_Delete(raw);
    raw = vis_raw("traffic", "live", "A1A1A1A1A1A1A1A1 LEAK"); /* not 16 hex digits: no word */
    gw_vision_step(&v, raw, &changed, &stats, &seen);
    CHECK(!changed && seen.live && seen.client[0] == '\0');
    cJSON_Delete(stats);
    cJSON_Delete(raw);
    raw = vis_raw("face", "off", "");
    gw_vision_step(&v, raw, &changed, &stats, &seen);
    CHECK(changed && seen.biometric && !seen.live);
    cJSON_Delete(stats);
    cJSON_Delete(raw);
    raw = vis_raw("recognize", "live", "");
    gw_vision_step(&v, raw, &changed, &stats, &seen);
    CHECK(seen.biometric && seen.live);
    cJSON_Delete(stats);
    cJSON_Delete(raw);
    raw = vis_raw("read", "off", "");
    gw_vision_step(&v, raw, &changed, &stats, &seen);
    CHECK(seen.biometric);
    cJSON_Delete(stats);
    cJSON_Delete(raw);
    raw = parse("{\"available\":false}");
    gw_vision_step(&v, raw, &changed, &stats, &seen);
    CHECK(changed && stats == NULL && !seen.available && !seen.live && !seen.biometric);
    gw_vision_step(&v, raw, &changed, &stats, &seen);
    CHECK(!changed && stats == NULL);
    cJSON_Delete(raw);
    CHECK(!gw_vision_lost(&v)); /* clients were told it is not there: nothing is owed */
    raw = vis_raw("detect", "off", "");
    gw_vision_step(&v, raw, &changed, &stats, &seen);
    CHECK(changed);
    cJSON_Delete(stats);
    cJSON_Delete(raw);
    CHECK(gw_vision_lost(&v) && !gw_vision_lost(&v));
    raw = vis_raw("detect", "off", "");
    gw_vision_step(&v, raw, &changed, &stats, &seen);
    CHECK(changed); /* after the link came back: said again */
    cJSON_Delete(stats);
    cJSON_Delete(raw);
    gw_vision_step(&v, NULL, &changed, &stats, &seen);
    CHECK(!changed && stats == NULL && !seen.available);
}
