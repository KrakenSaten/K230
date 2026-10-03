/*
 * meshcored: the MeshCore runtime and the radiod adapter
 * (services/meshcored/mesh_runtime.cpp).
 *
 * Two runtimes and an in-memory air that stands in for radiod. The air is
 * deliberately crude - lossless, instant, no range - because none of that is
 * what is under test here. What is under test is the seam: what the adapter
 * does with a received frame, what it does with each of the four transmit
 * outcomes radiod can report, and whether the protocol above it still
 * behaves like MeshCore when it is driven by an IPC service instead of a
 * transceiver.
 *
 * The crypto is real. The identities are written to disk before the runtimes
 * are created, so the test holds both private keys and can craft a frame
 * that a genuine MeshCore node would accept - which is what the PATH case at
 * the end needs.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <Packet.h>
#include <Utils.h>
#include <helpers/AdvertDataHelpers.h>

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

struct Node;

struct Frame {
    int from;
    uint64_t submit_id;
    int len;
    uint8_t bytes[MCD_MAX_FRAME];
};

static const int MAX_NODES = 3;
static const int MAX_QUEUE = 64;
static const int MAX_EVENTS = 64;

struct Air {
    Node* nodes[MAX_NODES];
    int count;
    Frame queue[MAX_QUEUE];
    int qn;
    bool deliver;                 /* false: frames are transmitted into nothing */
    mcd_tx_outcome outcome;       /* what radiod is pretending to report */
    bool report_outcome;          /* false: no completion ever arrives */
    bool duplicate_outcome;       /* true: report the same completion twice */
    bool refuse_submit;           /* true: the daemon cannot submit at all */
    int frames_carried;

    Air() : count(0), qn(0), deliver(true), outcome(MCD_TX_OK), report_outcome(true),
            duplicate_outcome(false), refuse_submit(false), frames_carried(0)
    {
        memset(nodes, 0, sizeof(nodes));
    }
};

struct Node {
    mcd_runtime* rt;
    Air* air;
    int index;
    char dir[256];

    /* what the hooks recorded */
    int node_events;
    int node_discovered;
    int node_path;
    int node_removed;
    struct mcd_node last_removed;
    int message_events;
    int channel_events;
    int channel_added;
    int channel_removed;
    int frame_events;
    char last_message[MCD_MAX_TEXT + 1];
    char last_message_peer[MCD_NODE_NAME_LEN];
    struct mcd_message last_msg;
    struct mcd_channel last_channel;
    char last_frame_type[24];
    bool last_frame_rssi_known;
    bool last_frame_snr_known;
    int tx_submits;
    int app_events;
    struct mcd_app_datagram last_app;

    Node() : rt(NULL), air(NULL), index(0), node_events(0), node_discovered(0),
             node_path(0), node_removed(0), message_events(0), channel_events(0),
             channel_added(0), channel_removed(0), frame_events(0), tx_submits(0), app_events(0)
    {
        memset(&last_app, 0, sizeof(last_app));
        memset(&last_removed, 0, sizeof(last_removed));
        memset(&last_msg, 0, sizeof(last_msg));
        memset(&last_channel, 0, sizeof(last_channel));
        dir[0] = '\0';
        last_message[0] = '\0';
        last_message_peer[0] = '\0';
        last_frame_type[0] = '\0';
        last_frame_rssi_known = false;
        last_frame_snr_known = false;
    }
};

static int hook_tx_submit(void* user, const uint8_t* bytes, int len, uint64_t* submit_id)
{
    Node* n = (Node*)user;
    Air* a = n->air;

    if (a->refuse_submit || a->qn >= MAX_QUEUE) {
        return -1;
    }
    Frame& f = a->queue[a->qn++];

    f.from = n->index;
    f.submit_id = (uint64_t)(1000 + a->frames_carried);
    f.len = len;
    memcpy(f.bytes, bytes, (size_t)len);
    a->frames_carried++;
    n->tx_submits++;
    *submit_id = f.submit_id;
    return 0;
}

static void hook_on_node(void* user, const struct mcd_node* nd, const char* reason)
{
    Node* n = (Node*)user;

    n->node_events++;
    if (strcmp(reason, "discovered") == 0) {
        n->node_discovered++;
    } else if (strcmp(reason, "path") == 0) {
        n->node_path++;
    } else if (strcmp(reason, "removed") == 0) {
        n->node_removed++;
        n->last_removed = *nd;
    }
}

static void hook_on_message(void* user, const struct mcd_message* m)
{
    Node* n = (Node*)user;

    n->message_events++;
    n->last_msg = *m;
    snprintf(n->last_message, sizeof(n->last_message), "%s", m->text);
    snprintf(n->last_message_peer, sizeof(n->last_message_peer), "%s", m->peer_name);
}

static void hook_on_channel(void* user, const struct mcd_channel* c, const char* reason)
{
    Node* n = (Node*)user;

    n->channel_events++;
    n->last_channel = *c;
    if (strcmp(reason, "added") == 0) {
        n->channel_added++;
    } else if (strcmp(reason, "removed") == 0) {
        n->channel_removed++;
    }
}

static void hook_on_frame(void* user, const struct mcd_rx_meta* meta, int bytes,
                          const char* outcome)
{
    Node* n = (Node*)user;

    n->frame_events++;
    snprintf(n->last_frame_type, sizeof(n->last_frame_type), "%s", outcome);
    n->last_frame_rssi_known = meta->rssi_known;
    n->last_frame_snr_known = meta->snr_known;
    (void)bytes;
}

static void hook_on_app(void* user, const struct mcd_app_datagram* d)
{
    Node* n = (Node*)user;

    n->app_events++;
    n->last_app = *d;
}

