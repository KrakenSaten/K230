/*
 * What Vision answers a paired companion's gateway (apps/vision/
 * vision_remote.h) and the shared picture's bounds (core/pocketvision/
 * vision_share.h, the session's `share` lines):
 *
 *  - FACE, RECOGNIZE and READ are never shareable and cannot be chosen;
 *  - a status carries the contract's fields and fits the shell's ordinary
 *    answer limit, with the detector's eighty class names in it;
 *  - the largest picture there can be, with the most boxes, fits the one
 *    larger answer the shell allows, and no larger picture is taken in;
 *  - a setting is changed only by its own words and only in its mode;
 *  - who is shown the picture is a key of 16 hex digits and a printable name.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_remote.h"

#include "pocketipc/pocketipc.h"
#include "pocketvision/vision_share.h"
#include "pocketvision/vision_share_worker.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ui/shell/app_ipc.h, which this test does not include (it is the shell's):
 * the two limits an answer is held to. tests/shell_app_methods_test.sh holds
 * the shell to the same numbers. */
#define RESULT_MAX 8192u
#define VISION_PULL_MAX 90112u

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static struct vision_session session;
static struct vision_model model;

static void go_live(uint32_t classes)
{
    struct vision_event e = { .kind = VISION_EV_READY };

    vision_model_init(&model);
    snprintf(e.text, sizeof(e.text), "fake");
    snprintf(e.name, sizeof(e.name), "det.kmodel");
    e.w = 640;
    e.h = 360;
    e.value = (int)classes;
    e.simulated = true;
    vision_model_event(&model, &e, NULL, 1000);
    e = (struct vision_event) { .kind = VISION_EV_CAPS, .value = (1 << VISION_MODES) - 1 };
    vision_model_event(&model, &e, NULL, 1010);
    vision_model_event(&model, &(struct vision_event) { .kind = VISION_EV_FRAME }, NULL, 1100);
    vision_session_init(&session);
    session.share_cap = true;
    session.streaming = true;
    session.frame_w = 528;
    session.frame_h = 938;
}

static void hold_picture(uint32_t len, int boxes)
{
    uint32_t i;

    session.share_on = true;
    for (i = 0; i < len; i++) {
        session.share_jpeg[i] = (uint8_t)(i * 31u + 7u);
    }
    session.share_jpeg[0] = 0xff;
    session.share_jpeg[1] = 0xd8;
    session.share_jpeg[len - 2] = 0xff;
    session.share_jpeg[len - 1] = 0xd9;
    session.share_len = len;
    session.share_w = 264;
    session.share_h = 469;
    session.share_seq = 1;
    session.share_at_ms = 5000;
    session.share_box_count = boxes;
    for (i = 0; i < (uint32_t)boxes; i++) {
        session.share_boxes[i] = (struct vision_shown) { .id = 4000000000u, .cls = 79, .conf = 1000, .x = 1000, .y = 1000,
                                                         .w = 1000, .h = 1000, .dir = 4, .kmh10 = 4000 };
    }
}

static const char *str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsString(v) ? v->valuestring : "";
}

static void test_private(void)
{
    go_live(80);
    check("the people and text modes are private", vision_remote_private(VISION_MODE_FACE) &&
                                                       vision_remote_private(VISION_MODE_RECOGNIZE) &&
                                                       vision_remote_private(VISION_MODE_READ));
    check("no other mode is", !vision_remote_private(VISION_MODE_DETECT) && !vision_remote_private(VISION_MODE_TRACK) &&
                                  !vision_remote_private(VISION_MODE_TRAFFIC) && !vision_remote_private(VISION_MODE_COLOR) &&
                                  !vision_remote_private(VISION_MODE_EDGE) && !vision_remote_private(VISION_MODE_TRACE) &&
                                  !vision_remote_private(VISION_MODE_SEGMENT) && !vision_remote_private(VISION_MODE_INSTANCES));
    check("a live detector's picture can be shared", vision_remote_shareable(&model, &session));
    model.mode = VISION_MODE_FACE;
    check("FACE's cannot", !vision_remote_shareable(&model, &session));
    model.mode = VISION_MODE_READ;
    check("READ's cannot", !vision_remote_shareable(&model, &session));
    model.mode = VISION_MODE_TRACK;
    session.share_cap = false;
    check("nor any from a helper that cannot make one", !vision_remote_shareable(&model, &session));
    session.share_cap = true;
    session.streaming = false;
    check("nor while nothing streams", !vision_remote_shareable(&model, &session));
}

