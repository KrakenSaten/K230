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
#include "gw_feed.h"
#include "gw_frame.h"
#include "gw_gate.h"
#include "gw_journal.h"
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

/* ---- the mesh group: filters, parameters, topics (M4.1) ---------------- */

static bool has(const cJSON *o, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(o, name) != NULL;
}

static int upstream_params(const char *method, const char *params_text, cJSON **out)
{
    const struct gw_method *m = gw_method_find(method);
    cJSON *p = params_text != NULL ? parse(params_text) : NULL;
    int r = m->params(p, out);

    cJSON_Delete(p);
    return r;
}

#define K64 "aa00000000000000000000000000000000000000000000000000000000000001"
#define K64B "bb00000000000000000000000000000000000000000000000000000000000002"

static void test_mesh_policy(void)
{
    static const char *const read_rows[] = {"mesh.identity", "mesh.nodes",        "mesh.node",
                                            "mesh.channels", "mesh.path_preview", "mesh.sync", NULL};
    const struct gw_method *m;
    cJSON *r, *f, *a, *p;
    unsigned mask;
    uint64_t after;
    bool have;
    int i, off, lim;

    /* Every reading method needs mesh_read and nothing else gives it; the
     * status scope does not, the send scope alone is not a valid set. */
    for (i = 0; read_rows[i] != NULL; i++) {
        m = gw_method_find(read_rows[i]);
        CHECK(m != NULL && m->scope == GW_S_MESH_READ && m->who == GW_W_PAIRED);
        CHECK(gw_policy_check(m, GW_W_PAIRED, GW_S_STATUS) == GW_V_FORBIDDEN);
        CHECK(gw_policy_check(m, GW_W_PAIRED, GW_S_ALL & ~GW_S_MESH_READ) == GW_V_FORBIDDEN);
        CHECK(gw_policy_check(m, GW_W_PAIRED, GW_S_MESH_READ) == GW_V_OK);
        CHECK(gw_policy_check(m, GW_W_CANDIDATE, GW_S_ALL) == GW_V_UNKNOWN);
    }
    /* Not in M4.1: sending, and reading that was never approved. */
    CHECK(gw_method_find("mesh.send") == NULL && gw_method_find("mesh.resend") == NULL);
    CHECK(gw_method_find("mesh.messages") == NULL && gw_method_find("mesh.subscribe") == NULL);
    CHECK(gw_method_find("mesh.app_inbox") == NULL && gw_method_find("mesh.remote_session") == NULL);
    CHECK(gw_method_find("mesh.discovered") == NULL && gw_method_find("mesh.channel") == NULL);
    CHECK(gw_method_find("capture.status") == NULL);
    m = gw_method_find("gateway.subscribe");
    CHECK(m != NULL && m->scope == 0 && gw_policy_check(m, GW_W_CANDIDATE, 0) == GW_V_UNKNOWN);

    /* The identity: the public half only, whatever the service would add. */
    r = parse("{\"public_key\":\"ab\",\"node_hash\":\"ab\",\"name\":\"n\",\"name_source\":\"stored\",\"name_max\":31,"
              "\"private_key\":\"LEAK\",\"prv\":\"LEAK\",\"seed\":\"LEAK\",\"state_path\":\"/var/lib\"}");
    f = gw_filter_result(gw_method_find("mesh.identity"), r);
    CHECK(f != NULL && cJSON_GetArraySize(f) == 5 && has(f, "public_key") && !has(f, "private_key") &&
          !has(f, "prv") && !has(f, "seed") && !has(f, "state_path"));
    cJSON_Delete(f);
    cJSON_Delete(r);

    /* Channels: never a key, by whatever name. */
    r = parse("{\"channels\":[{\"channel\":0,\"name\":\"Public\",\"channel_hash\":\"11\",\"key_bits\":128,"
              "\"well_known\":\"public\",\"text_limit\":140,\"ack_expected\":false,\"key\":\"LEAK\","
              "\"secret_hex\":\"LEAK\",\"psk\":\"LEAK\",\"k\":\"LEAK\"},\"x\"],\"count\":1,\"max\":8,"
              "\"persistent\":true,\"store\":\"/var/lib/x\"}");
    f = gw_filter_result(gw_method_find("mesh.channels"), r);
    a = cJSON_GetObjectItem(f, "channels");
    CHECK(f != NULL && cJSON_GetArraySize(a) == 1 && !has(f, "store") && has(f, "max"));
    a = cJSON_GetArrayItem(a, 0);
    CHECK(cJSON_GetArraySize(a) == 7 && has(a, "key_bits") && !has(a, "key") && !has(a, "secret_hex") &&
          !has(a, "psk") && !has(a, "k"));
    cJSON_Delete(f);
    cJSON_Delete(r);

    /* A node and a message keep the documented fields, the peer's public
     * key among them, and lose every other. */
    r = parse("{\"public_key\":\"" K64 "\",\"name\":\"n\",\"type\":1,\"path_known\":true,\"hops\":2,"
              "\"path_hex\":\"0102\",\"lat\":59.9,\"lon\":10.7,\"shared_secret\":\"LEAK\",\"out_path_key\":\"LEAK\","
              "\"internal_slot\":7}");
    f = gw_filter_node(r);
    CHECK(f != NULL && cJSON_GetArraySize(f) == 8 && !has(f, "shared_secret") && !has(f, "out_path_key") &&
          !has(f, "internal_slot"));
    cJSON_Delete(f);
    cJSON_Delete(r);
    r = parse("{\"id\":4,\"direction\":\"in\",\"kind\":\"direct\",\"peer_public_key\":\"" K64 "\","
              "\"peer_name\":\"p\",\"text\":\"hello\",\"timestamp\":1,\"mono_ms\":2,\"state\":\"received\","
              "\"ack_expected\":false,\"snr_db\":4.5,\"ack_hash\":\"LEAK\",\"expected_ack\":123,\"raw_hex\":\"00\","
              "\"session_key\":\"LEAK\"}");
    f = gw_filter_message(r);
    CHECK(f != NULL && cJSON_GetArraySize(f) == 11 && has(f, "peer_public_key") && has(f, "text") &&
          !has(f, "ack_hash") && !has(f, "expected_ack") && !has(f, "raw_hex") && !has(f, "session_key"));
    cJSON_Delete(f);
    cJSON_Delete(r);
    CHECK(gw_filter_message(NULL) == NULL && gw_filter_node(NULL) == NULL);
    r = parse("[1]");
    CHECK(gw_filter_message(r) == NULL && gw_filter_channel(r) == NULL);
    cJSON_Delete(r);

    /* mesh.nodes: only the list, each node filtered; no list, no answer. */
    r = parse("{\"nodes\":[{\"public_key\":\"" K64 "\",\"secret\":\"LEAK\"},3],\"count\":1,\"debug\":1}");
    f = gw_filter_result(gw_method_find("mesh.nodes"), r);
    a = cJSON_GetObjectItem(f, "nodes");
    CHECK(f != NULL && cJSON_GetArraySize(f) == 1 && cJSON_GetArraySize(a) == 1 &&
          !has(cJSON_GetArrayItem(a, 0), "secret"));
    cJSON_Delete(f);
    cJSON_Delete(r);
    r = parse("{\"nodes\":7}");
    CHECK(gw_filter_result(gw_method_find("mesh.nodes"), r) == NULL);
    cJSON_Delete(r);

    /* Pages of it. */
    r = parse("{\"nodes\":[{\"name\":\"a\"},{\"name\":\"b\"},{\"name\":\"c\"}]}");
    f = gw_nodes_page(r, 1, 1, "run1");
    a = cJSON_GetObjectItem(f, "nodes");
    CHECK(f != NULL && cJSON_GetArraySize(a) == 1 &&
          strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(a, 0), "name")->valuestring, "b") == 0);
    CHECK(cJSON_GetObjectItem(f, "count")->valueint == 1 && cJSON_GetObjectItem(f, "total")->valueint == 3 &&
          strcmp(cJSON_GetObjectItem(f, "run_id")->valuestring, "run1") == 0);
    cJSON_Delete(f);
    f = gw_nodes_page(r, 5, 100, "run1");
    CHECK(f != NULL && cJSON_GetObjectItem(f, "count")->valueint == 0 && cJSON_GetObjectItem(f, "total")->valueint == 3);
    cJSON_Delete(f);
    cJSON_Delete(r);

    /* Events: three kinds pass, reduced; nothing else has a filter at all. */
    r = parse("{\"reason\":\"discovered\",\"node\":{\"public_key\":\"" K64 "\",\"name\":\"n\",\"secret\":\"LEAK\"},"
              "\"extra\":1}");
    f = gw_filter_event("mesh.node", r);
    CHECK(f != NULL && cJSON_GetArraySize(f) == 2 && !has(cJSON_GetObjectItem(f, "node"), "secret"));
    cJSON_Delete(f);
    CHECK(gw_filter_event("mesh.channel", r) == NULL); /* no channel in it */
    CHECK(gw_filter_event("mesh.rx", r) == NULL && gw_filter_event("mesh.app", r) == NULL &&
          gw_filter_event("mesh.remote", r) == NULL && gw_filter_event("mesh.discover", r) == NULL &&
          gw_filter_event("mesh.activity", r) == NULL && gw_filter_event("mesh.message", r) == NULL &&
          gw_filter_event(NULL, r) == NULL && gw_filter_event("mesh.node", NULL) == NULL);
    cJSON_Delete(r);
    r = parse("{\"reason\":\"added\",\"channel\":{\"channel\":1,\"name\":\"c\",\"key\":\"LEAK\"}}");
    f = gw_filter_event("mesh.channel", r);
    CHECK(f != NULL && !has(cJSON_GetObjectItem(f, "channel"), "key") && has(cJSON_GetObjectItem(f, "channel"), "name"));
    cJSON_Delete(f);
    cJSON_Delete(r);
    r = parse("{\"state\":\"online\",\"reason\":\"none\",\"mono_ms\":5,\"lease_owner_id\":9}");
    f = gw_filter_event("mesh.state", r);
    CHECK(f != NULL && cJSON_GetArraySize(f) == 3);
    cJSON_Delete(f);
    cJSON_Delete(r);

    /* Parameters: checked, and written again; nothing else reaches a service. */
    CHECK(upstream_params("mesh.node", "{\"node\":\"aB01\"}", &p) == 0 && cJSON_GetArraySize(p) == 1 &&
          strcmp(cJSON_GetObjectItem(p, "node")->valuestring, "aB01") == 0);
    cJSON_Delete(p);
    CHECK(upstream_params("mesh.node", NULL, &p) != 0 && p == NULL);
    CHECK(upstream_params("mesh.node", "{}", &p) != 0);
    CHECK(upstream_params("mesh.node", "{\"node\":\"abc\"}", &p) != 0);      /* odd */
    CHECK(upstream_params("mesh.node", "{\"node\":\"zz\"}", &p) != 0);
    CHECK(upstream_params("mesh.node", "{\"node\":\"\"}", &p) != 0);
    CHECK(upstream_params("mesh.node", "{\"node\":7}", &p) != 0);
    CHECK(upstream_params("mesh.node", "{\"node\":\"" K64 "aa\"}", &p) != 0); /* too long */
    CHECK(upstream_params("mesh.node", "{\"node\":\"ab\",\"remove\":true}", &p) != 0);
    CHECK(upstream_params("mesh.path_preview", "{\"to\":\"" K64 "\",\"via\":[\"" K64B "\"]}", &p) == 0 &&
          cJSON_GetArraySize(p) == 2 && cJSON_GetArraySize(cJSON_GetObjectItem(p, "via")) == 1);
    cJSON_Delete(p);
    CHECK(upstream_params("mesh.path_preview", "{\"to\":\"" K64 "\",\"via\":[]}", &p) == 0);
    cJSON_Delete(p);
    CHECK(upstream_params("mesh.path_preview", "{\"to\":\"" K64 "\"}", &p) != 0);
    CHECK(upstream_params("mesh.path_preview", "{\"to\":\"ab\",\"via\":[]}", &p) != 0); /* a prefix */
    CHECK(upstream_params("mesh.path_preview", "{\"to\":\"" K64 "\",\"via\":[\"ab\"]}", &p) != 0);
    CHECK(upstream_params("mesh.path_preview", "{\"to\":\"" K64 "\",\"via\":[7]}", &p) != 0);
    CHECK(upstream_params("mesh.path_preview", "{\"to\":\"" K64 "\",\"via\":{}}", &p) != 0);
    CHECK(upstream_params("mesh.path_preview", "{\"to\":\"" K64 "\",\"via\":[],\"text\":\"x\"}", &p) != 0);
    CHECK(gw_method_find("mesh.identity")->params == NULL && gw_method_find("mesh.channels")->params == NULL);

    a = parse("{\"offset\":200,\"limit\":50}");
    CHECK(gw_page_params(a, &off, &lim) == 0 && off == 200 && lim == 50);
    cJSON_Delete(a);
    CHECK(gw_page_params(NULL, &off, &lim) == 0 && off == 0 && lim == GW_NODES_LIMIT);
    a = parse("{\"limit\":101}");
    CHECK(gw_page_params(a, &off, &lim) != 0);
    cJSON_Delete(a);
    a = parse("{\"limit\":0}");
    CHECK(gw_page_params(a, &off, &lim) != 0);
    cJSON_Delete(a);
    a = parse("{\"offset\":-1}");
    CHECK(gw_page_params(a, &off, &lim) != 0);
    cJSON_Delete(a);
    a = parse("{\"offset\":1.5}");
    CHECK(gw_page_params(a, &off, &lim) != 0);
    cJSON_Delete(a);
    a = parse("{\"offset\":1e30}");
    CHECK(gw_page_params(a, &off, &lim) != 0);
    cJSON_Delete(a);
    a = parse("{\"page\":1}");
    CHECK(gw_page_params(a, &off, &lim) != 0);
    cJSON_Delete(a);

    CHECK(gw_sync_params(NULL, &have, &after, &lim) == 0 && !have && lim == GW_SYNC_LIMIT);
    a = parse("{\"after_seq\":0,\"limit\":1}");
    CHECK(gw_sync_params(a, &have, &after, &lim) == 0 && have && after == 0 && lim == 1);
    cJSON_Delete(a);
    a = parse("{\"after_seq\":9007199254740992}");
    CHECK(gw_sync_params(a, &have, &after, &lim) == 0 && after == 9007199254740992ull);
    cJSON_Delete(a);
    a = parse("{\"after_seq\":-1}");
    CHECK(gw_sync_params(a, &have, &after, &lim) != 0);
    cJSON_Delete(a);
    a = parse("{\"after_seq\":\"5\"}");
    CHECK(gw_sync_params(a, &have, &after, &lim) != 0);
    cJSON_Delete(a);
    a = parse("{\"after_seq\":1e300}");
    CHECK(gw_sync_params(a, &have, &after, &lim) != 0);
    cJSON_Delete(a);
    a = parse("{\"limit\":1000}");
    CHECK(gw_sync_params(a, &have, &after, &lim) != 0);
    cJSON_Delete(a);
    a = parse("{\"run_id\":\"x\"}");
    CHECK(gw_sync_params(a, &have, &after, &lim) != 0);
    cJSON_Delete(a);

    /* Topics: served and allowed, or left out; a wrong shape is refused. */
    a = parse("{\"topics\":[\"mesh\",\"mesh.activity\",\"device\",\"capture\",\"nonsense\"]}");
    CHECK(gw_topics_parse(a, GW_S_ALL, &mask) == 0 && mask == (GW_T_MESH | GW_T_MESH_ACTIVITY));
    CHECK(gw_topics_parse(a, GW_S_STATUS, &mask) == 0 && mask == 0);
    CHECK(gw_topics_parse(a, GW_S_ALL & ~GW_S_MESH_READ, &mask) == 0 && mask == 0);
    cJSON_Delete(a);
    a = parse("{\"topics\":[]}");
    CHECK(gw_topics_parse(a, GW_S_ALL, &mask) == 0 && mask == 0);
    cJSON_Delete(a);
    CHECK(gw_topics_parse(NULL, GW_S_ALL, &mask) != 0);
    a = parse("{\"topics\":\"mesh\"}");
    CHECK(gw_topics_parse(a, GW_S_ALL, &mask) != 0);
    cJSON_Delete(a);
    a = parse("{\"topics\":[1]}");
    CHECK(gw_topics_parse(a, GW_S_ALL, &mask) != 0);
    cJSON_Delete(a);
    a = parse("{\"topics\":[\"mesh\"],\"rx_log\":true}");
    CHECK(gw_topics_parse(a, GW_S_ALL, &mask) != 0);
    cJSON_Delete(a);
    a = parse("{\"topics\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}");
    CHECK(gw_topics_parse(a, GW_S_ALL, &mask) != 0);
    cJSON_Delete(a);
    CHECK(gw_topics_allowed(GW_T_ALL, GW_S_STATUS) == 0 && gw_topics_allowed(GW_T_MESH, GW_S_MESH_READ) == GW_T_MESH);
    a = gw_topics_json(GW_T_ALL);
    CHECK(cJSON_GetArraySize(a) == 2);
    cJSON_Delete(a);
}

