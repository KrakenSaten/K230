/*
 * gatewayd: unit tests of the pure modules and of the identity handling
 * (services/gatewayd; docs/decisions/ADR-019). The pairing state machine is
 * walked with a clock of the test's own; the paired list and the identity
 * file are read back from a temporary directory, damaged, and read again.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE

#include "gw.h"
#include "gw_frame.h"
#include "gw_gate.h"
#include "gw_pair.h"
#include "gw_policy.h"
#include "gw_store.h"
#include "gw_tls.h"
#include "gw_util.h"

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failed++;                                                            \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                        \
    } while (0)

static char tmpdir[256];

static void put_file(const char *name, const char *text, mode_t mode)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
    unlink(path);
    f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
    chmod(path, mode);
}

static mode_t mode_of(const char *name)
{
    char path[512];
    struct stat sb;

    snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
    return stat(path, &sb) == 0 ? (sb.st_mode & 0777) : 0;
}

/* ---- gw_util ----------------------------------------------------------- */

static void test_util(void)
{
    uint8_t b[32], o[32];
    char text[128];
    struct gw_bucket bk;
    size_t i;
    int n;

    for (i = 0; i < sizeof(b); i++) {
        b[i] = (uint8_t)(i * 7 + 3);
    }
    gw_hex(b, 4, text);
    CHECK(strcmp(text, "030a1118") == 0);
    CHECK(gw_unhex("030A1118", o, 4) == 0 && memcmp(o, b, 4) == 0);
    CHECK(gw_unhex("030a111", o, 4) != 0);
    CHECK(gw_unhex("030a111g", o, 4) != 0);
    CHECK(gw_unhex("030a111800", o, 4) != 0);

    /* base64url: every length up to 32 round-trips; padding, the standard
     * alphabet and a second encoding of the same bytes are refused. */
    for (i = 1; i <= 32; i++) {
        CHECK(gw_b64url(b, i, text, sizeof(text)) == (i * 4 + 2) / 3);
        CHECK(gw_unb64url(text, o, i) == 0 && memcmp(o, b, i) == 0);
    }
    CHECK(gw_b64url(b, 16, text, 22) == 0);         /* no room for the NUL */
    CHECK(gw_unb64url("AAAAAAAAAAAAAAAAAAAAAA", o, 16) == 0);
    CHECK(gw_unb64url("AAAAAAAAAAAAAAAAAAAAAB", o, 16) != 0); /* stray low bits */
    CHECK(gw_unb64url("AAAAAAAAAAAAAAAAAAAAA=", o, 16) != 0);
    CHECK(gw_unb64url("AAAAAAAAAAAAAAAAAAAA+/", o, 16) != 0);
    CHECK(gw_unb64url("AAAAAAAAAAAAAAAAAAAAA", o, 16) != 0);
    CHECK(gw_unb64url("AAAAAAAAAAAAAAAAAAAAAAA", o, 16) != 0);
    CHECK(gw_unb64url(NULL, o, 16) != 0);

    CHECK(gw_utf8_ok((const uint8_t *)"plain", 5));
    CHECK(gw_utf8_ok((const uint8_t *)"\xc3\xa5\xe2\x82\xac\xf0\x9f\x98\x80", 9));
    CHECK(!gw_utf8_ok((const uint8_t *)"\xc0\xaf", 2));         /* overlong */
    CHECK(!gw_utf8_ok((const uint8_t *)"\xe0\x80\xaf", 3));     /* overlong */
    CHECK(!gw_utf8_ok((const uint8_t *)"\xed\xa0\x80", 3));     /* surrogate */
    CHECK(!gw_utf8_ok((const uint8_t *)"\xf4\x90\x80\x80", 4)); /* above U+10FFFF */
    CHECK(!gw_utf8_ok((const uint8_t *)"\xe2\x82", 2));         /* cut short */
    CHECK(!gw_utf8_ok((const uint8_t *)"\x80", 1));
    CHECK(!gw_utf8_ok((const uint8_t *)"\xff", 1));

    CHECK(gw_name_ok("Pixel 8"));
    CHECK(gw_name_ok("Andr\xc3\xa9s telefon"));
    CHECK(!gw_name_ok(""));
    CHECK(!gw_name_ok(NULL));
    CHECK(!gw_name_ok("line\nbreak"));
    CHECK(!gw_name_ok("esc\x1b[31m"));
    CHECK(!gw_name_ok("c1\xc2\x85"));
    CHECK(!gw_name_ok("123456789012345678901234567890123")); /* 33 bytes */
    CHECK(gw_name_ok("12345678901234567890123456789012"));   /* 32 bytes */

    CHECK(gw_ct_eq("abcd", "abcd", 4) == 1);
    CHECK(gw_ct_eq("abcd", "abce", 4) == 0);
    CHECK(gw_ct_eq("abcd", "bbcd", 4) == 0);

    /* 3 at once, then 2 a second. */
    gw_bucket_init(&bk, 3, 2, 1000);
    n = 0;
    for (i = 0; i < 10; i++) {
        n += gw_bucket_take(&bk, 1000) ? 1 : 0;
    }
    CHECK(n == 3);
    CHECK(!gw_bucket_take(&bk, 1400));
    CHECK(gw_bucket_take(&bk, 1500));
    CHECK(!gw_bucket_take(&bk, 1500));
    n = 0;
    for (i = 0; i < 10; i++) {
        n += gw_bucket_take(&bk, 100000) ? 1 : 0; /* never more than the burst */
    }
    CHECK(n == 3);
    CHECK(!gw_bucket_take(&bk, 90000)); /* a clock that went back gives nothing */
}