static void test_status(void)
{
    cJSON *o;
    char *text;
    const cJSON *set;
    const cJSON *pic;

    go_live(80);
    o = vision_remote_status(&model, &session, VISION_SHARE_OFF, "0123456789abcdef");
    text = o ? cJSON_PrintUnformatted(o) : NULL;
    check("a status is made", o != NULL && text != NULL);
    if (!o || !text) {
        return;
    }
    printf("     status: %zu bytes with %d class names\n", strlen(text), cJSON_GetArraySize(cJSON_GetObjectItem(o, "labels")));
    check("it fits the shell's ordinary answer", strlen(text) <= RESULT_MAX);
    check("it says available, the owner, the mode", cJSON_IsTrue(cJSON_GetObjectItem(o, "available")) &&
                                                         strcmp(str(o, "owner"), "vision") == 0 && str(o, "mode")[0] != '\0');
    check("all eighty class names", cJSON_GetArraySize(cJSON_GetObjectItem(o, "labels")) == 80);
    check("every mode the helper offers", cJSON_GetArraySize(cJSON_GetObjectItem(o, "modes")) == VISION_MODES);
    set = cJSON_GetObjectItem(o, "settings");
    check("the eight settings and no other", cJSON_GetArraySize(set) == 8 && str(set, "track.line")[0] && str(set, "seg.view")[0] &&
                                                 str(set, "traffic.range")[0] && str(set, "trace.dark")[0]);
    pic = cJSON_GetObjectItem(o, "pic");
    check("the size the shared picture will have", cJSON_GetObjectItem(pic, "w")->valueint == 264 &&
                                                       cJSON_GetObjectItem(pic, "h")->valueint == 469);
    check("not shared: off, and no viewer named", strcmp(str(o, "share"), "off") == 0 && !cJSON_GetObjectItem(o, "client"));
    free(text);
    cJSON_Delete(o);

    o = vision_remote_status(&model, &session, VISION_SHARE_LIVE, "0123456789abcdef");
    check("shared: live, and to whom", o && strcmp(str(o, "share"), "live") == 0 && strcmp(str(o, "client"), "0123456789abcdef") == 0);
    cJSON_Delete(o);
    o = vision_remote_status(&model, &session, VISION_SHARE_STOPPED, "0123456789abcdef");
    check("stopped here: stopped, and nobody", o && strcmp(str(o, "share"), "stopped") == 0 && !cJSON_GetObjectItem(o, "client"));
    cJSON_Delete(o);

    /* A detector with more names than fit: none, never some. */
    go_live(365);
    o = vision_remote_status(&model, &session, VISION_SHARE_OFF, NULL);
    text = o ? cJSON_PrintUnformatted(o) : NULL;
    check("365 classes still fit the ordinary answer", text && strlen(text) <= RESULT_MAX);
    if (text) {
        printf("     status: %zu bytes with %d class names of 365\n", strlen(text), cJSON_GetArraySize(cJSON_GetObjectItem(o, "labels")));
    }
    check("with all of the names or none", o && (cJSON_GetArraySize(cJSON_GetObjectItem(o, "labels")) == 365 ||
                                                 cJSON_GetArraySize(cJSON_GetObjectItem(o, "labels")) == 0));
    free(text);
    cJSON_Delete(o);
}

