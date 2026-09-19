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
    int message_events;
    int frame_events;
    char last_message[MCD_MAX_TEXT + 1];
    char last_message_peer[MCD_NODE_NAME_LEN];
    char last_frame_type[24];
    bool last_frame_rssi_known;
    bool last_frame_snr_known;
    int tx_submits;

    Node() : rt(NULL), air(NULL), index(0), node_events(0), node_discovered(0),
             node_path(0), message_events(0), frame_events(0), tx_submits(0)
    {
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
    }
    (void)nd;
}

static void hook_on_message(void* user, const struct mcd_message* m)
{
    Node* n = (Node*)user;

    n->message_events++;
    snprintf(n->last_message, sizeof(n->last_message), "%s", m->text);
    snprintf(n->last_message_peer, sizeof(n->last_message_peer), "%s", m->peer_name);
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
    hooks.on_frame = hook_on_frame;
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
        uint8_t path_before = 0;

        if (mcd_runtime_node_by_prefix(b.rt, a_key, 8, &a_seen_by_b) == 1) {
            path_before = a_seen_by_b.path_len;
        }

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
        /* Asserted on the stored path rather than on a counter of events:
         * the two nodes are still exchanging return paths from the message
         * above, so "no path update happened at all" would be a claim about
         * the rest of the mesh going quiet. What must hold is that THIS
         * payload's 63 hops were not adopted. */
        check("and its impossible path was not adopted",
              mcd_runtime_node_by_prefix(b.rt, a_key, 8, &a_seen_by_b) != 1 ||
              (a_seen_by_b.path_len != 63 && a_seen_by_b.path_len == path_before));
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
    test_path_guard(a, b, air, a_id, b_id);
    test_restart(a, air);
    test_corrupt_identity_stops_the_runtime();

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