static uint64_t nowMs(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void defaultMeta(struct mcd_rx_meta& meta, bool rssi = true, bool snr = true)
{
    memset(&meta, 0, sizeof(meta));
    meta.mono_ms = nowMs();
    meta.rssi_known = rssi;
    meta.rssi_dbm = -66.0;
    meta.snr_known = snr;
    meta.snr_db = 9.5;
    meta.freq_error_known = true;
    meta.freq_error_hz = -223.0;
}

/* One turn of every node, then whatever they transmitted is carried. Real
 * time, because the runtime reads CLOCK_MONOTONIC: MeshCore's own scheduling
 * is what is being exercised, and winding a clock it does not read would
 * prove nothing about it. */
static void pump(Air& air, int steps, int step_ms = 10)
{
    for (int s = 0; s < steps; s++) {
        for (int i = 0; i < air.count; i++) {
            mcd_runtime_tick(air.nodes[i]->rt);
        }
        int n = air.qn;

        air.qn = 0;
        for (int q = 0; q < n; q++) {
            Frame& f = air.queue[q];

            if (air.deliver) {
                struct mcd_rx_meta meta;

                defaultMeta(meta);
                for (int i = 0; i < air.count; i++) {
                    if (i == f.from) {
                        continue;
                    }
                    mcd_runtime_deliver_rx(air.nodes[i]->rt, f.bytes, f.len, &meta);
                }
            }
            if (air.report_outcome) {
                mcd_runtime_tx_done(air.nodes[f.from]->rt, f.submit_id, air.outcome);
                if (air.duplicate_outcome) {
                    mcd_runtime_tx_done(air.nodes[f.from]->rt, f.submit_id, air.outcome);
                }
            }
        }
        usleep((useconds_t)step_ms * 1000);
    }
}

/* Pump until something has happened, rather than for a number of turns and
 * hope. MeshCore schedules in real milliseconds - a returned path waits 500 ms
 * before it goes out - and the same sequence takes several times as long
 * under the sanitisers, where every Ed25519 verify is instrumented. A fixed
 * step count that is generous on one build is a flaky test on the other. */
template <typename Pred>
static bool pumpUntil(Air& air, Pred pred, int max_steps = 600, int step_ms = 10)
{
    for (int i = 0; i < max_steps; i++) {
        if (pred()) {
            return true;
        }
        pump(air, 1, step_ms);
    }
    return pred();
}

/* MeshCore stamps an advert with whole seconds - Mesh::createAdvert uses
 * RTCClock::getCurrentTime(), not the unique variant - and a receiver drops
 * one whose timestamp is not newer than the last it holds for that node, as a
 * replay guard (vendor/RIFT/src/helpers/BaseChatMesh.cpp:130). Two adverts
 * from the same node inside one second are therefore indistinguishable to a
 * peer and the second is discarded.
 *
 * On a real node adverts are seconds or minutes apart and this never bites. A
 * test that fires several in a row has to wait for the clock, or it is
 * quietly asserting that the replay guard does not work. */
static void waitForANewSecond(Air& air)
{
    time_t start = time(NULL);

    while (time(NULL) == start) {
        pump(air, 1);
    }
}

/* ---- setting up --------------------------------------------------------- */

static char g_root[200];

static bool makeNode(Node& n, Air& air, const char* name, const mesh::LocalIdentity* id)
{
    char err[256] = "";
    char store_err[mcdstore::ERR_SIZE] = "";
    struct mcd_runtime_hooks hooks;
    struct mcd_runtime_config cfg;

    snprintf(n.dir, sizeof(n.dir), "%s/%s", g_root, name);
    if (!mcdstore::ensureDir(n.dir, store_err)) {
        fprintf(stderr, "ensureDir: %s\n", store_err);
        return false;
    }
    /* The identity is written first, so the test holds the private key the
     * runtime will load and can craft frames this node would really accept. */
    if (id && !mcdstore::identitySave(*id, n.dir, store_err)) {
        fprintf(stderr, "identitySave: %s\n", store_err);
        return false;
    }
    n.air = &air;
    n.index = air.count;

    memset(&hooks, 0, sizeof(hooks));
    hooks.tx_submit = hook_tx_submit;
    hooks.on_node = hook_on_node;
    hooks.on_message = hook_on_message;
    hooks.on_channel = hook_on_channel;
    hooks.on_frame = hook_on_frame;
    hooks.on_app = hook_on_app;
    hooks.user = &n;

    memset(&cfg, 0, sizeof(cfg));
    cfg.state_dir = n.dir;
    cfg.node_name = name;

    n.rt = mcd_runtime_create(&cfg, &hooks, err, sizeof(err));
    if (!n.rt) {
        fprintf(stderr, "mcd_runtime_create: %s\n", err);
        return false;
    }
    air.nodes[air.count++] = &n;
    return true;
}

/* Wrap a payload in the wire format mesh::Dispatcher writes: header, path
 * length, path, payload. Only the shapes this test needs (no transport
 * codes). */
static int buildFrame(uint8_t* out, uint8_t header, const uint8_t* payload, int payload_len)
{
    int len = 0;

    out[len++] = header;
    out[len++] = 0;  /* path_len: zero hops */
    memcpy(&out[len], payload, (size_t)payload_len);
    return len + payload_len;
}

/* ---- the cases ---------------------------------------------------------- */

static void test_offline(Node& a)
{
    uint64_t id = 0;
    uint32_t timeout = 0;
    uint8_t prefix[1] = { 0x00 };

    check("a fresh runtime is not online", !mcd_runtime_radio_online(a.rt));
    check("an advert is refused while the radio is not available",
          !mcd_runtime_send_advert(a.rt));
    check("and so is a message",
          mcd_runtime_send_text(a.rt, prefix, 1, "hello", &id, &timeout) == MCD_SEND_NO_RADIO);
    check("nothing was submitted for transmission", a.tx_submits == 0);

    /* The protocol core keeps running without a radio: this must not crash
     * or wedge, and is the behaviour the service depends on when radiod is
     * away. */
    for (int i = 0; i < 20; i++) {
        mcd_runtime_tick(a.rt);
    }
    check("and the runtime keeps ticking with no radio", true);
}

static void test_rx_queue(Node& a)
{
    struct mcd_runtime_stats before;
    struct mcd_runtime_stats after;
    struct mcd_rx_meta meta;
    uint8_t junk[16];

    memset(junk, 0x5A, sizeof(junk));
    defaultMeta(meta);
    mcd_runtime_stats(a.rt, &before);

    check("a frame is accepted",
          mcd_runtime_deliver_rx(a.rt, junk, (int)sizeof(junk), &meta));
    check("and is pending", mcd_runtime_rx_pending(a.rt));
    mcd_runtime_tick(a.rt);
    check("one turn takes it", !mcd_runtime_rx_pending(a.rt));

    mcd_runtime_stats(a.rt, &after);
    check("exactly one frame was queued", after.rx_queued == before.rx_queued + 1);

    /* An empty or over-long frame is refused by the adapter itself. */
    check("an empty frame is refused", !mcd_runtime_deliver_rx(a.rt, junk, 0, &meta));
    check("an over-long frame is refused",
          !mcd_runtime_deliver_rx(a.rt, junk, MCD_MAX_FRAME + 1, &meta));

    /* Fill the queue. A burst larger than it is dropped and counted, not
     * allocated for: the daemon must not grow because somebody is
     * transmitting quickly. */
    {
        int accepted = 0;

        for (int i = 0; i < 200; i++) {
            if (mcd_runtime_deliver_rx(a.rt, junk, (int)sizeof(junk), &meta)) {
                accepted++;
            }
        }
        check("the receive queue is bounded", accepted < 200 && accepted > 0);
        check("and the overflow is counted, not silent",
              mcd_runtime_rx_dropped(a.rt) > 0);
        /* Drain it again; one frame per turn. */
        for (int i = 0; i < 300 && mcd_runtime_rx_pending(a.rt); i++) {
            mcd_runtime_tick(a.rt);
        }
        check("and the queue drains", !mcd_runtime_rx_pending(a.rt));
    }

    /* Rubbish is refused by MeshCore's own bounded parse, without a
     * callback and without a crash. */
    check("none of that junk became a message or a node",
          a.message_events == 0 && a.node_events == 0);
}

static void test_telemetry_stays_unknown(Node& a, Node& b)
{
    struct mcd_rx_meta meta;
    uint8_t frame[MCD_MAX_FRAME];
    int len;
    struct mcd_node node;
    uint8_t key[MCD_PUB_KEY_LEN];
    char name[MCD_NODE_NAME_LEN];

    mcd_runtime_set_radio_online(a.rt, true);
    mcd_runtime_set_radio_online(b.rt, true);

    /* B adverts; A hears it with no RSSI and no SNR reported. */
    check("B builds an advert", mcd_runtime_send_advert(b.rt));
    check("the advert went out",
          pumpUntil(*b.air, [&] { return b.tx_submits >= 1; }));
    check("A learned B",
          pumpUntil(*b.air, [&] { return a.node_discovered >= 1; }));

    mcd_runtime_identity(b.rt, key, name, sizeof(name));
    check("A can look B up", mcd_runtime_node_by_prefix(a.rt, key, 1, &node) == 1);
    check("with the signal it was heard at", node.last_rssi_known && node.last_snr_known);
    check("and the value radiod reported", node.last_rssi_dbm == -66.0);

    /* Now the same node heard again through a frame carrying no telemetry.
     * The absence must propagate: "unknown" and "-66 dBm from ten minutes
     * ago" are different answers, and a UI drawing a bar from the stale one
     * would be showing a measurement that was not made. */
    {
        uint8_t payload[4] = { 0xAA, 0xBB, 0xCC, 0xDD };

        len = buildFrame(frame, (uint8_t)((PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD),
                         payload, (int)sizeof(payload));
        defaultMeta(meta, false, false);
        a.frame_events = 0;
        mcd_runtime_deliver_rx(a.rt, frame, len, &meta);
        mcd_runtime_tick(a.rt);
        check("a frame with no telemetry is still delivered", a.frame_events >= 1);
        check("and its RSSI stays unknown", !a.last_frame_rssi_known);
        check("and its SNR stays unknown", !a.last_frame_snr_known);
        check("and it is reported as the payload type it is",
              strcmp(a.last_frame_type, "ack") == 0);
    }
}

static void test_tx_outcomes(Node& a, Air& air)
{
    struct mcd_runtime_stats before;
    struct mcd_runtime_stats after;

    /* A transmit radiod refuses outright: the frame never goes out and the
     * runtime must not be left believing one is in flight. */
    air.refuse_submit = true;
    a.tx_submits = 0;
    check("an advert is still built when the radio is online",
          mcd_runtime_send_advert(a.rt));
    pump(air, 20);
    check("but nothing was submitted while submission is refused", a.tx_submits == 0);
    air.refuse_submit = false;

    /* A transmit that fails: the bytes did not go out. The dispatcher must
     * recover by its own deadline rather than waiting for a completion that
     * says success. */
    mcd_runtime_stats(a.rt, &before);
    air.outcome = MCD_TX_FAILED;
    a.tx_submits = 0;
    check("another advert is built", mcd_runtime_send_advert(a.rt));
    check("it was submitted", pumpUntil(air, [&] { return a.tx_submits == 1; }));
    pump(air, 60);
    mcd_runtime_stats(a.rt, &after);
    check("and MeshCore did not count a failed transmit as sent",
          after.sent_flood == before.sent_flood);

    /* One that transmitted but left the radio unable to receive. The bytes
     * DID go out: reporting that as a failure would invite a retransmission,
     * which is exactly the duplicate airtime the distinction exists to
     * avoid. */
    mcd_runtime_stats(a.rt, &before);
    air.outcome = MCD_TX_RX_RESUME_FAILED;
    a.tx_submits = 0;
    check("a third advert is built", mcd_runtime_send_advert(a.rt));
    check("a transmit that went out is counted as sent even when RX did not resume",
          pumpUntil(air, [&] {
              struct mcd_runtime_stats s;
              mcd_runtime_stats(a.rt, &s);
              return s.sent_flood == before.sent_flood + 1;
          }));

    /* A completion that never arrives - radiod restarting mid-transmit.
     * The dispatcher's own outbound deadline must clear it, or the node
     * would never transmit again. */
    mcd_runtime_set_radio_online(a.rt, true);
    air.outcome = MCD_TX_OK;
    air.report_outcome = false;
    a.tx_submits = 0;
    check("a fourth advert is built", mcd_runtime_send_advert(a.rt));
    check("it was submitted", pumpUntil(air, [&] { return a.tx_submits == 1; }));
    pump(air, 60);
    air.report_outcome = true;
    /* And the node recovers: the next advert gets submitted too. */
    a.tx_submits = 0;
    mcd_runtime_stats(a.rt, &before);
    check("a fifth advert is built after the lost completion",
          mcd_runtime_send_advert(a.rt));
    check("and it was submitted, so a lost completion does not wedge the node",
          pumpUntil(air, [&] { return a.tx_submits == 1; }));
    check("and it completed", pumpUntil(air, [&] {
              struct mcd_runtime_stats s;
              mcd_runtime_stats(a.rt, &s);
              return s.sent_flood == before.sent_flood + 1;
          }));

    /* Two completions for the same transmit. The second must change
     * nothing. */
    air.duplicate_outcome = true;
    mcd_runtime_stats(a.rt, &before);
    a.tx_submits = 0;
    check("a sixth advert is built", mcd_runtime_send_advert(a.rt));
    check("a duplicate completion counts one transmit, not two",
          pumpUntil(air, [&] {
              struct mcd_runtime_stats s;
              mcd_runtime_stats(a.rt, &s);
              return s.sent_flood == before.sent_flood + 1;
          }));
    pump(air, 30);
    mcd_runtime_stats(a.rt, &after);
    check("and exactly one", after.sent_flood == before.sent_flood + 1);
    air.duplicate_outcome = false;

    /* A completion for a transmit the runtime never made. */
    mcd_runtime_stats(a.rt, &before);
    mcd_runtime_tx_done(a.rt, 987654321u, MCD_TX_OK);
    mcd_runtime_tick(a.rt);
    mcd_runtime_stats(a.rt, &after);
    check("a completion for an unknown submission changes nothing",
          after.sent_flood == before.sent_flood);
}

static void test_two_nodes(Node& a, Node& b, Air& air)
{
    uint8_t b_key[MCD_PUB_KEY_LEN];
    uint8_t a_key[MCD_PUB_KEY_LEN];
    char name[MCD_NODE_NAME_LEN];
    uint64_t msg_id = 0;
    uint32_t timeout = 0;
    struct mcd_node node;

    mcd_runtime_identity(b.rt, b_key, name, sizeof(name));
    mcd_runtime_identity(a.rt, a_key, name, sizeof(name));

    /* Both ways, so each holds the other's real public key. */
    a.node_discovered = 0;
    b.node_discovered = 0;
    waitForANewSecond(air);
    check("A adverts", mcd_runtime_send_advert(a.rt));
    check("B adverts", mcd_runtime_send_advert(b.rt));
    check("B learned A",
          pumpUntil(air, [&] { return b.node_discovered >= 1 && a.node_discovered >= 1; }));
    check("A has B as a node", mcd_runtime_node_by_prefix(a.rt, b_key, 8, &node) == 1);
    check("with B's whole public key",
          memcmp(node.public_key, b_key, MCD_PUB_KEY_LEN) == 0);
    check("as a chat node", node.type == ADV_TYPE_CHAT);
    check("and B has A", mcd_runtime_node_by_prefix(b.rt, a_key, 8, &node) == 1);

    /* A message, and the ACK that comes back with the return path. */
    b.message_events = 0;
    {
        enum mcd_send_result rc =
            mcd_runtime_send_text(a.rt, b_key, 8, "hello from the K230", &msg_id, &timeout);

        check("A sends B a message", rc == MCD_SEND_ACCEPTED_FLOOD);
        check("it has an id", msg_id != 0);
        check("and an ACK deadline", timeout > 0);
    }
    check("B received it", pumpUntil(air, [&] { return b.message_events >= 1; }));
    check("with the text intact", strcmp(b.last_message, "hello from the K230") == 0);
    check("and knows who sent it", strcmp(b.last_message_peer, "K230-A") == 0);

    /* The ACK comes back inside the PATH return, so A both matches its ACK
     * and learns a route in one frame. */
    check("A's message was acknowledged", pumpUntil(air, [&] {
              int n = mcd_runtime_message_count(a.rt);
              struct mcd_message m;

              for (int i = 0; i < n; i++) {
                  if (mcd_runtime_message_at(a.rt, i, &m) && m.id == msg_id &&
                      m.state == MCD_MSG_ACKED) {
                      return true;
                  }
              }
              return false;
          }));
    check("and A learned a path to B",
          mcd_runtime_node_by_prefix(a.rt, b_key, 8, &node) == 1 && node.path_known);

    /* With a path known, the next one goes direct rather than flood. */
    b.message_events = 0;
    {
        enum mcd_send_result rc =
            mcd_runtime_send_text(a.rt, b_key, 8, "and a second one", &msg_id, &timeout);

        check("the second message goes direct", rc == MCD_SEND_ACCEPTED_DIRECT);
    }
    check("B received the directed message too",
          pumpUntil(air, [&] { return b.message_events >= 1; }));
    check("with its own text", strcmp(b.last_message, "and a second one") == 0);

    /* Addressing. */
    {
        uint8_t nobody[2] = { 0x00, 0x00 };

        check("a message to a node nobody knows is refused",
              mcd_runtime_send_text(a.rt, nobody, 2, "hi", &msg_id, &timeout) ==
                  MCD_SEND_NO_CONTACT);
    }
    {
        char too_long[MCD_MAX_TEXT + 10];

        memset(too_long, 'x', sizeof(too_long));
        too_long[sizeof(too_long) - 1] = '\0';
        check("a message that does not fit is refused",
              mcd_runtime_send_text(a.rt, b_key, 8, too_long, &msg_id, &timeout) ==
                  MCD_SEND_TOO_LONG);
    }
}

static void test_duplicate_suppression(Node& a, Node& b, Air& air)
{
    uint8_t frame[MCD_MAX_FRAME];
    int len = 0;
    struct mcd_rx_meta meta;

    /* Capture an advert off the air by intercepting the submission. A new
     * second first: B advertised during the exchange above, and a second
     * advert inside the same second is dropped by the receiver's replay
     * guard (see waitForANewSecond). */
    air.deliver = false;
    air.qn = 0;
    waitForANewSecond(air);
    check("B builds an advert to capture", mcd_runtime_send_advert(b.rt));
    for (int i = 0; i < 40 && air.qn == 0; i++) {
        for (int j = 0; j < air.count; j++) {
            mcd_runtime_tick(air.nodes[j]->rt);
        }
        usleep(10000);
    }
    check("the advert reached the air", air.qn > 0);
    if (air.qn > 0) {
        len = air.queue[0].len;
        memcpy(frame, air.queue[0].bytes, (size_t)len);
        mcd_runtime_tx_done(b.rt, air.queue[0].submit_id, MCD_TX_OK);
        air.qn = 0;
    }
    air.deliver = true;
    if (len == 0) {
        return;
    }

    a.node_events = 0;
    defaultMeta(meta);
    mcd_runtime_deliver_rx(a.rt, frame, len, &meta);
    /* The first delivery must be acted on, or the assertion below would hold
     * for the wrong reason: a frame nobody processed raises no second event
     * either. */
    check("the captured advert is acted on the first time",
          pumpUntil(air, [&] { return a.node_events >= 1; }));
    int after_first = a.node_events;

    /* The same bytes again. MeshCore's duplicate table must swallow it: this
     * is the ordinary case on a mesh, where the same flood packet arrives by
     * several routes. */
    defaultMeta(meta);
    mcd_runtime_deliver_rx(a.rt, frame, len, &meta);
    defaultMeta(meta);
    mcd_runtime_deliver_rx(a.rt, frame, len, &meta);
    pump(air, 30);
    check("a repeated frame raises no further node events",
          a.node_events == after_first);
}

/* ---- how far an advert came --------------------------------------------- *
 *
 * A node's advert_hops is the advert packet's own hop count: what RIFT's
 * zero-hop view is built on. Read off the packet, never inferred: a copy that
 * two repeaters handled says 2, the node heard straight says 0. */
static void test_advert_hops(Node& a, Node& b, Air& air)
{
    uint8_t b_key[MCD_PUB_KEY_LEN];
    char name[MCD_NODE_NAME_LEN];
    uint8_t frame[MCD_MAX_FRAME];
    uint8_t relayed[MCD_MAX_FRAME];
    struct mcd_rx_meta meta;
    struct mcd_node node;
    int len = 0;

    mcd_runtime_identity(b.rt, b_key, name, sizeof(name));
    check("A has heard B's adverts straight, and says so",
          mcd_runtime_node_by_prefix(a.rt, b_key, 8, &node) == 1 && node.advert_hops_known &&
              node.advert_hops == 0);

    /* B's next flood advert, taken off the air before anyone hears it. */
    air.deliver = false;
    air.qn = 0;
    waitForANewSecond(air);
    check("B builds an advert to relay by hand", mcd_runtime_send_advert(b.rt));
    for (int i = 0; i < 40 && air.qn == 0; i++) {
        mcd_runtime_tick(b.rt);
        usleep(10000);
    }
    if (air.qn > 0) {
        len = air.queue[0].len;
        memcpy(frame, air.queue[0].bytes, (size_t)len);
        mcd_runtime_tx_done(b.rt, air.queue[0].submit_id, MCD_TX_OK);
        air.qn = 0;
    }
    air.deliver = true;
    check("and it is a flood with no hops on it yet", len > 2 && frame[1] == 0);
    if (len <= 2 || frame[1] != 0) {
        return;
    }
    /* What it looks like after two repeaters: each appended its one-byte
     * hash and the count says 2 (Mesh::routeRecvPacket). */
    relayed[0] = frame[0];
    relayed[1] = 2;
    relayed[2] = 0x31;
    relayed[3] = 0x32;
    memcpy(&relayed[4], &frame[2], (size_t)(len - 2));
    defaultMeta(meta);
    mcd_runtime_deliver_rx(a.rt, relayed, len + 2, &meta);
    pump(air, 10);
    check("a relayed advert is recorded as two hops away",
          mcd_runtime_node_by_prefix(a.rt, b_key, 8, &node) == 1 && node.advert_hops_known &&
              node.advert_hops == 2);
    check("and the route back is not taken from it",
          !node.path_known || node.path_hops != 2 || node.path_bytes != 2 ||
              node.path[0] != 0x31);

    /* And straight again: a zero-hop advert is heard with nothing between. */
    waitForANewSecond(air);
    a.node_discovered = 0;
    check("B sends a zero-hop advert", mcd_runtime_send_advert_zero_hop(b.rt));
    check("A hears it", pumpUntil(air, [&] { return a.node_discovered >= 1; }));
    check("and B is zero hops away again",
          mcd_runtime_node_by_prefix(a.rt, b_key, 8, &node) == 1 && node.advert_hops_known &&
              node.advert_hops == 0 && node.advert_mono_ms > 0);
}

/* Start a node again on its own state directory, as the daemon would after a
 * restart. With no --name whatever is stored is what it comes back with;
 * name is what MESHCORED_NAME would pass. */
static bool restartNode(Node& a, Air& air, const char* name = NULL)
{
    char err[256] = "";
    struct mcd_runtime_hooks hooks;
    struct mcd_runtime_config cfg;

    mcd_runtime_destroy(a.rt);
    a.rt = NULL;
    memset(&hooks, 0, sizeof(hooks));
    hooks.tx_submit = hook_tx_submit;
    hooks.on_node = hook_on_node;
    hooks.on_message = hook_on_message;
    hooks.on_channel = hook_on_channel;
    hooks.on_frame = hook_on_frame;
    hooks.on_app = hook_on_app;
    hooks.user = &a;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_dir = a.dir;
    cfg.node_name = name;
    a.rt = mcd_runtime_create(&cfg, &hooks, err, sizeof(err));
    if (!a.rt) {
        fprintf(stderr, "restart: %s\n", err);
        return false;
    }
    air.nodes[a.index] = &a;
    /* As the daemon does once radiod has applied the profile again. */
    mcd_runtime_set_radio_online(a.rt, true);
    return true;
}

/* Take the next frame a node submits off the air, undelivered. */
static int captureNext(Node& n, Air& air, uint8_t* frame)
{
    int len = 0;

    for (int i = 0; i < 60 && air.qn == 0; i++) {
        mcd_runtime_tick(n.rt);
        usleep(10000);
    }
    if (air.qn > 0) {
        len = air.queue[0].len;
        memcpy(frame, air.queue[0].bytes, (size_t)len);
        mcd_runtime_tx_done(n.rt, air.queue[0].submit_id, MCD_TX_OK);
        air.qn = 0;
    }
    return len;
}

static void readFile(const char* dir, const char* name, char* out, size_t out_len)
{
    char path[512];
    FILE* f;
    size_t n = 0;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    out[0] = '\0';
    f = fopen(path, "r");
    if (f) {
        n = fread(out, 1, out_len - 1, f);
        fclose(f);
    }
    out[n] = '\0';
}

/* ---- renaming this node, and the path hash size -------------------------- */
static void test_rename_and_path_hash(Node& a, Node& b, Air& air)
{
    uint8_t key[MCD_PUB_KEY_LEN];
    uint8_t a_key[MCD_PUB_KEY_LEN];
    char name[MCD_NODE_NAME_LEN];
    char was[MCD_NODE_NAME_LEN];
    char longest[MCD_NODE_NAME_LEN];
    char too_long[MCD_NODE_NAME_LEN + 8];
    char file[256];
    uint8_t frame[MCD_MAX_FRAME];
    struct mcd_node node;
    bool persisted = false;
    int len;

    mcd_runtime_identity(a.rt, a_key, was, sizeof(was));
    check("a node started without --name runs on its stored name",
          mcd_runtime_name_source(a.rt) == MCD_NAME_STORED);
    check("an empty name is refused", mcd_runtime_set_name(a.rt, "", &persisted) ==
                                          MCD_RENAME_BAD_NAME);
    check("so is a name on two lines",
          mcd_runtime_set_name(a.rt, "two\nlines", &persisted) == MCD_RENAME_BAD_NAME);
    check("and a tab", mcd_runtime_set_name(a.rt, "a\tb", &persisted) == MCD_RENAME_BAD_NAME);
    check("and a name of spaces", mcd_runtime_set_name(a.rt, "   ", &persisted) ==
                                      MCD_RENAME_BAD_NAME);
    check("and bytes that are not UTF-8",
          mcd_runtime_set_name(a.rt, "bad\xff", &persisted) == MCD_RENAME_BAD_NAME);
    check("and an escape sequence",
          mcd_runtime_set_name(a.rt, "x\x1b[31m", &persisted) == MCD_RENAME_BAD_NAME);
    memset(too_long, 'n', MCD_NODE_NAME_LEN);
    too_long[MCD_NODE_NAME_LEN] = '\0';
    check("and a name longer than MeshCore keeps",
          mcd_runtime_set_name(a.rt, too_long, &persisted) == MCD_RENAME_BAD_NAME);
    mcd_runtime_identity(a.rt, key, name, sizeof(name));
    check("a refused rename changes nothing", strcmp(name, was) == 0);
    memset(longest, 'n', MCD_NODE_NAME_LEN - 1);
    longest[MCD_NODE_NAME_LEN - 1] = '\0';
    check("the longest name MeshCore keeps is taken",
          mcd_runtime_set_name(a.rt, longest, &persisted) == MCD_RENAME_OK);

    persisted = false;
    check("a rename is taken", mcd_runtime_set_name(a.rt, "K230-\xC3\x98st", &persisted) ==
                                   MCD_RENAME_OK);
    check("and written to state.v1 straight away", persisted);
    mcd_runtime_identity(a.rt, key, name, sizeof(name));
    check("the identity carries the new name, and nothing else changed",
          strcmp(name, "K230-\xC3\x98st") == 0 && memcmp(key, a_key, MCD_PUB_KEY_LEN) == 0);
    check("the name is still the stored one", mcd_runtime_name_source(a.rt) == MCD_NAME_STORED);
    check("the node restarts", restartNode(a, air));
    if (!a.rt) {
        return;
    }
    mcd_runtime_identity(a.rt, key, name, sizeof(name));
    check("and comes back with the new name and the same key",
          strcmp(name, "K230-\xC3\x98st") == 0 && memcmp(key, a_key, MCD_PUB_KEY_LEN) == 0);

    /* A peer learns it at this node's next advert, and only then: nothing
     * was transmitted by the rename itself. */
    check("renaming put nothing on the air", air.qn == 0);
    waitForANewSecond(air);
    b.node_discovered = 0;
    check("A adverts under its new name", mcd_runtime_send_advert(a.rt));
    check("B hears it", pumpUntil(air, [&] { return b.node_discovered >= 1; }));
    check("and B now calls A by it",
          mcd_runtime_node_by_prefix(b.rt, a_key, 8, &node) == 1 &&
              strcmp(node.name, "K230-\xC3\x98st") == 0);

    /* B was started with --name: its name is the operator's until somebody
     * renames it (test_rename_over_config, on a node of its own). */
    mcd_runtime_identity(b.rt, key, was, sizeof(was));
    check("a name given on the command line says so",
          mcd_runtime_name_source(b.rt) == MCD_NAME_CONFIG);

    /* ---- the path hash size ---- */
    check("the path hash size starts at 1 byte, as MeshCore always has",
          mcd_runtime_path_hash_bytes(a.rt) == 1);
    check("0 bytes is refused", !mcd_runtime_set_path_hash_bytes(a.rt, 0, &persisted));
    check("so is 4: upstream reserves the mode", !mcd_runtime_set_path_hash_bytes(a.rt, 4, &persisted));
    check("and nothing changed", mcd_runtime_path_hash_bytes(a.rt) == 1);
    persisted = false;
    check("2 bytes is taken", mcd_runtime_set_path_hash_bytes(a.rt, 2, &persisted) && persisted);
    readFile(a.dir, "settings.v1", file, sizeof(file));
    check("and written to settings.v1 as a line an operator can read",
          strcmp(file, "path_hash_bytes=2\n") == 0);

    air.deliver = false;
    air.qn = 0;
    waitForANewSecond(air);
    check("A builds an advert", mcd_runtime_send_advert(a.rt));
    len = captureNext(a, air, frame);
    air.deliver = true;
    check("the flood asks for 2-byte hashes: size bits 01, no hops yet",
          len > 2 && (frame[1] >> 6) == 1 && (frame[1] & 63) == 0);
    if (len > 2) {
        struct mcd_rx_meta meta;

        b.node_discovered = 0;
        defaultMeta(meta);
        mcd_runtime_deliver_rx(b.rt, frame, len, &meta);
        check("and a meshcored hears it all the same",
              pumpUntil(air, [&] { return b.node_discovered >= 1; }));
    }
    check("the size survives a restart", restartNode(a, air) && mcd_runtime_path_hash_bytes(a.rt) == 2);
    if (!a.rt) {
        return;
    }
    check("3 bytes is taken", mcd_runtime_set_path_hash_bytes(a.rt, 3, &persisted) && persisted);
    air.deliver = false;
    air.qn = 0;
    waitForANewSecond(air);
    check("A builds another advert", mcd_runtime_send_advert(a.rt));
    len = captureNext(a, air, frame);
    air.deliver = true;
    check("which asks for 3-byte hashes", len > 2 && (frame[1] >> 6) == 2);
    check("and back to 1 byte", mcd_runtime_set_path_hash_bytes(a.rt, 1, &persisted) && persisted);
    air.deliver = false;
    air.qn = 0;
    waitForANewSecond(air);
    check("A builds a third advert", mcd_runtime_send_advert(a.rt));
    len = captureNext(a, air, frame);
    air.deliver = true;
    check("which is the plain 1-byte flood again", len > 2 && frame[1] == 0);
    /* A zero-hop advert carries no path at all, whatever the size. */
    check("the old name is put back for the tests after this",
          mcd_runtime_set_name(a.rt, "K230-A", &persisted) == MCD_RENAME_OK);
}

/* ---- renaming a node whose name is configured, and a rename not saved ---- */
static void test_rename_over_config(void)
{
    Air air;
    Node c;
    uint8_t key[MCD_PUB_KEY_LEN];
    char name[MCD_NODE_NAME_LEN];
    char file[256];
    char state[512];
    bool persisted = false;

    /* Started as S65meshcored starts a unit with MESHCORED_NAME=CFG-NAME. */
    check("a node with a configured name starts", makeNode(c, air, "CFG-NAME", NULL));
    if (!c.rt) {
        return;
    }
    check("its name is the configured one",
          mcd_runtime_name_source(c.rt) == MCD_NAME_CONFIG);
    check("it is renamed all the same",
          mcd_runtime_set_name(c.rt, "Renamed", &persisted) == MCD_RENAME_OK);
    check("and the rename is saved", persisted);
    mcd_runtime_identity(c.rt, key, name, sizeof(name));
    check("the node runs under the new name", strcmp(name, "Renamed") == 0);
    check("which is the stored one now", mcd_runtime_name_source(c.rt) == MCD_NAME_STORED);
    readFile(c.dir, "settings.v1", file, sizeof(file));
    check("settings.v1 marks which configured name was renamed over",
          strstr(file, "renamed_over=") != NULL);
    check("and holds no name: the name is in state.v1 alone",
          strstr(file, "Renamed") == NULL && strstr(file, "CFG-NAME") == NULL);
    readFile(c.dir, "state.v1", state, sizeof(state));
    check("state.v1 carries it", memcmp(state + 8, "Renamed", 8) == 0);

    check("the node restarts, configured as before", restartNode(c, air, "CFG-NAME"));
    if (!c.rt) {
        return;
    }
    mcd_runtime_identity(c.rt, key, name, sizeof(name));
    check("and keeps the rename over the configured name",
          strcmp(name, "Renamed") == 0 && mcd_runtime_name_source(c.rt) == MCD_NAME_STORED);
    check("a path hash change", mcd_runtime_set_path_hash_bytes(c.rt, 2, &persisted) &&
                                     persisted);
    readFile(c.dir, "settings.v1", file, sizeof(file));
    check("writes settings.v1 with the mark still in it",
          strstr(file, "path_hash_bytes=2\n") != NULL && strstr(file, "renamed_over=") != NULL);
    check("so the rename survives that too",
          restartNode(c, air, "CFG-NAME") && mcd_runtime_name_source(c.rt) == MCD_NAME_STORED);
    if (!c.rt) {
        return;
    }
    persisted = false;
    check("a second rename needs no new mark",
          mcd_runtime_set_name(c.rt, "Again", &persisted) == MCD_RENAME_OK && persisted);

    /* The operator keeps the last word: a different configured name wins. */
    check("the node restarts with another configured name",
          restartNode(c, air, "OPERATOR"));
    if (!c.rt) {
        return;
    }
    mcd_runtime_identity(c.rt, key, name, sizeof(name));
    check("which replaces the rename",
          strcmp(name, "OPERATOR") == 0 && mcd_runtime_name_source(c.rt) == MCD_NAME_CONFIG);
    readFile(c.dir, "settings.v1", file, sizeof(file));
    check("the mark is spent, and the path hash size kept",
          strstr(file, "renamed_over=") == NULL && strstr(file, "path_hash_bytes=2\n") != NULL);
    check("the first configured name put back", restartNode(c, air, "CFG-NAME"));
    if (!c.rt) {
        return;
    }
    mcd_runtime_identity(c.rt, key, name, sizeof(name));
    check("is used as given: an old rename does not come back",
          strcmp(name, "CFG-NAME") == 0 && mcd_runtime_name_source(c.rt) == MCD_NAME_CONFIG);

    /* A rename that cannot be written: taken for this run, and said to be
     * unsaved - never answered as if it would survive. The directory is made
     * read-only, which stops a temporary file being created in it (root is
     * not stopped by that, and the case is skipped for root). */
    if (geteuid() != 0) {
        check("the state directory is made read-only", chmod(c.dir, 0500) == 0);
        persisted = true;
        check("a rename is still taken",
              mcd_runtime_set_name(c.rt, "Unsaved", &persisted) == MCD_RENAME_OK);
        check("but is reported as not saved", !persisted);
        mcd_runtime_identity(c.rt, key, name, sizeof(name));
        check("the node runs under it for now", strcmp(name, "Unsaved") == 0);
        check("the directory is writable again", chmod(c.dir, 0700) == 0);
        readFile(c.dir, "state.v1", state, sizeof(state));
        check("state.v1 was not touched", memcmp(state + 8, "CFG-NAME", 9) == 0);
        readFile(c.dir, "settings.v1", file, sizeof(file));
        check("nor was a mark written", strstr(file, "renamed_over=") == NULL);
        /* SIGKILL, in effect: destroy would write the state on the way out. */
        check("asked again once it can be written, the same name is saved",
              mcd_runtime_set_name(c.rt, "Unsaved", &persisted) == MCD_RENAME_OK && persisted);
        check("and survives a restart",
              restartNode(c, air, "CFG-NAME") && mcd_runtime_name_source(c.rt) == MCD_NAME_STORED);
        if (c.rt) {
            mcd_runtime_identity(c.rt, key, name, sizeof(name));
            check("under the saved name", strcmp(name, "Unsaved") == 0);
        }
    }
    if (c.rt) {
        mcd_runtime_destroy(c.rt);
        c.rt = NULL;
    }
}

/* ---- the well-known Public channel ---------------------------------------- */
static void test_public_channel(void)
{
    Air air;
    Node c;
    struct mcd_channel pub;
    struct mcd_channel other;
    struct mcd_channel named;
    struct mcd_channel at;

    check("a node for the Public channel case starts", makeNode(c, air, "PUB-NODE", NULL));
    if (!c.rt) {
        return;
    }
    memset(&pub, 0, sizeof(pub));
    memset(&other, 0, sizeof(other));
    memset(&named, 0, sizeof(named));
    /* A channel only called Public: a 128-bit key that is not the one. */
    check("a channel named Public with another key is joined",
          mcd_runtime_channel_add(c.rt, "Public", "AAECAwQFBgcICQoLDA0ODw==", &named) ==
              MCD_CHANNEL_OK);
    check("and is not the Public channel: the name decides nothing", !named.is_public);
    /* MeshCore's PUBLIC_GROUP_PSK, 8b3387e9c5cdea6ac9e5edbaa115cd72, under a
     * local name of the operator's choosing. */
    check("the well-known key is joined under another name",
          mcd_runtime_channel_add(c.rt, "torget", "izOH6cXN6mrJ5e26oRXNcg==", &pub) ==
              MCD_CHANNEL_OK);
    check("and is the Public channel, by its key", pub.is_public && pub.key_bits == 128);
    check("its hash is the one MeshCore nodes put on the air for Public", pub.hash == 0x11);
    check("a second copy of that key is refused: there is one Public row",
          mcd_runtime_channel_add(c.rt, "Public 2", "izOH6cXN6mrJ5e26oRXNcg==", &other) !=
              MCD_CHANNEL_OK);
    check("the list says the same after a restart", restartNode(c, air, "PUB-NODE"));
    if (c.rt) {
        int publics = 0;

        for (int i = 0; mcd_runtime_channel_at(c.rt, i, &at); i++) {
            if (at.is_public) {
                publics++;
                check("the Public channel kept its slot and its local name",
                      at.slot == pub.slot && strcmp(at.name, "torget") == 0);
            }
        }
        check("exactly one channel is the Public one", publics == 1);
        mcd_runtime_destroy(c.rt);
        c.rt = NULL;
    }
}

/* ---- the PATH guard ----------------------------------------------------- */

static int craftPath(uint8_t* frame, const mesh::LocalIdentity& from,
                     const mesh::Identity& to, const uint8_t* inner, int inner_len)
{
    uint8_t secret[PUB_KEY_SIZE];
    uint8_t payload[MAX_PACKET_PAYLOAD];
    int len = 0;

    from.calcSharedSecret(secret, to);
    payload[len++] = to.pub_key[0];    /* dest hash */
    payload[len++] = from.pub_key[0];  /* src hash */
    len += mesh::Utils::encryptThenMAC(secret, &payload[len], inner, inner_len);
    return buildFrame(frame,
                      (uint8_t)((PAYLOAD_TYPE_PATH << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD),
                      payload, len);
}

static void test_path_guard(Node& a, Node& b, Air& air, const mesh::LocalIdentity& a_id,
                            const mesh::LocalIdentity& b_id)
{
    struct mcd_runtime_stats before;
    struct mcd_runtime_stats after;
    struct mcd_rx_meta meta;
    uint8_t frame[MCD_MAX_FRAME];
    int len;

    (void)a;

    /* The well-formed case first, so the crafted one below means something:
     * it proves this really is the code path a PATH packet takes, and that
     * the guard does not refuse a payload MeshCore would accept. */
    {
        uint8_t good[5] = { 2, 0x11, 0x22, 0x00, 0xDE };  /* 2 one-byte hops, extra_type 0 */

        mcd_runtime_stats(b.rt, &before);
        b.node_path = 0;
        len = craftPath(frame, a_id, b_id, good, (int)sizeof(good));
        defaultMeta(meta);
        mcd_runtime_deliver_rx(b.rt, frame, len, &meta);
        check("a well-formed PATH payload is accepted",
              pumpUntil(air, [&] { return b.node_path >= 1; }));
        mcd_runtime_stats(b.rt, &after);
        check("and nothing was refused",
              after.path_payloads_refused == before.path_payloads_refused);
    }

    /* And the crafted one. mesh::Mesh computes the trailing field's length
     * as `len - k` with nothing checking k <= len (vendor/RIFT/src/Mesh.cpp:172,
     * recorded debt 2). Here k is 65 and the decrypted length is 16, so the
     * uint8_t truncation turns -49 into 207 - a length the payload cannot
     * hold, handed on with a pointer near the end of a 184-byte stack buffer.
     *
     * The MAC is genuine, so a real MeshCore node would act on it. What
     * makes it reachable over the air is that MeshCore adds contacts from
     * adverts by itself, so any node that adverts can get here. meshcored
     * refuses the payload in its own handler rather than changing the
     * vendored tree; running this under ASan is what proves nothing read
     * through that pointer. */
    {
        uint8_t crafted[3] = { 63, 0xAA, 0xBB };  /* claims 63 one-byte hops */
        struct mcd_node a_seen_by_b;
        const uint8_t* a_key = a_id.pub_key;

        mcd_runtime_stats(b.rt, &before);
        len = craftPath(frame, a_id, b_id, crafted, (int)sizeof(crafted));
        defaultMeta(meta);
        mcd_runtime_deliver_rx(b.rt, frame, len, &meta);
        check("a PATH payload claiming more than it carries is refused",
              pumpUntil(air, [&] {
                  struct mcd_runtime_stats s;
                  mcd_runtime_stats(b.rt, &s);
                  return s.path_payloads_refused == before.path_payloads_refused + 1;
              }));
        /* Asserted on what the stored path is, not on whether it changed.
         * The only thing that rewrites B's path to A is a PATH from A
         * (vendor/RIFT/src/helpers/BaseChatMesh.cpp:331), and A may still
         * owe B one from the messages above: it answers a flood PATH with a
         * return path of its own 500 ms later (vendor/RIFT/src/Mesh.cpp:177).
         * The duplicate test normally transmits that into nothing, but a slow
         * enough run lets it through to land here, genuinely rewriting B's
         * path while this payload is refused. "The path is what it was
         * before" was a claim about the mesh having gone quiet, and it failed
         * that way, rarely, under ASan.
         *
         * What must hold is that THIS payload's hops were not adopted:
         * neither its packed length nor the 0xAA 0xBB it carries. A genuine
         * update cannot look like either. meshcored leaves MeshCore's
         * allowPacketForward() at its default of false, so every return path
         * A and B build for each other is empty, and the only other path B
         * has been given is the 0x11 0x22 above. B must also still know A,
         * or this would pass for a node that is not there. */
        check("and its impossible path was not adopted",
              mcd_runtime_node_by_prefix(b.rt, a_key, 8, &a_seen_by_b) == 1 &&
              a_seen_by_b.path_len != crafted[0] &&
              !(a_seen_by_b.path_bytes >= 2 && a_seen_by_b.path[0] == crafted[1] &&
                a_seen_by_b.path[1] == crafted[2]));
    }

    /* The boundary. A payload whose declared path exactly fills the
     * decrypted block is well formed and must still be accepted, or the
     * guard would be rejecting real traffic. */
    {
        uint8_t edge[16];

        memset(edge, 0, sizeof(edge));
        edge[0] = 13;  /* 13 one-byte hops: k = 2 + 13 = 15, inside 16 */
        mcd_runtime_stats(b.rt, &before);
        b.node_path = 0;
        len = craftPath(frame, a_id, b_id, edge, 15);
        defaultMeta(meta);
        mcd_runtime_deliver_rx(b.rt, frame, len, &meta);
        pumpUntil(air, [&] { return b.node_path >= 1; });
        mcd_runtime_stats(b.rt, &after);
        check("a PATH payload that exactly fills its block is accepted",
              after.path_payloads_refused == before.path_payloads_refused);
        check("and its path is taken", b.node_path >= 1);
    }
}

/* ---- persistence across a restart --------------------------------------- */

static void test_restart(Node& a, Air& air)
{
    uint8_t key_before[MCD_PUB_KEY_LEN];
    uint8_t key_after[MCD_PUB_KEY_LEN];
    char name_before[MCD_NODE_NAME_LEN];
    char name_after[MCD_NODE_NAME_LEN];
    int nodes_before;
    char err[256] = "";
    struct mcd_runtime_hooks hooks;
    struct mcd_runtime_config cfg;

    mcd_runtime_identity(a.rt, key_before, name_before, sizeof(name_before));
    nodes_before = mcd_runtime_node_count(a.rt);
    check("this node knows at least one other", nodes_before >= 1);
    check("its state is written out", mcd_runtime_persist(a.rt) == 0);

    mcd_runtime_destroy(a.rt);
    a.rt = NULL;

    memset(&hooks, 0, sizeof(hooks));
    hooks.tx_submit = hook_tx_submit;
    hooks.on_node = hook_on_node;
    hooks.on_message = hook_on_message;
    hooks.on_channel = hook_on_channel;
    hooks.on_frame = hook_on_frame;
    hooks.user = &a;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_dir = a.dir;
    cfg.node_name = NULL;  /* no name given: the stored one must be used */

    a.rt = mcd_runtime_create(&cfg, &hooks, err, sizeof(err));
    check("the runtime starts again", a.rt != NULL);
    if (!a.rt) {
        return;
    }
    air.nodes[a.index] = &a;

    mcd_runtime_identity(a.rt, key_after, name_after, sizeof(name_after));
    check("with the same identity", memcmp(key_before, key_after, MCD_PUB_KEY_LEN) == 0);
    check("and the same name", strcmp(name_before, name_after) == 0);
    check("and the nodes it had learned", mcd_runtime_node_count(a.rt) == nodes_before);

    /* Messages are runtime-only in this phase, and the API says so rather
     * than pretending otherwise. */
    check("messages did not survive, which is this phase's documented shape",
          mcd_runtime_message_count(a.rt) == 0);
}

/* ---- the contact table, full -------------------------------------------
 *
 * MeshCore's table holds MAX_CONTACTS (1000) contacts. Once it is full, allocateContactSlot()
 * returns NULL and BaseChatMesh reports the discovery anyway, with a
 * ContactInfo on its own stack, purely so a UI can say "somebody adverted and
 * I could not keep them". It is not in the table and it is gone the moment
 * the callback returns.
 *
 * A service that takes that at face value emits a mesh.node event for a node
 * mesh.node cannot then find, marks its state dirty so state.v1 is rewritten
 * with nothing changed, and evicts a real node's telemetry slot to hold
 * readings for a node it did not keep. On a busy mesh that repeats for every
 * advert from every stranger.
 */
static int craftAdvert(uint8_t* frame, const mesh::LocalIdentity& id, const char* name,
                       uint32_t timestamp, const double* lat = NULL, const double* lon = NULL,
                       uint8_t type = ADV_TYPE_CHAT)
{
    uint8_t payload[MAX_PACKET_PAYLOAD];
    uint8_t app_data[MAX_ADVERT_DATA_SIZE];
    uint8_t message[PUB_KEY_SIZE + 4 + MAX_ADVERT_DATA_SIZE];
    AdvertDataBuilder plain(type, name);
    AdvertDataBuilder located(type, name, lat ? *lat : 0.0, lon ? *lon : 0.0);
    uint8_t app_len = (lat && lon) ? located.encodeTo(app_data) : plain.encodeTo(app_data);
    int len = 0;
    int msg_len = 0;

    /* The same bytes Mesh::createAdvert() lays down, in the same order, and
     * signed over the same message: public key, timestamp, signature, app
     * data. Built here because the test needs adverts from identities that
     * have no Mesh of their own. */
    memcpy(&payload[len], id.pub_key, PUB_KEY_SIZE);
    len += PUB_KEY_SIZE;
    memcpy(&payload[len], &timestamp, 4);
    len += 4;
    uint8_t* signature = &payload[len];
    len += SIGNATURE_SIZE;
    memcpy(&payload[len], app_data, app_len);
    len += app_len;

    memcpy(&message[msg_len], id.pub_key, PUB_KEY_SIZE);
    msg_len += PUB_KEY_SIZE;
    memcpy(&message[msg_len], &timestamp, 4);
    msg_len += 4;
    memcpy(&message[msg_len], app_data, app_len);
    msg_len += app_len;
    id.sign(signature, message, msg_len);

    return buildFrame(frame,
                      (uint8_t)((PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD),
                      payload, len);
}

/* ---- where a node says it is --------------------------------------------
 *
 * MeshCore carries an optional latitude/longitude in the advert
 * (ADV_LATLON_MASK, degrees x 1e6) and BaseChatMesh keeps it in the contact;
 * mesh.nodes reports it. 0,0 is MeshCore's "never set" and is not a
 * location; neither is anything outside the valid range. */
static void test_location(void)
{
    Air air;
    Node b;
    mesh::LocalIdentity b_id;
    mesh::LocalIdentity r_id;
    mesh::LocalIdentity z_id;
    mesh::LocalIdentity x_id;
    char store_err[mcdstore::ERR_SIZE] = "";
    uint8_t frame[MCD_MAX_FRAME];
    struct mcd_rx_meta meta;
    struct mcd_node node;
    double lat = 59.913900;
    double lon = 10.752200;
    double zero = 0.0;
    double far = 95.0;
    int len;

    check("identities for the location case",
          mcdstore::identityCreate(b_id, store_err) && mcdstore::identityCreate(r_id, store_err) &&
              mcdstore::identityCreate(z_id, store_err) &&
              mcdstore::identityCreate(x_id, store_err));
    check("the map listener starts", makeNode(b, air, "MAPLISTENER", &b_id));
    if (!b.rt) {
        return;
    }
    mcd_runtime_set_radio_online(b.rt, true);

    len = craftAdvert(frame, r_id, "RPT-OSLO", 1789300000u, &lat, &lon, ADV_TYPE_REPEATER);
    defaultMeta(meta);
    mcd_runtime_deliver_rx(b.rt, frame, len, &meta);
    pumpUntil(air, [&] { return mcd_runtime_node_count(b.rt) >= 1; });
    check("an advert with a location gives the node one",
          mcd_runtime_node_by_prefix(b.rt, r_id.pub_key, 8, &node) == 1 && node.location_known &&
              node.lat_e6 == 59913900 && node.lon_e6 == 10752200 && node.type == ADV_TYPE_REPEATER);
    len = craftAdvert(frame, r_id, "RPT-OSLO", 1789300010u);
    defaultMeta(meta);
    mcd_runtime_deliver_rx(b.rt, frame, len, &meta);
    pump(air, 10);
    check("a later advert without one keeps it, as MeshCore does",
          mcd_runtime_node_by_prefix(b.rt, r_id.pub_key, 8, &node) == 1 && node.location_known &&
              node.lat_e6 == 59913900);

    len = craftAdvert(frame, z_id, "NULL-ISLAND", 1789300020u, &zero, &zero);
    defaultMeta(meta);
    mcd_runtime_deliver_rx(b.rt, frame, len, &meta);
    pumpUntil(air, [&] { return mcd_runtime_node_count(b.rt) >= 2; });
    check("0,0 is MeshCore's none, not a place",
          mcd_runtime_node_by_prefix(b.rt, z_id.pub_key, 8, &node) == 1 && !node.location_known);

    len = craftAdvert(frame, x_id, "OFF-THE-MAP", 1789300030u, &far, &lon);
    defaultMeta(meta);
    mcd_runtime_deliver_rx(b.rt, frame, len, &meta);
    pumpUntil(air, [&] { return mcd_runtime_node_count(b.rt) >= 3; });
    check("a latitude past the pole is not a location",
          mcd_runtime_node_by_prefix(b.rt, x_id.pub_key, 8, &node) == 1 && !node.location_known);
    mcd_runtime_destroy(b.rt);
    b.rt = NULL;
}

/* Remote text, where it is actually reachable.
 *
 * An advert name and a message body are chosen by whoever is on the air,
 * and the two are not equally exposed. MeshCore truncates a NAME at the
 * first byte that is not valid UTF-8, so a conforming encoder cannot put
 * raw bytes there - but nothing anywhere looks at what a MESSAGE contains,
 * and an escape sequence is valid UTF-8 so it travels either way.
 *
 * The point is that the bytes DO arrive, so the sanitiser at the mesh.*
 * boundary is answering a real question rather than a hypothetical one.
 */
static void test_hostile_remote_text(void)
{
    Air air;
    Node a;
    Node b;
    mesh::LocalIdentity a_id;
    mesh::LocalIdentity b_id;
    char store_err[mcdstore::ERR_SIZE] = "";
    uint8_t frame[MCD_MAX_FRAME];
    struct mcd_rx_meta meta;
    struct mcd_node node;
    uint64_t msg_id = 0;
    uint32_t timeout = 0;
    int len;

    check("two identities for the hostile-text case",
          mcdstore::identityCreate(a_id, store_err) &&
          mcdstore::identityCreate(b_id, store_err));
    check("the sender starts", makeNode(a, air, "RUDE", &a_id));
    check("the listener starts", makeNode(b, air, "LISTENER", &b_id));
    if (!a.rt || !b.rt) {
        return;
    }
    mcd_runtime_set_radio_online(a.rt, true);
    mcd_runtime_set_radio_online(b.rt, true);

    /* ---- an advert name with an escape sequence ----
     *
     * Crafted rather than sent, because MeshCore's own AdvertDataBuilder
     * truncates a name at the first byte that is not valid UTF-8 - which is
     * why tools/meshcore-frame refuses one too. An escape sequence IS valid
     * UTF-8, so it travels, and that is the case this proves. */
    len = craftAdvert(frame, a_id, "A\x1b" "[2JB", 1789200000u);
    defaultMeta(meta);
    mcd_runtime_deliver_rx(b.rt, frame, len, &meta);
    check("the advert is accepted and the node learned",
          pumpUntil(air, [&] { return mcd_runtime_node_count(b.rt) >= 1; }));
    check("it can be looked up",
          mcd_runtime_node_by_prefix(b.rt, a_id.pub_key, 8, &node) == 1);
    check("and the escape byte reached the runtime intact",
          strchr(node.name, 0x1b) != NULL);
    {
        char safe[MCD_SANITIZED_SIZE(MCD_MAX_TEXT)];

        mcd_text_sanitize(node.name, safe, sizeof(safe));
        check("the sanitiser takes it out of the name", strchr(safe, 0x1b) == NULL);
        check("while keeping what was printable",
              strchr(safe, 'A') != NULL && strchr(safe, 'B') != NULL);
    }

    /* ---- a message body with bytes that are not text at all ----
     *
     * Nothing between the sender and onMessageRecv() looks at what a message
     * body contains: it is a decrypted blob up to its terminator. The IPC
     * side refuses a client that tries this (mcd_text_acceptable), but a node
     * on the air answers to nobody, so the bytes arrive. This sends them the
     * way that node would - past the IPC check, straight into the runtime. */
    check("the listener adverts back", mcd_runtime_send_advert(b.rt));
    check("and the sender learns it",
          pumpUntil(air, [&] { return mcd_runtime_node_count(a.rt) >= 1; }));

    {
        const char nasty[] = { 'h', 'i', 0x1b, '[', '2', 'J', (char)0xff, (char)0xfe,
                               '!', '\0' };
        enum mcd_send_result rc =
            mcd_runtime_send_text(a.rt, b_id.pub_key, 8, nasty, &msg_id, &timeout);

        check("a message of bytes that are not text is sent",
              rc == MCD_SEND_ACCEPTED_FLOOD || rc == MCD_SEND_ACCEPTED_DIRECT);
        check("and arrives", pumpUntil(air, [&] { return b.message_events >= 1; }));
        check("with the escape byte intact", strchr(b.last_message, 0x1b) != NULL);
        {
            bool has_raw = false;

            for (const char* p = b.last_message; *p; p++) {
                if ((unsigned char)*p == 0xff || (unsigned char)*p == 0xfe) {
                    has_raw = true;
                }
            }
            check("and the bytes that are not UTF-8 intact", has_raw);
        }
        {
            char safe[MCD_SANITIZED_SIZE(MCD_MAX_TEXT)];
            bool clean = true;

            mcd_text_sanitize(b.last_message, safe, sizeof(safe));
            for (const char* p = safe; *p; p++) {
                unsigned char c = (unsigned char)*p;

                if (c == 0xff || c == 0xfe || (c < 0x20 && c != '\n' && c != '\t')) {
                    clean = false;
                }
            }
            check("the sanitiser leaves nothing a terminal would act on", clean);
            check("and keeps the text that was text",
                  strstr(safe, "hi") != NULL && strchr(safe, '!') != NULL);
        }
    }

    mcd_runtime_destroy(a.rt);
    mcd_runtime_destroy(b.rt);
    a.rt = NULL;
    b.rt = NULL;
}

static void test_full_contact_table(void)
{
    Air air;
    Node n;
    mesh::LocalIdentity self;
    char store_err[mcdstore::ERR_SIZE] = "";
    const int TABLE = MAX_CONTACTS;
    const int EXTRA = 6;
    static mesh::LocalIdentity peers[TABLE + EXTRA];
    uint8_t frame[MCD_MAX_FRAME];
    struct mcd_rx_meta meta;
    struct mcd_runtime_stats st;
    uint32_t stamp = 1789000000u;
    int len;
    bool ids_ok = true;

    check("the table is 1000, not upstream's 32", TABLE == 1000 && TABLE == MCD_MAX_NODES);
    check("an identity for the crowded node", mcdstore::identityCreate(self, store_err));
    check("the crowded node starts", makeNode(n, air, "CROWD", &self));
    if (!n.rt) {
        return;
    }
    mcd_runtime_set_radio_online(n.rt, true);
    for (int i = 0; i < TABLE + EXTRA; i++) {
        ids_ok = ids_ok && mcdstore::identityCreate(peers[i], store_err);
    }
    check("an identity for every peer, the spares included", ids_ok);

    /* Fill it. Each advert carries its own timestamp, because a receiver
     * drops one that is not newer than the last it holds for that node. */
    for (int i = 0; i < TABLE; i++) {
        char name[16];

        snprintf(name, sizeof(name), "PEER-%d", i);
        len = craftAdvert(frame, peers[i], name, stamp++);
        defaultMeta(meta);
        mcd_runtime_deliver_rx(n.rt, frame, len, &meta);
        pumpUntil(air, [&] { return mcd_runtime_node_count(n.rt) == i + 1; });
        if (i == TABLE - 2) {
            mcd_runtime_stats(n.rt, &st);
            check("999 nodes are held", mcd_runtime_node_count(n.rt) == TABLE - 1);
            check("with nothing turned away yet", st.nodes_unretained == 0 && st.contacts_full == 0);
        }
    }
    mcd_runtime_stats(n.rt, &st);
    check("the 1000th is kept: the table fills to its limit", mcd_runtime_node_count(n.rt) == TABLE);
    check("and nothing was turned away to get there",
          st.nodes_unretained == 0 && st.contacts_full == 0);
    check("and every one of them raised a discovery", n.node_discovered == TABLE);
    check("the last one in can be looked up",
          mcd_runtime_node_by_prefix(n.rt, peers[TABLE - 1].pub_key, 8, NULL) == 1);

    /* A node that IS kept, whose telemetry must survive what follows. */
    struct mcd_node kept;
    check("the first peer is in the table",
          mcd_runtime_node_by_prefix(n.rt, peers[0].pub_key, 8, &kept) == 1);
    check("with the signal it was heard at", kept.last_rssi_known);
    double kept_rssi = kept.last_rssi_dbm;
    uint64_t kept_heard = kept.last_heard_mono_ms;

    mcd_runtime_persist(n.rt);
    int events_before = n.node_events;
    int discovered_before = n.node_discovered;
    mcd_runtime_stats(n.rt, &st);
    uint64_t unretained_before = st.nodes_unretained;

    /* The 1001st, on its own first: turned away, counted, and nowhere. */
    {
        uint64_t full_before = st.contacts_full;

        len = craftAdvert(frame, peers[TABLE], "SPARE-0", stamp++);
        defaultMeta(meta);
        mcd_runtime_deliver_rx(n.rt, frame, len, &meta);
        pumpUntil(air, [&] {
            struct mcd_runtime_stats s;

            mcd_runtime_stats(n.rt, &s);
            return s.nodes_unretained > unretained_before;
        });
        mcd_runtime_stats(n.rt, &st);
        check("the 1001st is turned away and counted",
              st.nodes_unretained == unretained_before + 1 && st.contacts_full == full_before + 1);
        check("the table still holds 1000", mcd_runtime_node_count(n.rt) == TABLE);
        check("and the 1001st cannot be looked up",
              mcd_runtime_node_by_prefix(n.rt, peers[TABLE].pub_key, 8, NULL) == 0);
        unretained_before = st.nodes_unretained;
    }

    /* Now node 1001 and onwards, repeatedly - the case that used to churn. */
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < EXTRA; i++) {
            char name[16];

            snprintf(name, sizeof(name), "SPARE-%d", i);
            /* Different metadata, so an eviction would be visible. */
            memset(&meta, 0, sizeof(meta));
            meta.mono_ms = nowMs();
            meta.rssi_known = true;
            meta.rssi_dbm = -11.0;
            meta.snr_known = true;
            meta.snr_db = 1.0;
            len = craftAdvert(frame, peers[TABLE + i], name, stamp++);
            mcd_runtime_deliver_rx(n.rt, frame, len, &meta);
            pump(air, 6);
        }
    }
    pump(air, 30);

    check("no node event is raised for a node that was not kept",
          n.node_events == events_before);
    check("and no discovery either", n.node_discovered == discovered_before);
    check("the node count is unchanged", mcd_runtime_node_count(n.rt) == TABLE);

    /* Internally consistent: every node the list reports can be looked up,
     * and none of the unstored ones can. */
    {
        bool all_found = true;

        for (int i = 0; i < mcd_runtime_node_count(n.rt); i++) {
            struct mcd_node node;

            if (!mcd_runtime_node_at(n.rt, i, &node) ||
                mcd_runtime_node_by_prefix(n.rt, node.public_key, 8, &node) != 1) {
                all_found = false;
            }
        }
        check("every node the list reports can be looked up", all_found);
    }
    {
        int phantom = 0;

        for (int i = 0; i < EXTRA; i++) {
            struct mcd_node node;

            if (mcd_runtime_node_by_prefix(n.rt, peers[TABLE + i].pub_key, 8, &node) != 0) {
                phantom++;
            }
        }
        check("and not one of the unstored nodes is findable", phantom == 0);
    }

    check("the state is not marked dirty by nodes that were not stored",
          !mcd_runtime_dirty(n.rt));

    {
        uint64_t want = unretained_before + (uint64_t)(EXTRA * 3);
        char label[160];
        bool reached = pumpUntil(air, [&] {
            struct mcd_runtime_stats s;

            mcd_runtime_stats(n.rt, &s);
            return s.nodes_unretained >= want;
        });

        mcd_runtime_stats(n.rt, &st);
        snprintf(label, sizeof(label),
                 "but they are counted, so the table being full is visible "
                 "(%llu of %llu)", (unsigned long long)st.nodes_unretained,
                 (unsigned long long)want);
        check(label, reached);
    }
    check("and MeshCore's own table-full signal is counted too", st.contacts_full > 0);

    /* The telemetry slot of a node that IS kept must not have been taken. */
    check("a retained node is still in the table",
          mcd_runtime_node_by_prefix(n.rt, peers[0].pub_key, 8, &kept) == 1);
    check("with the signal it was heard at, not a stranger's",
          kept.last_rssi_known && kept.last_rssi_dbm == kept_rssi);
    check("and the time it was heard", kept.last_heard_mono_ms == kept_heard);

    /* A retained node adverting again still works: the guard refuses the
     * unstored, not everything. */
    {
        int before = n.node_discovered;

        len = craftAdvert(frame, peers[1], "PEER-1", stamp++);
        defaultMeta(meta);
        mcd_runtime_deliver_rx(n.rt, frame, len, &meta);
        check("a node that IS in the table still raises its events",
              pumpUntil(air, [&] { return n.node_discovered > before; }));
        check("and marks the state dirty", mcd_runtime_dirty(n.rt));
    }

    /* Forgetting a node frees a telemetry slot in the MIDDLE of the table.
     * A node held further on that is heard again must keep its own slot,
     * not take the free one as a second; otherwise the next new node finds
     * no free slot and evicts a real node's last-heard time. */
    {
        struct mcd_node was;
        struct mcd_node node;
        int discovered = n.node_discovered;
        bool all_heard = true;

        check("a node in the middle of a full table is forgotten",
              mcd_runtime_node_remove(n.rt, peers[5].pub_key, &was, NULL) &&
                  mcd_runtime_node_count(n.rt) == TABLE - 1);
        len = craftAdvert(frame, peers[20], "PEER-20", stamp++);
        defaultMeta(meta);
        mcd_runtime_deliver_rx(n.rt, frame, len, &meta);
        pumpUntil(air, [&] { return n.node_discovered > discovered; });
        len = craftAdvert(frame, peers[TABLE], "SPARE-0", stamp++);
        defaultMeta(meta);
        mcd_runtime_deliver_rx(n.rt, frame, len, &meta);
        check("a node that had no room is taken into the room made",
              pumpUntil(air, [&] {
                  return mcd_runtime_node_by_prefix(n.rt, peers[TABLE].pub_key, 8, &node) == 1;
              }) &&
                  mcd_runtime_node_count(n.rt) == TABLE);
        for (int i = 0; i < mcd_runtime_node_count(n.rt); i++) {
            if (!mcd_runtime_node_at(n.rt, i, &node) || !node.last_heard_known) {
                all_heard = false;
            }
        }
        check("and no node the table holds lost the time it was heard", all_heard);
        check("the first peer's signal least of all",
              mcd_runtime_node_by_prefix(n.rt, peers[0].pub_key, 8, &kept) == 1 &&
                  kept.last_heard_known && kept.last_heard_mono_ms == kept_heard);
    }

    /* ---- a full table across a restart ----
     *
     * All 1000 written, all 1000 read back - none lost to a limit on either
     * side of the file - and the table is still full afterwards, so the next
     * stranger is turned away exactly as before. */
    {
        static uint8_t keys[TABLE][PUB_KEY_SIZE];
        int held = mcd_runtime_node_count(n.rt);
        bool all_back = true;
        char err[256] = "";
        struct mcd_runtime_hooks hooks;
        struct mcd_runtime_config cfg;

        check("the table is full before the restart", held == TABLE);
        for (int i = 0; i < held; i++) {
            struct mcd_node node;

            mcd_runtime_node_at(n.rt, i, &node);
            memcpy(keys[i], node.public_key, PUB_KEY_SIZE);
        }
        check("the full table is written", mcd_runtime_persist(n.rt) == 0);
        mcd_runtime_destroy(n.rt);

        memset(&hooks, 0, sizeof(hooks));
        hooks.tx_submit = hook_tx_submit;
        hooks.on_node = hook_on_node;
        hooks.user = &n;
        memset(&cfg, 0, sizeof(cfg));
        cfg.state_dir = n.dir;
        n.rt = mcd_runtime_create(&cfg, &hooks, err, sizeof(err));
        check("the runtime starts again on the full table", n.rt != NULL);
        if (!n.rt) {
            return;
        }
        air.nodes[n.index] = &n;
        mcd_runtime_set_radio_online(n.rt, true);
        check("with all 1000 nodes", mcd_runtime_node_count(n.rt) == TABLE);
        for (int i = 0; i < held; i++) {
            if (mcd_runtime_node_by_prefix(n.rt, keys[i], PUB_KEY_SIZE, NULL) != 1) {
                all_back = false;
            }
        }
        check("every one of them the node it was", all_back);

        mcd_runtime_stats(n.rt, &st);
        len = craftAdvert(frame, peers[TABLE + 1], "SPARE-1", stamp++);
        defaultMeta(meta);
        mcd_runtime_deliver_rx(n.rt, frame, len, &meta);
        pumpUntil(air, [&] {
            struct mcd_runtime_stats s;

            mcd_runtime_stats(n.rt, &s);
            return s.nodes_unretained > st.nodes_unretained;
        });
        check("and a new node is still turned away after the reload",
              mcd_runtime_node_count(n.rt) == TABLE &&
                  mcd_runtime_node_by_prefix(n.rt, peers[TABLE + 1].pub_key, 8, NULL) == 0);
    }

    mcd_runtime_destroy(n.rt);
    n.rt = NULL;
}

/* ---- mesh.nodes: most recently heard first ------------------------------
 *
 * The service holds 1000 nodes and a client may keep fewer (Fleet keeps 64),
 * so the order mesh.nodes lists them in decides which ones a client keeps.
 * Nodes heard during this run
 * come first, newest first; after a restart nothing has been heard yet, and
 * the stored last-updated time (MeshCore's lastmod) is what orders them. */
static void test_nodes_newest_first(void)
{
    Air air;
    Node n;
    mesh::LocalIdentity self;
    mesh::LocalIdentity p[3];
    char store_err[mcdstore::ERR_SIZE] = "";
    uint8_t frame[MCD_MAX_FRAME];
    struct mcd_rx_meta meta;
    struct mcd_node out[MCD_MAX_NODES];
    uint32_t stamp = 1789500000u;
    uint64_t t0 = nowMs();
    char dir[512];
    int len;

    /* ---- after a restart: three stored nodes, none heard yet ----
     *
     * Table order A, B, C; last updated at 100, 300 and 200 seconds. */
    {
        mcdstore::NodeState* st = new mcdstore::NodeState();

        snprintf(dir, sizeof(dir), "%s/ORDER", g_root);
        check("a state directory for the ordering case", mcdstore::ensureDir(dir, store_err));
        snprintf(st->name, sizeof(st->name), "ORDER");
        for (int i = 0; i < 3; i++) {
            ContactInfo& c = st->nodes[i];
            static const uint32_t lastmod[3] = { 100, 300, 200 };

            c = ContactInfo();
            memset(c.id.pub_key, 0xA0 + i, PUB_KEY_SIZE);
            snprintf(c.name, sizeof(c.name), "STORED-%c", 'A' + i);
            c.type = ADV_TYPE_CHAT;
            c.out_path_len = OUT_PATH_UNKNOWN;
            c.lastmod = lastmod[i];
        }
        st->count = 3;
        check("and a stored table", mcdstore::stateSave(*st, dir, store_err));
        delete st;
    }
    check("an identity for the ordering node", mcdstore::identityCreate(self, store_err));
    check("the ordering node starts on it", makeNode(n, air, "ORDER", &self));
    if (!n.rt) {
        return;
    }
    mcd_runtime_set_radio_online(n.rt, true);
    check("the three stored nodes are listed",
          mcd_runtime_nodes_recent(n.rt, out, MCD_MAX_NODES) == 3);
    check("with none heard during this run",
          !out[0].last_heard_known && !out[1].last_heard_known && !out[2].last_heard_known);
    check("newest stored first: B (300), C (200), A (100)",
          out[0].public_key[0] == 0xA1 && out[1].public_key[0] == 0xA2 &&
              out[2].public_key[0] == 0xA0);

    /* ---- heard during this run ----
     *
     * Three peers advert in the order P0, P1, P2, each heard a second after
     * the one before. The table holds them in that order too; the list must
     * not. */
    for (int i = 0; i < 3; i++) {
        char name[16];

        check("a peer identity for the ordering case", mcdstore::identityCreate(p[i], store_err));
        snprintf(name, sizeof(name), "HEARD-%d", i);
        len = craftAdvert(frame, p[i], name, stamp++);
        defaultMeta(meta);
        meta.mono_ms = t0 + 1000u * (uint64_t)i;
        mcd_runtime_deliver_rx(n.rt, frame, len, &meta);
        pumpUntil(air, [&] { return mcd_runtime_node_count(n.rt) == 4 + i; });
    }
    check("all six are held", mcd_runtime_nodes_recent(n.rt, out, MCD_MAX_NODES) == 6);
    check("the newest heard first: P2, P1, P0",
          memcmp(out[0].public_key, p[2].pub_key, PUB_KEY_SIZE) == 0 &&
              memcmp(out[1].public_key, p[1].pub_key, PUB_KEY_SIZE) == 0 &&
              memcmp(out[2].public_key, p[0].pub_key, PUB_KEY_SIZE) == 0);
    check("every heard node ahead of every one not heard this run",
          out[2].last_heard_known && !out[3].last_heard_known);
    check("which keep their stored order behind them",
          out[3].public_key[0] == 0xA1 && out[4].public_key[0] == 0xA2 &&
              out[5].public_key[0] == 0xA0);
    {
        struct mcd_node first;

        check("and the table itself is not in that order",
              mcd_runtime_node_at(n.rt, 0, &first) && first.public_key[0] == 0xA0);
    }

    /* P0 heard again, later than anyone: it moves to the front. */
    {
        struct mcd_node was;

        mcd_runtime_node_by_prefix(n.rt, p[0].pub_key, 8, &was);
        len = craftAdvert(frame, p[0], "HEARD-0", stamp++);
        defaultMeta(meta);
        meta.mono_ms = t0 + 5000u;
        mcd_runtime_deliver_rx(n.rt, frame, len, &meta);
        check("P0 is heard again", pumpUntil(air, [&] {
                  struct mcd_node node;

                  return mcd_runtime_node_by_prefix(n.rt, p[0].pub_key, 8, &node) == 1 &&
                         node.last_heard_mono_ms != was.last_heard_mono_ms;
              }));
    }
    mcd_runtime_nodes_recent(n.rt, out, MCD_MAX_NODES);
    check("a node heard again moves to the front",
          memcmp(out[0].public_key, p[0].pub_key, PUB_KEY_SIZE) == 0 &&
              memcmp(out[1].public_key, p[2].pub_key, PUB_KEY_SIZE) == 0);

    /* A shorter list is the head of the same order, not the table's first. */
    check("a smaller max gets the newest",
          mcd_runtime_nodes_recent(n.rt, out, 2) == 2 &&
              memcmp(out[0].public_key, p[0].pub_key, PUB_KEY_SIZE) == 0 &&
              memcmp(out[1].public_key, p[2].pub_key, PUB_KEY_SIZE) == 0);

    mcd_runtime_destroy(n.rt);
    n.rt = NULL;
}

/* ---- a node state this build will not read ------------------------------
 *
 * The identity is fatal. The node table is a cache the mesh refills, so a
 * corrupt one must not take the node off the air - which is what it did:
 * the runtime refused to start, meshcored exited, the supervisor restarted
 * it, it read the same file and exited again, and after five rounds gave up
 * with a crash-loop marker.
 */
static void test_corrupt_state_is_survivable(void)
{
    char dir[256];
    char err[256] = "";
    char store_err[mcdstore::ERR_SIZE] = "";
    struct mcd_runtime_hooks hooks;
    struct mcd_runtime_config cfg;
    struct mcd_runtime* rt;
    mesh::LocalIdentity id;
    Air air;
    Node n;

    snprintf(dir, sizeof(dir), "%s/badstate", g_root);
    check("a directory for the corrupt-state case", mcdstore::ensureDir(dir, store_err));
    check("with a good identity in it",
          mcdstore::identityCreate(id, store_err) && mcdstore::identitySave(id, dir, store_err));

    memset(&hooks, 0, sizeof(hooks));
    hooks.tx_submit = hook_tx_submit;
    hooks.on_node = hook_on_node;
    hooks.on_message = hook_on_message;
    hooks.on_channel = hook_on_channel;
    hooks.on_frame = hook_on_frame;
    n.air = &air;
    hooks.user = &n;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_dir = dir;
    cfg.node_name = "SURVIVOR";

    struct Case {
        const char* what;
        const uint8_t* bytes;
        size_t len;
    };
    uint8_t good[44 + 148];
    /* A valid header with one node, built by the store itself, so the
     * mutations below are mutations of something real. */
    {
        mcdstore::NodeState st = mcdstore::NodeState();
        ContactInfo c = ContactInfo();
        FILE* f;
        char p[512];

        snprintf(st.name, sizeof(st.name), "SURVIVOR");
        for (int i = 0; i < PUB_KEY_SIZE; i++) {
            c.id.pub_key[i] = (uint8_t)(0x40 + i);
        }
        snprintf(c.name, sizeof(c.name), "A-NODE");
        c.type = ADV_TYPE_CHAT;
        c.out_path_len = OUT_PATH_UNKNOWN;
        st.nodes[0] = c;
        st.count = 1;
        check("a good state file to mutate", mcdstore::stateSave(st, dir, store_err));
        snprintf(p, sizeof(p), "%s/state.v1", dir);
        f = fopen(p, "rb");
        check("which can be read as bytes",
              f != NULL && fread(good, 1, sizeof(good), f) == sizeof(good));
        if (f) {
            fclose(f);
        }
    }

    uint8_t bad_magic[44 + 148];
    uint8_t bad_version[44 + 148];
    uint8_t bad_record[44 + 148];
    uint8_t dup_keys[44 + 296];

    memcpy(bad_magic, good, sizeof(bad_magic));
    bad_magic[0] = 'X';
    memcpy(bad_version, good, sizeof(bad_version));
    bad_version[4] = 9;
    memcpy(bad_record, good, sizeof(bad_record));
    bad_record[44 + 66] = 0xC0 | 10;  /* a path length MeshCore would refuse */
    memcpy(dup_keys, good, 44 + 148);
    memcpy(&dup_keys[44 + 148], &good[44], 148);
    dup_keys[40] = 2;

    Case cases[] = {
        { "truncated", good, 44 + 60 },
        { "a wrong magic", bad_magic, sizeof(bad_magic) },
        { "a version this build does not read", bad_version, sizeof(bad_version) },
        { "an invalid record", bad_record, sizeof(bad_record) },
        { "two nodes with one key", dup_keys, sizeof(dup_keys) },
        { "empty", good, 0 },
    };

    for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        char p[512];
        FILE* f;
        char fault[256] = "";
        uint8_t pub[MCD_PUB_KEY_LEN];
        char name[MCD_NODE_NAME_LEN];

        snprintf(p, sizeof(p), "%s/state.v1", dir);
        unlink(p);
        f = fopen(p, "wb");
        if (f) {
            if (cases[ci].len > 0) {
                if (fwrite(cases[ci].bytes, 1, cases[ci].len, f) != cases[ci].len) {
                    check("the case file was written", false);
                }
            }
            fclose(f);
        }

        rt = mcd_runtime_create(&cfg, &hooks, err, sizeof(err));
        char label[128];

        snprintf(label, sizeof(label), "%s state does not stop the runtime", cases[ci].what);
        check(label, rt != NULL);
        if (!rt) {
            continue;
        }
        snprintf(label, sizeof(label), "%s: the identity is kept", cases[ci].what);
        mcd_runtime_identity(rt, pub, name, sizeof(name));
        check(label, memcmp(pub, id.pub_key, PUB_KEY_SIZE) == 0);

        snprintf(label, sizeof(label), "%s: it starts with no known nodes", cases[ci].what);
        check(label, mcd_runtime_node_count(rt) == 0);

        snprintf(label, sizeof(label), "%s: and says what was wrong", cases[ci].what);
        check(label, mcd_runtime_state_fault(rt, fault, sizeof(fault)) && fault[0] != '\0');

        /* Moved aside, not destroyed: the bytes are the only evidence. */
        snprintf(label, sizeof(label), "%s: the bad file was kept for inspection",
                 cases[ci].what);
        check(label, strstr(fault, "kept as") != NULL);

        /* And it is a working node, not just a live object. */
        snprintf(label, sizeof(label), "%s: and it runs normally afterwards", cases[ci].what);
        n.rt = rt;
        air.count = 0;
        air.nodes[air.count++] = &n;
        n.index = 0;
        mcd_runtime_set_radio_online(rt, true);
        {
            mesh::LocalIdentity peer;
            uint8_t frame[MCD_MAX_FRAME];
            struct mcd_rx_meta meta;
            int len;

            mcdstore::identityCreate(peer, store_err);
            len = craftAdvert(frame, peer, "NEWCOMER", 1789100000u + (uint32_t)ci);
            defaultMeta(meta);
            mcd_runtime_deliver_rx(rt, frame, len, &meta);
            check(label, pumpUntil(air, [&] { return mcd_runtime_node_count(rt) == 1; }));
        }
        snprintf(label, sizeof(label), "%s: and rebuilds its table from the air",
                 cases[ci].what);
        check(label, mcd_runtime_node_count(rt) == 1);

        mcd_runtime_destroy(rt);
        n.rt = NULL;
    }

    /* A good state file after all that still loads normally. */
    {
        char p[512];
        FILE* f;
        char fault[256] = "x";

        snprintf(p, sizeof(p), "%s/state.v1", dir);
        unlink(p);
        f = fopen(p, "wb");
        if (f) {
            if (fwrite(good, 1, sizeof(good), f) != sizeof(good)) {
                check("the good file was restored", false);
            }
            fclose(f);
        }
        rt = mcd_runtime_create(&cfg, &hooks, err, sizeof(err));
        check("a good state file still loads", rt != NULL);
        if (rt) {
            check("with its node", mcd_runtime_node_count(rt) == 1);
            check("and no fault reported",
                  !mcd_runtime_state_fault(rt, fault, sizeof(fault)) && fault[0] == '\0');
            mcd_runtime_destroy(rt);
        }
    }
}

static void test_corrupt_identity_stops_the_runtime(void)
{
    char dir[256];
    char err[256] = "";
    char store_err[mcdstore::ERR_SIZE] = "";
    struct mcd_runtime_hooks hooks;
    struct mcd_runtime_config cfg;
    struct mcd_runtime* rt;
    uint8_t rubbish[96];

    snprintf(dir, sizeof(dir), "%s/corrupt", g_root);
    check("a directory for the corrupt case", mcdstore::ensureDir(dir, store_err));
    memset(rubbish, 0x41, sizeof(rubbish));
    {
        char p[512];
        FILE* f;

        snprintf(p, sizeof(p), "%s/identity.id", dir);
        f = fopen(p, "wb");
        check("a corrupt identity file is written",
              f != NULL && fwrite(rubbish, 1, sizeof(rubbish), f) == sizeof(rubbish));
        if (f) {
            fclose(f);
        }
    }
    memset(&hooks, 0, sizeof(hooks));
    hooks.tx_submit = hook_tx_submit;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_dir = dir;

    rt = mcd_runtime_create(&cfg, &hooks, err, sizeof(err));
    /* Refusing to start is the safe answer. Generating a replacement would
     * silently make this a different node to every peer that knows it, and
     * would destroy the only copy of a key nothing else holds. */
    check("a corrupt identity stops the runtime rather than being replaced", rt == NULL);
    check("and says why", err[0] != '\0');
    {
        char p[512];
        struct stat st;

        snprintf(p, sizeof(p), "%s/identity.id", dir);
        check("and the file is left alone", stat(p, &st) == 0 && st.st_size == 96);
    }
    if (rt) {
        mcd_runtime_destroy(rt);
    }
}

/* ---- channels ----------------------------------------------------------
 *
 * The keys below are literals rather than anything derived, so the two nodes
 * demonstrably hold the same bytes and the test says which. They are 32-byte
 * keys with a non-zero upper half, because a 32-byte key whose upper half is
 * zero is the one shape this service refuses - see the ambiguity case.
 */
static const char KEY_A[] = "//79/Pv6+fj39vX08/Lx8O/u7ezr6uno5+bl5OPi4eA=";
static const char KEY_B[] = "kJeepayzusHIz9bd5Ovy+QAHDhUcIyoxOD9GTVRbYmk=";

/* How many messages this node holds for a given channel slot. The merged
 * list is what a client reads, so the count is taken from it. */
static int channelMessagesFor(Node& n, int slot)
{
    int total = mcd_runtime_message_count(n.rt);
    int found = 0;

    for (int i = 0; i < total; i++) {
        struct mcd_message m;

        if (mcd_runtime_message_at(n.rt, i, &m) && m.is_channel && m.channel_slot == slot) {
            found++;
        }
    }
    return found;
}

static void test_channels(Node& a, Node& b, Air& air)
{
    struct mcd_channel ca;
    struct mcd_channel cb;
    struct mcd_runtime_stats st;
    uint64_t msg_id = 0;

    /* test_restart left node A with a runtime that has just been created, and
     * a fresh runtime's radio is offline until the daemon says otherwise. */
    mcd_runtime_set_radio_online(a.rt, true);
    mcd_runtime_set_radio_online(b.rt, true);

    check("this node starts with no channels", mcd_runtime_channel_count(a.rt) == 0);
    check("and sending on one is refused while there are none",
          mcd_runtime_send_channel_text(a.rt, 0, "nobody", &msg_id) == MCD_SEND_NO_CHANNEL);

    /* ---- joining ---- */
    a.channel_events = 0;
    check("node A joins a channel",
          mcd_runtime_channel_add(a.rt, "SITE", KEY_A, &ca) == MCD_CHANNEL_OK);
    check("it went into the lowest free slot", ca.slot == 0);
    {
        /* Written on the join, not on the daemon's persist timer. A channel
         * key is the one thing here nothing can give back: a power cut inside
         * a ten-second window would cost a key the operator typed by hand,
         * where the same window costs the node table only a rediscovery. */
        char path[512];
        struct stat sb;

        snprintf(path, sizeof(path), "%s/channels.v1", a.dir);
        check("and the key is on disk before anything else happens",
              stat(path, &sb) == 0 && sb.st_size > 0);
        check("at 0600, because it is key material", (sb.st_mode & 07777) == 0600);
        check("and the join left nothing for the timer to write",
              !mcd_runtime_dirty(a.rt));
    }
    check("an added event was raised", a.channel_added == 1 && a.last_channel.slot == 0);
    check("the channel is a 256-bit one", ca.key_bits == 256);
    check("and its name is what was asked for", strcmp(ca.name, "SITE") == 0);
    /* MeshCore puts "<our name>: " inside the payload, so the body a composer
     * may send is shorter than a direct message's 160 by exactly that. */
    check("the text limit allows for this node's name prefix",
          ca.text_limit == MCD_MAX_TEXT - (int)strlen("K230-A: "));

    /* The name is local. Node B joins the SAME key under a DIFFERENT name,
     * which is the ordinary case on a real mesh: nothing about the name
     * crosses the air. */
    check("node B joins the same key under another name",
          mcd_runtime_channel_add(b.rt, "site-b", KEY_A, &cb) == MCD_CHANNEL_OK);
    check("and derives the same channel hash from it", cb.hash == ca.hash);
    check("though the names differ", strcmp(ca.name, cb.name) != 0);

    /* ---- refusals ---- */
    check("the same key twice is refused",
          mcd_runtime_channel_add(a.rt, "AGAIN", KEY_A, NULL) == MCD_CHANNEL_DUPLICATE);
    check("a key that is not base64 is refused",
          mcd_runtime_channel_add(a.rt, "BAD", "not base64!", NULL) == MCD_CHANNEL_BAD_KEY);
    check("a key of the wrong length is refused",
          mcd_runtime_channel_add(a.rt, "BAD", "Zm9vYmFy", NULL) == MCD_CHANNEL_BAD_KEY);
    check("an all-zero key is refused",
          mcd_runtime_channel_add(a.rt, "ZERO", "AAAAAAAAAAAAAAAAAAAAAA==", NULL) ==
              MCD_CHANNEL_BAD_KEY);
    /* 32 bytes whose upper 16 are zero: MeshCore's setChannel() would read it
     * as a 128-bit key and hash it over 16 bytes, while its addChannel()
     * would hash it over 32. Two peers could then derive different channel
     * hashes for one key and never reach each other, so it is refused with a
     * reason rather than joined. */
    check("a 32-byte key with an all-zero upper half is refused as ambiguous",
          mcd_runtime_channel_add(a.rt, "AMBIG",
                                  "AQIDBAUGBwgJCgsMDQ4PEAAAAAAAAAAAAAAAAAAAAAA=",
                                  NULL) == MCD_CHANNEL_AMBIGUOUS_KEY);
    check("an empty name is refused",
          mcd_runtime_channel_add(a.rt, "", KEY_B, NULL) == MCD_CHANNEL_BAD_NAME);

    /* ---- sending and receiving ---- */
    b.message_events = 0;
    a.message_events = 0;
    check("node A sends on the channel",
          mcd_runtime_send_channel_text(a.rt, 0, "site check", &msg_id) ==
              MCD_SEND_ACCEPTED_FLOOD);
    check("the send has a message id", msg_id != 0);
    check("node B received it", pumpUntil(air, [&] { return b.message_events >= 1; }));

    check("it is a channel message", b.last_msg.is_channel);
    check("in the slot node B holds that key in", b.last_msg.channel_slot == cb.slot);
    check("carrying node B's own name for the channel",
          strcmp(b.last_msg.channel_name, "site-b") == 0);
    check("and the channel hash that was on the air", b.last_msg.channel_hash == ca.hash);
    /* The text is the whole payload, prefix and all, and sender_name is the
     * claim parsed back out of it. Neither is authenticated: nothing signs a
     * group frame. */
    check("the text is the whole payload, prefix included",
          strcmp(b.last_msg.text, "K230-A: site check") == 0);
    check("with the sender's claimed name parsed out of it",
          strcmp(b.last_msg.sender_name, "K230-A") == 0);
    check("and no peer key at all: a group frame names no node",
          b.last_msg.peer_key[0] == 0 && b.last_msg.peer_name[0] == '\0');
    check("no acknowledgement is expected for it", !b.last_msg.ack_expected);
    check("and it is recorded as received", b.last_msg.state == MCD_MSG_RECEIVED);

    /* The sender's own copy. */
    {
        int total = mcd_runtime_message_count(a.rt);
        struct mcd_message mine;
        bool found = false;

        for (int i = 0; i < total; i++) {
            if (mcd_runtime_message_at(a.rt, i, &mine) && mine.id == msg_id) {
                found = true;
                break;
            }
        }
        check("node A kept its own copy", found);
        check("as an outgoing channel message", found && mine.outgoing && mine.is_channel);
        check("with the same bytes the receiver saw",
              found && strcmp(mine.text, "K230-A: site check") == 0);
        /* sent_flood is where this ends. There is no ACK for a group frame,
         * so nothing will ever move it to acked or to no_ack. */
        check("its state is sent_flood", found && mine.state == MCD_MSG_SENT_FLOOD);
        check("and no acknowledgement is expected", found && !mine.ack_expected);
    }
    check("node A did not receive its own message back",
          channelMessagesFor(a, 0) == 1);

    /* ---- a channel nobody holds ---- */
    check("sending on an empty slot is refused",
          mcd_runtime_send_channel_text(a.rt, 5, "nobody", &msg_id) == MCD_SEND_NO_CHANNEL);
    check("and so is a slot outside the table",
          mcd_runtime_send_channel_text(a.rt, MCD_MAX_CHANNELS, "nobody", &msg_id) ==
              MCD_SEND_NO_CHANNEL);

    /* ---- the length limit is a refusal, not a truncation ---- */
    {
        char body[MCD_MAX_TEXT + 4];

        memset(body, 'x', sizeof(body));
        body[ca.text_limit] = '\0';
        check("a body of exactly the limit is accepted",
              mcd_runtime_send_channel_text(a.rt, 0, body, &msg_id) == MCD_SEND_ACCEPTED_FLOOD);
        body[ca.text_limit] = 'x';
        body[ca.text_limit + 1] = '\0';
        /* Upstream's sendGroupMessage() would silently cut this to fit
         * (BaseChatMesh.cpp:496). Reporting success for a message somebody
         * typed and this node shortened is not something the service does. */
        check("one byte more is refused rather than truncated",
              mcd_runtime_send_channel_text(a.rt, 0, body, &msg_id) == MCD_SEND_TOO_LONG);
        check("and an empty body is refused",
              mcd_runtime_send_channel_text(a.rt, 0, "", &msg_id) == MCD_SEND_TOO_LONG);
    }

    /* ---- two channels, and messages kept apart ---- */
    {
        struct mcd_channel c2;
        int before;

        check("a second channel joins the next free slot",
              mcd_runtime_channel_add(a.rt, "OPS", KEY_B, &c2) == MCD_CHANNEL_OK &&
                  c2.slot == 1);
        check("node B joins it too",
              mcd_runtime_channel_add(b.rt, "OPS", KEY_B, NULL) == MCD_CHANNEL_OK);
        check("node A now holds two channels", mcd_runtime_channel_count(a.rt) == 2);

        before = channelMessagesFor(b, 1);
        b.message_events = 0;
        check("a message on the second channel is sent",
              mcd_runtime_send_channel_text(a.rt, 1, "ops only", &msg_id) ==
                  MCD_SEND_ACCEPTED_FLOOD);
        check("and arrives", pumpUntil(air, [&] { return b.message_events >= 1; }));
        check("on the second channel and not the first",
              channelMessagesFor(b, 1) == before + 1 && b.last_msg.channel_slot == 1);
    }

    /* ---- a key node B does not hold ---- */
    {
        struct mcd_channel c3;
        static const char KEY_C[] = "q6qpqKempaSjoqGgn56dnJuamZiXlpWUk5KRkI+OjYw=";
        int before = mcd_runtime_message_count(b.rt);

        check("node A joins a third channel alone",
              mcd_runtime_channel_add(a.rt, "PRIVATE", KEY_C, &c3) == MCD_CHANNEL_OK);
        check("and sends on it",
              mcd_runtime_send_channel_text(a.rt, c3.slot, "not for you", &msg_id) ==
                  MCD_SEND_ACCEPTED_FLOOD);
        pump(air, 30);
        check("node B, which does not hold that key, read nothing",
              mcd_runtime_message_count(b.rt) == before);
        mcd_runtime_stats(b.rt, &st);
        check("and counted a group frame it could not match",
              st.channel_frames_unmatched >= 1);
        check("node A's own copy is there", channelMessagesFor(a, c3.slot) == 1);
    }

    /* ---- leaving ---- */
    {
        struct mcd_channel gone;
        int held = mcd_runtime_channel_count(a.rt);

        a.channel_removed = 0;
        check("leaving a channel nobody is in is refused",
              mcd_runtime_channel_remove(a.rt, 6) == MCD_CHANNEL_NOT_FOUND);
        check("leaving channel 1 works", mcd_runtime_channel_remove(a.rt, 1) == MCD_CHANNEL_OK);
        /* mesh.channel_remove answers key_forgotten; a key still on the disk
         * ten seconds later would make that untrue. */
        check("and the removal is on disk at once", !mcd_runtime_dirty(a.rt));
        check("a removed event was raised",
              a.channel_removed == 1 && a.last_channel.slot == 1);
        check("one fewer channel is held", mcd_runtime_channel_count(a.rt) == held - 1);
        check("slot 1 is empty", !mcd_runtime_channel_by_slot(a.rt, 1, &gone));
        /* The slot is emptied, not compacted: every other channel keeps the
         * slot a client already knows it by. */
        check("and slot 0 is still the same channel",
              mcd_runtime_channel_by_slot(a.rt, 0, &gone) && gone.hash == ca.hash &&
                  strcmp(gone.name, "SITE") == 0);
        check("sending on the slot that was left is refused",
              mcd_runtime_send_channel_text(a.rt, 1, "gone", &msg_id) == MCD_SEND_NO_CHANNEL);
        /* And the freed slot is the next one an add takes. */
        check("a new channel takes the freed slot",
              mcd_runtime_channel_add(a.rt, "BACK", KEY_B, &gone) == MCD_CHANNEL_OK &&
                  gone.slot == 1);
        check("leaving it again", mcd_runtime_channel_remove(a.rt, 1) == MCD_CHANNEL_OK);
    }

    /* ---- the table is bounded ---- */
    {
        char key[64];
        int added = 0;
        enum mcd_channel_result rc = MCD_CHANNEL_OK;

        /* Distinct 32-byte keys, each with a non-zero upper half. */
        for (int i = 0; i < MCD_MAX_CHANNELS + 2; i++) {
            uint8_t raw[32];
            static const char alphabet[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            int o = 0;

            for (int j = 0; j < 32; j++) {
                raw[j] = (uint8_t)(0x80 + i * 3 + j);
            }
            for (int j = 0; j < 30; j += 3) {
                key[o++] = alphabet[raw[j] >> 2];
                key[o++] = alphabet[((raw[j] & 3) << 4) | (raw[j + 1] >> 4)];
                key[o++] = alphabet[((raw[j + 1] & 15) << 2) | (raw[j + 2] >> 6)];
                key[o++] = alphabet[raw[j + 2] & 63];
            }
            key[o++] = alphabet[raw[30] >> 2];
            key[o++] = alphabet[((raw[30] & 3) << 4) | (raw[31] >> 4)];
            key[o++] = alphabet[(raw[31] & 15) << 2];
            key[o++] = '=';
            key[o] = '\0';
            rc = mcd_runtime_channel_add(a.rt, "FILL", key, NULL);
            if (rc != MCD_CHANNEL_OK) {
                break;
            }
            added++;
        }
        check("the table fills and then refuses", rc == MCD_CHANNEL_FULL);
        check("with every slot taken", mcd_runtime_channel_count(a.rt) == MCD_MAX_CHANNELS);
        check("and it took as many as there were free slots",
              added == MCD_MAX_CHANNELS - 2);
        mcd_runtime_stats(a.rt, &st);
        check("the stats agree", st.channels == MCD_MAX_CHANNELS);

        /* Back to two, so what follows is not testing a full table. */
        for (int i = 2; i < MCD_MAX_CHANNELS; i++) {
            mcd_runtime_channel_remove(a.rt, i);
        }
        check("and back down again", mcd_runtime_channel_count(a.rt) == 2);
    }
}

/* ---- the empty-slot guard, from the air --------------------------------
 *
 * The one thing enabling channels could have let through. An unused slot in
 * MeshCore's table is all zeroes, so its hash byte is 0 and its key is 32
 * zero bytes; an unguarded node offers that key to any frame whose channel
 * hash is 0, and the key is not a secret. This builds exactly such a frame -
 * real AES, real HMAC, encrypted to the all-zero key - and delivers it. */
static void test_channel_empty_slot_guard(Node& a, Air& air)
{
    uint8_t zero_key[32];
    uint8_t plain[32];
    uint8_t payload[MCD_MAX_FRAME];
    uint8_t frame[MCD_MAX_FRAME];
    int before = mcd_runtime_message_count(a.rt);
    int plen = 0;
    int flen;

    memset(zero_key, 0, sizeof(zero_key));
    /* A GRP_TXT payload: timestamp(4), txt_type(1), then "name: text". */
    memset(plain, 0, sizeof(plain));
    plain[0] = 0x01;
    plain[4] = 0;  /* TXT_TYPE_PLAIN */
    memcpy(&plain[5], "EVIL: pwn", 9);

    payload[plen++] = 0x00;  /* the channel hash an unused slot carries */
    plen += mesh::Utils::encryptThenMAC(zero_key, &payload[plen], plain, 5 + 9);
    flen = buildFrame(frame, (uint8_t)(PAYLOAD_TYPE_GRP_TXT << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD,
                      payload, plen);

    {
        struct mcd_rx_meta meta;

        defaultMeta(meta);
        check("the frame encrypted to the all-zero key is delivered",
              mcd_runtime_deliver_rx(a.rt, frame, flen, &meta));
    }
    pump(air, 20);
    check("and the node did not accept it as a message",
          mcd_runtime_message_count(a.rt) == before);
}

/* ---- channels survive a restart ---------------------------------------- */
static void test_channel_restart(Node& a, Air& air)
{
    struct mcd_runtime_hooks hooks;
    struct mcd_runtime_config cfg;
    char err[256] = "";
    struct mcd_channel before[MCD_MAX_CHANNELS];
    int n_before = mcd_runtime_channel_count(a.rt);
    uint64_t msg_id = 0;

    for (int i = 0; i < n_before; i++) {
        check("a held channel reads back", mcd_runtime_channel_at(a.rt, i, &before[i]));
    }
    check("there are channels to lose", n_before > 0);

    /* Persist, then stop and start again against the same directory. */
    /* Nothing is waiting to be written: the channels went to disk as each
     * one was joined, not on the daemon's timer, so this restart is not
     * standing on a persist the test performed for it. The node table is
     * written here as the daemon would, and is allowed to be clean too. */
    check("the channels are already written", !mcd_runtime_dirty(a.rt));
    check("and writing again is harmless", mcd_runtime_persist(a.rt) == 0);
    mcd_runtime_destroy(a.rt);

    memset(&hooks, 0, sizeof(hooks));
    hooks.tx_submit = hook_tx_submit;
    hooks.on_node = hook_on_node;
    hooks.on_message = hook_on_message;
    hooks.on_channel = hook_on_channel;
    hooks.on_frame = hook_on_frame;
    hooks.user = &a;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_dir = a.dir;
    cfg.node_name = "K230-A";

    a.rt = mcd_runtime_create(&cfg, &hooks, err, sizeof(err));
    check("the node starts again", a.rt != NULL);
    if (!a.rt) {
        return;
    }
    check("with the same number of channels", mcd_runtime_channel_count(a.rt) == n_before);
    for (int i = 0; i < n_before; i++) {
        struct mcd_channel now;

        check("a channel came back", mcd_runtime_channel_at(a.rt, i, &now));
        check("in the same slot", now.slot == before[i].slot);
        check("with the same name", strcmp(now.name, before[i].name) == 0);
        /* The hash is derived from the key, so it coming back identical is
         * what says the KEY came back - the one thing that is not reported
         * over IPC and cannot be checked directly. */
        check("and the same derived hash, so the same key", now.hash == before[i].hash);
        check("and the same key length", now.key_bits == before[i].key_bits);
    }
    /* Restoring is not a change: a start that rewrote the file it had just
     * read would churn the flash on every boot. */
    check("a restored table is not dirty", !mcd_runtime_dirty(a.rt));
    /* And it still works, which a hash comparison alone would not prove. */
    mcd_runtime_set_radio_online(a.rt, true);
    check("and a restored channel can still be sent on",
          mcd_runtime_send_channel_text(a.rt, before[0].slot, "after a restart", &msg_id) ==
              MCD_SEND_ACCEPTED_FLOOD);
    pump(air, 5);

    /* Messages, by contrast, do NOT survive: they are runtime-only, and this
     * says so rather than leaving it to be discovered. */
    check("but the messages did not come back",
          mcd_runtime_message_count(a.rt) == 1);
}

/* ---- an unreadable channels.v1 is survivable --------------------------- */
static void test_corrupt_channels_is_survivable(void)
{
    struct mcd_runtime_hooks hooks;
    struct mcd_runtime_config cfg;
    struct mcd_runtime* rt;
    char err[256] = "";
    char dir[300];
    char path[400];
    char fault[256] = "";
    Node holder;
    Air air;
    FILE* f;

    snprintf(dir, sizeof(dir), "%s/corrupt-channels", g_root);
    {
        char store_err[mcdstore::ERR_SIZE] = "";

        check("a directory for it", mcdstore::ensureDir(dir, store_err));
    }
    snprintf(path, sizeof(path), "%s/channels.v1", dir);
    f = fopen(path, "wb");
    check("a channels.v1 this build will not read", f != NULL);
    if (f) {
        /* The right magic and a version this build does not know, so the
         * refusal is the version check and not the magic. */
        fwrite("MCDC\x09\x00\x00\x00\x00\x00\x00\x00", 1, 12, f);
        fclose(f);
    }

    holder.air = &air;
    memset(&hooks, 0, sizeof(hooks));
    hooks.tx_submit = hook_tx_submit;
    hooks.on_node = hook_on_node;
    hooks.on_message = hook_on_message;
    hooks.on_channel = hook_on_channel;
    hooks.on_frame = hook_on_frame;
    hooks.user = &holder;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_dir = dir;
    cfg.node_name = "K230-C";

    rt = mcd_runtime_create(&cfg, &hooks, err, sizeof(err));
    /* Not fatal. A daemon that will not start is a node off the air, and the
     * channels are a cache of what an operator typed - painful to lose, but
     * not a reason to take the radio down. */
    check("the runtime starts anyway", rt != NULL);
    if (!rt) {
        return;
    }
    check("with no channels", mcd_runtime_channel_count(rt) == 0);
    check("and says what happened to them", mcd_runtime_channel_fault(rt, fault, sizeof(fault)));
    check("naming the version it could not read", strstr(fault, "version") != NULL);
    check("and where the file was kept", strstr(fault, "kept as") != NULL);
    /* Reported separately from the node table's fault, because the two
     * losses are not comparable: the mesh re-advertises a forgotten node. */
    check("the node state is reported as fine",
          !mcd_runtime_state_fault(rt, fault, sizeof(fault)));
    {
        struct stat sb;

        snprintf(path, sizeof(path), "%s/channels.v1.corrupt.0", dir);
        check("the unreadable file was kept, not deleted", stat(path, &sb) == 0);
        snprintf(path, sizeof(path), "%s/channels.v1", dir);
        check("and is no longer in the way", stat(path, &sb) != 0);
    }
    /* A channel can be joined again on top of it. */
    check("a channel can be joined after the fault",
          mcd_runtime_channel_add(rt, "RECOVERED", KEY_A, NULL) == MCD_CHANNEL_OK);
    check("and written", mcd_runtime_persist(rt) == 0);
    mcd_runtime_destroy(rt);
}

/* ---- each message waits for its own ACK ---------------------------------- */

/* A message's state as the runtime holds it, or -1 when it is not held. */
static int stateOf(mcd_runtime* rt, uint64_t id)
{
    int n = mcd_runtime_message_count(rt);
    struct mcd_message m;

    for (int i = 0; i < n; i++) {
        if (mcd_runtime_message_at(rt, i, &m) && m.id == id) {
            return (int)m.state;
        }
    }
    return -1;
}

/* Every sent message is watched for its ACK against its OWN deadline.
 *
 * MeshCore keeps one timeout for the whole node: each send overwrites it and
 * any matched ACK clears it (BaseChatMesh.cpp:339/350/451/455). Before this
 * service kept its own deadlines, an ACK for one message cancelled the
 * timeout of another still in flight - which then stayed `sent_*` for ever -
 * and a timeout that did fire was pinned on the oldest message, not the one
 * it was for. The deadlines are driven here through mcd_runtime_expire_acks
 * with times computed from the reported timeouts, because the real ones are
 * six to twelve seconds long. */
static void test_ack_deadlines(Node& a, Node& b, Air& air)
{
    Node c;
    uint8_t b_key[MCD_PUB_KEY_LEN];
    uint8_t c_key[MCD_PUB_KEY_LEN];
    char name[MCD_NODE_NAME_LEN];
    struct mcd_node node;
    uint64_t m1 = 0, m2 = 0, m3 = 0, id = 0;
    uint32_t t1 = 0, t2 = 0, t3 = 0, t = 0;
    uint64_t sent1_lo, sent1_hi, sent3_hi;
    int path_events;

    check("a third node starts", makeNode(c, air, "K230-C", NULL));
    if (!c.rt) {
        return;
    }
    mcd_runtime_set_radio_online(a.rt, true);
    mcd_runtime_set_radio_online(b.rt, true);
    mcd_runtime_set_radio_online(c.rt, true);
    mcd_runtime_identity(b.rt, b_key, name, sizeof(name));
    mcd_runtime_identity(c.rt, c_key, name, sizeof(name));

    /* A knows B and C, C knows A, and A has a direct route to C. */
    a.node_discovered = 0;
    c.node_discovered = 0;
    waitForANewSecond(air);
    check("A adverts again", mcd_runtime_send_advert(a.rt));
    check("B adverts again", mcd_runtime_send_advert(b.rt));
    check("C adverts", mcd_runtime_send_advert(c.rt));
    check("A holds B and C, and C holds A", pumpUntil(air, [&] {
              uint8_t a_key[MCD_PUB_KEY_LEN];
              char nm[MCD_NODE_NAME_LEN];

              mcd_runtime_identity(a.rt, a_key, nm, sizeof(nm));
              return mcd_runtime_node_by_prefix(a.rt, b_key, 8, &node) == 1 &&
                     mcd_runtime_node_by_prefix(a.rt, c_key, 8, &node) == 1 &&
                     mcd_runtime_node_by_prefix(c.rt, a_key, 8, &node) == 1;
          }));
    check("A writes to C once, to learn a route",
          mcd_runtime_send_text(a.rt, c_key, 8, "route please", &id, &t) ==
              MCD_SEND_ACCEPTED_FLOOD);
    check("which C's answer teaches", pumpUntil(air, [&] {
              return stateOf(a.rt, id) == MCD_MSG_ACKED &&
                     mcd_runtime_node_by_prefix(a.rt, c_key, 8, &node) == 1 && node.path_known;
          }));
    check("nothing is left waiting", mcd_runtime_acks_waiting(a.rt) == 0);

    /* ---- resetting a route ---- */
    path_events = a.node_path;
    check("the route to B is forgotten on request",
          mcd_runtime_node_reset_path(a.rt, b_key, &node) && !node.path_known);
    check("and subscribers are told, as a path change", a.node_path == path_events + 1);
    check("so the next message to B floods", mcd_runtime_node_by_prefix(a.rt, b_key, 8, &node) ==
                                                     1 && !node.path_known);

    /* ---- an ACK for one message does not strand another ---- */
    air.deliver = false;
    sent1_lo = nowMs();
    check("M1 goes to B by flood, into an air that carries nothing",
          mcd_runtime_send_text(a.rt, b_key, 8, "lost one", &m1, &t1) == MCD_SEND_ACCEPTED_FLOOD);
    sent1_hi = nowMs();
    pump(air, 5);
    air.deliver = true;
    check("M2 goes to C directly",
          mcd_runtime_send_text(a.rt, c_key, 8, "kept one", &m2, &t2) ==
              MCD_SEND_ACCEPTED_DIRECT);
    check("and C acknowledges it",
          pumpUntil(air, [&] { return stateOf(a.rt, m2) == MCD_MSG_ACKED; }));
    check("M1 is still waiting for its own answer, not closed by M2's",
          stateOf(a.rt, m1) == MCD_MSG_SENT_FLOOD);
    check("one message is still watched", mcd_runtime_acks_waiting(a.rt) == 1);

    /* ---- the timeout lands on the message it belongs to ---- */
    air.deliver = false;
    check("M3 goes to C directly, and is lost",
          mcd_runtime_send_text(a.rt, c_key, 8, "lost two", &m3, &t3) ==
              MCD_SEND_ACCEPTED_DIRECT);
    sent3_hi = nowMs();
    pump(air, 5);
    air.deliver = true;
    /* Sent later with the shorter (direct) timeout, M3 falls due first. The
     * old rule - "the oldest unanswered one" - would have marked M1. */
    check("M3 falls due before M1 does", sent3_hi + t3 < sent1_lo + t1);
    {
        uint64_t between = (sent3_hi + t3 + sent1_lo + t1) / 2;

        check("at a time between the two deadlines exactly one message times out",
              mcd_runtime_expire_acks(a.rt, between) == 1);
        check("and it is M3", stateOf(a.rt, m3) == MCD_MSG_NO_ACK);
        check("M1 is still waiting", stateOf(a.rt, m1) == MCD_MSG_SENT_FLOOD);
        check("M2 is still acknowledged", stateOf(a.rt, m2) == MCD_MSG_ACKED);
    }
    check("at M1's own deadline M1 times out",
          mcd_runtime_expire_acks(a.rt, sent1_hi + t1) == 1 &&
              stateOf(a.rt, m1) == MCD_MSG_NO_ACK);
    check("and nothing is left waiting", mcd_runtime_acks_waiting(a.rt) == 0);
    check("a deadline is not answered twice", mcd_runtime_expire_acks(a.rt, sent1_hi + t1) == 0);

    /* ---- a full watch refuses rather than overwriting ---- */
    {
        int accepted = 0;
        int msgs_before;
        int submits_before;
        uint64_t last = 0;

        air.deliver = false;
        for (int i = 0; i < 8; i++) {
            if (mcd_runtime_send_text(a.rt, c_key, 8, "one of eight", &last, &t) ==
                MCD_SEND_ACCEPTED_DIRECT) {
                accepted++;
            }
        }
        /* Long enough for all eight to have been transmitted, so the count
         * below can only move if the ninth were built. */
        pump(air, 80);
        check("eight messages can wait at once", accepted == 8 &&
                                                     mcd_runtime_acks_waiting(a.rt) == 8);
        msgs_before = mcd_runtime_message_count(a.rt);
        submits_before = a.tx_submits;
        check("a ninth is refused as busy",
              mcd_runtime_send_text(a.rt, c_key, 8, "ninth", &id, &t) == MCD_SEND_BUSY);
        pump(air, 10);
        check("and is neither recorded nor transmitted",
              mcd_runtime_message_count(a.rt) == msgs_before && a.tx_submits == submits_before);
        check("none of the eight was pushed out to make room",
              mcd_runtime_acks_waiting(a.rt) == 8 &&
                  stateOf(a.rt, last) == MCD_MSG_SENT_DIRECT);
        /* Requests are served between ticks. Once every deadline has
         * passed, a send that comes before the next tick has answered them
         * answers them itself, rather than being turned away as busy by
         * eight messages nobody is waiting for any more. No tick, and no
         * mcd_runtime_expire_acks, between the wait and the send. */
        {
            uint64_t due = nowMs() + t + 100;

            while (nowMs() < due) {
                usleep(10000);
            }
        }
        air.deliver = true;
        check("once their deadlines have passed, a send is not turned away as busy",
              mcd_runtime_send_text(a.rt, c_key, 8, "room again", &id, &t) ==
                  MCD_SEND_ACCEPTED_DIRECT);
        check("the eight were answered first, each as no ACK",
              stateOf(a.rt, last) == MCD_MSG_NO_ACK && mcd_runtime_acks_waiting(a.rt) == 1);
        check("and the new one is acknowledged",
              pumpUntil(air, [&] { return stateOf(a.rt, id) == MCD_MSG_ACKED; }));
    }

    /* ---- an ACK that came in time is not overtaken by its deadline ---- */
    {
        uint64_t m4 = 0;
        uint64_t sent4_hi;
        uint8_t ahead[MCD_MAX_FRAME];
        int ahead_len = 0;
        bool queued = false;
        struct mcd_rx_meta meta;

        check("M4 goes to C directly",
              mcd_runtime_send_text(a.rt, c_key, 8, "answer me", &m4, &t) ==
                  MCD_SEND_ACCEPTED_DIRECT);
        sent4_hi = nowMs();
        /* The air, carried by hand: when C's answer goes out, A's receive
         * queue gets another frame first - A's own M4, heard back, which it
         * drops as already seen. The daemon hands over one frame a turn, so
         * the ACK waits a turn behind it. */
        for (int step = 0; step < 600 && !queued; step++) {
            for (int i = 0; i < air.count; i++) {
                mcd_runtime_tick(air.nodes[i]->rt);
            }
            int n = air.qn;

            air.qn = 0;
            for (int q = 0; q < n; q++) {
                Frame& f = air.queue[q];

                defaultMeta(meta);
                if (f.from == a.index && f.len <= (int)sizeof(ahead)) {
                    memcpy(ahead, f.bytes, (size_t)f.len);
                    ahead_len = f.len;
                }
                if (f.from == c.index && ahead_len > 0 && !queued) {
                    mcd_runtime_deliver_rx(a.rt, ahead, ahead_len, &meta);
                    queued = true;
                }
                for (int i = 0; i < air.count; i++) {
                    if (i != f.from) {
                        mcd_runtime_deliver_rx(air.nodes[i]->rt, f.bytes, f.len, &meta);
                    }
                }
                mcd_runtime_tx_done(air.nodes[f.from]->rt, f.submit_id, MCD_TX_OK);
            }
            usleep(10000);
        }
        check("C's answer reaches A behind another frame, before M4's deadline",
              queued && mcd_runtime_rx_pending(a.rt) && nowMs() < sent4_hi + t);
        {
            uint64_t due = sent4_hi + t + 100;

            while (nowMs() < due) {
                usleep(10000);
            }
        }
        /* The deadline has passed with the ACK sitting in the queue. */
        mcd_runtime_tick(a.rt);
        check("one turn takes the frame ahead of it, and the ACK still waits",
              mcd_runtime_rx_pending(a.rt));
        check("so M4 is not called unacknowledged while its ACK is queued",
              stateOf(a.rt, m4) == MCD_MSG_SENT_DIRECT);
        mcd_runtime_tick(a.rt);
        check("the next turn takes the ACK, and M4 is acknowledged",
              stateOf(a.rt, m4) == MCD_MSG_ACKED && mcd_runtime_acks_waiting(a.rt) == 0);
        pump(air, 5);
    }

    /* ---- forgetting a node ---- */
    {
        int removed_before = a.node_removed;
        bool persisted = false;
        struct mcd_node was;
        uint8_t nobody[MCD_PUB_KEY_LEN];

        memset(&was, 0, sizeof(was));
        check("C is forgotten on request", mcd_runtime_node_remove(a.rt, c_key, &was, &persisted));
        check("and the forgetting is written to the node table", persisted);
        check("the answer is C as it was, route included",
              memcmp(was.public_key, c_key, MCD_PUB_KEY_LEN) == 0 && was.path_known);
        check("subscribers are told it was removed", a.node_removed == removed_before + 1 &&
                                                          memcmp(a.last_removed.public_key,
                                                                 c_key, MCD_PUB_KEY_LEN) == 0);
        check("A no longer holds C", mcd_runtime_node_by_prefix(a.rt, c_key, 8, &node) == 0);
        check("and has written that down already", !mcd_runtime_dirty(a.rt));
        check("a message to C is refused: there is no contact to encrypt to",
              mcd_runtime_send_text(a.rt, c_key, 8, "hello?", &id, &t) == MCD_SEND_NO_CONTACT);
        check("forgetting C twice is refused", !mcd_runtime_node_remove(a.rt, c_key, &was, NULL));
        memset(nobody, 0x5a, sizeof(nobody));
        check("so is forgetting a node never held", !mcd_runtime_node_remove(a.rt, nobody, &was, NULL));
        check("or resetting its route", !mcd_runtime_node_reset_path(a.rt, nobody, &node));
        check("B is untouched", mcd_runtime_node_by_prefix(a.rt, b_key, 8, &node) == 1);
    }

    /* ---- and it comes back when it adverts, zero-hop included ---- */
    {
        int discovered = a.node_discovered;
        bool zero_hop_frame = false;

        waitForANewSecond(air);
        air.deliver = false;
        air.qn = 0;
        check("C sends a zero-hop advert", mcd_runtime_send_advert_zero_hop(c.rt));
        for (int i = 0; i < 40 && air.qn == 0; i++) {
            mcd_runtime_tick(c.rt);
            usleep(10000);
        }
        if (air.qn > 0) {
            const Frame& f = air.queue[0];

            /* Header route bits DIRECT with an empty path: MeshCore's
             * zero-hop, which a repeater does not forward. */
            zero_hop_frame = f.len > 2 && (f.bytes[0] & PH_ROUTE_MASK) == ROUTE_TYPE_DIRECT &&
                             f.bytes[1] == 0;
        }
        check("the frame is zero-hop: direct, with no path", zero_hop_frame);
        air.deliver = true;
        {
            /* Hand the captured frame to the air as if it had just gone out. */
            int n = air.qn;

            air.qn = 0;
            for (int q = 0; q < n; q++) {
                struct mcd_rx_meta meta;

                defaultMeta(meta);
                mcd_runtime_deliver_rx(a.rt, air.queue[q].bytes, air.queue[q].len, &meta);
                mcd_runtime_tx_done(c.rt, air.queue[q].submit_id, MCD_TX_OK);
            }
        }
        check("A learns C again from it", pumpUntil(air, [&] {
                  return a.node_discovered > discovered &&
                         mcd_runtime_node_by_prefix(a.rt, c_key, 8, &node) == 1;
              }));
        check("with no route yet: the old one was forgotten with it", !node.path_known);
    }

    air.nodes[c.index] = NULL;
    air.count = c.index;
    mcd_runtime_destroy(c.rt);
    c.rt = NULL;
}

/* ---- app datagrams ------------------------------------------------------ */

/* A REQ from `from` to `to`, as MeshCore's sendRequest() builds one: the
 * inner bytes (tag first) encrypted and MACed to the pair's shared secret.
 * Zero hops, flood or direct. */
static int craftReq(uint8_t* frame, const mesh::LocalIdentity& from, const mesh::Identity& to,
                    bool flood, const uint8_t* inner, int inner_len)
{
    uint8_t secret[PUB_KEY_SIZE];
    uint8_t payload[MAX_PACKET_PAYLOAD];
    int len = 0;

    from.calcSharedSecret(secret, to);
    payload[len++] = to.pub_key[0];
    payload[len++] = from.pub_key[0];
    len += mesh::Utils::encryptThenMAC(secret, &payload[len], inner, inner_len);
    return buildFrame(frame,
                      (uint8_t)((PAYLOAD_TYPE_REQ << PH_TYPE_SHIFT) |
                                (flood ? ROUTE_TYPE_FLOOD : ROUTE_TYPE_DIRECT)),
                      payload, len);
}

/* Hand B a crafted REQ from A and let B handle it. */
static void deliverReq(Node& b, Air& air, const mesh::LocalIdentity& a_id,
                       const mesh::LocalIdentity& b_id, bool flood, uint32_t tag,
                       const uint8_t* data, int data_len)
{
    uint8_t inner[MAX_PACKET_PAYLOAD];
    uint8_t frame[MCD_MAX_FRAME];
    struct mcd_rx_meta meta;
    int len;

    memcpy(inner, &tag, 4);
    memcpy(inner + 4, data, (size_t)data_len);
    len = craftReq(frame, a_id, b_id, flood, inner, 4 + data_len);
    defaultMeta(meta);
    mcd_runtime_deliver_rx(b.rt, frame, len, &meta);
    for (int i = 0; i < 20 && mcd_runtime_rx_pending(b.rt); i++) {
        mcd_runtime_tick(b.rt);
    }
    pump(air, 3);
}

static void test_app_datagrams(Node& a, Node& b, Air& air, const mesh::LocalIdentity& a_id,
                               const mesh::LocalIdentity& b_id)
{
    uint8_t a_key[MCD_PUB_KEY_LEN];
    uint8_t b_key[MCD_PUB_KEY_LEN];
    char name[MCD_NODE_NAME_LEN];
    struct mcd_runtime_stats st;
    struct mcd_app_datagram got[MCD_APP_INBOX];
    struct mcd_node node;
    uint8_t payload[MCD_APP_PAYLOAD_MAX + 1];
    uint32_t est = 0;
    uint64_t first_id;
    int n;

    mcd_runtime_identity(a.rt, a_key, name, sizeof(name));
    mcd_runtime_identity(b.rt, b_key, name, sizeof(name));
    check("A still holds B", mcd_runtime_node_reset_path(a.rt, b_key, &node));

    /* ---- the first one floods, and the receipt teaches A a route ---- */
    b.app_events = 0;
    check("A sends B an app datagram",
          mcd_runtime_send_app(a.rt, b_key, 1, (const uint8_t*)"\x41\x00\x00\x01\x00", 5, &est) ==
              MCD_SEND_ACCEPTED_FLOOD);
    check("with MeshCore's estimate of an answer's time", est > 0);
    check("B receives it", pumpUntil(air, [&] { return b.app_events >= 1; }));
    check("on the port it was sent to", b.last_app.port == 1);
    check("with the payload whole, and its own length rather than the padded one",
          b.last_app.len == 5 && memcmp(b.last_app.payload, "\x41\x00\x00\x01\x00", 5) == 0);
    check("from A, by A's whole key", memcmp(b.last_app.from, a_key, MCD_PUB_KEY_LEN) == 0);
    check("by flood", b.last_app.flood);
    check("with the signal it came in on", b.last_app.rssi_known && b.last_app.snr_known);
    first_id = b.last_app.id;
    check("numbered from 1", first_id >= 1);
    check("B answers a flood with a receipt, and A learns a direct route to B",
          pumpUntil(air, [&] {
              return mcd_runtime_node_by_prefix(a.rt, b_key, 8, &node) == 1 && node.path_known;
          }));
    mcd_runtime_stats(b.rt, &st);
    check("B counted one received and one receipt", st.app_rx == 1 && st.app_receipts == 1);
    mcd_runtime_stats(a.rt, &st);
    check("A counted one sent", st.app_tx == 1);

    /* ---- the second goes direct, and is not answered ---- */
    b.app_events = 0;
    for (int i = 0; i < MCD_APP_PAYLOAD_MAX; i++) {
        payload[i] = (uint8_t)(i * 7 + 1);
    }
    check("the next one goes direct",
          mcd_runtime_send_app(a.rt, b_key, 1, payload, MCD_APP_PAYLOAD_MAX, &est) ==
              MCD_SEND_ACCEPTED_DIRECT);
    check("B receives it", pumpUntil(air, [&] { return b.app_events >= 1; }));
    check("all 160 bytes of it", b.last_app.len == MCD_APP_PAYLOAD_MAX &&
                                     memcmp(b.last_app.payload, payload, MCD_APP_PAYLOAD_MAX) == 0);
    check("not by flood", !b.last_app.flood);
    check("with the next id", b.last_app.id == first_id + 1);
    pump(air, 60);
    mcd_runtime_stats(b.rt, &st);
    check("and a direct one gets no receipt", st.app_rx == 2 && st.app_receipts == 1);

    /* ---- what send refuses ---- */
    {
        uint8_t nobody[MCD_PUB_KEY_LEN];

        memset(nobody, 0x5a, sizeof(nobody));
        check("port 0 is refused",
              mcd_runtime_send_app(a.rt, b_key, 0, payload, 4, &est) == MCD_SEND_TOO_LONG);
        check("so is port 16",
              mcd_runtime_send_app(a.rt, b_key, 16, payload, 4, &est) == MCD_SEND_TOO_LONG);
        check("and an empty payload",
              mcd_runtime_send_app(a.rt, b_key, 1, payload, 0, &est) == MCD_SEND_TOO_LONG);
        check("and one byte over 160",
              mcd_runtime_send_app(a.rt, b_key, 1, payload, MCD_APP_PAYLOAD_MAX + 1, &est) ==
                  MCD_SEND_TOO_LONG);
        check("and a node that is not held",
              mcd_runtime_send_app(a.rt, nobody, 1, payload, 4, &est) == MCD_SEND_NO_CONTACT);
    }

    /* ---- what receive refuses: MeshCore's own requests, and bytes that do
     * not describe themselves ---- */
    {
        int events = b.app_events;
        int submits = b.tx_submits;
        const uint8_t stats_req[9] = { 0x01, 0, 0, 0, 0, 1, 2, 3, 4 };
        const uint8_t port0[4] = { 0xD0, 2, 1, 2 };
        const uint8_t overlong[5] = { 0xD1, 50, 1, 2, 3 };
        const uint8_t zero_len[3] = { 0xD1, 0, 9 };

        deliverReq(b, air, a_id, b_id, true, 0x11111111u, stats_req, sizeof(stats_req));
        check("an upstream request type is not an app datagram", b.app_events == events);
        check("and this node still serves no requests: nothing is sent back",
              b.tx_submits == submits);
        deliverReq(b, air, a_id, b_id, true, 0x22222222u, port0, sizeof(port0));
        check("port 0 is not a port", b.app_events == events);
        deliverReq(b, air, a_id, b_id, true, 0x33333333u, overlong, sizeof(overlong));
        check("a length longer than what arrived is refused, padding and all",
              b.app_events == events);
        deliverReq(b, air, a_id, b_id, true, 0x44444444u, zero_len, sizeof(zero_len));
        check("and so is a length of 0", b.app_events == events && b.tx_submits == submits);
    }

    /* ---- the inbox ---- */
    n = mcd_runtime_app_inbox(b.rt, 1, 0, got, MCD_APP_INBOX);
    check("the inbox holds both, oldest first",
          n == 2 && got[0].id == first_id && got[1].id == first_id + 1);
    n = mcd_runtime_app_inbox(b.rt, 1, first_id, got, MCD_APP_INBOX);
    check("after_id skips what the client has", n == 1 && got[0].id == first_id + 1);
    check("and another port's inbox is its own",
          mcd_runtime_app_inbox(b.rt, 2, 0, got, MCD_APP_INBOX) == 0);
    check("port 0 has no inbox", mcd_runtime_app_inbox(b.rt, 0, 0, got, MCD_APP_INBOX) == 0);
    {
        const uint8_t on2[3] = { 0xD2, 1, 0x77 };

        deliverReq(b, air, a_id, b_id, false, 0x55555555u, on2, sizeof(on2));
        n = mcd_runtime_app_inbox(b.rt, 2, 0, got, MCD_APP_INBOX);
        check("a datagram on port 2 is held for port 2",
              n == 1 && got[0].port == 2 && got[0].len == 1 && got[0].payload[0] == 0x77);
    }
    /* Bounded: the oldest go first, and ids keep counting. */
    for (int i = 0; i < MCD_APP_INBOX + 8; i++) {
        uint8_t d[4] = { 0xD1, 2, (uint8_t)i, 0xEE };

        deliverReq(b, air, a_id, b_id, false, 0x60000000u + (uint32_t)i, d, sizeof(d));
    }
    n = mcd_runtime_app_inbox(b.rt, 1, 0, got, MCD_APP_INBOX);
    check("the inbox is bounded", n == MCD_APP_INBOX);
    check("and keeps the newest", n > 0 && got[n - 1].payload[0] == MCD_APP_INBOX + 7 &&
                                      got[n - 1].id == first_id + 3 + MCD_APP_INBOX + 7);
    mcd_runtime_stats(b.rt, &st);
    check("every one was counted", st.app_rx == (uint64_t)(3 + MCD_APP_INBOX + 8));
}

int main(void)
{
    char tmpl[] = "/tmp/meshcored-runtime-XXXXXX";
    char* root = mkdtemp(tmpl);
    Air air;
    Node a;
    Node b;
    mesh::LocalIdentity a_id;
    mesh::LocalIdentity b_id;
    char store_err[mcdstore::ERR_SIZE] = "";

    if (!root) {
        fprintf(stderr, "cannot create a temporary directory: %s\n", strerror(errno));
        return 2;
    }
    snprintf(g_root, sizeof(g_root), "%s", root);

    check("an identity for A", mcdstore::identityCreate(a_id, store_err));
    check("an identity for B", mcdstore::identityCreate(b_id, store_err));
    check("node A starts", makeNode(a, air, "K230-A", &a_id));
    check("node B starts", makeNode(b, air, "K230-B", &b_id));
    if (!a.rt || !b.rt) {
        printf("meshcored_runtime_test: %d check(s), %d failure(s)\n", checks, failed + 1);
        return 1;
    }

    test_offline(a);
    test_rx_queue(a);
    test_telemetry_stays_unknown(a, b);
    test_tx_outcomes(a, air);
    test_two_nodes(a, b, air);
    test_duplicate_suppression(a, b, air);
    test_advert_hops(a, b, air);
    test_path_guard(a, b, air, a_id, b_id);
    test_restart(a, air);
    test_rename_and_path_hash(a, b, air);
    test_rename_over_config();
    test_public_channel();
    test_channels(a, b, air);
    test_channel_empty_slot_guard(a, air);
    test_channel_restart(a, air);
    test_corrupt_channels_is_survivable();
    test_corrupt_identity_stops_the_runtime();
    test_corrupt_state_is_survivable();
    test_hostile_remote_text();
    test_location();
    test_full_contact_table();
    test_nodes_newest_first();
    test_ack_deadlines(a, b, air);
    test_app_datagrams(a, b, air, a_id, b_id);

    mcd_runtime_destroy(a.rt);
    mcd_runtime_destroy(b.rt);
    {
        char cmd[512];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_root);
        if (system(cmd) != 0) {
            fprintf(stderr, "note: could not remove %s\n", g_root);
        }
    }
    printf("meshcored_runtime_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