static void test_picture(void)
{
    cJSON *o;
    char *text;
    char b64[16];

    check("base64 of nothing", vision_remote_base64((const uint8_t *)"", 0, b64) == 0 && b64[0] == '\0');
    check("base64 with two pads", vision_remote_base64((const uint8_t *)"f", 1, b64) == 4 && strcmp(b64, "Zg==") == 0);
    check("base64 with one pad", vision_remote_base64((const uint8_t *)"fo", 2, b64) == 4 && strcmp(b64, "Zm8=") == 0);
    check("base64 with none", vision_remote_base64((const uint8_t *)"foobar", 6, b64) == 8 && strcmp(b64, "Zm9vYmFy") == 0);

    go_live(80);
    check("no picture is held: none is made", vision_remote_picture(&session, 1, 6000) == NULL);
    hold_picture(12000, 3);
    o = vision_remote_picture(&session, 7, 5090);
    check("a picture is made", o != NULL);
    if (o) {
        const cJSON *boxes = cJSON_GetObjectItem(o, "boxes");

        check("numbered as the caller says, with its size and age", cJSON_GetObjectItem(o, "seq")->valueint == 7 &&
                                                                         cJSON_GetObjectItem(o, "w")->valueint == 264 &&
                                                                         cJSON_GetObjectItem(o, "h")->valueint == 469 &&
                                                                         cJSON_GetObjectItem(o, "age_ms")->valueint == 90);
        check("the JPEG as base64 of its length", strlen(str(o, "jpeg")) == 4 * ((12000 + 2) / 3));
        check("its boxes, nine numbers each", cJSON_GetArraySize(boxes) == 3 && cJSON_GetArraySize(cJSON_GetArrayItem(boxes, 0)) == 9);
        cJSON_Delete(o);
    }
    session.share_on = false;
    check("sharing off: the held picture is not handed out", vision_remote_picture(&session, 8, 5090) == NULL);

    /* The largest there can be. */
    hold_picture(VISION_SHARE_MAX_BYTES, VISION_MAX_SHOWN);
    o = vision_remote_picture(&session, 4000000000u, 5000 + 60000);
    text = o ? cJSON_PrintUnformatted(o) : NULL;
    check("the largest picture with the most boxes is made", text != NULL);
    if (text) {
        printf("     largest vision.pull answer: %zu bytes of %u allowed\n", strlen(text), VISION_PULL_MAX);
        check("and fits the one larger answer the shell allows", strlen(text) <= VISION_PULL_MAX);
        check("which the ordinary limit would have refused", strlen(text) > RESULT_MAX);
    }
    free(text);
    cJSON_Delete(o);
}

static void test_share_lines(void)
{
    struct vision_event e;

    check("sharecap is read", vision_session_parse_line(NULL, "sharecap 640 65536", &e) == 1 && e.kind == VISION_EV_SHARECAP);
    check("a sharecap beyond the bounds is not", vision_session_parse_line(NULL, "sharecap 2000 65536", &e) == 0 &&
                                                     vision_session_parse_line(NULL, "sharecap 640 100000", &e) == 0);
    check("share is read", vision_session_parse_line(NULL, "share 0 264 469 12000 14", &e) == 1 && e.kind == VISION_EV_SHARE &&
                               e.w == 264 && e.h == 469 && e.value == 12000);
    check("a picture too wide is refused", vision_session_parse_line(NULL, "share 0 641 469 12000 14", &e) == 0);
    check("a picture too tall is refused", vision_session_parse_line(NULL, "share 0 264 2000 12000 14", &e) == 0);
    check("a file over 64 KiB is refused", vision_session_parse_line(NULL, "share 0 264 469 65537 14", &e) == 0);
    check("exactly 64 KiB is taken", vision_session_parse_line(NULL, "share 0 264 469 65536 14", &e) == 1);
    check("an empty one is refused", vision_session_parse_line(NULL, "share 0 264 469 0 14", &e) == 0);
    check("a short line is refused", vision_session_parse_line(NULL, "share 0 264 469", &e) == 0);
    check("sharefail is read", vision_session_parse_line(NULL, "sharefail 0 toolarge", &e) == 1 && e.kind == VISION_EV_SHAREFAIL);

    check("the divisor halves the reference picture", vision_share_divisor(528, 938, 480) == 2);
    check("a small picture is not reduced", vision_share_divisor(320, 240, 480) == 1);
    check("the longer side decides", vision_share_divisor(1024, 100, 640) == 2 && vision_share_divisor(100, 1024, 640) == 2);
    check("no side over the limit is ever asked for", vision_share_divisor(528, 938, 641) == 0 && vision_share_divisor(528, 938, 10) == 0);
    check("nothing of nothing", vision_share_divisor(0, 938, 480) == 0);
}

static cJSON *params(const char *json)
{
    return cJSON_Parse(json);
}

