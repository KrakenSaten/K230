/*
 * meshcored: the receive log (struct mcd_rx_obs, services/meshcored/mesh_rxlog.h).
 *
 * Three runtimes and an in-memory air, as in meshcored_runtime_test.cpp,
 * with every node's on_rx_obs recorded. What is under test is the claim the
 * receive log makes about itself: one observation per reception, duplicates
 * included, read before MeshCore changed anything, with what MeshCore then
 * made of it - and that watching changes nothing MeshCore does.
 *
 * The crypto is real; the frames that are replayed or rewritten were built by
 * a real runtime and captured off the air.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <Packet.h>

#include "mcd_util.h"
#include "mesh_runtime.h"
#include "mesh_store.h"

static int failed;
static int checks;

static void check(const char* name, bool ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    checks++;
    failed += !ok;
}

/* ---- the air ------------------------------------------------------------ */

struct Frame {
    int from;
    uint64_t submit_id;
    int len;
    uint8_t bytes[MCD_MAX_FRAME];
};

static const int MAX_NODES = 3;
static const int MAX_QUEUE = 64;
static const int MAX_OBS = 160;

struct Node;

struct Air {
    Node* nodes[MAX_NODES];
    int count;
    Frame queue[MAX_QUEUE];
    int qn;
    bool deliver; /* false: frames are captured, not carried */
    int carried;
};

struct Node {
    mcd_runtime* rt;
    Air* air;
    int index;
    char dir[256];
    int message_events;
    int node_events;
    int obs_count;
    mcd_rx_obs obs[MAX_OBS];
};

static Air g_air;
static Node g_a, g_b, g_c;

static int hook_tx_submit(void* user, const uint8_t* bytes, int len, uint64_t* submit_id)
{
    Node* n = (Node*)user;
    Air* a = n->air;

    if (a->qn >= MAX_QUEUE) {
        return -1;
    }
    Frame& f = a->queue[a->qn++];

    f.from = n->index;
    f.submit_id = (uint64_t)(1000 + a->carried++);
    f.len = len;
    memcpy(f.bytes, bytes, (size_t)len);
    *submit_id = f.submit_id;
    return 0;
}

static void hook_on_node(void* user, const struct mcd_node*, const char*)
{
    ((Node*)user)->node_events++;
}

static void hook_on_message(void* user, const struct mcd_message*)
{
    ((Node*)user)->message_events++;
}

static void hook_on_rx_obs(void* user, const struct mcd_rx_obs* o)
{
    Node* n = (Node*)user;

    if (n->obs_count < MAX_OBS) {
        n->obs[n->obs_count] = *o;
    }
    n->obs_count++;
}

