/*
 * RIFT's model: what it keeps, what it refuses, and what it never invents.
 *
 * mesh.* is protocol-oriented and leaves out what nobody measured
 * (docs/api/mesh.md). The failures worth testing are all one mistake in two
 * directions: filling a gap in - a missing RSSI read as 0 dBm, a missing hop
 * count read as 0 hops, a node the service has forgotten still on screen -
 * or throwing away something real, which is what a second row for a node
 * that adverted twice would do to a list somebody is counting.
 *
 * No LVGL, no socket, no service.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_model.h"

#include "rift_format.h"
#include "rift_keys.h"
#include "rift_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void text_is(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want);
    }
}

#define KEY_A "a19ac21e7d0411223344556677889900aabbccddeeff001122334455667788b1"
#define KEY_B "b2cafe1e7d0411223344556677889900aabbccddeeff001122334455667788b2"
#define KEY_C "c3beef1e7d0411223344556677889900aabbccddeeff001122334455667788b3"
#define KEY_SELF "5f000000000000000000000000000000000000000000000000000000000000ff"

/* Apply a JSON literal, and say whether the model took it. */
static int apply_nodes(struct rift_model *m, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_nodes(m, o);

    cJSON_Delete(o);
    return rc;
}

static int apply_event(struct rift_model *m, const char *name, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_event(m, name, o);

    cJSON_Delete(o);
    return rc;
}

/* A status read at a given moment on the shared monotonic clock. The clock
 * is a parameter of the model's rather than something it reads, so which
 * run of the service answered - derived from the moment and the uptime the
 * reply carries - can be exercised without waiting for a real one. */
static int apply_status_at(struct rift_model *m, const char *json, int64_t now_ms)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_status(m, o, now_ms);

    cJSON_Delete(o);
    return rc;
}

static int apply_status(struct rift_model *m, const char *json)
{
    return apply_status_at(m, json, rift_mono_ms());
}

static int apply_identity(struct rift_model *m, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_identity(m, o);

    cJSON_Delete(o);
    return rc;
}

/* ---- what RIFT observed survives a snapshot; what the service said does not */
static void test_snapshot_keeps_observations(void)
{
    static struct rift_model m;
    const struct rift_node *n;

    rift_model_init(&m);
    apply_nodes(&m, "{\"nodes\":[{\"public_key\":\"" KEY_B "\",\"name\":\"HYTTA\","
                    "\"path_known\":true,\"hops\":3,\"direct\":false,\"path_hex\":\"a1c2d3\","
                    "\"last_heard_mono_ms\":500}]}");
    apply_event(&m, "mesh.node",
                "{\"reason\":\"path\",\"node\":{\"public_key\":\"" KEY_B "\","
                "\"path_known\":true,\"hops\":5,\"direct\":false,\"path_hex\":\"a1c2d3e4f5\","
                "\"last_heard_mono_ms\":9000,\"last_rssi_dbm\":-88.0,\"last_snr_db\":4.5}}");
    n = rift_model_find(&m, KEY_B);
    check("two paths and one event are known before the periodic snapshot",
          n && n->hist_count == 2 && n->observations == 1);
    /* A route change is dated when this app saw it. The node's last-heard
     * time is when the service last heard ANYTHING from it - an advert on
     * the old route, often - and dating the change by it put route changes
     * minutes or hours before they happened. */
    {
        int64_t now = rift_mono_ms();

        check("a route change is dated when it was seen, not when the node was last heard",
              n && n->hist[0].have_mono && n->hist[0].mono_ms != 9000 &&
                  n->hist[0].mono_ms <= now && now - n->hist[0].mono_ms < 5000);
    }

    /* The periodic snapshot (every RIFT_NODES_PERIOD_MS) names the same node
     * on the same path. It used to wipe RIFT's history of it every time. */
    apply_nodes(&m, "{\"nodes\":[{\"public_key\":\"" KEY_B "\",\"name\":\"HYTTA\","
                    "\"path_known\":true,\"hops\":5,\"direct\":false,\"path_hex\":\"a1c2d3e4f5\","
                    "\"last_heard_mono_ms\":9000}]}");
    n = rift_model_find(&m, KEY_B);
    check("the path history survives a snapshot of the same node", n && n->hist_count == 2);
    check("newest first, as it was", n && n->hist[0].hops == 5 && n->hist[1].hops == 3);
    check("and so does the count of events that named it", n && n->observations == 1);
    /* What the service says is replaced outright, and an absent value is
     * absent: this snapshot carried no signal, so none is shown - not the
     * one an earlier event carried, which the service no longer vouches for. */
    check("a signal the snapshot does not carry is not kept from before",
          n && !n->have_rssi && !n->have_snr);

    apply_nodes(&m, "{\"nodes\":[{\"public_key\":\"" KEY_B "\",\"name\":\"HYTTA\","
                    "\"path_known\":true,\"hops\":1,\"direct\":false,\"path_hex\":\"e1\","
                    "\"last_heard_mono_ms\":12000}]}");
    n = rift_model_find(&m, KEY_B);
    check("a route that changed where no event was seen is recorded from the snapshot",
          n && n->hist_count == 3 && n->hist[0].hops == 1);

    apply_nodes(&m, "{\"nodes\":[{\"public_key\":\"" KEY_A "\",\"name\":\"OSLO-01\"}]}");
    check("a node the snapshot does not name is gone, history and all",
          rift_model_find(&m, KEY_B) == NULL && m.node_count == 1);
    apply_nodes(&m, "{\"nodes\":[{\"public_key\":\"" KEY_A "\",\"name\":\"OSLO-01\"},"
                    "{\"public_key\":\"" KEY_B "\",\"name\":\"HYTTA\",\"path_known\":false}]}");
    n = rift_model_find(&m, KEY_B);
    check("and one that comes back starts a history of its own",
          n && n->hist_count == 1 && n->observations == 0);
}

/* ---- finding nodes: a question asked of the list, never a change to it ----- */
#define KEY_R "71aa000000000000000000000000000000000000000000000000000000000071"
#define KEY_S "72bb000000000000000000000000000000000000000000000000000000000072"
static void test_find_and_zero_hop(void)
{
    static struct rift_model m;
    const struct rift_node *list[8];
    const struct rift_node *n;
    int count;

    rift_model_init(&m);
    apply_nodes(&m, "{\"nodes\":["
                    "{\"public_key\":\"" KEY_A "\",\"name\":\"OSLO-01\",\"type\":1,"
                    "\"path_known\":true,\"hops\":0,\"direct\":true,\"last_heard_mono_ms\":100},"
                    "{\"public_key\":\"" KEY_B "\",\"name\":\"S\xC3\xB8rlandet HYTTA\",\"type\":2,"
                    "\"path_known\":true,\"hops\":2,\"direct\":false,\"path_hex\":\"a1c3\","
                    "\"advert_hops\":2,\"advert_mono_ms\":90,\"last_heard_mono_ms\":90},"
                    "{\"public_key\":\"" KEY_R "\",\"name\":\"RPT-NORD\",\"type\":2,"
                    "\"path_known\":false,\"advert_hops\":0,\"advert_mono_ms\":80,"
                    "\"last_heard_mono_ms\":80},"
                    "{\"public_key\":\"" KEY_S "\",\"name\":\"RPT-SYD\",\"type\":2,"
                    "\"path_known\":false,\"last_heard_mono_ms\":70},"
                    "{\"public_key\":\"" KEY_C "\",\"type\":1,\"path_known\":false}]}");
    n = rift_model_find(&m, KEY_R);
    check("advert_hops is read, with when it was heard",
          n && n->have_advert_hops && n->advert_hops == 0 && n->have_advert_mono &&
              n->advert_mono_ms == 80);
    n = rift_model_find(&m, KEY_S);
    check("and left absent when the service did not say: absent is not 0",
          n && !n->have_advert_hops);
    {
        static struct rift_model w;

        rift_model_init(&w);
        apply_nodes(&w, "{\"nodes\":[{\"public_key\":\"" KEY_A "\",\"advert_hops\":-1},"
                        "{\"public_key\":\"" KEY_B "\",\"advert_hops\":64}]}");
        check("a hop count MeshCore cannot carry is not taken",
              rift_model_find(&w, KEY_A) && !rift_model_find(&w, KEY_A)->have_advert_hops &&
                  rift_model_find(&w, KEY_B) && !rift_model_find(&w, KEY_B)->have_advert_hops);
    }

    n = rift_model_find(&m, KEY_A);
    check("an empty query matches everything", rift_node_matches(n, "") &&
                                                  rift_node_matches(n, NULL) &&
                                                  rift_node_matches(n, "   "));
    check("part of a name matches, whatever its case", rift_node_matches(n, "slo-0") &&
                                                          rift_node_matches(n, "OsLo"));
    check("the spaces around a query do not count", rift_node_matches(n, "  oslo  "));
    check("a name it does not hold does not", !rift_node_matches(n, "bergen"));
    n = rift_model_find(&m, KEY_B);
    check("a capital \xC3\x98 finds a small \xC3\xB8", rift_node_matches(n, "S\xC3\x98RLANDET"));
    check("and a small one a small one", rift_node_matches(n, "s\xC3\xB8r"));
    check("an O is not an \xC3\x98", !rift_node_matches(n, "sorlandet"));
    check("two hex characters find a key by its start", rift_node_matches(n, "b2") &&
                                                           rift_node_matches(n, "B2CAFE"));
    check("but not from its middle", !rift_node_matches(n, "cafe1e"));
    check("one hex character is not a search for a key", !rift_node_matches(n, "b"));
    n = rift_model_find(&m, KEY_C);
    check("a node with no name is found by its hash", rift_node_matches(n, "c3be") &&
                                                         !rift_node_matches(n, "x"));
    check("and a NULL node by nothing", !rift_node_matches(NULL, ""));

    check("a repeater heard by advert with no relay is zero-hop",
          rift_node_zero_hop(rift_model_find(&m, KEY_R)));
    check("a node with a direct route is zero-hop", rift_node_zero_hop(rift_model_find(&m, KEY_A)));
    check("two relays out is not", !rift_node_zero_hop(rift_model_find(&m, KEY_B)));
    check("nor a node nothing was observed of",
          !rift_node_zero_hop(rift_model_find(&m, KEY_S)) &&
              !rift_node_zero_hop(rift_model_find(&m, KEY_C)));
    check("a repeater is type 2", rift_node_is_repeater(rift_model_find(&m, KEY_R)) &&
                                      !rift_node_is_repeater(rift_model_find(&m, KEY_A)) &&
                                      !rift_node_is_repeater(rift_model_find(&m, KEY_C)));

    count = rift_model_order(&m, 1000, list, 8);
    check("the whole list is five", count == 5);
    count = rift_node_filter(list, count, "", 1);
    check("the zero-hop view keeps the repeater heard straight, and only it",
          count == 1 && strcmp(list[0]->key, KEY_R) == 0);
    count = rift_model_order(&m, 1000, list, 8);
    count = rift_node_filter(list, count, "rpt", 0);
    check("a search keeps the matches in list order, each once",
          count == 2 && strcmp(list[0]->key, KEY_R) == 0 && strcmp(list[1]->key, KEY_S) == 0);
    count = rift_model_order(&m, 1000, list, 8);
    count = rift_node_filter(list, count, "zzz", 0);
    check("nothing matching is an empty list, not an error", count == 0);
    check("and nothing in the cache moved", m.node_count == 5 &&
                                               strcmp(rift_model_find(&m, KEY_A)->name, "OSLO-01") == 0);
}