static void test_mode_and_set(void)
{
    const char *msg = NULL;
    unsigned acts = 0;
    cJSON *p;

    go_live(80);
    p = params("{\"mode\":\"face\"}");
    check("FACE cannot be chosen", vision_remote_mode(&model, p, &acts, &msg) == POCKETIPC_ERR_POLICY && model.mode != VISION_MODE_FACE);
    cJSON_Delete(p);
    p = params("{\"mode\":\"recognize\"}");
    check("nor RECOGNIZE", vision_remote_mode(&model, p, &acts, &msg) == POCKETIPC_ERR_POLICY);
    cJSON_Delete(p);
    p = params("{\"mode\":\"read\"}");
    check("nor READ", vision_remote_mode(&model, p, &acts, &msg) == POCKETIPC_ERR_POLICY);
    cJSON_Delete(p);
    p = params("{\"mode\":\"everything\"}");
    check("a word that is no mode is refused", vision_remote_mode(&model, p, &acts, &msg) == POCKETIPC_ERR_INVALID_PARAMS);
    cJSON_Delete(p);
    p = params("{\"mode\":7}");
    check("so is a number", vision_remote_mode(&model, p, &acts, &msg) == POCKETIPC_ERR_INVALID_PARAMS);
    cJSON_Delete(p);
    check("and no parameters at all", vision_remote_mode(&model, NULL, &acts, &msg) == POCKETIPC_ERR_INVALID_PARAMS);
    p = params("{\"mode\":\"traffic\"}");
    check("TRAFFIC is chosen", vision_remote_mode(&model, p, &acts, &msg) == 0 && model.mode == VISION_MODE_TRAFFIC && acts != 0);
    cJSON_Delete(p);

    p = params("{\"key\":\"track.line\",\"value\":\"down\"}");
    check("TRACK's line is not set in TRAFFIC", vision_remote_set(&model, p, &acts, &msg) == POCKETIPC_ERR_POLICY);
    cJSON_Delete(p);
    p = params("{\"key\":\"traffic.line\",\"value\":\"down\"}");
    check("TRAFFIC's is", vision_remote_set(&model, p, &acts, &msg) == 0 && model.set.traffic.line == VISION_LINE_DOWN);
    cJSON_Delete(p);
    p = params("{\"key\":\"traffic.line\",\"value\":\"sideways\"}");
    check("not to a word it does not offer", vision_remote_set(&model, p, &acts, &msg) == POCKETIPC_ERR_INVALID_PARAMS &&
                                                 model.set.traffic.line == VISION_LINE_DOWN);
    cJSON_Delete(p);
    p = params("{\"key\":\"traffic.range\",\"value\":\"far\"}");
    check("the range", vision_remote_set(&model, p, &acts, &msg) == 0 && model.set.traffic.range == VISION_RANGE_FAR);
    cJSON_Delete(p);
    p = params("{\"key\":\"traffic.speed\",\"value\":\"wide\"}");
    check("the speed lines", vision_remote_set(&model, p, &acts, &msg) == 0 && model.set.traffic.speed == VISION_SPEED_WIDE);
    cJSON_Delete(p);
    p = params("{\"key\":\"traffic.distance_cm\",\"value\":\"100\"}");
    check("a setting that is not on the list is refused", vision_remote_set(&model, p, &acts, &msg) == POCKETIPC_ERR_INVALID_PARAMS);
    cJSON_Delete(p);
    p = params("{\"key\":\"detector\",\"value\":\"r0\"}");
    check("so is the detector", vision_remote_set(&model, p, &acts, &msg) == POCKETIPC_ERR_INVALID_PARAMS);
    cJSON_Delete(p);

    p = params("{\"mode\":\"color\"}");
    vision_remote_mode(&model, p, &acts, &msg);
    cJSON_Delete(p);
    p = params("{\"key\":\"color.tol\",\"value\":\"high\"}");
    check("COLOR's tolerance", vision_remote_set(&model, p, &acts, &msg) == 0 && model.set.color.tol_idx == 2);
    cJSON_Delete(p);
    p = params("{\"mode\":\"edge\"}");
    vision_remote_mode(&model, p, &acts, &msg);
    cJSON_Delete(p);
    p = params("{\"key\":\"edge.hard\",\"value\":\"1\"}");
    check("EDGE's hardness", vision_remote_set(&model, p, &acts, &msg) == 0 && model.set.edge.hard);
    cJSON_Delete(p);
    p = params("{\"key\":\"edge.hard\",\"value\":\"yes\"}");
    check("by 0 or 1 only", vision_remote_set(&model, p, &acts, &msg) == POCKETIPC_ERR_INVALID_PARAMS && model.set.edge.hard);
    cJSON_Delete(p);
    p = params("{\"mode\":\"segment\"}");
    vision_remote_mode(&model, p, &acts, &msg);
    cJSON_Delete(p);
    p = params("{\"key\":\"seg.view\",\"value\":\"mask\"}");
    check("SEGMENT's view", vision_remote_set(&model, p, &acts, &msg) == 0 && model.set.seg.view == VISION_SEG_MASK);
    cJSON_Delete(p);
}