/* ---- gw_frame ---------------------------------------------------------- */

/* Feeds bytes the way the service does: whatever space is offered. */
static size_t feed(struct gw_frame *f, const uint8_t *p, size_t n)
{
    size_t room, used = 0;

    while (used < n) {
        uint8_t *space = gw_frame_space(f, &room);

        if (room == 0) {
            break;
        }
        if (room > n - used) {
            room = n - used;
        }
        memcpy(space, p + used, room);
        gw_frame_added(f, room);
        used += room;
    }
    return used;
}

static void test_frame(void)
{
    struct gw_frame f;
    const uint8_t two[] = {0, 0, 0, 3, 'a', 'b', 'c', 0, 0, 0, 2, 'x', 'y'};
    const uint8_t zero[] = {0, 0, 0, 0, 'z'};
    const uint8_t big[] = {0, 0, 0, 17, 'z'};
    const uint8_t huge[] = {0xff, 0xff, 0xff, 0xff};
    const uint8_t *p;
    size_t n, used, i;

    CHECK(gw_frame_init(&f, 16) == 0);
    /* Two frames in one segment: one at a time, the second untouched. */
    used = feed(&f, two, sizeof(two));
    CHECK(used == 7);
    CHECK(gw_frame_next(&f, &p, &n) == 1 && n == 3 && memcmp(p, "abc", 3) == 0);
    gw_frame_consume(&f);
    CHECK(gw_frame_next(&f, &p, &n) == 0);
    used += feed(&f, two + used, sizeof(two) - used);
    CHECK(used == sizeof(two));
    CHECK(gw_frame_next(&f, &p, &n) == 1 && n == 2 && memcmp(p, "xy", 2) == 0);
    gw_frame_consume(&f);

    /* One byte at a time. */
    for (i = 0; i < 7; i++) {
        CHECK(gw_frame_next(&f, &p, &n) == 0);
        CHECK(feed(&f, two + i, 1) == 1);
    }
    CHECK(gw_frame_next(&f, &p, &n) == 1 && n == 3);
    gw_frame_consume(&f);

    /* A zero length, one over the limit, and the largest announcement: each
     * is refused at the header, and no byte of a body is taken. */
    CHECK(feed(&f, zero, sizeof(zero)) == 4 && gw_frame_next(&f, &p, &n) == -1);
    gw_frame_consume(&f);
    CHECK(feed(&f, big, sizeof(big)) == 4 && gw_frame_next(&f, &p, &n) == -1);
    gw_frame_consume(&f);
    CHECK(feed(&f, huge, sizeof(huge)) == 4 && gw_frame_next(&f, &p, &n) == -1);
    (void)gw_frame_space(&f, &n);
    CHECK(n == 0);
    gw_frame_free(&f);
}

/* ---- gw_pair ----------------------------------------------------------- */

static const uint8_t TOKEN[GW_TOKEN_LEN] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint8_t WRONG[GW_TOKEN_LEN] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17};
static const uint8_t FP_A[GW_FP_LEN] = {0xaa, 1};
static const uint8_t FP_B[GW_FP_LEN] = {0xbb, 2};

