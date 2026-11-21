/*
 * gatewayd: a fuzzer over everything that parses bytes a peer or a damaged
 * file can supply: the frame reader, the request parser and the policy
 * lookup behind it, the paired-list loader, and the shadow-file gate.
 * Built with the address and undefined-behaviour sanitizers; the target is
 * LLVMFuzzerTestOneInput, so a libFuzzer build can drive it, and this file
 * brings its own driver because the build host has gcc only (as
 * tests/peer_fuzz.c).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE

#include "gw.h"
#include "gw_feed.h"
#include "gw_frame.h"
#include "gw_gate.h"
#include "gw_journal.h"
#include "gw_policy.h"
#include "gw_send.h"
#include "gw_store.h"
#include "gw_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[256];

static void write_file(const char *name, const uint8_t *p, size_t n)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    f = fopen(path, "w");
    if (f != NULL) {
        fwrite(p, 1, n, f);
        fclose(f);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct gw_frame f;
    struct gw_store st;
    char path[512];
    size_t used = 0;

    /* 1. As a byte stream of frames, read as the service reads. */
    if (gw_frame_init(&f, 4096) != 0) {
        return 0;
    }
    for (;;) {
        const uint8_t *body;
        size_t n, room;
        uint8_t *space;
        int r = gw_frame_next(&f, &body, &n);

        if (r < 0) {
            break;
        }
        if (r == 1) {
            const cJSON *id, *params;
            const char *method;
            cJSON *root = NULL;

            if (gw_request_parse(body, n, &root, &id, &method, &params) == 0) {
                const struct gw_method *m = gw_method_find(method);

                (void)gw_policy_check(m, GW_W_PAIRED, (unsigned)size & GW_S_ALL);
                (void)gw_policy_check(m, GW_W_CANDIDATE, 0);
                (void)gw_params_empty(params);
                if (m != NULL && m->filter != NULL && params != NULL) {
                    /* The filters, on an object the fuzzer made. */
                    cJSON_Delete(gw_filter_result(m, params));
                }
                /* The mesh group (M4.1): the parameter checks on what a
                 * client sent, and - as if meshcored had sent it - the
                 * event filters, the feed and the journal. */
                {
                    static struct gw_journal j;
                    static struct gw_feed feed;
                    static int64_t t;
                    static bool ready;
                    uint64_t after;
                    unsigned mask;
                    bool have;
                    int off, lim;
                    cJSON *up = NULL, *d;

                    if (!ready) {
                        ready = gw_journal_init(&j, "fuzz") == 0 && gw_journal_set_run(&j, "run") == 1;
                        gw_feed_init(&feed);
                    }
                    if (m != NULL && m->params != NULL && m->params(params, &up) == 0) {
                        cJSON_Delete(up);
                    }
                    (void)gw_page_params(params, &off, &lim);
                    (void)gw_topics_parse(params, (unsigned)size & GW_S_ALL, &mask);
                    if (gw_sync_params(params, &have, &after, &lim) == 0) {
                        cJSON_Delete(gw_journal_sync(&j, have, after, lim));
                    }
                    t += 700;
                    cJSON_Delete(gw_feed_node(&feed, gw_filter_event("mesh.node", params), t));
                    cJSON_Delete(gw_filter_event("mesh.channel", params));
                    cJSON_Delete(gw_filter_event("mesh.state", params));
                    while ((d = gw_feed_node_due(&feed, t)) != NULL) {
                        cJSON_Delete(d);
                    }
                    gw_feed_activity(&feed, params, t);
                    cJSON_Delete(gw_feed_activity_due(&feed, t));
                    (void)gw_journal_put(&j, gw_filter_message(params), NULL);
                    cJSON_Delete(gw_nodes_page(params, off, lim, "run"));
                    /* Sending (M4.2): the request check, and the records
                     * through every state a noise answer can put them in. */
                    {
                        static struct gw_send gs;
                        static const uint8_t fp[GW_FP_LEN] = {7};
                        struct gw_send_client *sc = gw_send_client(&gs, fp, NULL);
                        struct gw_send_rec *sr = NULL;
                        struct gw_send_req q;
                        int k;

                        for (k = 0; sc != NULL && k < 2; k++) {
                            if (gw_send_parse(params, k == 1, &q) != 0) {
                                continue;
                            }
                            cJSON_Delete(gw_send_upstream(&q, params));
                            switch (gw_send_begin(sc, &q, (size & 1u) != 0 ? "run" : NULL, (double)(size % 7u), t, &sr)) {
                            case GW_SA_FORWARD:
                                (void)gw_send_answered(sr, (size & 2u) != 0 ? cJSON_GetObjectItemCaseSensitive(root, "answer") : NULL,
                                                       (int)(size % 11u));
                                break;
                            case GW_SA_LOOKUP:
                                if (gw_send_looked(&gs, sr, cJSON_GetObjectItemCaseSensitive(root, "list")) ==
                                    GW_SL_ABSENT) {
                                    gw_send_unresolved(sr);
                                }
                                break;
                            case GW_SA_REPLY:
                                cJSON_Delete(gw_send_result(sr, true));
                                break;
                            default:
                                break;
                            }
                        }
                    }
                }
                cJSON_Delete(root);
            }
            gw_frame_consume(&f);
            continue;
        }
        space = gw_frame_space(&f, &room);
        if (room == 0 || used == size) {
            break;
        }
        if (room > size - used) {
            room = size - used;
        }
        memcpy(space, data + used, room);
        gw_frame_added(&f, room);
        used += room;
    }
    gw_frame_free(&f);

    /* 2. As one request body. */
    {
        const cJSON *id, *params;
        const char *method;
        cJSON *root = NULL;

        if (gw_request_parse(data, size, &root, &id, &method, &params) == 0) {
            cJSON_Delete(root);
        }
    }
    /* 3. As a paired list and as a shadow file on storage. */
    write_file(GW_STORE_FILE, data, size);
    if (gw_store_load(&st, g_dir) == 0) {
        int i;

        for (i = 0; i < st.n; i++) {
            if (!gw_name_ok(st.c[i].name) || !gw_scopes_valid(st.c[i].scopes)) {
                abort(); /* the loader let something through it must not */
            }
        }
    }
    write_file("shadow", data, size);
    snprintf(path, sizeof(path), "%s/shadow", g_dir);
    (void)gw_gate_password_set(path, path); /* as the passwd file and as the shadow file */
    return 0;
}