static void test_share_params(void)
{
    char client[VISION_REMOTE_CLIENT_MAX];
    char name[VISION_REMOTE_NAME_MAX];
    const char *msg = NULL;
    bool on = false;
    cJSON *p;

    p = params("{\"on\":true,\"client\":\"0123456789abcdef\",\"name\":\"Flip\\u00e98\\n\"}");
    check("a share names its viewer", vision_remote_share_params(p, &on, client, name, &msg) == 0 && on &&
                                          strcmp(client, "0123456789abcdef") == 0);
    check("whose name is made printable", strncmp(name, "Flip", 4) == 0 && strchr(name, '\n') == NULL && strlen(name) < VISION_REMOTE_NAME_MAX);
    cJSON_Delete(p);
    p = params("{\"on\":true}");
    check("one without a viewer is refused", vision_remote_share_params(p, &on, client, name, &msg) == POCKETIPC_ERR_INVALID_PARAMS);
    cJSON_Delete(p);
    p = params("{\"on\":true,\"client\":\"0123456789ABCDEF\"}");
    check("so is a key that is not lower-case hex", vision_remote_share_params(p, &on, client, name, &msg) == POCKETIPC_ERR_INVALID_PARAMS);
    cJSON_Delete(p);
    p = params("{\"on\":true,\"client\":\"0123\"}");
    check("or of another length", vision_remote_share_params(p, &on, client, name, &msg) == POCKETIPC_ERR_INVALID_PARAMS);
    cJSON_Delete(p);
    p = params("{\"on\":\"yes\"}");
    check("or an `on` that is not a boolean", vision_remote_share_params(p, &on, client, name, &msg) == POCKETIPC_ERR_INVALID_PARAMS);
    cJSON_Delete(p);
    p = params("{\"on\":false}");
    check("ending needs nothing more", vision_remote_share_params(p, &on, client, name, &msg) == 0 && !on);
    cJSON_Delete(p);
    p = params("{\"after_seq\":41}");
    check("after_seq is read", vision_remote_after(p) == 41);
    cJSON_Delete(p);
    p = params("{\"after_seq\":-3}");
    check("a negative one is nothing", vision_remote_after(p) == 0 && vision_remote_after(NULL) == 0);
    cJSON_Delete(p);
}

static void test_encoder(void)
{
    static uint16_t px[64 * 64];
    static uint8_t out[VISION_SHARE_MAX_BYTES];
    uint32_t w = 0;
    uint32_t h = 0;
    int n;
    size_t i;

    for (i = 0; i < sizeof(px) / sizeof(px[0]); i++) {
        px[i] = (uint16_t)(i * 2654435761u >> 16);
    }
    n = vision_share_encode(px, 64, 64, 64, 2, 50, out, sizeof(out), &w, &h);
    if (!vision_share_supported()) {
        printf("     this build has no libjpeg: the encoder itself is not exercised here\n");
        check("without libjpeg nothing is encoded", n == -ENOTSUP);
        return;
    }
    check("a picture is encoded, reduced", n > 4 && w == 32 && h == 32 && out[0] == 0xff && out[1] == 0xd8 &&
                                              out[n - 2] == 0xff && out[n - 1] == 0xd9);
    check("one that does not fit is refused, not grown into", vision_share_encode(px, 64, 64, 64, 1, 85, out, 64, &w, &h) == -ENOSPC);
    check("sizes that are none are refused", vision_share_encode(px, 64, 64, 64, 0, 50, out, sizeof(out), &w, &h) == -EINVAL &&
                                                 vision_share_encode(px, 64, 64, 32, 1, 50, out, sizeof(out), &w, &h) == -EINVAL);
}

/* The encoder thread: one picture at a time, by copy, the same bytes the
 * encoder gives directly, and nothing left behind when it is freed with a
 * picture in the making. */