static void test_pair(void)
{
    struct gw_pair p;
    uint64_t sid = 0;
    int i;

    gw_pair_init(&p);
    /* Nothing is admitted and nothing is decided without a window. */
    CHECK(!gw_pair_admits(&p, 0));
    CHECK(gw_pair_token(&p, 5, TOKEN, GW_TOKEN_LEN, "x", 0) == GW_PAIR_TOKEN_REFUSED);
    CHECK(gw_pair_decide(&p, true, 0, &sid) == -1);
    gw_pair_candidate(&p, 5, FP_A, 0); /* ignored: no window */
    CHECK(p.state == GW_PAIR_IDLE);

    /* The whole path. */
    CHECK(gw_pair_begin(&p, TOKEN, 1000) == 0);
    CHECK(gw_pair_begin(&p, TOKEN, 1000) == -1);
    CHECK(gw_pair_admits(&p, 1000));
    gw_pair_candidate(&p, 5, FP_A, 2000);
    CHECK(p.state == GW_PAIR_CANDIDATE);
    CHECK(!gw_pair_admits(&p, 2000));                 /* one at the door */
    CHECK(gw_pair_token(&p, 6, TOKEN, GW_TOKEN_LEN, "x", 2100) == GW_PAIR_TOKEN_REFUSED); /* not it */
    CHECK(gw_pair_decide(&p, true, 2100, &sid) == -1); /* no token yet: nothing to decide */
    CHECK(gw_pair_token(&p, 5, TOKEN, GW_TOKEN_LEN, "Pixel 8", 2200) == GW_PAIR_TOKEN_OK);
    CHECK(p.state == GW_PAIR_WAITING);
    /* The token is spent the moment it is accepted. */
    CHECK(gw_pair_token(&p, 5, TOKEN, GW_TOKEN_LEN, "Pixel 8", 2300) == GW_PAIR_TOKEN_REFUSED);
    CHECK(gw_pair_decide(&p, true, 3000, &sid) == 1 && sid == 5);
    CHECK(memcmp(p.fp, FP_A, GW_FP_LEN) == 0 && strcmp(p.name, "Pixel 8") == 0);
    CHECK(gw_pair_cancel(&p) == 5);
    CHECK(p.state == GW_PAIR_IDLE && !gw_pair_open(&p));
    /* A window that closed takes its token with it. */
    CHECK(gw_pair_begin(&p, WRONG, 4000) == 0);
    gw_pair_candidate(&p, 7, FP_B, 4000);
    CHECK(gw_pair_token(&p, 7, TOKEN, GW_TOKEN_LEN, "x", 4100) == GW_PAIR_TOKEN_BAD);
    (void)gw_pair_cancel(&p);

    /* Refusal. */
    CHECK(gw_pair_begin(&p, TOKEN, 0) == 0);
    gw_pair_candidate(&p, 8, FP_A, 0);
    CHECK(gw_pair_token(&p, 8, TOKEN, GW_TOKEN_LEN, "x", 1) == GW_PAIR_TOKEN_OK);
    CHECK(gw_pair_decide(&p, false, 2, &sid) == 0 && sid == 8);
    CHECK(p.state == GW_PAIR_IDLE);

    /* Three wrong tokens close the window; the third candidate finds none. */
    CHECK(gw_pair_begin(&p, TOKEN, 0) == 0);
    for (i = 0; i < GW_PAIR_MAX_BAD; i++) {
        CHECK(gw_pair_admits(&p, 10));
        gw_pair_candidate(&p, 20 + (uint64_t)i, FP_B, 10);
        CHECK(gw_pair_token(&p, 20 + (uint64_t)i, WRONG, GW_TOKEN_LEN, "x", 11) == GW_PAIR_TOKEN_BAD);
    }
    CHECK(p.state == GW_PAIR_IDLE);
    CHECK(!gw_pair_admits(&p, 12));

    /* Wrong in other ways: short, long, missing, and a right token with a
     * name that is not one. Each is a failure. */
    CHECK(gw_pair_begin(&p, TOKEN, 0) == 0);
    gw_pair_candidate(&p, 30, FP_B, 0);
    CHECK(gw_pair_token(&p, 30, TOKEN, GW_TOKEN_LEN - 1, "x", 1) == GW_PAIR_TOKEN_BAD);
    gw_pair_candidate(&p, 31, FP_B, 1);
    CHECK(gw_pair_token(&p, 31, NULL, 0, "x", 2) == GW_PAIR_TOKEN_BAD);
    gw_pair_candidate(&p, 32, FP_B, 2);
    CHECK(gw_pair_token(&p, 32, TOKEN, GW_TOKEN_LEN, "bad\nname", 3) == GW_PAIR_TOKEN_BAD);
    CHECK(p.state == GW_PAIR_IDLE);

    /* A candidate that shows no token in time is dropped, and frees the
     * door without counting: it presented nothing. */
    CHECK(gw_pair_begin(&p, TOKEN, 0) == 0);
    gw_pair_candidate(&p, 40, FP_B, 100);
    CHECK(gw_pair_tick(&p, 100 + GW_PAIR_TOKEN_MS - 1) == 0);
    CHECK(gw_pair_tick(&p, 100 + GW_PAIR_TOKEN_MS) == 40);
    CHECK(p.state == GW_PAIR_OPEN && p.bad == 0);
    /* One that leaves is not counted either: three connections from a
     * stranger must not close the owner's window. */
    gw_pair_candidate(&p, 41, FP_B, 40000);
    gw_pair_gone(&p, 99); /* somebody else's connection: nothing */
    CHECK(p.state == GW_PAIR_CANDIDATE);
    gw_pair_gone(&p, 41);
    CHECK(p.state == GW_PAIR_OPEN && p.bad == 0);
    {
        int k;

        for (k = 0; k < 10; k++) {
            gw_pair_candidate(&p, 400 + (uint64_t)k, FP_B, 41000);
            gw_pair_gone(&p, 400 + (uint64_t)k);
        }
        CHECK(p.state == GW_PAIR_OPEN && gw_pair_admits(&p, 41000));
    }

    /* The window ends at 120 s whatever is going on, and takes whoever is
     * at the door with it. */
    gw_pair_candidate(&p, 42, FP_A, 50000);
    CHECK(gw_pair_token(&p, 42, TOKEN, GW_TOKEN_LEN, "x", 50001) == GW_PAIR_TOKEN_OK);
    CHECK(gw_pair_tick(&p, GW_PAIR_WINDOW_MS - 1) == 0);
    CHECK(gw_pair_decide(&p, true, GW_PAIR_WINDOW_MS, &sid) == -1); /* too late */
    CHECK(gw_pair_tick(&p, GW_PAIR_WINDOW_MS) == 42);
    CHECK(p.state == GW_PAIR_IDLE);
    CHECK(!gw_pair_admits(&p, GW_PAIR_WINDOW_MS));

    /* An expired window admits nobody even before a tick notices. */
    CHECK(gw_pair_begin(&p, TOKEN, 0) == 0);
    CHECK(!gw_pair_admits(&p, GW_PAIR_WINDOW_MS));
    gw_pair_candidate(&p, 50, FP_A, GW_PAIR_WINDOW_MS);
    CHECK(p.state == GW_PAIR_OPEN);
    (void)gw_pair_cancel(&p);

    /* A candidate that showed the token and then left: the window ends. A
     * decision made afterwards decides nothing. */
    CHECK(gw_pair_begin(&p, TOKEN, 0) == 0);
    gw_pair_candidate(&p, 60, FP_A, 0);
    CHECK(gw_pair_token(&p, 60, TOKEN, GW_TOKEN_LEN, "x", 1) == GW_PAIR_TOKEN_OK);
    gw_pair_gone(&p, 60);
    CHECK(p.state == GW_PAIR_IDLE);
    CHECK(gw_pair_decide(&p, true, 2, &sid) == -1);
}

/* ---- gw_policy --------------------------------------------------------- */

static cJSON *parse(const char *text)
{
    cJSON *v = cJSON_Parse(text);

    CHECK(v != NULL);
    return v;
}

