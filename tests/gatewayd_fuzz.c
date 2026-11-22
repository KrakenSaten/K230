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
#include "gw_frame.h"
#include "gw_gate.h"
#include "gw_policy.h"
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
                if (m != NULL && m->kind == GW_K_UPSTREAM && params != NULL) {
                    /* The filters, on an object the fuzzer made. */
                    cJSON_Delete(gw_filter_result(m, params));
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