static void test_worker(void)
{
    static uint16_t px[128 * 96];
    static uint8_t direct[VISION_SHARE_MAX_BYTES];
    static uint8_t got[VISION_SHARE_MAX_BYTES];
    struct vision_share_worker *w = vision_share_worker_new();
    uint32_t ow = 0;
    uint32_t oh = 0;
    uint32_t ms = 0;
    uint32_t tag = 0;
    uint32_t dw = 0;
    uint32_t dh = 0;
    int n = 0;
    int dn;
    int tries;
    int taken = 0;
    size_t i;

    check("the worker starts", w != NULL);
    if (!w) {
        return;
    }
    for (i = 0; i < sizeof(px) / sizeof(px[0]); i++) {
        px[i] = (uint16_t)((i % 128) * 500u + (i / 128) * 37u);
    }
    check("nothing to take before anything was given", vision_share_worker_take(w, got, sizeof(got), &n, &ow, &oh, &ms, &tag) == 0 &&
                                                           !vision_share_worker_busy(w));
    check("a picture without a size is refused", vision_share_worker_submit(w, px, 0, 96, 128, 2, 50, 25, 1) == -EINVAL &&
                                                     vision_share_worker_submit(w, px, 128, 96, 64, 2, 50, 25, 1) == -EINVAL);
    check("a picture is taken in", vision_share_worker_submit(w, px, 128, 96, 128, 2, 50, 25, 77) == 0 && vision_share_worker_busy(w));
    check("a second one meanwhile is refused, not queued", vision_share_worker_submit(w, px, 128, 96, 128, 2, 50, 25, 78) == -EBUSY);
    /* The caller's picture may change at once: the worker has its own copy. */
    memset(px, 0, sizeof(px));
    for (tries = 0; tries < 400 && !taken; tries++) {
        struct timespec ts = { 0, 5 * 1000 * 1000 };

        taken = vision_share_worker_take(w, got, sizeof(got), &n, &ow, &oh, &ms, &tag);
        if (!taken) {
            nanosleep(&ts, NULL);
        }
    }
    check("its result comes, once, with its tag", taken == 1 && tag == 77 &&
                                                      vision_share_worker_take(w, got, sizeof(got), &n, &ow, &oh, &ms, &tag) == 0);
    for (i = 0; i < sizeof(px) / sizeof(px[0]); i++) {
        px[i] = (uint16_t)((i % 128) * 500u + (i / 128) * 37u);
    }
    dn = vision_share_encode(px, 128, 96, 128, 2, 50, direct, sizeof(direct), &dw, &dh);
    if (!vision_share_supported()) {
        check("without libjpeg the worker says so too", n == -ENOTSUP && dn == -ENOTSUP);
    } else {
        check("it is the picture that was given, not what the caller wrote after", n == dn && n > 4 && ow == dw && oh == dh &&
                                                                                       memcmp(got, direct, (size_t)n) == 0);
        check("a result that does not fit where it is taken is refused", vision_share_worker_submit(w, px, 128, 96, 128, 2, 50, 25, 79) == 0);
        for (tries = 0, taken = 0; tries < 400 && !taken; tries++) {
            struct timespec ts = { 0, 5 * 1000 * 1000 };

            taken = vision_share_worker_take(w, got, 16, &n, &ow, &oh, &ms, &tag);
            if (!taken) {
                nanosleep(&ts, NULL);
            }
        }
        check("as too large, with nothing written past the room given", taken == 1 && n == -ENOSPC);
    }
    check("the worker is free again", !vision_share_worker_busy(w));
    /* Freed with a picture in the making: it ends cleanly (the sanitizer run says whether it did). */
    vision_share_worker_submit(w, px, 128, 96, 128, 1, 50, 25, 80);
    vision_share_worker_free(w);
    vision_share_worker_free(NULL);
    check("freed, with one in the making", 1);
}

/* The reduction: a flat picture stays its colour through every divisor. */
static void test_reduction(void)
{
    static uint16_t px[96 * 96];
    static uint8_t out[VISION_SHARE_MAX_BYTES];
    uint32_t w = 0;
    uint32_t h = 0;
    size_t i;
    uint32_t div;
    int ok = 1;

    if (!vision_share_supported()) {
        return;
    }
    for (i = 0; i < sizeof(px) / sizeof(px[0]); i++) {
        px[i] = 0xffff;
    }
    for (div = 1; div <= 4; div++) {
        int n = vision_share_encode(px, 96, 96, 96, div, 85, out, sizeof(out), &w, &h);

        if (n <= 4 || w != 96 / div || h != 96 / div) {
            ok = 0;
        }
    }
    check("every divisor gives a picture of its size", ok);
}

int main(void)
{
    test_private();
    test_status();
    test_picture();
    test_share_lines();
    test_mode_and_set();
    test_share_params();
    test_encoder();
    test_worker();
    test_reduction();
    printf("vision_remote_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