static int request(const char *text, size_t n)
{
    const cJSON *id, *params;
    const char *method;
    cJSON *root = NULL;
    int rc = gw_request_parse((const uint8_t *)text, n, &root, &id, &method, &params);

    cJSON_Delete(root);
    return rc;
}
#define REQ(text) request(text, strlen(text))

static void test_policy(void)
{
    static const char *const never[] = {
        "system.reboot", "system.poweroff", "storage.expand", "storage.eject", "radio_setup.start",
        "wifi.connect", "wifi.forget", "wifi.set_enabled", "wifi.disconnect", "wifi.scan",
        "radio.configure", "radio.send", "radio.send_async", "radio.set_enabled", "radio.acquire",
        "mesh.set_name", "mesh.set_path_hash", "mesh.channel_add", "mesh.channel_remove",
        "mesh.node_remove", "mesh.node_reset_path", "mesh.advert", "mesh.discover", "mesh.remote_login",
        "mesh.remote_cli", "mesh.app_send", "shell.key", "shell.open", "shell.action", "shell.screenshot",
        "gateway.pair_begin", "gateway.pair_decide", "gateway.revoke", "gateway.set_scopes",
        "gateway.clients", "System.Status", "system.status ", "", NULL};
    const struct gw_method *tab, *m;
    size_t count, i, k;
    unsigned mask;
    cJSON *a, *r, *f;

    /* The table: every upstream row needs exactly one scope, names a
     * service and has a filter; no row is reachable by a candidate except
     * the two a candidate needs; no name appears twice. */
    tab = gw_methods(&count);
    CHECK(count > 0);
    for (i = 0; i < count; i++) {
        m = &tab[i];
        CHECK(gw_method_find(m->name) == m);
        if (m->kind == GW_K_UPSTREAM) {
            CHECK(m->service != NULL && m->filter != NULL);
            CHECK(m->scope != 0 && gw_scope_name(m->scope) != NULL);
            CHECK(m->who == GW_W_PAIRED);
        }
        if ((m->who & GW_W_CANDIDATE) != 0) {
            CHECK(m->kind == GW_K_HELLO || m->kind == GW_K_PAIR);
        }
        CHECK(strncmp(m->name, "shell.", 6) != 0);
        for (k = 0; k < i; k++) {
            CHECK(strcmp(tab[k].name, m->name) != 0);
        }
        /* Authorization, method by method: without the row's scope it is
         * forbidden, with exactly that scope it is allowed, and every other
         * scope together does not make up for it. */
        if (m->scope != 0) {
            CHECK(gw_policy_check(m, GW_W_PAIRED, 0) == GW_V_FORBIDDEN);
            CHECK(gw_policy_check(m, GW_W_PAIRED, m->scope) == GW_V_OK);
            CHECK(gw_policy_check(m, GW_W_PAIRED, GW_S_ALL & ~m->scope) == GW_V_FORBIDDEN);
            CHECK(gw_policy_check(m, GW_W_CANDIDATE, GW_S_ALL) == GW_V_UNKNOWN);
        }
    }
    /* What ADR-019 rule 25 says no client can ever call does not exist. */
    for (i = 0; never[i] != NULL; i++) {
        CHECK(gw_method_find(never[i]) == NULL);
        CHECK(gw_policy_check(gw_method_find(never[i]), GW_W_PAIRED, GW_S_ALL) == GW_V_UNKNOWN);
    }
    CHECK(gw_method_find(NULL) == NULL);
    /* A candidate cannot call a paired client's method, and the answer does
     * not say the method exists. */
    CHECK(gw_policy_check(gw_method_find("gateway.ping"), GW_W_CANDIDATE, GW_S_ALL) == GW_V_UNKNOWN);
    CHECK(gw_policy_check(gw_method_find("gateway.pair"), GW_W_PAIRED, GW_S_ALL) == GW_V_UNKNOWN);
    CHECK(gw_policy_check(gw_method_find("gateway.pair"), GW_W_CANDIDATE, 0) == GW_V_OK);
    CHECK(gw_policy_check(gw_method_find("gateway.hello"), GW_W_CANDIDATE, 0) == GW_V_OK);

    /* Scopes. */
    CHECK(gw_scope_parse("status") == GW_S_STATUS);
    CHECK(gw_scope_parse("capture_control") == GW_S_CAPTURE_CONTROL);
    CHECK(gw_scope_parse("admin") == 0 && gw_scope_parse("") == 0 && gw_scope_parse(NULL) == 0);
    CHECK(gw_scopes_valid(0) && gw_scopes_valid(GW_S_ALL) && gw_scopes_valid(GW_S_DEFAULT));
    CHECK(!gw_scopes_valid(GW_S_MESH_SEND));
    CHECK(!gw_scopes_valid(GW_S_CAPTURE_CONTROL | GW_S_STATUS));
    CHECK(!gw_scopes_valid(GW_S_ALL + 1));
    a = parse("[\"status\",\"mesh_read\",\"mesh_send\"]");
    CHECK(gw_scopes_from_json(a, &mask) == 0 && mask == GW_S_DEFAULT);
    cJSON_Delete(a);
    a = parse("[]");
    CHECK(gw_scopes_from_json(a, &mask) == 0 && mask == 0);
    cJSON_Delete(a);
    a = parse("[\"status\",\"status\"]");
    CHECK(gw_scopes_from_json(a, &mask) != 0);
    cJSON_Delete(a);
    a = parse("[\"status\",\"root\"]");
    CHECK(gw_scopes_from_json(a, &mask) != 0);
    cJSON_Delete(a);
    a = parse("[\"mesh_send\"]");
    CHECK(gw_scopes_from_json(a, &mask) != 0);
    cJSON_Delete(a);
    a = parse("[1]");
    CHECK(gw_scopes_from_json(a, &mask) != 0);
    cJSON_Delete(a);
    a = parse("{\"status\":true}");
    CHECK(gw_scopes_from_json(a, &mask) != 0);
    cJSON_Delete(a);
    a = gw_scopes_json(GW_S_STATUS | GW_S_CAPTURE_VIEW);
    CHECK(cJSON_GetArraySize(a) == 2);
    cJSON_Delete(a);

    /* Requests. */
    CHECK(REQ("{\"id\":1,\"method\":\"gateway.ping\"}") == 0);
    CHECK(REQ("{\"id\":-7,\"method\":\"x\",\"params\":{}}") == 0);
    CHECK(REQ("{\"id\":1,\"method\":\"x\",\"params\":[]}") != 0);
    CHECK(REQ("{\"id\":1,\"method\":\"x\",\"params\":null}") != 0);
    CHECK(REQ("{\"id\":\"1\",\"method\":\"x\"}") != 0);
    CHECK(REQ("{\"id\":1.5,\"method\":\"x\"}") != 0);
    CHECK(REQ("{\"id\":1e300,\"method\":\"x\"}") != 0);
    CHECK(REQ("{\"method\":\"x\"}") != 0);
    CHECK(REQ("{\"id\":1}") != 0);
    CHECK(REQ("{\"id\":1,\"method\":\"\"}") != 0);
    CHECK(REQ("{\"id\":1,\"method\":7}") != 0);
    CHECK(REQ("[{\"id\":1,\"method\":\"x\"}]") != 0);
    CHECK(REQ("{\"id\":1,\"method\":\"x\"") != 0);
    CHECK(REQ("") != 0);
    CHECK(REQ("{\"id\":1,\"method\":\"\xff\"}") != 0);
    {
        static const char nul[] = "{\"id\":1,\"method\":\"x\"}\0junk";

        CHECK(request(nul, sizeof(nul) - 1) != 0); /* a NUL inside the frame */
    }
    CHECK(REQ("{\"id\":1,\"method\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}") != 0);
    a = parse("{}");
    CHECK(gw_params_empty(a) && gw_params_empty(NULL));
    cJSON_Delete(a);
    a = parse("{\"x\":1}");
    CHECK(!gw_params_empty(a));
    cJSON_Delete(a);

    /* Filters: what the row names passes, the rest does not. */
    r = parse("{\"uptime_s\":5,\"secret_thing\":1,\"extra\":2,"
              "\"network\":[{\"name\":\"wlan0\",\"mac\":\"02:00:00:00:00:01\",\"ipv4\":\"10.0.0.2\",\"rx_bytes\":9},7],"
              "\"services\":[{\"name\":\"sysd\",\"pid\":123,\"running\":true}],"
              "\"power\":{\"status\":\"charging\",\"auth_token\":\"x\",\"nested\":{\"api_key\":\"y\",\"ok\":1}}}");
    f = gw_filter_result(gw_method_find("system.status"), r);
    CHECK(f != NULL);
    CHECK(cJSON_GetObjectItem(f, "uptime_s") != NULL);
    CHECK(cJSON_GetObjectItem(f, "extra") == NULL && cJSON_GetObjectItem(f, "secret_thing") == NULL);
    a = cJSON_GetObjectItem(f, "network");
    CHECK(cJSON_GetArraySize(a) == 1);
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(a, 0), "mac") == NULL);
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(a, 0), "rx_bytes") == NULL);
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(a, 0), "ipv4") != NULL);
    a = cJSON_GetObjectItem(f, "services");
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(a, 0), "pid") == NULL);
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(a, 0), "running") != NULL);
    a = cJSON_GetObjectItem(f, "power");
    CHECK(cJSON_GetObjectItem(a, "status") != NULL && cJSON_GetObjectItem(a, "auth_token") == NULL);
    CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(a, "nested"), "api_key") == NULL);
    CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(a, "nested"), "ok") != NULL);
    cJSON_Delete(f);
    cJSON_Delete(r);

    r = parse("{\"state\":\"connected\",\"ssid\":\"home\",\"bssid\":\"aa:bb\",\"passphrase\":\"hunter2\",\"psk\":\"x\"}");
    f = gw_filter_result(gw_method_find("wifi.status"), r);
    CHECK(f != NULL && cJSON_GetObjectItem(f, "ssid") != NULL);
    CHECK(cJSON_GetObjectItem(f, "bssid") == NULL && cJSON_GetObjectItem(f, "passphrase") == NULL &&
          cJSON_GetObjectItem(f, "psk") == NULL);
    cJSON_Delete(f);
    cJSON_Delete(r);

    /* A row that passes the whole result still loses anything named for a
     * secret, at any depth; a public key is not one. */
    r = parse("{\"state\":\"ready\",\"public_key\":\"ab\",\"private_key\":\"cd\",\"key_bits\":128,"
              "\"list\":[{\"channel_secret\":\"s\",\"n\":1},{\"Password\":\"p\"}]}");
    f = gw_filter_result(gw_method_find("mesh.status"), r);
    CHECK(f != NULL && cJSON_GetObjectItem(f, "public_key") != NULL && cJSON_GetObjectItem(f, "key_bits") != NULL);
    CHECK(cJSON_GetObjectItem(f, "private_key") == NULL);
    a = cJSON_GetObjectItem(f, "list");
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(a, 0), "channel_secret") == NULL);
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(a, 0), "n") != NULL);
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(a, 1), "Password") == NULL);
    cJSON_Delete(f);
    cJSON_Delete(r);

    /* A result that is not an object is not passed on at all. */
    r = parse("[1,2]");
    CHECK(gw_filter_result(gw_method_find("mesh.status"), r) == NULL);
    CHECK(gw_filter_result(gw_method_find("system.info"), r) == NULL);
    cJSON_Delete(r);
}