/* ---- gw_journal -------------------------------------------------------- */

static cJSON *msg(int id, const char *state)
{
    cJSON *m = cJSON_CreateObject();

    cJSON_AddNumberToObject(m, "id", id);
    cJSON_AddStringToObject(m, "state", state);
    cJSON_AddStringToObject(m, "text", "t");
    return m;
}

static double num(const cJSON *o, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);

    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}

/* seq of entry i of a sync answer, and the id of its message. */
static double sync_seq(const cJSON *r, int i)
{
    return num(cJSON_GetArrayItem(cJSON_GetObjectItem(r, "messages"), i), "seq");
}

static double sync_id(const cJSON *r, int i)
{
    return num(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(r, "messages"), i), "message"), "id");
}

static int sync_n(const cJSON *r)
{
    return cJSON_GetArraySize(cJSON_GetObjectItem(r, "messages"));
}

static void test_journal(void)
{
    static struct gw_journal j;
    const cJSON *kept;
    cJSON *r, *big;
    char *text;
    int i;

    CHECK(gw_ident_ok("a1B2-_") && !gw_ident_ok("") && !gw_ident_ok(NULL) && !gw_ident_ok("a b") &&
          !gw_ident_ok("a\"b") && !gw_ident_ok("0123456789012345678901234567890123"));
    CHECK(gw_journal_init(&j, "bad id") != 0);
    CHECK(gw_journal_init(&j, "j1") == 0);
    CHECK(gw_journal_seq(&j) == 0 && gw_journal_first_seq(&j) == 1);

    /* No run known: nothing is journaled, since nothing could say whose
     * ids these are. */
    CHECK(gw_journal_put(&j, msg(1, "received"), NULL) == 0);
    CHECK(gw_journal_set_run(&j, "not a run id") == -1 && j.run_id[0] == '\0');
    CHECK(gw_journal_set_run(&j, "runA") == 1);
    CHECK(gw_journal_set_run(&j, "runA") == 0);

    CHECK(gw_journal_put(&j, msg(1, "sent_direct"), &kept) == 1 && kept != NULL);
    CHECK(gw_journal_put(&j, msg(2, "received"), NULL) == 2);
    /* The same again is not news (a repeated event; a snapshot after it). */
    CHECK(gw_journal_put(&j, msg(1, "sent_direct"), &kept) == 0 && kept == NULL);
    CHECK(gw_journal_put(&j, msg(2, "received"), NULL) == 0);
    /* A change of state is: a new seq, the same message id. */
    CHECK(gw_journal_put(&j, msg(1, "acked"), NULL) == 3);
    CHECK(gw_journal_put(&j, msg(1, "acked"), NULL) == 0);
    /* Going back to an earlier state is a change too, and is kept: the
     * journal records what was said, in order. */
    CHECK(gw_journal_put(&j, msg(1, "sent_direct"), NULL) == 4);
    /* Unusable: no id, a fraction, zero, text, not an object, nothing. */
    r = cJSON_CreateObject();
    CHECK(gw_journal_put(&j, r, NULL) == 0);
    r = parse("{\"id\":1.5}");
    CHECK(gw_journal_put(&j, r, NULL) == 0);
    r = parse("{\"id\":0}");
    CHECK(gw_journal_put(&j, r, NULL) == 0);
    r = parse("{\"id\":\"7\"}");
    CHECK(gw_journal_put(&j, r, NULL) == 0);
    r = parse("{\"id\":1e300}");
    CHECK(gw_journal_put(&j, r, NULL) == 0);
    r = parse("[1]");
    CHECK(gw_journal_put(&j, r, NULL) == 0);
    CHECK(gw_journal_put(&j, NULL, NULL) == 0);
    /* Larger than one message may be. */
    big = msg(9, "received");
    text = malloc(GW_JOURNAL_MSG_MAX + 1);
    memset(text, 'x', GW_JOURNAL_MSG_MAX);
    text[GW_JOURNAL_MSG_MAX] = '\0';
    cJSON_AddStringToObject(big, "more", text);
    free(text);
    CHECK(gw_journal_put(&j, big, NULL) == 0);
    CHECK(gw_journal_seq(&j) == 4 && gw_journal_first_seq(&j) == 1 && j.count == 4);

    /* Without a cursor: the newest, oldest first. */
    r = gw_journal_sync(&j, false, 0, 100);
    CHECK(r != NULL && sync_n(r) == 4 && sync_seq(r, 0) == 1 && sync_seq(r, 3) == 4 && sync_id(r, 3) == 1);
    CHECK(strcmp(cJSON_GetObjectItem(r, "journal_id")->valuestring, "j1") == 0 &&
          strcmp(cJSON_GetObjectItem(r, "run_id")->valuestring, "runA") == 0);
    CHECK(num(r, "seq") == 4 && num(r, "first_seq") == 1 && cJSON_IsFalse(cJSON_GetObjectItem(r, "more")) &&
          !has(r, "gap"));
    cJSON_Delete(r);
    r = gw_journal_sync(&j, false, 0, 2);
    CHECK(sync_n(r) == 2 && sync_seq(r, 0) == 3 && sync_seq(r, 1) == 4);
    cJSON_Delete(r);
    /* With one: what came after it, in pages. */
    r = gw_journal_sync(&j, true, 0, 3);
    CHECK(sync_n(r) == 3 && sync_seq(r, 0) == 1 && cJSON_IsTrue(cJSON_GetObjectItem(r, "more")) && !has(r, "gap"));
    cJSON_Delete(r);
    r = gw_journal_sync(&j, true, 3, 3);
    CHECK(sync_n(r) == 1 && sync_seq(r, 0) == 4 && cJSON_IsFalse(cJSON_GetObjectItem(r, "more")));
    cJSON_Delete(r);
    r = gw_journal_sync(&j, true, 4, 100); /* up to date */
    CHECK(sync_n(r) == 0 && !has(r, "gap") && cJSON_IsFalse(cJSON_GetObjectItem(r, "more")));
    cJSON_Delete(r);
    /* A cursor from the future is not one of this journal's. */
    r = gw_journal_sync(&j, true, 5, 100);
    CHECK(sync_n(r) == 4 && cJSON_IsTrue(cJSON_GetObjectItem(r, "gap")));
    cJSON_Delete(r);
    r = gw_journal_sync(&j, true, 4, 0); /* a limit below 1 is 1 */
    CHECK(r != NULL && sync_n(r) == 0);
    cJSON_Delete(r);

    /* The ring: 512 held, the oldest gone, and a cursor into what is gone
     * is told so and gets what is left from its start. */
    for (i = 0; i < GW_JOURNAL_MAX + 10; i++) {
        CHECK(gw_journal_put(&j, msg(100 + i, "received"), NULL) == (uint64_t)(5 + i));
    }
    CHECK(j.count == GW_JOURNAL_MAX && gw_journal_seq(&j) == 4 + GW_JOURNAL_MAX + 10);
    CHECK(gw_journal_first_seq(&j) == gw_journal_seq(&j) - GW_JOURNAL_MAX + 1);
    r = gw_journal_sync(&j, true, 2, 100);
    CHECK(sync_n(r) == 100 && cJSON_IsTrue(cJSON_GetObjectItem(r, "gap")) &&
          sync_seq(r, 0) == (double)gw_journal_first_seq(&j) && cJSON_IsTrue(cJSON_GetObjectItem(r, "more")));
    cJSON_Delete(r);
    r = gw_journal_sync(&j, true, gw_journal_first_seq(&j) - 1, 5); /* exactly the edge: nothing lost */
    CHECK(sync_n(r) == 5 && !has(r, "gap") && sync_seq(r, 0) == (double)gw_journal_first_seq(&j));
    cJSON_Delete(r);
    r = gw_journal_sync(&j, true, gw_journal_first_seq(&j) - 2, 5); /* one was lost */
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "gap")));
    cJSON_Delete(r);
    /* Walking it by cursor reaches the end exactly once, in order. */
    {
        uint64_t cur = gw_journal_first_seq(&j) - 1, seen = 0;
        bool more = true, ordered = true;

        while (more) {
            int n;

            r = gw_journal_sync(&j, true, cur, 100);
            n = sync_n(r);
            for (i = 0; i < n; i++) {
                ordered = ordered && sync_seq(r, i) == (double)(cur + 1);
                cur = (uint64_t)sync_seq(r, i);
                seen++;
            }
            more = cJSON_IsTrue(cJSON_GetObjectItem(r, "more"));
            cJSON_Delete(r);
            if (n == 0) {
                break;
            }
        }
        CHECK(ordered && seen == GW_JOURNAL_MAX && cur == gw_journal_seq(&j));
    }
    /* A duplicate is looked for across the whole ring, not just at its end. */
    CHECK(gw_journal_put(&j, msg(100 + 20, "received"), NULL) == 0);

    /* meshcored started again: its ids start again, so what is held goes,
     * the sequence goes on, and every old cursor is a gap. */
    {
        uint64_t before = gw_journal_seq(&j);

        CHECK(gw_journal_set_run(&j, "runB") == 1 && j.count == 0);
        CHECK(gw_journal_seq(&j) == before && gw_journal_first_seq(&j) == before + 1);
        r = gw_journal_sync(&j, true, before - 3, 100);
        CHECK(sync_n(r) == 0 && cJSON_IsTrue(cJSON_GetObjectItem(r, "gap")) &&
              strcmp(cJSON_GetObjectItem(r, "run_id")->valuestring, "runB") == 0);
        cJSON_Delete(r);
        r = gw_journal_sync(&j, false, 0, 100);
        CHECK(sync_n(r) == 0 && !has(r, "gap"));
        cJSON_Delete(r);
        /* Message id 1 of the new run is a new message, not the old one. */
        CHECK(gw_journal_put(&j, msg(1, "sent_direct"), NULL) == before + 1);
    }

    /* An answer stays under its size however much is held. */
    CHECK(gw_journal_set_run(&j, "runC") == 1);
    text = malloc(6000);
    memset(text, 'y', 5999);
    text[5999] = '\0';
    for (i = 0; i < 100; i++) {
        big = msg(i + 1, "received");
        cJSON_AddStringToObject(big, "more", text);
        CHECK(gw_journal_put(&j, big, NULL) != 0);
    }
    free(text);
    r = gw_journal_sync(&j, false, 0, 100);
    text = cJSON_PrintUnformatted(r);
    CHECK(text != NULL && strlen(text) < GW_FRAME_OUT_MAX - 4096 && sync_n(r) > 10 && sync_n(r) < 100);
    CHECK(sync_seq(r, sync_n(r) - 1) == (double)gw_journal_seq(&j)); /* the newest are the ones kept */
    free(text);
    cJSON_Delete(r);
    r = gw_journal_sync(&j, true, gw_journal_first_seq(&j) - 1, 100);
    text = cJSON_PrintUnformatted(r);
    CHECK(text != NULL && strlen(text) < GW_FRAME_OUT_MAX - 4096 && cJSON_IsTrue(cJSON_GetObjectItem(r, "more")));
    free(text);
    cJSON_Delete(r);
    gw_journal_free(&j);
    CHECK(j.count == 0);
}