#ifdef GATEWAYD_FUZZ_STANDALONE
static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

static const char *const SEEDS[] = {
    "{\"id\":1,\"method\":\"gateway.hello\",\"params\":{\"proto_min\":1,\"proto_max\":1,\"app\":\"a\",\"app_version\":\"1\"}}",
    "{\"id\":2,\"method\":\"system.status\",\"params\":{\"network\":[{\"name\":\"w\",\"mac\":\"m\"}],\"services\":[1,{}],\"k\":{\"api_key\":[{\"token\":1}]}}}",
    "{\"id\":3,\"method\":\"gateway.pair\",\"params\":{\"token\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"name\":\"n\"}}",
    "{\"id\":5,\"method\":\"mesh.send\",\"params\":{\"to\":\"aa00000000000000000000000000000000000000000000000000000000000001\",\"text\":\"hello\",\"client_msg_id\":\"0123456789abcdef\",\"via\":[]}}",
    "{\"id\":8,\"method\":\"mesh.send\",\"params\":{\"to\":\"aa00000000000000000000000000000000000000000000000000000000000001\",\"text\":\"hello\",\"client_msg_id\":\"0123456789abcdei\"},\"answer\":{\"accepted\":true,\"message_id\":3,\"route\":\"flood\",\"ack_expected\":true,\"ack_timeout_ms\":9},\"list\":[{\"id\":2,\"direction\":\"out\",\"kind\":\"direct\",\"peer_public_key\":\"aa00000000000000000000000000000000000000000000000000000000000001\",\"text\":\"hello\",\"state\":\"sent_flood\",\"attempts\":1},{\"id\":3,\"direction\":\"out\",\"kind\":\"channel\",\"channel\":0,\"sender_name\":\"u\",\"text\":\"u: hello\"}]}",
    "{\"id\":6,\"method\":\"mesh.send\",\"params\":{\"channel\":0,\"text\":\"hello\",\"client_msg_id\":\"0123456789abcdeg\"}}",
    "{\"id\":7,\"method\":\"mesh.resend\",\"params\":{\"message_id\":3,\"run_id\":\"run\",\"client_msg_id\":\"0123456789abcdeh\"}}",
    "{\"id\":4,\"method\":\"mesh.node\",\"params\":{\"node\":\"ab12\",\"id\":7,\"state\":\"acked\",\"text\":\"t\",\"reason\":\"path\",\"node2\":1}}",
    "{\"id\":5,\"method\":\"mesh.sync\",\"params\":{\"after_seq\":3,\"limit\":9}}",
    "{\"id\":6,\"method\":\"mesh.nodes\",\"params\":{\"offset\":1,\"limit\":2,\"nodes\":[{\"public_key\":\"aa\",\"key\":1},2]}}",
    "{\"id\":7,\"method\":\"gateway.subscribe\",\"params\":{\"topics\":[\"mesh\",\"mesh.activity\",\"x\"]}}",
    "{\"id\":8,\"method\":\"mesh.path_preview\",\"params\":{\"to\":\"aa00000000000000000000000000000000000000000000000000000000000001\",\"via\":[\"bb00000000000000000000000000000000000000000000000000000000000002\"]}}",
    "{\"id\":9,\"method\":\"x\",\"params\":{\"reason\":\"path\",\"node\":{\"public_key\":\"aa00000000000000000000000000000000000000000000000000000000000001\",\"n\":1},\"kind\":\"rx\",\"payload_type\":\"advert\",\"bytes\":40,\"id\":3}}",
    "doors-gatewayd-paired 1\nclient aa00000000000000000000000000000000000000000000000000000000000000 7 1763700000 506978656c\n",
    "root:$6$salt$hash:19000:0:99999:7:::\ndaemon:*:1::::::\n",
};