/* ---- NET: rings of observed hop count, and nothing inferred ---------------- */
static void test_net_rings(void)
{
    static struct rift_model m;
    static struct rift_net net;
    const struct rift_node *path[8];
    enum rift_net_source src;
    static char json[8192];
    int i;

    rift_model_init(&m);
    rift_net_build(&m, 1000, &net);
    check("an empty cache places nobody", net.nodes == 0 && net.deepest == 0);
    apply_nodes(&m, "{\"nodes\":["
                    "{\"public_key\":\"" KEY_A "\",\"name\":\"OSLO-01\",\"path_known\":true,"
                    "\"hops\":0,\"direct\":true,\"last_heard_mono_ms\":900},"
                    "{\"public_key\":\"" KEY_B "\",\"name\":\"HYTTA\",\"path_known\":true,"
                    "\"hops\":2,\"direct\":false,\"path_hex\":\"a1c3\",\"advert_hops\":5,"
                    "\"last_heard_mono_ms\":800},"
                    "{\"public_key\":\"" KEY_C "\",\"name\":\"NO-3241\",\"path_known\":false,"
                    "\"advert_hops\":1,\"last_heard_mono_ms\":700},"
                    "{\"public_key\":\"" KEY_R "\",\"name\":\"FAR\",\"path_known\":true,"
                    "\"hops\":12,\"direct\":false},"
                    "{\"public_key\":\"" KEY_S "\",\"name\":\"NONE\",\"path_known\":false}]}");
    check("a direct route is ring 1, placed by the route",
          rift_net_ring_of(rift_model_find(&m, KEY_A), &src) == 1 && src == RIFT_NET_SOURCE_ROUTE);
    check("a learned route wins over the last advert's count",
          rift_net_ring_of(rift_model_find(&m, KEY_B), &src) == 3 && src == RIFT_NET_SOURCE_ROUTE);
    check("with no route, the advert places it, and says so",
          rift_net_ring_of(rift_model_find(&m, KEY_C), &src) == 2 && src == RIFT_NET_SOURCE_ADVERT);
    check("twelve relays is the last counted ring",
          rift_net_ring_of(rift_model_find(&m, KEY_R), NULL) == RIFT_NET_RING_LAST_HOPS);
    check("nothing observed is NO PATH, never ring 1",
          rift_net_ring_of(rift_model_find(&m, KEY_S), &src) == RIFT_NET_RING_NO_PATH &&
              src == RIFT_NET_SOURCE_NONE);
    check("and nobody is ever placed on ring 0", rift_net_ring_of(NULL, NULL) != 0);

    rift_net_build(&m, 1000, &net);
    check("every node is placed once", net.nodes == 5 && net.ring[1].count == 1 &&
                                          net.ring[2].count == 1 && net.ring[3].count == 1 &&
                                          net.ring[9].count == 1 && net.ring[10].count == 1);
    check("ring 0 is this device's, from the identity", net.ring[0].count == 0);
    check("the counts say which observation placed them",
          net.ring[2].advert == 1 && net.ring[2].route == 0 && net.ring[3].route == 1);
    check("the deepest ring holding a node", net.deepest == 9 && net.hop_known == 4);
    text_is("ring words", rift_net_ring_word(1), "DIRECT");
    text_is("the last counted ring", rift_net_ring_word(9), "9+");
    text_is("NO PATH", rift_net_ring_word(10), "NO PATH");

    /* HYTTA's route is a1 c3: OSLO-01 and NO-3241, nearest first. */
    check("a route's relays are named by the nodes that answer to them",
          rift_net_path_nodes(&m, rift_model_find(&m, KEY_B), path, 8) == 2 &&
              strcmp(path[0]->key, KEY_A) == 0 && strcmp(path[1]->key, KEY_C) == 0);
    check("a node with no route runs through nobody",
          rift_net_path_nodes(&m, rift_model_find(&m, KEY_C), path, 8) == 0);
    /* A second node starting a1: the hop names neither. */
    apply_event(&m, "mesh.node",
                "{\"reason\":\"discovered\",\"node\":{\"public_key\":\"a1ff000000000000000000000000000000"
                "000000000000000000000000000001\",\"name\":\"TWIN\",\"path_known\":false}}");
    check("an ambiguous hop names nobody rather than a guess",
          rift_net_path_nodes(&m, rift_model_find(&m, KEY_B), path, 8) == 1 &&
              strcmp(path[0]->key, KEY_C) == 0);

    /* A crowded ring is bounded. */
    rift_model_init(&m);
    {
        size_t at = 0;

        at += (size_t)snprintf(json + at, sizeof(json) - at, "{\"nodes\":[");
        for (i = 0; i < 30; i++) {
            at += (size_t)snprintf(json + at, sizeof(json) - at,
                                   "%s{\"public_key\":\"%02x00000000000000000000000000000000000000"
                                   "0000000000000000000000ab\",\"path_known\":true,\"hops\":0,"
                                   "\"direct\":true}",
                                   i ? "," : "", 0x20 + i);
        }
        snprintf(json + at, sizeof(json) - at, "]}");
    }
    apply_nodes(&m, json);
    rift_net_build(&m, 1000, &net);
    check("a ring of thirty draws a bounded number and counts the rest",
          net.ring[1].count == 30 && net.ring[1].shown == RIFT_NET_RING_SHOWN);
}

/* ---- channel keys: made or checked here, kept nowhere --------------------- */
static void hex_of(const uint8_t *b, size_t n, char *out)
{
    size_t i;

    for (i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", b[i]);
    }
    out[2 * n] = '\0';
}