/* ---- gw_feed ----------------------------------------------------------- */

static cJSON *node_event(const char *key, const char *reason, int n)
{
    cJSON *d = cJSON_CreateObject();
    cJSON *node = cJSON_AddObjectToObject(d, "node");

    cJSON_AddStringToObject(d, "reason", reason);
    cJSON_AddStringToObject(node, "public_key", key);
    cJSON_AddNumberToObject(node, "n", n);
    return d;
}

static double node_n(const cJSON *d)
{
    return num(cJSON_GetObjectItem(d, "node"), "n");
}

static void test_feed(void)
{
    static struct gw_feed f;
    cJSON *d, *a;
    char key[65];
    int i, out;

    gw_feed_init(&f);
    /* The first event of a node goes out; what follows within ten seconds
     * is held, the newest replacing the older, and goes out when they end. */
    d = gw_feed_node(&f, node_event(K64, "discovered", 1), 1000);
    CHECK(d != NULL && node_n(d) == 1);
    cJSON_Delete(d);
    CHECK(gw_feed_node(&f, node_event(K64, "path", 2), 1100) == NULL);
    CHECK(gw_feed_node(&f, node_event(K64, "path", 3), 9000) == NULL);
    CHECK(gw_feed_node_due(&f, 10999) == NULL);
    /* Another node is not held up by this one. */
    d = gw_feed_node(&f, node_event(K64B, "discovered", 7), 2000);
    CHECK(d != NULL && node_n(d) == 7);
    cJSON_Delete(d);
    d = gw_feed_node_due(&f, 11000);
    CHECK(d != NULL && node_n(d) == 3);
    cJSON_Delete(d);
    CHECK(gw_feed_node_due(&f, 11000) == NULL);
    /* That counted as its event: the next is held again. */
    CHECK(gw_feed_node(&f, node_event(K64, "path", 4), 12000) == NULL);
    /* "removed" is never held, and nothing held may follow it. */
    d = gw_feed_node(&f, node_event(K64, "removed", 5), 12500);
    CHECK(d != NULL && node_n(d) == 5);
    cJSON_Delete(d);
    CHECK(gw_feed_node_due(&f, 60000) == NULL);
    /* After the time with nothing held, a node is new again. */
    d = gw_feed_node(&f, node_event(K64B, "path", 8), 12001);
    CHECK(d != NULL);
    cJSON_Delete(d);
    /* Unusable events are dropped. */
    CHECK(gw_feed_node(&f, node_event("abcd", "path", 1), 1) == NULL);
    CHECK(gw_feed_node(&f, NULL, 1) == NULL);
    d = cJSON_CreateObject();
    CHECK(gw_feed_node(&f, d, 1) == NULL);

    /* A burst: per node at most one now and one later, whatever is sent;
     * more nodes than the table holds are not sent unbounded. */
    gw_feed_reset(&f);
    out = 0;
    for (i = 0; i < 3 * GW_FEED_NODES * 4; i++) {
        snprintf(key, sizeof(key), "%064x", i % (3 * GW_FEED_NODES));
        d = gw_feed_node(&f, node_event(key, "path", i), 100000 + i);
        if (d != NULL) {
            out++;
            cJSON_Delete(d);
        }
    }
    CHECK(out == GW_FEED_NODES);
    out = 0;
    while ((d = gw_feed_node_due(&f, 100000 + GW_NODE_EVENT_MS + 3 * GW_FEED_NODES * 4)) != NULL) {
        out++;
        cJSON_Delete(d);
    }
    CHECK(out == GW_FEED_NODES);
    gw_feed_reset(&f);
    CHECK(gw_feed_node_due(&f, 1 << 30) == NULL);

    /* Activity: counted into a bin, one bin a second, counts only. */
    CHECK(gw_feed_activity_due(&f, 5000) == NULL);
    a = parse("{\"kind\":\"rx\",\"payload_type\":\"advert\",\"bytes\":100,\"rssi_dbm\":-90,\"snr_db\":3.5,\"mono_ms\":1}");
    gw_feed_activity(&f, a, 5000);
    gw_feed_activity(&f, a, 5100);
    cJSON_Delete(a);
    a = parse("{\"kind\":\"rx\",\"payload_type\":\"Group Text!\",\"bytes\":1e9}");
    gw_feed_activity(&f, a, 5200);
    cJSON_Delete(a);
    a = parse("{\"kind\":\"tx\",\"result\":\"ok\",\"bytes\":20,\"submit_id\":4}");
    gw_feed_activity(&f, a, 5300);
    cJSON_Delete(a);
    a = parse("{\"kind\":\"tx\",\"result\":\"rx_resume_failed\"}");
    gw_feed_activity(&f, a, 5300);
    cJSON_Delete(a);
    a = parse("{\"kind\":\"tx\",\"result\":\"refused\"}");
    gw_feed_activity(&f, a, 5300);
    cJSON_Delete(a);
    a = parse("{\"kind\":\"tx\",\"result\":\"unknown\"}");
    gw_feed_activity(&f, a, 5300);
    cJSON_Delete(a);
    a = parse("{\"kind\":\"other\"}");
    gw_feed_activity(&f, a, 5300);
    cJSON_Delete(a);
    gw_feed_activity(&f, NULL, 5300);
    CHECK(gw_feed_activity_due(&f, 5999) == NULL);
    d = gw_feed_activity_due(&f, 6000);
    CHECK(d != NULL && num(d, "start_mono_ms") == 5000 && num(d, "rx_frames") == 3 && num(d, "rx_bytes") == 200);
    CHECK(num(d, "tx_ok") == 2 && num(d, "tx_failed") == 1 && num(d, "tx_unknown") == 1);
    a = cJSON_GetObjectItem(d, "by_type");
    CHECK(num(a, "advert") == 2 && num(a, "other") == 1 && cJSON_GetArraySize(a) == 2);
    CHECK(cJSON_GetArraySize(d) == 7 && !has(d, "rssi_dbm") && !has(d, "snr_db")); /* no signal figure leaves */
    cJSON_Delete(d);
    CHECK(gw_feed_activity_due(&f, 7000) == NULL); /* nothing happened: no bin */
    /* More type words than a bin holds end up in "other". */
    for (i = 0; i < 40; i++) {
        char t[80];

        snprintf(t, sizeof(t), "{\"kind\":\"rx\",\"payload_type\":\"type_%d\"}", i);
        a = parse(t);
        gw_feed_activity(&f, a, 8000);
        cJSON_Delete(a);
    }
    d = gw_feed_activity_due(&f, 9000);
    a = cJSON_GetObjectItem(d, "by_type");
    CHECK(cJSON_GetArraySize(a) == GW_FEED_TYPES && num(a, "other") == 40 - (GW_FEED_TYPES - 1) &&
          num(d, "rx_frames") == 40);
    cJSON_Delete(d);
    gw_feed_reset(&f);
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
    test_mesh_policy();
    test_journal();
    test_feed();
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