/* ---- gw_store ---------------------------------------------------------- */

static void test_store(void)
{
    struct gw_store st, back;
    uint8_t fp[GW_FP_LEN];
    char line[400], fph[GW_FP_HEX + 1];
    int i;

    CHECK(gw_store_dir(tmpdir) == 0);
    CHECK(mode_of(".") == 0700);
    CHECK(!gw_store_exists(tmpdir));
    CHECK(gw_store_load(&st, tmpdir) == 0 && st.n == 0); /* absent is empty */

    CHECK(gw_store_add(&st, FP_A, "Pixel 8", GW_S_DEFAULT, 1763700000) == 0);
    CHECK(gw_store_add(&st, FP_A, "again", GW_S_STATUS, 0) != 0);        /* the same key */
    CHECK(gw_store_add(&st, FP_B, "bad\x01name", GW_S_STATUS, 0) != 0);
    CHECK(gw_store_add(&st, FP_B, "x", GW_S_MESH_SEND, 0) != 0);          /* not a valid set */
    CHECK(gw_store_add(&st, FP_B, "Andr\xc3\xa9", GW_S_STATUS | GW_S_CAPTURE_VIEW, 0) == 0);
    CHECK(gw_store_save(&st) == 0);
    CHECK(gw_store_exists(tmpdir));
    CHECK(mode_of(GW_STORE_FILE) == 0600);
    CHECK(mode_of("." GW_STORE_FILE ".tmp") == 0); /* nothing left behind */

    CHECK(gw_store_load(&back, tmpdir) == 0 && back.n == 2);
    CHECK(gw_store_find(&back, FP_A) == 0 && gw_store_find(&back, FP_B) == 1);
    CHECK(strcmp(back.c[0].name, "Pixel 8") == 0 && back.c[0].scopes == GW_S_DEFAULT);
    CHECK(back.c[0].paired_wall_s == 1763700000);
    CHECK(strcmp(back.c[1].name, "Andr\xc3\xa9") == 0);
    memset(fp, 0xcc, sizeof(fp));
    CHECK(gw_store_find(&back, fp) == -1);

    /* Revocation: gone from the file, the other client untouched. */
    gw_store_remove(&back, 0);
    CHECK(gw_store_save(&back) == 0);
    CHECK(gw_store_load(&st, tmpdir) == 0 && st.n == 1);
    CHECK(gw_store_find(&st, FP_A) == -1 && gw_store_find(&st, FP_B) == 0);

    /* The ninth client does not fit. */
    for (i = 0; st.n < GW_MAX_CLIENTS; i++) {
        memset(fp, 0, sizeof(fp));
        fp[0] = (uint8_t)(i + 1);
        CHECK(gw_store_add(&st, fp, "n", GW_S_STATUS, 0) == 0);
    }
    fp[0] = 0xee;
    CHECK(gw_store_add(&st, fp, "n", GW_S_STATUS, 0) != 0);
    CHECK(gw_store_save(&st) == 0);
    CHECK(gw_store_load(&back, tmpdir) == 0 && back.n == GW_MAX_CLIENTS);

    /* Damage is a fault, never an empty list and never a partial one. */
    gw_hex(FP_A, GW_FP_LEN, fph);
    put_file(GW_STORE_FILE, "", 0600);
    CHECK(gw_store_load(&st, tmpdir) != 0 && st.n == 0);
    put_file(GW_STORE_FILE, "doors-gatewayd-paired 2\n", 0600);
    CHECK(gw_store_load(&st, tmpdir) != 0);
    put_file(GW_STORE_FILE, "doors-gatewayd-paired 1", 0600); /* no final newline: cut short */
    CHECK(gw_store_load(&st, tmpdir) != 0);
    put_file(GW_STORE_FILE, "doors-gatewayd-paired 1\n", 0600);
    CHECK(gw_store_load(&st, tmpdir) == 0 && st.n == 0);
    snprintf(line, sizeof(line), "doors-gatewayd-paired 1\nclient %s 1 0 78\n", fph);
    put_file(GW_STORE_FILE, line, 0600);
    CHECK(gw_store_load(&st, tmpdir) == 0 && st.n == 1 && strcmp(st.c[0].name, "x") == 0);
    snprintf(line, sizeof(line), "doors-gatewayd-paired 1\nclient %s 1 0 78\nclient %s 1 0 79\n", fph, fph);
    put_file(GW_STORE_FILE, line, 0600);
    CHECK(gw_store_load(&st, tmpdir) != 0 && st.n == 0); /* the same key twice */
    snprintf(line, sizeof(line), "doors-gatewayd-paired 1\nclient %s 4 0 78\n", fph);
    put_file(GW_STORE_FILE, line, 0600);
    CHECK(gw_store_load(&st, tmpdir) != 0); /* mesh_send without mesh_read */
    snprintf(line, sizeof(line), "doors-gatewayd-paired 1\nclient %s ff 0 78\n", fph);
    put_file(GW_STORE_FILE, line, 0600);
    CHECK(gw_store_load(&st, tmpdir) != 0); /* scopes that do not exist */
    snprintf(line, sizeof(line), "doors-gatewayd-paired 1\nclient %s 1 0 0a\n", fph);
    put_file(GW_STORE_FILE, line, 0600);
    CHECK(gw_store_load(&st, tmpdir) != 0); /* a control character for a name */
    snprintf(line, sizeof(line), "doors-gatewayd-paired 1\nclient %s 1 0 7800\n", fph);
    put_file(GW_STORE_FILE, line, 0600);
    CHECK(gw_store_load(&st, tmpdir) != 0); /* a NUL inside the name */
    snprintf(line, sizeof(line), "doors-gatewayd-paired 1\nclient %s 1 0 78 extra\n", fph);
    put_file(GW_STORE_FILE, line, 0600);
    CHECK(gw_store_load(&st, tmpdir) != 0);
    snprintf(line, sizeof(line), "doors-gatewayd-paired 1\nclient %.62s 1 0 78\n", fph);
    put_file(GW_STORE_FILE, line, 0600);
    CHECK(gw_store_load(&st, tmpdir) != 0);
    snprintf(line, sizeof(line), "doors-gatewayd-paired 1\nclient %s 1 0 78\ngarbage\n", fph);
    put_file(GW_STORE_FILE, line, 0600);
    CHECK(gw_store_load(&st, tmpdir) != 0 && st.n == 0);
    put_file(GW_STORE_FILE, "", 0600);
    {
        char path[512];

        snprintf(path, sizeof(path), "%s/%s", tmpdir, GW_STORE_FILE);
        unlink(path);
    }
}