int main(int argc, char **argv)
{
    long runs = argc > 1 ? atol(argv[1]) : 100000;
    uint8_t buf[2048];
    char cmd[512];
    long i;

    snprintf(g_dir, sizeof(g_dir), "/tmp/gatewayd-fuzz-XXXXXX");
    if (mkdtemp(g_dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    for (i = 0; i < runs; i++) {
        size_t len, k, flips;
        const char *seed = SEEDS[rnd() % (sizeof(SEEDS) / sizeof(SEEDS[0]))];
        unsigned mode = rnd() % 4;

        if (mode == 0) { /* noise */
            len = rnd() % sizeof(buf);
            for (k = 0; k < len; k++) {
                buf[k] = (uint8_t)rnd();
            }
        } else { /* a genuine message, framed or not, with a few bytes changed */
            size_t sl = strlen(seed), off = 0;

            if (mode == 1) {
                buf[0] = 0;
                buf[1] = 0;
                buf[2] = (uint8_t)(sl >> 8);
                buf[3] = (uint8_t)sl;
                off = 4;
            }
            memcpy(buf + off, seed, sl);
            len = off + sl;
            flips = rnd() % 4;
            for (k = 0; k < flips; k++) {
                buf[rnd() % len] = (uint8_t)rnd();
            }
            if (mode == 3 && len > 2) {
                len = 1 + rnd() % (len - 1); /* cut short */
            }
        }
        LLVMFuzzerTestOneInput(buf, len);
    }
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) {
        return 1;
    }
    printf("gatewayd fuzz: %ld inputs, no fault\n", runs);
    return 0;
}
#endif