static void test_keys(void)
{
    uint8_t d[32];
    char hex[65];
    char b64[RIFT_KEY_B64_MAX];
    char b64b[RIFT_KEY_B64_MAX];
    char name[40];
    char why[160];
    int i;

    rift_sha256((const uint8_t *)"abc", 3, d);
    hex_of(d, 32, hex);
    text_is("SHA-256 of abc (FIPS 180-2)", hex,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    rift_sha256((const uint8_t *)"", 0, d);
    hex_of(d, 32, hex);
    text_is("SHA-256 of nothing", hex,
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    rift_sha256((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    hex_of(d, 32, hex);
    text_is("SHA-256 across two blocks", hex,
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* The vector upstream documents for hashtag channels
     * (MyMesh::addGroupChannelHashtag): "#test" keys 9cd8fcf2...b73f. */
    check("a hashtag channel's key is derived as upstream derives it",
          rift_hashtag_key("#test", b64, sizeof(b64)) == 0);
    text_is("the #test key", b64, "nNj88ipHMztZHZaiuEi3Pw==");
    check("and only from a canonical name", rift_hashtag_key("test", b64, sizeof(b64)) != 0);
    check("a typed name gets its #", rift_hashtag_name("test", name, sizeof(name), why,
                                                       sizeof(why)) == 0 &&
                                         strcmp(name, "#test") == 0);
    check("one, never two", rift_hashtag_name("  #oslo ", name, sizeof(name), why,
                                              sizeof(why)) == 0 &&
                                strcmp(name, "#oslo") == 0);
    check("a bare # is no name", rift_hashtag_name("#", name, sizeof(name), why, sizeof(why)) != 0);
    check("thirty characters and the # is the most",
          rift_hashtag_name("abcdefghijabcdefghijabcdefghij", name, sizeof(name), why,
                            sizeof(why)) == 0 &&
              strlen(name) == 31);
    check("one more would key a different channel, so it is refused",
          rift_hashtag_name("abcdefghijabcdefghijabcdefghijk", name, sizeof(name), why,
                            sizeof(why)) != 0 &&
              strstr(why, "31 bytes") != NULL);

    check("a random key is made", rift_random_key(b64, sizeof(b64)) == 0 && strlen(b64) == 24);
    check("and is one a channel will take", rift_key_check(b64, b64b, sizeof(b64b), why,
                                                           sizeof(why)) == 0);
    check("and a second is not the first", rift_random_key(b64b, sizeof(b64b)) == 0 &&
                                               strcmp(b64, b64b) != 0);

    check("a pasted 16-byte key is taken, spaces around it ignored",
          rift_key_check("  nNj88ipHMztZHZaiuEi3Pw== ", b64, sizeof(b64), why, sizeof(why)) == 0 &&
              strcmp(b64, "nNj88ipHMztZHZaiuEi3Pw==") == 0);
    check("Public's well-known key is taken",
          rift_key_check("izOH6cXN6mrJ5e26oRXNcg==", b64, sizeof(b64), why, sizeof(why)) == 0);
    check("a 32-byte key is taken",
          rift_key_check("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=", b64, sizeof(b64), why,
                         sizeof(why)) == 0);
    check("an empty key is asked for", rift_key_check("", b64, sizeof(b64), why, sizeof(why)) != 0);
    check("a character outside base64 is refused, not read as a different key",
          rift_key_check("nNj88ipHMztZHZaiuEi3P!==", b64, sizeof(b64), why, sizeof(why)) != 0);
    check("missing padding is refused",
          rift_key_check("nNj88ipHMztZHZaiuEi3Pw", b64, sizeof(b64), why, sizeof(why)) != 0);
    check("so are bits padding should have dropped",
          rift_key_check("nNj88ipHMztZHZaiuEi3Px==", b64, sizeof(b64), why, sizeof(why)) != 0);
    check("12 bytes is not a key",
          rift_key_check("AAECAwQFBgcICQoL", b64, sizeof(b64), why, sizeof(why)) != 0 &&
              strstr(why, "16 or 32") != NULL);
    check("all zero is an empty slot, not a key",
          rift_key_check("AAAAAAAAAAAAAAAAAAAAAA==", b64, sizeof(b64), why, sizeof(why)) != 0);
    check("a 32-byte key with an empty upper half is refused as ambiguous",
          rift_key_check("AQIDBAUGBwgJCgsMDQ4PEAAAAAAAAAAAAAAAAAAAAAA=", b64, sizeof(b64), why,
                         sizeof(why)) != 0 &&
              strstr(why, "second half") != NULL);
    check("padding inside a key is refused",
          rift_key_check("nNj8=ipHMztZHZaiuEi3Pw==", b64, sizeof(b64), why, sizeof(why)) != 0);

    check("a channel name is taken", rift_channel_name_check("SITE \xC3\x98st", why, sizeof(why)) == 0);
    check("an empty one is not", rift_channel_name_check("", why, sizeof(why)) != 0);
    check("nor one of spaces", rift_channel_name_check("   ", why, sizeof(why)) != 0);
    check("nor one on two lines", rift_channel_name_check("a\nb", why, sizeof(why)) != 0);
    for (i = 0; i < 32; i++) {
        name[i] = 'x';
    }
    name[32] = '\0';
    check("nor 32 bytes", rift_channel_name_check(name, why, sizeof(why)) != 0);
    name[31] = '\0';
    check("31 is the most", rift_channel_name_check(name, why, sizeof(why)) == 0);
}

/* ---- this node: where its name came from, and the path hash size ------------ */
static void test_identity_and_path_hash(void)
{
    static struct rift_model m;
    cJSON *o;

    rift_model_init(&m);
    apply_identity(&m, "{\"public_key\":\"" KEY_SELF "\",\"node_hash\":\"5f\",\"name\":\"K230-A\","
                       "\"name_source\":\"config\",\"name_max\":31}");
    check("a name from the command line says so", m.self_name_source == RIFT_NAME_SOURCE_CONFIG &&
                                                      m.self_name_max == 31);
    apply_identity(&m, "{\"public_key\":\"" KEY_SELF "\",\"name\":\"K230-A\","
                       "\"name_source\":\"stored\"}");
    check("a stored one", m.self_name_source == RIFT_NAME_SOURCE_STORED && m.self_name_max == 0);
    apply_identity(&m, "{\"public_key\":\"" KEY_SELF "\",\"name\":\"K230-A\"}");
    check("an older service that does not say is unknown, not stored",
          m.self_name_source == RIFT_NAME_SOURCE_UNKNOWN);

    check("no path hash size until the service says", !m.have_path_hash);
    o = cJSON_Parse("{\"bytes\":2,\"allowed\":[1,2,3],\"default\":1}");
    check("a path hash size is read", rift_model_apply_path_hash(&m, o) == 0 &&
                                          m.have_path_hash && m.path_hash_bytes == 2 &&
                                          m.path_hash_allowed == ((1u << 1) | (1u << 2) | (1u << 3)));
    cJSON_Delete(o);
    o = cJSON_Parse("{\"bytes\":2.5}");
    check("a size that is not a whole number is refused", rift_model_apply_path_hash(&m, o) != 0 &&
                                                              m.path_hash_bytes == 2);
    cJSON_Delete(o);
    o = cJSON_Parse("{\"bytes\":9}");
    check("and one MeshCore cannot encode", rift_model_apply_path_hash(&m, o) != 0);
    cJSON_Delete(o);

    check("the four management actions share one slot",
          rift_model_action_slot(&m, RIFT_ACTION_CHANNEL_ADD) == &m.manage_op &&
              rift_model_action_slot(&m, RIFT_ACTION_CHANNEL_REMOVE) == &m.manage_op &&
              rift_model_action_slot(&m, RIFT_ACTION_RENAME) == &m.manage_op &&
              rift_model_action_slot(&m, RIFT_ACTION_PATH_HASH) == &m.manage_op);
    check("and not the advert's or a node's",
          rift_model_action_slot(&m, RIFT_ACTION_ADVERT_NEAR) != &m.manage_op &&
              rift_model_action_slot(&m, RIFT_ACTION_FORGET) != &m.manage_op);
    check("one at a time", rift_model_action_begin(&m, RIFT_ACTION_RENAME, NULL, "Ny", 1) == 0 &&
                               rift_model_action_begin(&m, RIFT_ACTION_CHANNEL_ADD, NULL, "#x", 2) != 0);
    check("an advert is not held up by it",
          rift_model_action_begin(&m, RIFT_ACTION_ADVERT_NEAR, NULL, NULL, 2) == 0);
    rift_model_service_lost(&m, "gone");
    check("a management request with no answer is said to have none",
          !m.manage_op.active && m.manage_op.failed && m.manage_op.unknown &&
              m.manage_op.kind == RIFT_ACTION_RENAME);
    check("and the path hash size is the service's to say again", !m.have_path_hash);
    check("the label is the name asked for, never a key", strcmp(m.manage_op.label, "Ny") == 0);
}

/* ---- a reply is not an event, and a removal is not an update ---------------- */
static void test_replies_and_removals(void)
{
    static struct rift_model m;
    const struct rift_node *n;
    cJSON *o;
    unsigned malformed;

    rift_model_init(&m);
    apply_nodes(&m, "{\"nodes\":[{\"public_key\":\"" KEY_A "\",\"name\":\"OSLO-01\"},"
                    "{\"public_key\":\"" KEY_B "\",\"name\":\"HYTTA\"}]}");
    o = cJSON_Parse("{\"public_key\":\"" KEY_B "\",\"name\":\"HYTTA\",\"path_known\":true,"
                    "\"hops\":2,\"direct\":false,\"path_hex\":\"a1c2\"}");
    check("mesh.node's answer is filed", rift_model_apply_node_reply(&m, o) == 0);
    cJSON_Delete(o);
    n = rift_model_find(&m, KEY_B);
    check("as an update to the row that was there", m.node_count == 2 && n && n->hops == 2);
    check("and not counted as an event that named the node", n && n->observations == 0);
    check("a reply with no whole key is refused",
          rift_model_apply_node_reply(&m, NULL) == -1);

    malformed = m.events_malformed;
    check("a removal is applied",
          apply_event(&m, "mesh.node",
                      "{\"reason\":\"removed\",\"node\":{\"public_key\":\"" KEY_B "\","
                      "\"name\":\"HYTTA\",\"path_known\":true,\"hops\":2}}") == 0);
    /* The node that comes with the reason is the node as it WAS. Applying it
     * as an update would put back the very row the reader asked to be rid of. */
    check("and takes the node off the list rather than updating it",
          rift_model_find(&m, KEY_B) == NULL && m.node_count == 1);
    check("a removal of a node never held changes nothing and is no fault",
          apply_event(&m, "mesh.node",
                      "{\"reason\":\"removed\",\"node\":{\"public_key\":\"" KEY_C "\"}}") == 0 &&
              m.node_count == 1 && m.events_malformed == malformed);
    check("a removal with no usable key is refused",
          apply_event(&m, "mesh.node", "{\"reason\":\"removed\",\"node\":{\"name\":\"x\"}}") ==
                  -1 &&
              m.events_malformed == malformed + 1);
    check("dropping a node by key says whether it was held",
          rift_model_drop_node(&m, KEY_A) == 1 && rift_model_drop_node(&m, KEY_A) == 0 &&
              m.node_count == 0);
}

/* ---- the counters the service keeps, read as it words them ------------------ */
static void test_traffic_counters(void)
{
    static struct rift_model m;

    rift_model_init(&m);
    apply_status(&m, "{\"state\":\"online\",\"counters\":{\"rx_events\":10}}");
    check("no transmit counters reported is not zero transmits", !m.have_traffic);
    check("nor a table that was never reported full", !m.have_contacts_full);
    apply_status(&m, "{\"state\":\"online\",\"counters\":{\"rx_events\":2627,"
                     "\"tx_ok\":6,\"tx_failed\":1,\"tx_unknown\":2,\"tx_rx_resume_failed\":3,"
                     "\"sent_flood\":4,\"sent_direct\":5,\"recv_flood\":70,\"recv_direct\":8,"
                     "\"contacts_full\":3,\"nodes_unretained\":5}}");
    check("the transmit outcomes are kept apart, as the service words them",
          m.have_traffic && m.tx_ok == 6 && m.tx_failed == 1 && m.tx_unknown == 2 &&
              m.tx_rx_resume_failed == 3);
    check("and so are flood and direct, both ways",
          m.sent_flood == 4 && m.sent_direct == 5 && m.recv_flood == 70 && m.recv_direct == 8);
    check("a full contact table is kept with how often it was full",
          m.have_contacts_full && m.contacts_full == 3 && m.nodes_unretained == 5);
}

/* ---- "the table is full" is about now, not about the whole run ------------ */
static void test_unretained_since_forget(void)
{
    static struct rift_model m;

    rift_model_init(&m);
    apply_status(&m, "{\"state\":\"online\",\"counters\":{\"nodes_unretained\":5}}");
    check("adverts turned away with nothing forgotten since are recent",
          rift_model_unretained_recent(&m) == 5);

    /* The reader makes room: the service's cumulative counter does not move,
     * and the five it counts were turned away from a table that has room. */
    apply_nodes(&m, "{\"count\":1,\"nodes\":[{\"public_key\":\"" KEY_A "\",\"name\":\"OSLO-01\"}]}");
    rift_model_action_begin(&m, RIFT_ACTION_FORGET, KEY_A, "OSLO-01", 1000);
    rift_model_action_done(&m, RIFT_ACTION_FORGET, 1100);
    check("a forget answered makes room, and they stop counting",
          rift_model_unretained_recent(&m) == 0 && m.nodes_unretained == 5);
    apply_status(&m, "{\"state\":\"online\",\"counters\":{\"nodes_unretained\":5}}");
    check("the same count read again does not bring them back",
          rift_model_unretained_recent(&m) == 0);
    apply_status(&m, "{\"state\":\"online\",\"counters\":{\"nodes_unretained\":7}}");
    check("adverts turned away after it do count", rift_model_unretained_recent(&m) == 2);

    /* Another client forgets a node: the event says so as well. */
    apply_event(&m, "mesh.node",
                "{\"reason\":\"removed\",\"node\":{\"public_key\":\"" KEY_A "\"}}");
    check("a removed event makes room the same way", rift_model_unretained_recent(&m) == 0);
    apply_status(&m, "{\"state\":\"online\",\"counters\":{\"nodes_unretained\":8}}");
    check("and counts from there", rift_model_unretained_recent(&m) == 1);

    /* A new run of the service counts from nothing: a counter that went down
     * cannot be under a baseline set by the old run. */
    apply_status(&m, "{\"state\":\"online\",\"counters\":{\"nodes_unretained\":2}}");
    check("a counter that went backwards is a new run, all of it recent: nothing "
          "has been forgotten in that run",
          rift_model_unretained_recent(&m) == 2 && m.nodes_unretained == 2);
    apply_status(&m, "{\"state\":\"online\",\"counters\":{\"nodes_unretained\":3}}");
    check("and the new run's next refusal counts", rift_model_unretained_recent(&m) == 3);
    check("a model that is not there has turned nothing away",
          rift_model_unretained_recent(NULL) == 0);
}

/* ---- an advert, or a change to a node: asked, then answered --------------- */
static void test_actions(void)
{
    static struct rift_model m;
    const struct rift_action_state *s;
    char text[RIFT_ACTION_TEXT_MAX];

    rift_model_init(&m);
    check("nothing has been asked", !rift_model_action_busy(&m, RIFT_ACTION_ADVERT_NEAR) &&
                                        !rift_model_action_busy(&m, RIFT_ACTION_FORGET));
    rift_fmt_action(&m.advert, 0, text, sizeof(text));
    text_is("and there is nothing to say about it", text, "");

    check("a zero-hop advert is asked for",
          rift_model_action_begin(&m, RIFT_ACTION_ADVERT_NEAR, NULL, NULL, 1000) == 0);
    check("and is in flight", rift_model_action_busy(&m, RIFT_ACTION_ADVERT_MESH));
    check("a second advert is refused while it is",
          rift_model_action_begin(&m, RIFT_ACTION_ADVERT_MESH, NULL, NULL, 1100) == -1);
    rift_fmt_action(&m.advert, 1200, text, sizeof(text));
    text_is("which claims only that it was asked", text, "ZERO-HOP ADVERT \xC2\xB7 ASKED\xE2\x80\xA6");
    check("the advert and a node change are separate slots",
          rift_model_action_begin(&m, RIFT_ACTION_RESET_PATH, KEY_B, "HYTTA", 1300) == 0);
    rift_model_action_done(&m, RIFT_ACTION_ADVERT_NEAR, 4000);
    s = rift_model_action_of(&m, RIFT_ACTION_ADVERT_NEAR);
    check("an answer settles it", s && s->done && !s->active && !s->failed);
    rift_fmt_action(s, 16000, text, sizeof(text));
    /* ACCEPTED, never SENT: the service queued it, and how the transmit went
     * is the activity feed's to say. */
    text_is("as accepted, with how long ago", text,
            "ZERO-HOP ADVERT \xC2\xB7 ACCEPTED 12s AGO");
    rift_model_action_done(&m, RIFT_ACTION_ADVERT_NEAR, 5000);
    check("an answer with nothing in flight changes nothing", s->mono_ms == 4000);

    check("a node change is still in flight", rift_model_action_busy(&m, RIFT_ACTION_FORGET));
    check("so a forget is refused until it is answered",
          rift_model_action_begin(&m, RIFT_ACTION_FORGET, KEY_B, "HYTTA", 1400) == -1);
    rift_model_action_failed(&m, RIFT_ACTION_RESET_PATH, "no node with that public key", 1500);
    s = rift_model_action_of(&m, RIFT_ACTION_RESET_PATH);
    rift_fmt_action(s, 1600, text, sizeof(text));
    text_is("a refusal says so, in the service's words", text,
            "RE-ROUTE \xC2\xB7 NOT DONE: no node with that public key");
    check("a node change needs a whole key",
          rift_model_action_begin(&m, RIFT_ACTION_FORGET, "b2ca", "HYTTA", 1700) == -1);
    check("and is not an advert",
          rift_model_action_begin(&m, RIFT_ACTION_NONE, NULL, NULL, 1700) == -1);
    check("with one, a forget is asked for",
          rift_model_action_begin(&m, RIFT_ACTION_FORGET, KEY_B, "HYTTA", 1800) == 0);
    text_is("naming the node", m.node_op.key, KEY_B);

    /* The service goes away with the forget and an advert both unanswered:
     * nothing on this side knows whether either happened. */
    rift_model_action_begin(&m, RIFT_ACTION_ADVERT_MESH, NULL, NULL, 1900);
    rift_model_service_lost(&m, "meshcored closed the connection");
    check("an unanswered forget is not called done",
          m.node_op.failed && !m.node_op.done && !m.node_op.active);
    check("and says it may or may not have happened",
          strstr(m.node_op.error, "may or may not") != NULL);
    check("nor is an unanswered advert called sent",
          m.advert.failed && !m.advert.done && strstr(m.advert.error, "may or may not"));
    /* "NOT DONE" in front of "may or may not" would contradict itself: the
     * line says there was no answer, which is all that is known. */
    rift_fmt_action(&m.node_op, 2000, text, sizeof(text));
    check("and the line says there was no answer, not that it was not done",
          strncmp(text, "FORGET \xC2\xB7 NO ANSWER: ", 20) == 0 && strstr(text, "NOT DONE") == NULL);
    rift_fmt_action(&m.advert, 2000, text, sizeof(text));
    check("the advert's the same", strstr(text, "\xC2\xB7 NO ANSWER: ") != NULL);
    rift_model_action_failed(&m, RIFT_ACTION_ADVERT_MESH, "radio busy", 2100);
    rift_fmt_action(&m.advert, 2200, text, sizeof(text));
    text_is("while a refusal that did come back is still NOT DONE", text,
            "FLOOD ADVERT \xC2\xB7 NOT DONE: radio busy");
    rift_model_action_clear(&m, RIFT_ACTION_FORGET);
    check("a settled request can be put away", m.node_op.kind == RIFT_ACTION_NONE);
}

int main(void)
{
    struct rift_model m;
    const struct rift_node *n;
    const struct rift_node *order[RIFT_MAX_NODES];
    const struct rift_activity *act;
    int count;
    int i;

    /* ---- an empty model knows nothing, and says so ---------------------- */
    rift_model_init(&m);
    check("an empty model holds no nodes", m.node_count == 0);
    check("has no identity", !m.have_identity);
    check("has read no snapshot", !m.snapshot_valid);
    check("and its service state is unknown, not absent",
          m.state == RIFT_SVC_UNKNOWN && !m.stale);

    /* ---- the initial snapshot -------------------------------------------- */
    check("a snapshot is taken",
          apply_nodes(&m, "{\"count\":2,\"nodes\":["
                          "{\"public_key\":\"" KEY_A "\",\"node_hash\":\"a1\",\"name\":\"OSLO-01\","
                          "\"type\":1,\"path_known\":true,\"hops\":0,\"direct\":true,"
                          "\"last_heard_mono_ms\":1000,\"last_rssi_dbm\":-71.0,"
                          "\"last_snr_db\":9.5},"
                          "{\"public_key\":\"" KEY_B "\",\"node_hash\":\"b2\",\"name\":\"HYTTA\","
                          "\"type\":2,\"path_known\":true,\"hops\":3,\"direct\":false,"
                          "\"path_hex\":\"a1c2d3\",\"last_heard_mono_ms\":500}]}") == 0);
    check("both nodes are held", m.node_count == 2);
    check("and the snapshot is now valid", m.snapshot_valid && !m.stale);

    n = rift_model_find(&m, KEY_A);
    check("a node is found by its public key", n != NULL);
    text_is("with the name the service gave", n->name, "OSLO-01");
    check("its measured RSSI is kept", n->have_rssi && n->rssi_dbm < -70.0);
    check("its measured SNR is kept", n->have_snr);
    check("zero relays is direct", n->path_known && n->direct && n->hops == 0);
    check("and an unreported advert time stays unreported", !n->have_advert);

    n = rift_model_find(&m, KEY_B);
    check("the relayed node is held too", n != NULL);
    check("with its hop count", n->hops == 3 && !n->direct);
    /* The one that matters: this node's frame carried no signal metadata,
     * and nothing here turns that into 0 dBm at full scale. */
    check("an RSSI the service did not report is absent, not zero", !n->have_rssi);
    check("and so is the SNR", !n->have_snr);

    /* ---- a duplicate updates; it does not duplicate ---------------------- */
    check("an event for a node already held is applied",
          apply_event(&m, "mesh.node",
                      "{\"reason\":\"path\",\"node\":{\"public_key\":\"" KEY_B "\","
                      "\"node_hash\":\"b2\",\"name\":\"HYTTA\",\"type\":2,"
                      "\"path_known\":true,\"hops\":5,\"direct\":false,"
                      "\"path_hex\":\"a1c2d3e4f5\",\"last_heard_mono_ms\":9000,"
                      "\"last_rssi_dbm\":-88.0}}") == 0);
    check("and the list is still two nodes long", m.node_count == 2);
    n = rift_model_find(&m, KEY_B);
    check("the node's path is the new one", n->hops == 5);
    text_is("with the new bytes", n->path_hex, "a1c2d3e4f5");
    check("its signal is now known", n->have_rssi);
    check("and RIFT has counted the observation", n->observations == 1);

    /* Three more events for the same node, one of them the same path. */
    apply_event(&m, "mesh.node",
                "{\"reason\":\"discovered\",\"node\":{\"public_key\":\"" KEY_B "\","
                "\"path_known\":true,\"hops\":5,\"direct\":false,\"path_hex\":\"a1c2d3e4f5\","
                "\"last_heard_mono_ms\":9500}}");
    check("a repeated event still does not add a row", m.node_count == 2);
    n = rift_model_find(&m, KEY_B);
    check("but it is counted as another observation", n->observations == 2);
    /* Two paths so far: the snapshot's 3 hops - a snapshot is a witness to a
     * route too - and the event's 5. The repeat of the 5 is not a third. */
    check("and an unchanged path is not recorded as a change", n->hist_count == 2);
    check("the snapshot's path is in the history, oldest last",
          n->hist[1].hops == 3 && n->hist[0].hops == 5);
    apply_event(&m, "mesh.node",
                "{\"reason\":\"path\",\"node\":{\"public_key\":\"" KEY_B "\","
                "\"path_known\":true,\"hops\":2,\"direct\":false,\"path_hex\":\"a1c2\","
                "\"last_heard_mono_ms\":9800}}");
    check("a different path is", rift_model_find(&m, KEY_B)->hist_count == 3);
    check("newest first", rift_model_find(&m, KEY_B)->hist[0].hops == 2);

    /* A node that has never been heard of arrives as an event. */
    check("an event for an unknown node adds it",
          apply_event(&m, "mesh.node",
                      "{\"reason\":\"discovered\",\"node\":{\"public_key\":\"" KEY_C "\","
                      "\"node_hash\":\"c3\",\"name\":\"RPT-7\",\"type\":2,"
                      "\"path_known\":false}}") == 0);
    check("as a third row", m.node_count == 3);
    n = rift_model_find(&m, KEY_C);
    check("with no route back", !n->path_known);
    check("and no hop count at all", n->hops == 0 && !n->direct);

    /* ---- malformed events are refused whole, and counted ---------------- */
    {
        unsigned before = m.events_malformed;
        int nodes_before = m.node_count;

        check("an event whose data is not an object is refused",
              apply_event(&m, "mesh.node", "[1,2,3]") == -1);
        check("a node with no public key is refused",
              apply_event(&m, "mesh.node", "{\"node\":{\"name\":\"nobody\"}}") == -1);
        check("a key that is not hex is refused",
              apply_event(&m, "mesh.node",
                          "{\"node\":{\"public_key\":\"zzzz\",\"name\":\"nobody\"}}") == -1);
        check("a key of the wrong length is refused",
              apply_event(&m, "mesh.node",
                          "{\"node\":{\"public_key\":\"a1b2\",\"name\":\"nobody\"}}") == -1);
        check("an unknown event name is refused",
              apply_event(&m, "mesh.nonsense", "{}") == -1);
        check("a state event with no state is refused",
              apply_event(&m, "mesh.state", "{\"reason\":\"why\"}") == -1);
        check("an activity with no kind is refused",
              apply_event(&m, "mesh.activity", "{\"bytes\":10}") == -1);
        check("and an activity of a kind this build does not know is too",
              apply_event(&m, "mesh.activity", "{\"kind\":\"telepathy\"}") == -1);
        check("none of them added a node", m.node_count == nodes_before);
        check("and every one of them was counted", m.events_malformed == before + 8);
        /* Phase 1 ignored mesh.message, because COMMS was not drawn, and
         * this asserted that ignoring it was not a fault. COMMS is drawn
         * now, so the assertion has become untrue and is replaced by the
         * one that matters: a message event is applied, and a *malformed*
         * one is still refused and counted like any other.
         *
         * What a message event does with a well-formed message is
         * tests/rift_comms_test.c's subject; this is the event path. */
        check("an empty message is refused",
              apply_event(&m, "mesh.message", "{\"message\":{}}") == -1);
        check("and counted", m.events_malformed == before + 9);
        check("a message with everything it needs is applied",
              apply_event(&m, "mesh.message",
                          "{\"message\":{\"id\":1,\"direction\":\"in\","
                          "\"peer_public_key\":\"" KEY_A "\",\"text\":\"hei\","
                          "\"state\":\"received\",\"mono_ms\":1000}}") == 0);
        check("and is not counted as malformed", m.events_malformed == before + 9);
        check("nor did it become a node", m.node_count == nodes_before);
    }

    /* ---- a snapshot replaces; it does not merge -------------------------- */
    check("a second snapshot is taken",
          apply_nodes(&m, "{\"count\":1,\"nodes\":[{\"public_key\":\"" KEY_A "\","
                          "\"node_hash\":\"a1\",\"name\":\"OSLO-01\",\"path_known\":true,"
                          "\"hops\":0,\"direct\":true,\"last_heard_mono_ms\":1000}]}") == 0);
    check("a node the service no longer holds is gone from here too", m.node_count == 1);
    check("because nobody could be asked about it", rift_model_find(&m, KEY_B) == NULL);
    /* A snapshot carrying one entry this build will not read keeps the
     * rest: one bad entry costs one entry. */
    check("a snapshot with one unusable entry is still taken",
          apply_nodes(&m, "{\"nodes\":[{\"public_key\":\"" KEY_A "\",\"name\":\"OSLO-01\","
                          "\"last_heard_mono_ms\":1000},"
                          "{\"name\":\"no key at all\"},"
                          "{\"public_key\":\"" KEY_B "\",\"name\":\"HYTTA\","
                          "\"last_heard_mono_ms\":2000}]}") == 0);
    check("and holds the two that were usable", m.node_count == 2);
    check("an array that is not there is not a snapshot", apply_nodes(&m, "{\"count\":0}") == -1);
    check("and the nodes it would have replaced are still here", m.node_count == 2);

    /* ---- the cache is bounded -------------------------------------------- */
    {
        char json[512];
        char key[RIFT_KEY_HEX];
        int over = RIFT_MAX_NODES + 8;

        rift_model_init(&m);
        for (i = 0; i < over; i++) {
            snprintf(key, sizeof(key),
                     "%02x%02x000000000000000000000000000000000000000000000000000000000000",
                     i & 0xff, (i >> 8) & 0xff);
            snprintf(json, sizeof(json),
                     "{\"node\":{\"public_key\":\"%s\",\"name\":\"n%d\","
                     "\"path_known\":false,\"last_heard_mono_ms\":%d}}",
                     key, i, 1000 + i);
            apply_event(&m, "mesh.node", json);
        }
        check("the cache never grows past its bound", m.node_count == RIFT_MAX_NODES);
        check("and says how many it had to drop", m.nodes_dropped == (unsigned)(over -
                                                                                RIFT_MAX_NODES));
        /* The ones dropped are the ones heard longest ago. */
        check("the most recently heard node is still held",
              rift_model_find(&m,
                              "4700000000000000000000000000000000000000000000000000000000000000") !=
                  NULL);
    }

    /* ---- a snapshot larger than the cache ----------------------------------
     *
     * meshcored lists its nodes most recently heard first, so the cache must
     * be the head of that list: exactly the newest RIFT_MAX_NODES. Taking
     * every entry would have each one past the bound evict the stalest held
     * node, ending with the newest but one and the single stalest node of
     * all. The cache is now as large as meshcored's own table (256), so the
     * snapshot here is a service that lists more than that - a bigger table
     * one day, or a buggy service - and the bound still has to hold. */
    {
        const int total = RIFT_MAX_NODES + 64;
        size_t cap = (size_t)total * 160 + 64;
        char *json = malloc(cap);
        char key[RIFT_KEY_HEX];
        int shift;

        for (shift = 0; shift <= 10 && json; shift += 10) {
            size_t at = (size_t)snprintf(json, cap, "{\"nodes\":[");
            int held = 1;

            /* Entry i is node (i + shift) % total, heard i seconds before 1e6. */
            for (i = 0; i < total; i++) {
                snprintf(key, sizeof(key), "%04x%060d", (i + shift) % total, 0);
                at += (size_t)snprintf(json + at, cap - at,
                                       "%s{\"public_key\":\"%s\",\"name\":\"n%d\","
                                       "\"path_known\":false,\"last_heard_mono_ms\":%d}",
                                       i ? "," : "", key, i, 1000000 - 1000 * i);
            }
            snprintf(json + at, cap - at, "]}");
            if (shift == 0) {
                rift_model_init(&m);
            }
            check("a snapshot longer than the cache, newest first, is taken",
                  apply_nodes(&m, json) == 0);
            check("into a cache of RIFT_MAX_NODES", m.node_count == RIFT_MAX_NODES);
            for (i = 0; i < RIFT_MAX_NODES; i++) {
                snprintf(key, sizeof(key), "%04x%060d", (i + shift) % total, 0);
                held = held && rift_model_find(&m, key) != NULL;
            }
            check(shift ? "a later snapshot's newest replace the ones that aged out"
                        : "holding exactly the newest RIFT_MAX_NODES",
                  held);
            snprintf(key, sizeof(key), "%04x%060d", (total - 1 + shift) % total, 0);
            check("and not the stalest node the service has", rift_model_find(&m, key) == NULL);
        }
        check("the rest are counted as dropped",
              m.nodes_dropped == 2u * (unsigned)(total - RIFT_MAX_NODES));
        free(json);
    }

    /* ---- the activity ring ------------------------------------------------ */
    rift_model_init(&m);
    check("an rx activity is taken",
          apply_event(&m, "mesh.activity",
                      "{\"kind\":\"rx\",\"payload_type\":\"advert\",\"bytes\":48,"
                      "\"mono_ms\":1000,\"rssi_dbm\":-88.0,\"snr_db\":6.0}") == 0);
    check("a tx activity is taken",
          apply_event(&m, "mesh.activity",
                      "{\"kind\":\"tx\",\"submit_id\":7,\"result\":\"rx_resume_failed\","
                      "\"bytes\":72,\"mono_ms\":2000}") == 0);
    act = rift_model_activity_at(&m, 0);
    check("the newest is first", act != NULL && act->kind == RIFT_ACT_TX);
    /* The result, never the kind: three of the five results a tx can carry
     * are not "it went out". */
    text_is("and a transmit is shown by its result", act->word, "rx_resume_failed");
    check("a transmit carries no signal", !act->have_rssi && !act->have_snr);
    act = rift_model_activity_at(&m, 1);
    check("then the one before it", act != NULL && act->kind == RIFT_ACT_RX);
    text_is("named by its payload type", act->word, "advert");
    check("with the signal that was measured", act->have_rssi && act->have_snr);
    check("there is no third", rift_model_activity_at(&m, 2) == NULL);

    check("an rx with no signal metadata is still taken",
          apply_event(&m, "mesh.activity",
                      "{\"kind\":\"rx\",\"payload_type\":\"text\",\"bytes\":30,"
                      "\"mono_ms\":3000}") == 0);
    act = rift_model_activity_at(&m, 0);
    check("and its signal stays unknown rather than becoming zero",
          !act->have_rssi && !act->have_snr);

    for (i = 0; i < RIFT_MAX_ACTIVITY * 2; i++) {
        apply_event(&m, "mesh.activity", "{\"kind\":\"rx\",\"payload_type\":\"ack\"}");
    }
    check("the feed is a window, not a log", m.activity_count == RIFT_MAX_ACTIVITY);
    check("and counts everything it saw", m.activity_total == RIFT_MAX_ACTIVITY * 2 + 3);
    act = rift_model_activity_at(&m, 0);
    check("an activity with no time is kept with the time unknown", !act->have_mono);

    /* ---- service state ----------------------------------------------------- */
    rift_model_init(&m);
    check("a status is taken",
          apply_status(&m, "{\"state\":\"degraded\",\"reason\":\"the receiver did not come back\","
                           "\"state_since_mono_ms\":500,\"radio\":{\"connected\":true,"
                           "\"lease_held\":true,\"online\":false},\"nodes\":4,"
                           "\"counters\":{\"rx_events\":10,\"nodes_unretained\":2}}") == 0);
    check("with the state it named", m.state == RIFT_SVC_DEGRADED);
    text_is("and the reason in the service's own words", m.reason,
            "the receiver did not come back");
    check("radiod's state word is absent until radiod has said", !m.have_radio_state);
    check("which is not the same as off", m.radio_connected && !m.radio_online);
    check("the service's own node count is kept", m.have_nodes_reported && m.nodes_reported == 4);
    check("and so is what the contact table had no room for", m.nodes_unretained == 2);
    text_is("a degraded service with the radio merely unknown is still degraded",
            rift_model_state_label(&m), "degraded");
    check("and the radio is not called off", !rift_model_radio_off(&m));
    check("a status with no state is not a status", apply_status(&m, "{\"reason\":\"x\"}") == -1);
    check("and the state it had is untouched", m.state == RIFT_SVC_DEGRADED);

    /* The owner switched the radio off (docs/api/mesh.md): degraded, with
     * radiod saying off, is labelled as the choice it is. */
    rift_model_init(&m);
    check("a radio-off status is taken",
          apply_status(&m, "{\"state\":\"degraded\",\"reason\":\"the radio is switched off\","
                           "\"radio\":{\"connected\":true,\"lease_held\":true,\"online\":false,"
                           "\"radio_state\":\"off\"}}") == 0);
    check("radio off is recognised", rift_model_radio_off(&m));
    text_is("and labelled radio off, not degraded", rift_model_state_label(&m), "radio off");
    check("transmit is not ready while off", !m.radio_online);
    check("the lease is still held while off", m.radio_lease_held);
    check("a radio back on is online again",
          apply_status(&m, "{\"state\":\"online\",\"radio\":{\"connected\":true,"
                           "\"lease_held\":true,\"online\":true,\"radio_state\":\"rx\"}}") == 0 &&
              !rift_model_radio_off(&m));
    text_is("and labelled online", rift_model_state_label(&m), "online");
    check("an error with radiod off is still an error, not a choice",
          apply_status(&m, "{\"state\":\"error\",\"radio\":{\"radio_state\":\"off\"}}") == 0);
    text_is("and says error", rift_model_state_label(&m), "error");

    check("every state word the API defines is understood",
          apply_status(&m, "{\"state\":\"waiting_for_lease\"}") == 0 &&
              m.state == RIFT_SVC_WAITING_LEASE);
    check("a state word this build does not know is not guessed at",
          apply_status(&m, "{\"state\":\"transcendent\"}") == 0 &&
              m.state == RIFT_SVC_UNKNOWN);

    /* ---- which run of the service answered -------------------------------- */
    /* meshcored hands out message ids from 1 on every run, so the message
     * cache has to know when the run changed. The signal is mesh.status's
     * uptime, read against the clock the reply was read on. Nothing here
     * empties anything - that is rift_comms_test's half - this is only
     * whether a restart is seen, and whether one is seen that did not
     * happen. */
    {
        struct rift_model s;

        rift_model_init(&s);
        check("nothing is known about the run before a status",
              !s.have_svc_start && s.svc_restarts == 0);
        /* One run, started 3600 s before this status was read. */
        check("the first status names the run it is in",
              apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":3600}", 3600000) == 0);
        check("and is not a restart, because there was nothing to restart from",
              s.svc_restarts == 0 && s.have_svc_start && s.svc_start_ms == 0);

        /* Still the same process. The uptime is truncated to whole seconds,
         * so the derived start drifts up to a second later without anything
         * having happened - which must not read as a restart. */
        apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":3600}", 3600999);
        check("a start that drifts within the second is the same run", s.svc_restarts == 0);
        apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":3601}", 3601000);
        check("and so is the next whole second of it", s.svc_restarts == 0);
        check("with the lowest start seen kept, which is the least wrong one",
              s.svc_start_ms == 0);

        /* A new process: the uptime it reports is smaller than the one
         * before it, which within one run cannot happen. */
        check("a smaller uptime is a restart",
              apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":1}", 3602000) == 0 &&
                  s.svc_restarts == 1);
        check("and the run it belongs to is the new one", s.svc_start_ms == 3601000);
        apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":2}", 3603000);
        check("the run after a restart is a run like any other", s.svc_restarts == 1);
    }
    {
        /* The case the backwards test alone would miss, and the reason
         * there are two: a service that had been up three seconds when it
         * died, replaced ten seconds later by one whose uptime is already
         * larger than the three seconds last seen. Nothing went backwards;
         * the run changed all the same. */
        struct rift_model s;

        rift_model_init(&s);
        apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":3}", 3000);
        check("a short-lived run is a run", s.svc_restarts == 0 && s.svc_start_ms == 0);
        check("its replacement is caught by the start moving, not by the uptime",
              apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":10}", 23000) == 0 &&
                  s.svc_restarts == 1);
        check("and the new run's start is where it really is", s.svc_start_ms == 13000);
    }
    {
        /* Why the lowest start seen in a run is the one kept. The uptime is
         * truncated to whole seconds, so the first status of a run can put
         * its start up to a second later than it really was, and every
         * comparison after it would be measured from there. Here the run
         * really started at 0, the first status says 999, and the second
         * says 0 - and it is the second that makes the replacement visible. */
        struct rift_model s;

        rift_model_init(&s);
        apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":1}", 1999);
        check("the first status of a run can only put its start too late",
              s.svc_start_ms == 999);
        apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":2}", 2000);
        check("a later one can say where it really was", s.svc_start_ms == 0);
        check("which is the same run, not a restart", s.svc_restarts == 0);
        /* A different process, read when it happens to be exactly as old as
         * the one it replaced: nothing went backwards, and the whole jump is
         * 2.3 s - visible from where the run really started, and not from
         * where the first status put it. */
        check("and the replacement is seen from there",
              apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":2}", 4300) == 0 &&
                  s.svc_restarts == 1);
    }
    {
        /* And the other way round: a restart the start test alone would
         * miss, because the whole of it - the old run's life and the gap -
         * fits inside the slack. The uptime going backwards is what says
         * so. */
        struct rift_model s;

        rift_model_init(&s);
        apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":1}", 1900);
        check("a run barely a second old", s.svc_restarts == 0);
        check("replaced inside the slack is still a restart",
              apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":0}", 2100) == 0 &&
                  s.svc_restarts == 1);
    }
    {
        /* What must not happen: a status that says nothing about the run
         * must not be read as one, in either direction. */
        struct rift_model s;

        rift_model_init(&s);
        apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":600}", 600000);
        apply_status_at(&s, "{\"state\":\"online\"}", 601000);
        check("a status with no uptime concludes nothing", s.svc_restarts == 0);
        check("and does not forget the run it was already on",
              s.have_svc_start && s.svc_start_ms == 0 && s.uptime_s == 600);
        apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":601}", 601000);
        check("so the run it was on is still the one it is on", s.svc_restarts == 0);
        apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":\"ages\"}", 602000);
        check("an uptime that is not a number is not an uptime", s.svc_restarts == 0);
        apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":-5}", 603000);
        check("nor is a negative one", s.svc_restarts == 0);
        apply_status_at(&s, "{\"state\":\"online\",\"uptime_s\":1e30}", 604000);
        check("nor one no clock could have reached", s.svc_restarts == 0);
        check("and none of them moved the run", s.svc_start_ms == 0 && s.uptime_s == 601);
        /* A status that is refused outright is not a status at all. */
        check("a status with no state is refused",
              apply_status_at(&s, "{\"uptime_s\":1}", 605000) == -1);
        check("and says nothing about the run either", s.svc_restarts == 0);
    }

    /* ---- the service goes away, and comes back ---------------------------- */
    apply_nodes(&m, "{\"nodes\":[{\"public_key\":\"" KEY_A "\",\"name\":\"OSLO-01\","
                    "\"last_heard_mono_ms\":1000}]}");
    rift_model_service_lost(&m, "meshcored closed the connection");
    check("the service is absent", m.state == RIFT_SVC_ABSENT);
    /* The nodes stay. An empty list would say something less true than a
     * stale one - and would lose the only record of what was there. */
    check("what was known is still shown", m.node_count == 1);
    check("marked as cached", m.stale);
    check("and the snapshot is no longer valid", !m.snapshot_valid);
    check("nothing is claimed about the radio", !m.radio_connected && !m.have_radio_state);

    rift_model_service_found(&m);
    check("on reconnecting it is live again", !m.stale);
    check("with no snapshot read yet", !m.snapshot_valid);
    check("and the state unknown rather than still absent", m.state == RIFT_SVC_UNKNOWN);
    check("an event brings it out of unknown",
          apply_event(&m, "mesh.state",
                      "{\"state\":\"online\",\"reason\":\"receiving\",\"mono_ms\":99}") == 0 &&
              m.state == RIFT_SVC_ONLINE);

    /* ---- identity ---------------------------------------------------------- */
    check("an identity is taken",
          apply_identity(&m, "{\"public_key\":\"" KEY_SELF "\",\"node_hash\":\"5f\","
                             "\"name\":\"K230-A\"}") == 0);
    text_is("with this device's name", m.self_name, "K230-A");
    text_is("and its hash", m.self_hash, "5f");
    check("an identity with no key is refused",
          apply_identity(&m, "{\"name\":\"K230-A\"}") == -1);

    /* ---- naming a hop ------------------------------------------------------ */
    rift_model_init(&m);
    apply_identity(&m, "{\"public_key\":\"" KEY_SELF "\",\"name\":\"K230-A\"}");
    apply_nodes(&m, "{\"nodes\":["
                    "{\"public_key\":\"" KEY_A "\",\"name\":\"OSLO-01\"},"
                    "{\"public_key\":\"" KEY_B "\",\"name\":\"HYTTA\"}]}");
    text_is("a hop hash that matches one node is named", rift_model_name_for_hash(&m, "a1"),
            "OSLO-01");
    text_is("this device names itself", rift_model_name_for_hash(&m, "5f"), "K230-A");
    check("a hash that matches nothing is not named", rift_model_name_for_hash(&m, "ff") == NULL);
    /* Two nodes sharing a first byte: mesh.node refuses an ambiguous prefix
     * rather than answering one, and so does this. A guessed name on a hop
     * is a wrong route drawn confidently. */
    apply_nodes(&m, "{\"nodes\":["
                    "{\"public_key\":\"a1000000000000000000000000000000000000000000000000000000"
                    "00000001\",\"name\":\"ONE\"},"
                    "{\"public_key\":\"a1000000000000000000000000000000000000000000000000000000"
                    "00000002\",\"name\":\"TWO\"}]}");
    check("and a hash two nodes share is not named at all",
          rift_model_name_for_hash(&m, "a1") == NULL);

    /* ---- the order the list is drawn in ------------------------------------ */
    rift_model_init(&m);
    apply_nodes(&m,
                "{\"nodes\":["
                "{\"public_key\":\"" KEY_A "\",\"name\":\"fresh-old\","
                "\"last_heard_mono_ms\":1000},"
                "{\"public_key\":\"" KEY_B "\",\"name\":\"fresh-new\","
                "\"last_heard_mono_ms\":9000},"
                "{\"public_key\":\"" KEY_C "\",\"name\":\"never\"}]}");
    /* now is inside the stale boundary of both heard nodes. */
    count = rift_model_order(&m, 10000, order, RIFT_MAX_NODES);
    check("every node is in the order", count == 3);
    text_is("the most recently heard is first", order[0]->name, "fresh-new");
    text_is("then the one heard longer ago", order[1]->name, "fresh-old");
    text_is("and a node never heard is last", order[2]->name, "never");
    check("two of them are fresh", rift_model_fresh_count(&m, 10000) == 2);

    /* Move the clock past the stale boundary for one of them only. */
    count = rift_model_order(&m, RIFT_STALE_MS + 5000, order, RIFT_MAX_NODES);
    text_is("a node that has gone stale drops below the fresh ones", order[0]->name,
            "fresh-new");
    text_is("but stays above one never heard", order[1]->name, "fresh-old");
    text_is("which is still last", order[2]->name, "never");
    check("and only one is fresh now", rift_model_fresh_count(&m, RIFT_STALE_MS + 5000) == 1);

    {
        const struct rift_node *again[RIFT_MAX_NODES];

        rift_model_order(&m, 10000, again, RIFT_MAX_NODES);
        rift_model_order(&m, 10000, order, RIFT_MAX_NODES);
        check("the order is total, so a list rebuilt every second does not reshuffle",
              memcmp(again, order, sizeof(order[0]) * 3) == 0);
    }

    test_snapshot_keeps_observations();
    test_replies_and_removals();
    test_traffic_counters();
    test_unretained_since_forget();
    test_actions();
    test_find_and_zero_hop();
    test_net_rings();
    test_keys();
    test_identity_and_path_hash();

    /* ---- the order at scale: a merge sort, the same answer as before ---- */
    {
        struct rift_model big;
        const struct rift_node *order[RIFT_MAX_NODES];
        char json[256];
        int n;
        int sorted = 1;
        int i;

        rift_model_init(&big);
        /* A thousand nodes fed oldest first - the order that made the
         * insertion sort quadratic - with a stale one and an unheard one
         * in the middle of them. */
        for (i = 0; i < RIFT_MAX_NODES; i++) {
            char heard[48] = "\"path_known\":false"; /* 501: never heard */

            if (i != 501) {
                /* 500 was heard long ago; every other one at its own moment,
                 * so the order is total by time. */
                snprintf(heard, sizeof(heard), "\"last_heard_mono_ms\":%d",
                         i == 500 ? 1 : 100000000 + i);
            }
            snprintf(json, sizeof(json),
                     "{\"reason\":\"advert\",\"node\":{\"public_key\":\"%04x%060d\","
                     "\"name\":\"N%d\",\"type\":1,%s}}",
                     i, 0, i, heard);
            apply_event(&big, "mesh.node", json);
        }
        check("a thousand nodes are held", big.node_count == RIFT_MAX_NODES);
        n = rift_model_order(&big, 100000000 + RIFT_MAX_NODES, order, RIFT_MAX_NODES);
        check("and all of them ordered", n == RIFT_MAX_NODES);
        for (i = 1; i < n - 2; i++) {
            if (order[i - 1]->heard_mono_ms < order[i]->heard_mono_ms) {
                sorted = 0;
            }
        }
        check("newest first throughout", sorted && order[0]->heard_mono_ms == 100000000 + 999);
        check("the stale one after every fresh one", order[n - 2]->heard_mono_ms == 1);
        check("and the unheard one last", !order[n - 1]->have_heard);
    }

    /* ---- the minute bins: what was heard, by the minute -------------------- */
    {
        struct rift_traffic t;
        struct rift_traffic_bins b;

        rift_traffic_init(&t);
        rift_traffic_read(&t, 1000000, &b);
        check("before anything is heard the bins say so", !b.started);
        check("a text is a message", rift_traffic_class_of("text") == RIFT_TRAFFIC_MSG);
        check("so is a group text", rift_traffic_class_of("group_text") == RIFT_TRAFFIC_MSG);
        check("an advert is an advert", rift_traffic_class_of("advert") == RIFT_TRAFFIC_ADV);
        check("an ack is the mesh at work", rift_traffic_class_of("ack") == RIFT_TRAFFIC_OTHER);
        check("and so is a word this build does not know",
              rift_traffic_class_of("type13") == RIFT_TRAFFIC_OTHER &&
                  rift_traffic_class_of(NULL) == RIFT_TRAFFIC_OTHER);
        /* Minute 100 (6,000,000 ms): two messages and an advert. */
        rift_traffic_note(&t, 6000000, RIFT_TRAFFIC_MSG);
        rift_traffic_note(&t, 6000500, RIFT_TRAFFIC_MSG);
        rift_traffic_note(&t, 6059999, RIFT_TRAFFIC_ADV);
        rift_traffic_read(&t, 6059999, &b);
        check("a frame lands in the minute it was heard",
              b.started && b.count[19][RIFT_TRAFFIC_MSG] == 2 && b.count[19][RIFT_TRAFFIC_ADV] == 1 &&
                  b.count[19][RIFT_TRAFFIC_OTHER] == 0);
        check("and the minutes before it are empty", b.count[18][RIFT_TRAFFIC_MSG] == 0);
        check("the peak is that minute", rift_traffic_peak(&b) == 3);
        /* Read five minutes later with nothing heard since: the busy minute
         * has moved back five bars and the newest five are quiet. */
        rift_traffic_read(&t, 6000000 + 5 * 60000, &b);
        check("a quiet minute since is a quiet bar",
              b.count[19][RIFT_TRAFFIC_MSG] == 0 && b.count[14][RIFT_TRAFFIC_MSG] == 2);
        rift_traffic_note(&t, 6000000 + 3 * 60000, RIFT_TRAFFIC_OTHER);
        rift_traffic_read(&t, 6000000 + 5 * 60000, &b);
        check("a frame in between goes to its own minute",
              b.count[17][RIFT_TRAFFIC_OTHER] == 1 && b.count[14][RIFT_TRAFFIC_MSG] == 2);
        /* Twenty minutes on, the ring has rolled past everything. */
        rift_traffic_note(&t, 6000000 + 25 * 60000, RIFT_TRAFFIC_ADV);
        rift_traffic_read(&t, 6000000 + 25 * 60000, &b);
        check("a jump past the window clears it",
              b.count[19][RIFT_TRAFFIC_ADV] == 1 && rift_traffic_peak(&b) == 1);
        /* A frame from before the window is not drawn as if it were in it. */
        rift_traffic_note(&t, 6000000, RIFT_TRAFFIC_MSG);
        rift_traffic_read(&t, 6000000 + 25 * 60000, &b);
        check("a late frame from outside the window is dropped", rift_traffic_peak(&b) == 1);
        /* Saturation: the count stops at 65535 rather than wrapping to 0. */
        {
            long k;

            for (k = 0; k < 70000; k++) {
                rift_traffic_note(&t, 6000000 + 25 * 60000, RIFT_TRAFFIC_MSG);
            }
            rift_traffic_read(&t, 6000000 + 25 * 60000, &b);
            check("a count saturates", b.count[19][RIFT_TRAFFIC_MSG] == 65535);
        }
        /* And the model feeds it from the activity feed: rx frames with a
         * time on them, by their payload_type; tx frames are what this
         * device said, not what it heard. */
        rift_model_init(&m);
        apply_event(&m, "mesh.activity",
                    "{\"kind\":\"rx\",\"payload_type\":\"advert\",\"bytes\":40,\"mono_ms\":7200000}");
        apply_event(&m, "mesh.activity",
                    "{\"kind\":\"rx\",\"payload_type\":\"text\",\"bytes\":40,\"mono_ms\":7200100}");
        apply_event(&m, "mesh.activity",
                    "{\"kind\":\"tx\",\"result\":\"ok\",\"bytes\":40,\"mono_ms\":7200200}");
        apply_event(&m, "mesh.activity", "{\"kind\":\"rx\",\"payload_type\":\"ack\",\"bytes\":4}");
        rift_traffic_read(&m.traffic, 7200300, &b);
        check("the model counts what it heard by the minute",
              b.count[19][RIFT_TRAFFIC_ADV] == 1 && b.count[19][RIFT_TRAFFIC_MSG] == 1);
        check("not what it sent, and not a frame with no time on it",
              b.count[19][RIFT_TRAFFIC_OTHER] == 0 && m.activity_count == 4);
    }

    printf("rift_model_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