/* ---- gw_gate ----------------------------------------------------------- */

static bool gate2(const char *passwd, const char *shadow)
{
    char pp[512], sp[512];

    put_file("passwd", passwd, 0644);
    put_file("shadow", shadow, 0600);
    snprintf(pp, sizeof(pp), "%s/passwd", tmpdir);
    snprintf(sp, sizeof(sp), "%s/shadow", tmpdir);
    return gw_gate_password_set(pp, sp);
}

/* The usual layout: passwd says "x", the shadow file decides. */
static bool gate(const char *shadow)
{
    return gate2("root:x:0:0:root:/root:/bin/sh\n", shadow);
}

static void test_gate(void)
{
    CHECK(!gw_gate_password_set("/nonexistent/passwd", "/nonexistent/shadow"));
    /* What passwd says comes first. An empty field there means login asks
     * for nothing, whatever the shadow file holds. */
    CHECK(!gate2("root::0:0:root:/root:/bin/sh\n", "root:$6$salt$hashhashhash:1::::::\n"));
    CHECK(!gate2("root:*:0:0:root:/root:/bin/sh\n", "root:$6$salt$hashhashhash:1::::::\n"));
    CHECK(!gate2("daemon:x:1:1::/:/bin/false\n", "root:$6$salt$hashhashhash:1::::::\n"));
    CHECK(gate2("root:$6$salt$hashhashhash:0:0:root:/root:/bin/sh\n", ""));
    CHECK(gate2("root:x:0:0:root:/root:/bin/sh\n", "root:$6$salt$hashhashhash:1::::::\n"));
    CHECK(!gate(""));
    CHECK(!gate("root::19000:0:99999:7:::\n"));
    CHECK(!gate("root:*:19000:0:99999:7:::\n"));
    CHECK(!gate("root:!:19000:0:99999:7:::\n"));
    CHECK(!gate("root:x:19000:0:99999:7:::\n"));
    CHECK(!gate("root:!$6$salt$hashhashhash:19000:0:99999:7:::\n")); /* locked */
    CHECK(!gate("daemon:$6$salt$hash:19000::::::\n"));               /* not root's */
    CHECK(!gate("toor:$6$salt$hash:1::::::\nrootx:$6$salt$hash:1::::::\n"));
    CHECK(gate("root:$6$salt$hashhashhash:19000:0:99999:7:::\n"));
    CHECK(gate("daemon:*:1::::::\nroot:$y$j9T$salt$hash:1::::::\n"));
    CHECK(gate("root:abcdefghijkl.:1::::::\n"));  /* traditional DES, 13 characters */
    CHECK(!gate("root:abcdefghijkl:1::::::\n"));  /* 12: not a hash */
    CHECK(!gate("root:abcdefghijkl!:1::::::\n"));
    /* The first root entry is the one login uses. */
    CHECK(!gate("root::1::::::\nroot:$6$salt$hash:1::::::\n"));
    CHECK(!gate("root"));
}