static uint64_t nowMs(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void meta(struct mcd_rx_meta& m, double rssi, double snr)
{
    memset(&m, 0, sizeof(m));
    m.mono_ms = nowMs();
    m.rssi_known = true;
    m.rssi_dbm = rssi;
    m.snr_known = true;
    m.snr_db = snr;
}

static void pump(int steps, int step_ms = 10)
{
    for (int s = 0; s < steps; s++) {
        for (int i = 0; i < g_air.count; i++) {
            mcd_runtime_tick(g_air.nodes[i]->rt);
        }
        int n = g_air.qn;

        g_air.qn = 0;
        for (int q = 0; q < n; q++) {
            Frame& f = g_air.queue[q];

            if (g_air.deliver) {
                struct mcd_rx_meta m;

                meta(m, -66.0, 9.5);
                for (int i = 0; i < g_air.count; i++) {
                    if (i != f.from) {
                        mcd_runtime_deliver_rx(g_air.nodes[i]->rt, f.bytes, f.len, &m);
                    }
                }
            }
            mcd_runtime_tx_done(g_air.nodes[f.from]->rt, f.submit_id, MCD_TX_OK);
        }
        usleep((useconds_t)step_ms * 1000);
    }
}

template <typename Pred>
static bool pumpUntil(Pred pred, int max_steps = 600)
{
    for (int i = 0; i < max_steps; i++) {
        if (pred()) {
            return true;
        }
        pump(1);
    }
    return pred();
}

/* Adverts are stamped in whole seconds and a receiver drops one that is not
 * newer than the last it holds (meshcored_runtime_test.cpp, the same name). */
static void waitForANewSecond(void)
{
    time_t start = time(NULL);

    while (time(NULL) == start) {
        pump(1);
    }
}

/* Have `from` build a frame and take it off the air before anyone hears it. */
template <typename Send>
static int capture(Node& from, Send send, uint8_t* out)
{
    int len = 0;

    g_air.deliver = false;
    g_air.qn = 0;
    if (!send()) {
        g_air.deliver = true;
        return 0;
    }
    for (int i = 0; i < 200 && g_air.qn == 0; i++) {
        mcd_runtime_tick(from.rt);
        usleep(10000);
    }
    if (g_air.qn > 0) {
        len = g_air.queue[0].len;
        memcpy(out, g_air.queue[0].bytes, (size_t)len);
        for (int q = 0; q < g_air.qn; q++) {
            mcd_runtime_tx_done(g_air.nodes[g_air.queue[q].from]->rt, g_air.queue[q].submit_id,
                                MCD_TX_OK);
        }
        g_air.qn = 0;
    }
    g_air.deliver = true;
    return len;
}

static char g_root[200];

static bool makeNode(Node& n, const char* name)
{
    char err[256] = "";
    char store_err[mcdstore::ERR_SIZE] = "";
    struct mcd_runtime_hooks hooks;
    struct mcd_runtime_config cfg;

    snprintf(n.dir, sizeof(n.dir), "%s/%s", g_root, name);
    if (!mcdstore::ensureDir(n.dir, store_err)) {
        return false;
    }
    n.air = &g_air;
    n.index = g_air.count;
    memset(&hooks, 0, sizeof(hooks));
    hooks.tx_submit = hook_tx_submit;
    hooks.on_node = hook_on_node;
    hooks.on_message = hook_on_message;
    hooks.on_rx_obs = hook_on_rx_obs;
    hooks.user = &n;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_dir = n.dir;
    cfg.node_name = name;
    n.rt = mcd_runtime_create(&cfg, &hooks, err, sizeof(err));
    if (!n.rt) {
        fprintf(stderr, "mcd_runtime_create: %s\n", err);
        return false;
    }
    mcd_runtime_set_radio_online(n.rt, true);
    g_air.nodes[g_air.count++] = &n;
    return true;
}

/* The observations of one packet, by its hash, in the order reported. */
static int byHash(Node& n, const uint8_t hash[MCD_RX_HASH_LEN], const mcd_rx_obs** out, int max)
{
    int k = 0;

    for (int i = 0; i < n.obs_count && i < MAX_OBS && k < max; i++) {
        if (n.obs[i].hash_known && memcmp(n.obs[i].hash, hash, MCD_RX_HASH_LEN) == 0) {
            out[k++] = &n.obs[i];
        }
    }
    return k;
}

static const mcd_rx_obs* lastOfType(Node& n, int type, int from = 0)
{
    const mcd_rx_obs* hit = NULL;

    for (int i = from; i < n.obs_count && i < MAX_OBS; i++) {
        if (n.obs[i].parsed && n.obs[i].payload_type == type) {
            hit = &n.obs[i];
        }
    }
    return hit;
}

static void deliver(Node& to, const uint8_t* bytes, int len, double rssi, double snr)
{
    struct mcd_rx_meta m;

    meta(m, rssi, snr);
    mcd_runtime_deliver_rx(to.rt, bytes, len, &m);
}

/* A copy of frame (a flood with no path) with path_len and path written in. */
static int withPath(const uint8_t* frame, int len, uint8_t path_len, const uint8_t* path,
                    int path_bytes, uint8_t* out)
{
    int k = 0;

    out[k++] = frame[0];
    out[k++] = path_len;
    memcpy(&out[k], path, (size_t)path_bytes);
    k += path_bytes;
    memcpy(&out[k], &frame[2], (size_t)(len - 2));
    return k + len - 2;
}

/* ---- the cases ---------------------------------------------------------- */

static void test_adverts_and_duplicates(void)
{
    uint8_t frame[MCD_MAX_FRAME];
    const mcd_rx_obs* o[8];
    int len;

    waitForANewSecond();
    check("A, B and C advert", mcd_runtime_send_advert(g_a.rt) && mcd_runtime_send_advert(g_b.rt) &&
                                   mcd_runtime_send_advert(g_c.rt));
    check("and every node hears the other two", pumpUntil([] {
              return mcd_runtime_node_count(g_a.rt) >= 2 && mcd_runtime_node_count(g_b.rt) >= 2 &&
                     mcd_runtime_node_count(g_c.rt) >= 2;
          }));
    check("every reception was observed", g_a.obs_count >= 2 && g_b.obs_count >= 2);

    waitForANewSecond();
    len = capture(g_b, [] { return mcd_runtime_send_advert(g_b.rt); }, frame);
    check("B's advert is captured", len > 0);
    if (len == 0) {
        return;
    }
    int before = g_a.obs_count;
    int nodes_before = g_a.node_events;

    deliver(g_a, frame, len, -66.0, 9.5);
    deliver(g_a, frame, len, -80.0, 4.25);
    deliver(g_a, frame, len, -101.0, 3.0);
    check("three receptions, three observations",
          pumpUntil([&] { return g_a.obs_count >= before + 3; }) && g_a.obs_count == before + 3);
    int n = byHash(g_a, g_a.obs[before].hash, o, 8);

    check("all three carry the one packet hash", n == 3);
    if (n != 3) {
        return;
    }
    check("the first is new, reception 1", o[0]->verdict == MCD_RX_NEW && o[0]->dup == 1);
    check("the second is MeshCore's duplicate, reception 2",
          o[1]->verdict == MCD_RX_DUPLICATE && o[1]->dup == 2);
    check("the third likewise, reception 3", o[2]->verdict == MCD_RX_DUPLICATE && o[2]->dup == 3);
    check("seq rises with each reception", o[0]->seq < o[1]->seq && o[1]->seq < o[2]->seq);
    check("each keeps its own RSSI", o[0]->meta.rssi_dbm == -66.0 && o[1]->meta.rssi_dbm == -80.0 &&
                                         o[2]->meta.rssi_dbm == -101.0);
    check("and its own SNR", o[0]->meta.snr_db == 9.5 && o[1]->meta.snr_db == 4.25 &&
                                 o[2]->meta.snr_db == 3.0);
    check("the size is the frame's own", o[0]->bytes == len && o[2]->bytes == len);
    check("an advert", o[0]->parsed && o[0]->payload_type == PAYLOAD_TYPE_ADVERT &&
                           o[0]->route_type == ROUTE_TYPE_FLOOD);
    check("heard with no relay in its path", o[0]->path_hops == 0 && o[0]->path_bytes == 0);
    check("with the sender's hash", o[0]->has_src_hash);
    check("named from the node table", o[0]->decode == MCD_RX_DECODE_ADVERT &&
                                           strcmp(o[0]->sender, "RX-B") == 0);
    check("a repeat is named too: the key is the table's", o[2]->decode == MCD_RX_DECODE_ADVERT);
    check("nothing about it is relayed or ours", !o[0]->relayed && !o[0]->own);
    /* Watching changes nothing: MeshCore still acted on the advert once. */
    check("MeshCore's own deduplication is unchanged: one node event at most",
          g_a.node_events - nodes_before <= 1);
}

static void test_paths(void)
{
    uint8_t frame[MCD_MAX_FRAME];
    uint8_t routed[MCD_MAX_FRAME];
    const mcd_rx_obs* o;
    int len;

    waitForANewSecond();
    len = capture(g_b, [] { return mcd_runtime_send_advert(g_b.rt); }, frame);
    check("another advert is captured", len > 0);
    if (len == 0) {
        return;
    }
    /* Three relays, one byte each: 6E > 67 > 74. */
    {
        const uint8_t hops[3] = { 0x6E, 0x67, 0x74 };
        int rlen = withPath(frame, len, 3, hops, 3, routed);
        int before = g_c.obs_count;

        deliver(g_c, routed, rlen, -71.0, 9.0);
        check("the relayed copy is observed", pumpUntil([&] { return g_c.obs_count > before; }));
        o = &g_c.obs[before];
        check("with its three hops", o->path_hops == 3 && o->path_hash_size == 1 &&
                                         o->path_bytes == 3);
        check("in order", memcmp(o->path, hops, 3) == 0);
        check("and the size of the frame that carried them", o->bytes == rlen);
    }
    /* The longest path there is: 32 hops of two bytes. Nothing is cut. */
    {
        uint8_t hops[64];
        int before = g_c.obs_count;

        for (int i = 0; i < 64; i++) {
            hops[i] = (uint8_t)(0xA0 + i);
        }
        int rlen = withPath(frame, len, (uint8_t)((1 << 6) | 32), hops, 64, routed);

        deliver(g_c, routed, rlen, -90.0, 5.0);
        check("a 64-byte path is observed", pumpUntil([&] { return g_c.obs_count > before; }));
        o = &g_c.obs[before];
        check("all 32 hops of 2 bytes", o->path_hops == 32 && o->path_hash_size == 2 &&
                                            o->path_bytes == 64);
        check("every byte intact", memcmp(o->path, hops, 64) == 0);
        check("the same packet by another way is a repeat", o->dup == 2 &&
                                                                 o->verdict == MCD_RX_DUPLICATE);
    }
}

static void test_channel_message(void)
{
    uint8_t frame[MCD_MAX_FRAME];
    const mcd_rx_obs* o[4];
    static const char body[] = "Kommer opp om 10 min \xF0\x9F\x91\x8D";
    int len;

    len = capture(g_b, [] {
        uint64_t id = 0;

        return mcd_runtime_send_channel_text(g_b.rt, 0, body, &id) == MCD_SEND_ACCEPTED_FLOOD;
    }, frame);
    check("B's Public message is captured", len > 0);
    if (len == 0) {
        return;
    }
    int before = g_a.obs_count;
    int msgs = g_a.message_events;

    deliver(g_a, frame, len, -71.0, 9.0);
    deliver(g_a, frame, len, -76.0, 7.0);
    check("both receptions are observed", pumpUntil([&] { return g_a.obs_count >= before + 2; }));
    int n = byHash(g_a, g_a.obs[before].hash, o, 4);

    check("as one packet", n == 2);
    if (n != 2) {
        return;
    }
    check("a group text", o[0]->payload_type == PAYLOAD_TYPE_GRP_TXT);
    check("with its channel hash, which this node holds",
          o[0]->has_channel_hash && o[0]->channel_known);
    check("decoded on the channel", o[0]->decode == MCD_RX_DECODE_CHANNEL &&
                                        strcmp(o[0]->channel_name, "Public") == 0);
    check("from the claimed sender", strcmp(o[0]->sender, "RX-B") == 0);
    check("the body, without the prefix, emoji and all", strcmp(o[0]->text, body) == 0);
    check("the repeat is reception 2, a duplicate", o[1]->dup == 2 &&
                                                        o[1]->verdict == MCD_RX_DUPLICATE);
    check("and MeshCore did not read it twice", o[1]->decode == MCD_RX_DECODE_NONE);
    check("one message filed, as before the log existed", g_a.message_events == msgs + 1);
}

static void test_unknown_channel(void)
{
    static const char KEY[] = "kJeepayzusHIz9bd5Ovy+QAHDhUcIyoxOD9GTVRbYmk=";
    struct mcd_channel ch;
    uint64_t id = 0;
    int before = g_a.obs_count;
    int msgs = g_a.message_events;

    check("B joins a channel A does not hold",
          mcd_runtime_channel_add(g_b.rt, "SECRET", KEY, &ch) == MCD_CHANNEL_OK);
    check("and talks on it", mcd_runtime_send_channel_text(g_b.rt, ch.slot, "not for A", &id) ==
                                 MCD_SEND_ACCEPTED_FLOOD);
    check("A observes the frame",
          pumpUntil([&] { return lastOfType(g_a, PAYLOAD_TYPE_GRP_TXT, before) != NULL; }));
    const mcd_rx_obs* o = lastOfType(g_a, PAYLOAD_TYPE_GRP_TXT, before);

    if (!o) {
        return;
    }
    check("with the channel hash", o->has_channel_hash && o->channel_hash == ch.hash);
    check("of a channel it does not hold", !o->channel_known);
    check("and reads nothing of it", o->decode == MCD_RX_DECODE_NONE && o->text[0] == '\0');
    check("so nothing is filed", g_a.message_events == msgs);
}

static void test_direct_message(void)
{
    uint8_t a_key[MCD_PUB_KEY_LEN];
    char name[MCD_NODE_NAME_LEN];
    uint64_t id = 0;
    uint32_t timeout = 0;
    int a_before = g_a.obs_count;
    int c_before = g_c.obs_count;

    mcd_runtime_identity(g_a.rt, a_key, name, sizeof(name));
    check("B writes to A", mcd_runtime_send_text(g_b.rt, a_key, 8, "hei A", &id, &timeout) ==
                               MCD_SEND_ACCEPTED_FLOOD);
    check("A observes a text",
          pumpUntil([&] { return lastOfType(g_a, PAYLOAD_TYPE_TXT_MSG, a_before) != NULL; }));
    const mcd_rx_obs* o = lastOfType(g_a, PAYLOAD_TYPE_TXT_MSG, a_before);

    if (!o) {
        return;
    }
    check("addressed to this node", o->has_dest_hash && o->for_us && o->has_src_hash);
    check("decoded as a direct message", o->decode == MCD_RX_DECODE_DIRECT);
    check("from B, to A", strcmp(o->sender, "RX-B") == 0 && strcmp(o->recipient, "RX-A") == 0);
    check("with its text", strcmp(o->text, "hei A") == 0);

    check("C hears the same packet",
          pumpUntil([&] { return lastOfType(g_c, PAYLOAD_TYPE_TXT_MSG, c_before) != NULL; }));
    const mcd_rx_obs* oc = lastOfType(g_c, PAYLOAD_TYPE_TXT_MSG, c_before);

    if (!oc) {
        return;
    }
    check("the same hash", memcmp(oc->hash, o->hash, MCD_RX_HASH_LEN) == 0);
    check("not addressed to C", oc->has_dest_hash && !oc->for_us);
    check("and C reads nothing of it", oc->decode == MCD_RX_DECODE_NONE && oc->text[0] == '\0');
}

static void test_rejected(void)
{
    /* Path mode 3 is reserved: the dispatcher refuses the bytes. */
    uint8_t junk[12] = { 0x15, 0xC0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int before;
    int turned = 0;

    pump(100); /* whatever the last case left on the air settles first */
    before = g_a.obs_count;
    deliver(g_a, junk, (int)sizeof(junk), -95.0, -2.5);
    pump(3);
    check("an unparseable frame is observed", g_a.obs_count == before + 1);
    const mcd_rx_obs* o = &g_a.obs[before];

    check("rejected, unparsed", o->verdict == MCD_RX_REJECTED && o->reject == MCD_RX_REJECT_UNPARSED);
    check("with its size and signal", o->bytes == 12 && o->meta.rssi_dbm == -95.0 &&
                                          o->meta.snr_db == -2.5);
    check("its header read, nothing else", o->header_known && !o->parsed && !o->hash_known);

    /* A full receive queue: the frame never reaches the dispatcher. */
    before = g_a.obs_count;
    for (int i = 0; i < 40; i++) {
        struct mcd_rx_meta m;

        meta(m, -60.0, 8.0);
        if (!mcd_runtime_deliver_rx(g_a.rt, junk, (int)sizeof(junk), &m)) {
            turned++;
        }
    }
    check("some of a burst is turned away", turned > 0);
    check("each turned-away frame is observed at once", g_a.obs_count == before + turned);
    check("as a full queue", turned > 0 && g_a.obs[before].verdict == MCD_RX_REJECTED &&
                                 g_a.obs[before].reject == MCD_RX_REJECT_QUEUE_FULL);
    pump(60);
    check("and the rest when the queue drains", g_a.obs_count == before + 40);
}

static void test_own_packet_heard_back(void)
{
    uint8_t frame[MCD_MAX_FRAME];
    uint8_t routed[MCD_MAX_FRAME];
    const uint8_t hop[1] = { 0x42 };
    int len;

    /* A channel message rather than an advert: MeshCore drops its own advert
     * by identity before its seen-table is asked (Mesh.cpp, "receiving SELF
     * advert packet"), and the case here is the seen-table's. */
    len = capture(g_a, [] {
        uint64_t id = 0;

        return mcd_runtime_send_channel_text(g_a.rt, 0, "echo?", &id) == MCD_SEND_ACCEPTED_FLOOD;
    }, frame);
    check("A's own channel message is captured", len > 0);
    if (len == 0) {
        return;
    }
    int rlen = withPath(frame, len, 1, hop, 1, routed);
    int before = g_a.obs_count;

    deliver(g_a, routed, rlen, -70.0, 8.0);
    check("hearing it repeated back is observed", pumpUntil([&] { return g_a.obs_count > before; }));
    const mcd_rx_obs* o = &g_a.obs[before];

    check("as this node's own packet", o->own);
    check("which MeshCore had already marked seen", o->verdict == MCD_RX_DUPLICATE);
    check("through the repeater that relayed it", o->path_hops == 1 && o->path[0] == 0x42);
}

int main(void)
{
    snprintf(g_root, sizeof(g_root), "/tmp/meshcored-rxlog-XXXXXX");
    if (!mkdtemp(g_root)) {
        perror("mkdtemp");
        return 1;
    }
    g_air.deliver = true;
    if (!makeNode(g_a, "RX-A") || !makeNode(g_b, "RX-B") || !makeNode(g_c, "RX-C")) {
        return 1;
    }
    test_adverts_and_duplicates();
    test_paths();
    test_channel_message();
    test_unknown_channel();
    test_direct_message();
    test_rejected();
    test_own_packet_heard_back();

    mcd_runtime_destroy(g_a.rt);
    mcd_runtime_destroy(g_b.rt);
    mcd_runtime_destroy(g_c.rt);
    {
        char cmd[256];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_root);
        if (system(cmd) != 0) {
            fprintf(stderr, "could not remove %s\n", g_root);
        }
    }
    printf("meshcored_rxlog_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