/* ---- gw_tls: keys and the identity file -------------------------------- */

static int admit_nobody(void *user, SSL *ssl, const uint8_t fp[GW_FP_LEN])
{
    (void)user;
    (void)ssl;
    (void)fp;
    return 0;
}

static void key_to_file(EVP_PKEY *k, const char *name, mode_t mode)
{
    BIO *bio = BIO_new(BIO_s_mem());
    char *pem;
    long n;
    char *copy;

    PEM_write_bio_PrivateKey(bio, k, NULL, NULL, 0, NULL, NULL);
    n = BIO_get_mem_data(bio, &pem);
    copy = strndup(pem, (size_t)n);
    put_file(name, copy, mode);
    free(copy);
    BIO_free(bio);
}

static void test_tls(void)
{
    uint8_t fp[GW_FP_LEN], fp2[GW_FP_LEN], want[GW_FP_LEN];
    EVP_PKEY *k, *k2, *other;
    unsigned char *der = NULL;
    char path[512];
    int derlen;

    CHECK(gw_tls_init() == 0);

    /* The fingerprint is the SHA-256 of the key's SubjectPublicKeyInfo as
     * OpenSSL itself writes it (named curve, uncompressed point)... */
    k = EVP_EC_gen("P-256");
    CHECK(k != NULL);
    CHECK(gw_tls_fingerprint(k, fp) == 0);
    derlen = i2d_PUBKEY(k, &der);
    CHECK(derlen == 91);
    SHA256(der, (size_t)derlen, want);
    CHECK(memcmp(fp, want, GW_FP_LEN) == 0);
    OPENSSL_free(der);
    /* ...and stays that when the same key is written with a compressed
     * point, which changes the certificate's bytes and not the key. */
    CHECK(EVP_PKEY_set_utf8_string_param(k, "point-format", "compressed") == 1);
    der = NULL;
    derlen = i2d_PUBKEY(k, &der);
    CHECK(derlen == 59);
    OPENSSL_free(der);
    CHECK(gw_tls_fingerprint(k, fp2) == 0 && memcmp(fp, fp2, GW_FP_LEN) == 0);
    EVP_PKEY_free(k);

    /* Other keys have no fingerprint here at all. */
    other = EVP_EC_gen("P-384");
    CHECK(other != NULL && gw_tls_fingerprint(other, fp2) != 0);
    EVP_PKEY_free(other);
    other = EVP_RSA_gen(2048);
    CHECK(other != NULL && gw_tls_fingerprint(other, fp2) != 0);
    CHECK(gw_tls_ctx(other, admit_nobody, NULL) == NULL); /* not a P-256 identity */
    EVP_PKEY_free(other);
    other = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
    CHECK(other != NULL && gw_tls_fingerprint(other, fp2) != 0);
    EVP_PKEY_free(other);
    CHECK(gw_tls_fingerprint(NULL, fp2) != 0);

    /* The identity file: created once, 0600, read back as the same key. */
    snprintf(path, sizeof(path), "%s/%s", tmpdir, GW_TLS_IDENTITY_FILE);
    unlink(path);
    CHECK(gw_tls_identity(tmpdir, false, &k) == GW_TLS_ID_FAULT && k == NULL); /* absent, may not create */
    CHECK(access(path, F_OK) != 0);
    CHECK(gw_tls_identity(tmpdir, true, &k) == GW_TLS_ID_CREATED && k != NULL);
    CHECK(mode_of(GW_TLS_IDENTITY_FILE) == 0600);
    CHECK(gw_tls_fingerprint(k, fp) == 0);
    CHECK(gw_tls_identity(tmpdir, true, &k2) == GW_TLS_ID_OK && k2 != NULL);
    CHECK(gw_tls_fingerprint(k2, fp2) == 0 && memcmp(fp, fp2, GW_FP_LEN) == 0);
    EVP_PKEY_free(k2);
    /* A context can be made from it; none without an admit function. */
    CHECK(gw_tls_ctx(k, NULL, NULL) == NULL);
    {
        SSL_CTX *ctx = gw_tls_ctx(k, admit_nobody, NULL);

        CHECK(ctx != NULL);
        CHECK(SSL_CTX_get_min_proto_version(ctx) == TLS1_3_VERSION);
        CHECK(SSL_CTX_get_max_proto_version(ctx) == TLS1_3_VERSION);
        CHECK(SSL_CTX_get_verify_mode(ctx) == (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT));
        CHECK(SSL_CTX_get_num_tickets(ctx) == 0);
        SSL_CTX_free(ctx);
    }
    EVP_PKEY_free(k);

    /* A key others can read is a fault, and it is not replaced. */
    chmod(path, 0644);
    CHECK(gw_tls_identity(tmpdir, true, &k) == GW_TLS_ID_FAULT && k == NULL);
    chmod(path, 0600);
    CHECK(gw_tls_identity(tmpdir, true, &k) == GW_TLS_ID_OK);
    CHECK(gw_tls_fingerprint(k, fp2) == 0 && memcmp(fp, fp2, GW_FP_LEN) == 0);
    EVP_PKEY_free(k);
    /* Damage, and a key of another kind: faults, never a new identity. */
    put_file(GW_TLS_IDENTITY_FILE, "not a key\n", 0600);
    CHECK(gw_tls_identity(tmpdir, true, &k) == GW_TLS_ID_FAULT);
    put_file(GW_TLS_IDENTITY_FILE, "", 0600);
    CHECK(gw_tls_identity(tmpdir, true, &k) == GW_TLS_ID_FAULT);
    other = EVP_EC_gen("P-384");
    key_to_file(other, GW_TLS_IDENTITY_FILE, 0600);
    EVP_PKEY_free(other);
    CHECK(gw_tls_identity(tmpdir, true, &k) == GW_TLS_ID_FAULT);
    unlink(path);
    /* A symbolic link is not followed. */
    other = EVP_EC_gen("P-256");
    key_to_file(other, "elsewhere", 0600);
    EVP_PKEY_free(other);
    {
        char target[512];

        snprintf(target, sizeof(target), "%s/elsewhere", tmpdir);
        CHECK(symlink(target, path) == 0);
        CHECK(gw_tls_identity(tmpdir, true, &k) == GW_TLS_ID_FAULT);
        unlink(path);
        unlink(target);
    }
}

int main(void)
{
    char cmd[512];

    snprintf(tmpdir, sizeof(tmpdir), "/tmp/gatewayd-unit-XXXXXX");
    if (mkdtemp(tmpdir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    test_util();
    test_frame();
    test_pair();
    test_policy();
    test_store();
    test_gate();
    test_tls();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0) {
        printf("  (could not remove %s)\n", tmpdir);
    }
    printf("gatewayd unit: %d checks, %d failed\n", checks, failed);
    return failed == 0 ? 0 : 1;
}
