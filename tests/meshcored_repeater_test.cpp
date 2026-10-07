/*
 * meshcored: repeater discovery and the repeater session, against a repeater.
 *
 * Two halves. The first holds mesh_remote.h's rules to account on bytes
 * built by hand: the RepeaterStats tiers, a neighbour list that claims more
 * than it carries, a reply for a request that already ended.
 *
 * The second is a whole meshcored runtime (mcd_runtime) and a TEST REPEATER
 * on one in-memory air. The repeater is a mesh::Mesh whose handlers are
 * upstream's simple_repeater handlers (vendor/RIFT examples/simple_repeater/
 * MyMesh.cpp: handleLoginReq, handleRequest's STATUS / NEIGHBOURS /
 * OWNER_INFO, the CLI branch of onPeerDataRecv, onControlDataRecv's
 * DISCOVER_REQ), cut down to what these cases reach and with their byte
 * layouts kept exactly. Everything between the two is real: the adverts are
 * signed, the login is an ANON_REQ encrypted to the repeater, its answer
 * rides a PATH return, every request and reply is AES and MAC'd.
 *
 * What it does NOT prove: anything about a real repeater's firmware or radio.
 * The air is lossless and has no range unless a case says otherwise. The
 * hardware gate (docs/hardware/RIFT_REPEATER_CONTROL_GATE.md) is that.
 *
 * Waits are real time - the runtime runs on CLOCK_MONOTONIC - and every one
 * is a wait for a condition with a bound, never a fixed count. Request
 * deadlines (20 s at least) are driven with mcd_runtime_remote_expire rather
 * than waited out.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <Mesh.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/TxtDataHelpers.h>

#include "mc_port.h"
#include "mesh_remote.h"
#include "mesh_runtime.h"

static int failed;
static int checks;

static void check(const char* name, bool ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    checks++;
    if (!ok) {
        failed++;
    }
}

static uint64_t nowMs(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- part 1: the rules on hand-built bytes ----------------------------- */

static void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void test_decode_stats(void)
{
    uint8_t buf[4 + 56];
    mcd_repeater_stats st;

    memset(buf, 0, sizeof(buf));
    put32(buf, 0x11223344);
    put16(buf + 4 + 0, 4012);                /* battery */
    put16(buf + 4 + 4, (uint16_t)-118);      /* noise floor */
    put32(buf + 4 + 20, 93784);              /* uptime */
    put16(buf + 4 + 42, (uint16_t)(int16_t)-26); /* SNR x4 */
    put16(buf + 4 + 44, 7);                  /* direct dups */
    put32(buf + 4 + 52, 9);                  /* recv errors */

    check("a reply shorter than the oldest tier is not read",
          !mcdremote::decodeStats(buf, 4 + 43, &st));
    check("the oldest tier (44 bytes) is read", mcdremote::decodeStats(buf, 4 + 44, &st));
    check("and says it carried neither later tier", !st.have_dups && !st.have_rx_air);
    check("its fields are little-endian, signed where upstream's are",
          st.batt_milli_volts == 4012 && st.noise_floor == -118 && st.up_time_secs == 93784 &&
              st.last_snr_x4 == -26);
    check("48 bytes adds the duplicate counts only",
          mcdremote::decodeStats(buf, 4 + 48, &st) && st.have_dups && !st.have_rx_air &&
              st.direct_dups == 7);
    check("56 bytes is the whole struct",
          mcdremote::decodeStats(buf, 4 + 56, &st) && st.have_rx_air && st.recv_errors == 9);
}

static void test_decode_neighbours(void)
{
    uint8_t buf[8 + 3 * 11];
    mcd_remote_reply r;

    memset(buf, 0, sizeof(buf));
    put16(buf + 4, 5);  /* total */
    put16(buf + 6, 3);  /* count */
    for (int i = 0; i < 3; i++) {
        uint8_t* e = buf + 8 + i * 11;

        memset(e, 0xA0 + i, 6);
        put32(e + 6, 60u * (uint32_t)(i + 1));
        e[10] = (uint8_t)(int8_t)(-4 * i);
    }
    memset(&r, 0, sizeof(r));
    check("three neighbours of five are read",
          mcdremote::decodeNeighbours(buf, (int)sizeof(buf), 6, 11, &r) &&
              r.neighbours_total == 5 && r.neighbour_count == 3);
    check("with their prefix, age and SNR",
          r.neighbours[2].prefix[0] == 0xA2 && r.neighbours[2].heard_secs_ago == 180 &&
              r.neighbours[2].snr_x4 == -8);
    check("a count the reply does not hold is not read",
          !mcdremote::decodeNeighbours(buf, (int)sizeof(buf) - 1, 6, 11, &r));
    put16(buf + 6, 12);
    check("nor a count above what was asked",
          !mcdremote::decodeNeighbours(buf, (int)sizeof(buf), 6, 11, &r));
    put16(buf + 6, 3);
    put16(buf + 4, 2);
    check("nor one above the total it states",
          !mcdremote::decodeNeighbours(buf, (int)sizeof(buf), 6, 11, &r));
}

static void test_session_rules(void)
{
    mcdremote::Session s;
    uint8_t key[MCD_PUB_KEY_LEN];
    uint8_t other[MCD_PUB_KEY_LEN];
    uint8_t ok[13];
    uint8_t stats[4 + 44];
    mcd_remote_reply r;
    mcd_remote_session st;
    uint64_t t = 1000;

    memset(key, 0x42, sizeof(key));
    memset(other, 0x43, sizeof(other));
    memset(ok, 0, sizeof(ok));
    put32(ok, 1790000000u);
    ok[6] = 1;
    ok[12] = 2;

    check("a reply with no session is not this session's",
          s.onResponse(key, ok, 13, t, &r) == mcdremote::V_IGNORED);
    s.retarget(key, &r, t);
    s.begin(MCD_REMOTE_LOGIN, 0, 20000, t);
    check("a reply from another node is ignored",
          s.onResponse(other, ok, 13, t, &r) == mcdremote::V_IGNORED);
    check("the login OK is the answer", s.onResponse(key, ok, 13, t, &r) == mcdremote::V_ANSWER &&
                                            r.kind == MCD_REMOTE_LOGIN &&
                                            r.outcome == MCD_REMOTE_REPLIED);
    s.state(&st);
    check("and the session is logged in, as admin, with the repeater's clock",
          st.login == MCD_LOGIN_OK && st.admin && st.fw_level == 2 && st.server_clock_known &&
              st.server_clock == 1790000000u && st.pending == MCD_REMOTE_NONE);
    check("a second copy of it is stale",
          s.onResponse(key, ok, 13, t, &r) == mcdremote::V_STALE);

    memset(stats, 0, sizeof(stats));
    put32(stats, 777);
    s.begin(MCD_REMOTE_STATUS, 777, 20000, t);
    put32(stats, 776);
    check("a status reply with another tag is stale",
          s.onResponse(key, stats, (int)sizeof(stats), t, &r) == mcdremote::V_STALE);
    put32(stats, 777);
    check("a reply too short to read is malformed, and the request keeps waiting",
          s.onResponse(key, stats, 20, t, &r) == mcdremote::V_MALFORMED && s.busy());
    check("its tag's reply is the answer",
          s.onResponse(key, stats, (int)sizeof(stats), t, &r) == mcdremote::V_ANSWER &&
              r.kind == MCD_REMOTE_STATUS);

    s.begin(MCD_REMOTE_STATUS, 778, 20000, t);
    check("a wait is not over before its deadline", !s.expire(t + 19999, &r));
    check("and is at it", s.expire(t + 20000, &r) && r.outcome == MCD_REMOTE_TIMED_OUT);
    put32(stats, 778);
    check("the answer after that is stale",
          s.onResponse(key, stats, (int)sizeof(stats), t + 20001, &r) == mcdremote::V_STALE);

    /* A late status answer while a new login waits is stale, not a refusal:
     * its tag is the last request's. */
    s.begin(MCD_REMOTE_LOGIN, 0, 20000, t);
    check("a late reply to the request before a login is not taken as its verdict",
          s.onResponse(key, stats, (int)sizeof(stats), t, &r) == mcdremote::V_STALE &&
              s.busy());
    s.end(NULL, t);
    s.state(&st);
    check("ending forgets everything", !st.active && st.login == MCD_LOGIN_NONE);

    check("the wait has upstream's floor", mcdremote::waitFor(100) == MCD_REMOTE_MIN_WAIT_MS);
    check("and is twice the estimate plus 8 s above it", mcdremote::waitFor(10000) == 28000);
}

static void test_discovery_rules(void)
{
    mcdremote::Discovery d;
    uint8_t resp[6 + 32];
    uint8_t self[32];
    mcd_rx_meta meta;
    mcd_discovered e;
    mcd_discovered list[MCD_DISCOVER_MAX];
    mcd_discover_state st;
    uint64_t t = 5000;

    memset(self, 0x01, sizeof(self));
    memset(&meta, 0, sizeof(meta));
    resp[0] = 0x92;
    resp[1] = (uint8_t)(int8_t)22; /* 5.5 dB */
    put32(resp + 2, 0xCAFE);
    memset(resp + 6, 0x77, 32);

    check("an answer with no round open is not taken",
          !d.onResponse(resp, (int)sizeof(resp), self, &meta, false, t, &e));
    d.begin(0xCAFE, t);
    check("one to the open round is", d.onResponse(resp, (int)sizeof(resp), self, &meta, false,
                                                   t + 10, &e) &&
                                          e.round == 1 && e.their_snr_db == 5.5);
    check("twice from one node is one row",
          d.onResponse(resp, (int)sizeof(resp), self, &meta, false, t + 20, &e) &&
              d.list(list, MCD_DISCOVER_MAX) == 1);
    resp[0] = 0x91;
    check("a node that is not a repeater is not this round's",
          !d.onResponse(resp, (int)sizeof(resp), self, &meta, false, t + 30, &e));
    resp[0] = 0x92;
    put32(resp + 2, 0xBEEF);
    check("nor an answer to another tag",
          !d.onResponse(resp, (int)sizeof(resp), self, &meta, false, t + 30, &e));
    put32(resp + 2, 0xCAFE);
    memcpy(resp + 6, self, 32);
    check("nor our own key", !d.onResponse(resp, (int)sizeof(resp), self, &meta, false, t + 30, &e));
    check("nor a payload short of a whole key",
          !d.onResponse(resp, 6 + 31, self, &meta, false, t + 30, &e));
    check("the window is open before it ends", !d.expire(t + MCD_DISCOVER_WINDOW_MS - 1));
    check("and closes at its end", d.expire(t + MCD_DISCOVER_WINDOW_MS));
    memset(resp + 6, 0x77, 32);
    check("an answer after the window is not taken",
          !d.onResponse(resp, (int)sizeof(resp), self, &meta, false,
                        t + MCD_DISCOVER_WINDOW_MS + 5, &e));
    d.begin(0xD00D, t + 40000);
    d.state(&st);
    check("a second round has its own number", st.round == 2 && st.open);
    check("and the first round's repeater is still listed, as round 1",
          d.list(list, MCD_DISCOVER_MAX) == 1 && list[0].round == 1);
}

/* ---- part 2: a runtime and a repeater on one air ------------------------ */

struct Queue {
    uint8_t bytes[64][MAX_TRANS_UNIT];
    int len[64];
    int head;
    int tail;

    Queue() : head(0), tail(0) { memset(len, 0, sizeof(len)); }
    bool push(const uint8_t* b, int n)
    {
        int next = (tail + 1) % 64;

        if (next == head || n <= 0 || n > MAX_TRANS_UNIT) {
            return false;
        }
        memcpy(bytes[tail], b, (size_t)n);
        len[tail] = n;
        tail = next;
        return true;
    }
    int pop(uint8_t* out)
    {
        int n;

        if (head == tail) {
            return 0;
        }
        n = len[head];
        memcpy(out, bytes[head], (size_t)n);
        head = (head + 1) % 64;
        return n;
    }
    void clear() { head = tail = 0; }
};

static Queue g_to_rep;   /* what the runtime transmitted */
static Queue g_to_node;  /* what the repeater transmitted */
static bool g_rep_hears = true;   /* false: the repeater is out of range */
static bool g_hold_replies = false; /* true: the repeater's frames wait in the queue */
static uint64_t g_submits[32];
static int g_submit_count;
static uint64_t g_next_submit = 1;
static int g_control_tx;          /* CONTROL frames the runtime put on the air */
static int g_rep_tx;              /* frames the test repeater put on the air */
static int g_node_text;           /* TXT frames the runtime parsed */
static int g_node_frames;         /* every frame the runtime parsed */
static int g_delivered;           /* frames handed to the runtime */

static int hook_tx_submit(void*, const uint8_t* bytes, int len, uint64_t* submit_id)
{
    if (len >= 1 && ((bytes[0] >> PH_TYPE_SHIFT) & PH_TYPE_MASK) == PAYLOAD_TYPE_CONTROL) {
        g_control_tx++;
    }
    if (g_rep_hears) {
        g_to_rep.push(bytes, len);
    }
    *submit_id = g_next_submit++;
    if (g_submit_count < 32) {
        g_submits[g_submit_count++] = *submit_id;
    }
    return 0;
}

struct Seen {
    int discover_replies;
    int discover_closed;
    int remote_events;
    mcd_remote_reply last;
    mcd_remote_session session;
};
static Seen g_seen;

static void hook_on_frame(void*, const mcd_rx_meta*, int, const char* outcome)
{
    g_node_frames++;
    g_node_text += (outcome && strcmp(outcome, "text") == 0) ? 1 : 0;
}

static void hook_on_discover(void*, const mcd_discovered* d, const mcd_discover_state*)
{
    if (d) {
        g_seen.discover_replies++;
    } else {
        g_seen.discover_closed++;
    }
}

static void hook_on_remote(void*, const mcd_remote_reply* r, const mcd_remote_session* s)
{
    g_seen.remote_events++;
    g_seen.last = *r;
    g_seen.session = *s;
}

class RepRadio : public mesh::Radio {
public:
    int recvRaw(uint8_t* bytes, int sz) override
    {
        uint8_t tmp[MAX_TRANS_UNIT];
        int n = g_to_rep.pop(tmp);

        if (n > sz) {
            n = sz;
        }
        memcpy(bytes, tmp, (size_t)n);
        return n;
    }
    uint32_t getEstAirtimeFor(int len) override { return (uint32_t)(len > 0 ? len : 1); }
    float packetScore(float, int) override { return 1.0f; }
    bool startSendRaw(const uint8_t* bytes, int len) override
    {
        bool ok = g_to_node.push(bytes, len);

        g_rep_tx += ok ? 1 : 0;
        return ok;
    }
    bool isSendComplete() override { return true; }
    void onSendFinished() override { }
    bool isInRecvMode() const override { return true; }
    float getLastSNR() const override { return 8.0f; }
    float getLastRSSI() const override { return -71.0f; }
};

struct Client {
    bool used = false;
    mesh::Identity id;
    uint8_t secret[PUB_KEY_SIZE] = {};
    uint32_t last_timestamp = 0;
    bool admin = false;
    uint8_t out_path[MAX_PATH_SIZE] = {};
    uint8_t out_path_len = 0;
};

/* simple_repeater's handlers, as far as these cases reach them. */
class TestRepeater : public mesh::Mesh {
public:
    TestRepeater(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng,
                 mesh::RTCClock& rtc, mesh::PacketManager& mgr, mesh::MeshTables& tables)
        : mesh::Mesh(radio, ms, rng, rtc, mgr, tables), answer_discover(true),
          refuse_login(false), short_status(false), cli_silent(false), logins_ok(0), logins_bad(0), requests(0),
          cli_commands(0), discover_requests(0)
    {
        memset(matching, 0, sizeof(matching));
        last_cli[0] = '\0';
    }

    void advert()
    {
        uint8_t app[MAX_ADVERT_DATA_SIZE];
        AdvertDataBuilder b(ADV_TYPE_REPEATER, "TestRep");
        uint8_t n = b.encodeTo(app);
        mesh::Packet* p = createAdvert(self_id, app, n);

        if (p) {
            sendZeroHop(p);
        }
    }

    /* Forget every client, as a repeater that rebooted without saving a
     * guest would: the next login starts with no timestamp to beat. */
    void forget_clients()
    {
        for (int i = 0; i < 4; i++) {
            acl[i] = Client();
        }
    }

    bool answer_discover;
    bool refuse_login;   /* answer a wrong password explicitly (not upstream) */
    bool short_status;   /* answer STATUS with a reply too short to read */
    bool cli_silent;     /* take a command and answer nothing */
    int logins_ok;
    int logins_bad;
    int requests;
    int cli_commands;
    int discover_requests;
    char last_cli[MCD_MAX_TEXT + 1];

protected:
    bool allowPacketForward(const mesh::Packet*) override { return false; }

    int searchPeersByHash(const uint8_t* hash) override
    {
        int n = 0;

        for (int i = 0; i < 4; i++) {
            if (acl[i].used && acl[i].id.isHashMatch(hash)) {
                matching[n++] = i;
            }
        }
        return n;
    }

    void getPeerSharedSecret(uint8_t* dest, int peer_idx) override
    {
        memcpy(dest, acl[matching[peer_idx]].secret, PUB_KEY_SIZE);
    }

    Client* clientFor(const mesh::Identity& id)
    {
        for (int i = 0; i < 4; i++) {
            if (acl[i].used && acl[i].id.matches(id)) {
                return &acl[i];
            }
        }
        for (int i = 0; i < 4; i++) {
            if (!acl[i].used) {
                acl[i] = Client();
                acl[i].used = true;
                acl[i].id = id;
                acl[i].out_path_len = 0xFF; /* upstream OUT_PATH_UNKNOWN */
                return &acl[i];
            }
        }
        return NULL;
    }

    void reply(mesh::Packet* packet, const mesh::Identity& to, const uint8_t* secret,
               const uint8_t* data, int len)
    {
        if (packet->isRouteFlood()) {
            mesh::Packet* path = createPathReturn(to, secret, packet->path, packet->path_len,
                                                  PAYLOAD_TYPE_RESPONSE, data, (size_t)len);
            if (path) {
                sendFlood(path, 20);
            }
        } else {
            mesh::Packet* r = createDatagram(PAYLOAD_TYPE_RESPONSE, to, secret, data, (size_t)len);
            if (r) {
                sendFlood(r, 20);
            }
        }
    }

    /* handleLoginReq: admin password, guest password, or no answer at all. */
    void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender,
                        uint8_t* data, size_t len) override
    {
        uint32_t ts;
        uint8_t out[13];
        bool admin;
        Client* c;

        if (packet->getPayloadType() != PAYLOAD_TYPE_ANON_REQ || len < 5) {
            return;
        }
        memcpy(&ts, data, 4);
        data[len] = 0;
        if (strcmp((const char*)&data[4], "hunter2") == 0) {
            admin = true;
        } else if (strcmp((const char*)&data[4], "guest") == 0) {
            admin = false;
        } else {
            logins_bad++;
            if (refuse_login) {
                memset(out, 0, sizeof(out));
                out[4] = 0x7F; /* not RESP_SERVER_LOGIN_OK, not "OK" */
                reply(packet, sender, secret, out, 6);
            }
            return; /* upstream: return 0, nothing sent */
        }
        c = clientFor(sender);
        if (c == NULL || ts <= c->last_timestamp) {
            return; /* upstream: "Possible login replay attack!" */
        }
        c->last_timestamp = ts;
        c->admin = admin;
        memcpy(c->secret, secret, PUB_KEY_SIZE);
        logins_ok++;
        {
            uint32_t now = getRTCClock()->getCurrentTimeUnique();

            memcpy(out, &now, 4);
        }
        out[4] = 0;          /* RESP_SERVER_LOGIN_OK */
        out[5] = 0;
        out[6] = admin ? 1 : 0;
        out[7] = admin ? 3 : 2;
        getRNG()->random(&out[8], 4);
        out[12] = 2;         /* FIRMWARE_VER_LEVEL */
        reply(packet, sender, secret, out, 13);
    }

    void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret,
                        uint8_t* data, size_t len) override
    {
        Client* c = &acl[matching[sender_idx]];
        uint32_t ts;
        uint8_t out[MAX_PACKET_PAYLOAD];
        int n = 0;

        if (len < 5) {
            return;
        }
        memcpy(&ts, data, 4);
        if (type == PAYLOAD_TYPE_REQ) {
            if (ts <= c->last_timestamp) {
                return;
            }
            c->last_timestamp = ts;
            requests++;
            memcpy(out, &ts, 4); /* the tag, reflected */
            if (data[4] == mcdremote::REQ_GET_STATUS) {
                memset(out + 4, 0, 56);
                put16(out + 4, 3987);
                put32(out + 4 + 20, 4242);
                n = short_status ? 4 + 20 : 4 + 56;
            } else if (data[4] == mcdremote::REQ_GET_NEIGHBOURS) {
                int prefix = data[10];
                uint8_t* p = out + 8;

                put16(out + 4, 2);
                put16(out + 6, 2);
                for (int i = 0; i < 2; i++) {
                    memset(p, 0xB0 + i, (size_t)prefix);
                    p += prefix;
                    put32(p, 30u + (uint32_t)i);
                    p += 4;
                    *p++ = (uint8_t)(int8_t)(10 - i);
                }
                n = (int)(p - out);
            } else if (data[4] == mcdremote::REQ_GET_OWNER_INFO) {
                n = 4 + snprintf((char*)out + 4, sizeof(out) - 4, "%s\n%s\n%s", "v1.9.0",
                                 "TestRep", "bench owner");
            } else {
                return;
            }
            reply(packet, c->id, secret, out, n);
        } else if (type == PAYLOAD_TYPE_TXT_MSG && c->admin) {
            uint8_t flags = data[4] >> 2;
            uint32_t now;
            int tlen;

            if (flags != TXT_TYPE_CLI_DATA || ts < c->last_timestamp) {
                return;
            }
            c->last_timestamp = ts;
            data[len] = 0;
            snprintf(last_cli, sizeof(last_cli), "%s", (const char*)&data[5]);
            cli_commands++;
            if (cli_silent) {
                return; /* heard, and the answer lost on the air */
            }
            now = getRTCClock()->getCurrentTimeUnique();
            if (now == ts) {
                now++;
            }
            memcpy(out, &now, 4);
            out[4] = (uint8_t)(TXT_TYPE_CLI_DATA << 2);
            if (strcmp(last_cli, "clock") == 0) {
                /* CommonCLI's own answer to "clock". */
                tlen = snprintf((char*)out + 5, sizeof(out) - 5, "12:34 - 5/10/2026 UTC");
            } else {
                tlen = snprintf((char*)out + 5, sizeof(out) - 5, "-> %s", last_cli);
            }
            {
                mesh::Packet* r = createDatagram(PAYLOAD_TYPE_TXT_MSG, c->id, secret, out,
                                                 (size_t)(5 + tlen));
                if (r) {
                    sendFlood(r, 20);
                }
            }
        }
    }

    bool onPeerPathRecv(mesh::Packet*, int sender_idx, const uint8_t*, uint8_t* path,
                        uint8_t path_len, uint8_t, uint8_t*, uint8_t) override
    {
        Client* c = &acl[matching[sender_idx]];

        c->out_path_len = mesh::Packet::copyPath(c->out_path, path, path_len);
        return false;
    }

    void onControlDataRecv(mesh::Packet* packet) override
    {
        uint8_t out[6 + PUB_KEY_SIZE];

        if ((packet->payload[0] & 0xF0) != mcdremote::CTL_DISCOVER_REQ || packet->payload_len < 6) {
            return;
        }
        discover_requests++;
        if (!answer_discover || (packet->payload[1] & (1 << ADV_TYPE_REPEATER)) == 0) {
            return;
        }
        out[0] = mcdremote::CTL_DISCOVER_RESP | ADV_TYPE_REPEATER;
        out[1] = (uint8_t)packet->_snr;
        memcpy(out + 2, packet->payload + 2, 4);
        memcpy(out + 6, self_id.pub_key, PUB_KEY_SIZE);
        mesh::Packet* r = createControlData(out, sizeof(out));
        if (r) {
            sendZeroHop(r, 30);
        }
    }

private:
    Client acl[4];
    int matching[4];
};

struct World {
    mcport::MonotonicClock clock;
    mcport::SystemRTCClock rtc;
    mcport::HostRNG rng;
    StaticPoolPacketManager mgr;
    SimpleMeshTables tables;
    RepRadio radio;
    TestRepeater rep;
    mcd_runtime* rt;
    char dir[256];

    World() : mgr(16), rep(radio, clock, rng, rtc, mgr, tables), rt(NULL) { dir[0] = '\0'; }
};

static bool startRuntime(World& w)
{
    mcd_runtime_hooks hooks;
    mcd_runtime_config cfg;
    char err[256] = "";

    memset(&hooks, 0, sizeof(hooks));
    hooks.tx_submit = hook_tx_submit;
    hooks.on_discover = hook_on_discover;
    hooks.on_frame = hook_on_frame;
    hooks.on_remote = hook_on_remote;
    memset(&cfg, 0, sizeof(cfg));
    cfg.state_dir = w.dir;
    cfg.node_name = "DoorsTest";
    w.rt = mcd_runtime_create(&cfg, &hooks, err, sizeof(err));
    if (!w.rt) {
        fprintf(stderr, "mcd_runtime_create: %s\n", err);
        return false;
    }
    mcd_runtime_set_radio_online(w.rt, true);
    /* A fast profile: the cases are about the protocol, not the airtime. */
    mcd_runtime_set_profile(w.rt, 7, 250.0, 5, 8, true);
    return true;
}

static void step(World& w)
{
    uint8_t buf[MAX_TRANS_UNIT];
    int n;

    mcd_runtime_tick(w.rt);
    for (int i = 0; i < g_submit_count; i++) {
        mcd_runtime_tx_done(w.rt, g_submits[i], MCD_TX_OK);
    }
    g_submit_count = 0;
    w.rep.loop();
    while (!g_hold_replies && (n = g_to_node.pop(buf)) > 0) {
        mcd_rx_meta meta;

        memset(&meta, 0, sizeof(meta));
        meta.mono_ms = nowMs();
        meta.rssi_known = true;
        meta.rssi_dbm = -71.0;
        meta.snr_known = true;
        meta.snr_db = 8.25;
        g_delivered += mcd_runtime_deliver_rx(w.rt, buf, n, &meta) ? 1 : 0;
        mcd_runtime_tick(w.rt);
    }
    usleep(2000);
}

template <typename Pred>
static bool until(World& w, Pred pred, int max_ms = 8000)
{
    uint64_t end = nowMs() + (uint64_t)max_ms;

    while (nowMs() < end) {
        step(w);
        if (pred()) {
            return true;
        }
    }
    return false;
}

static void settle(World& w, int ms)
{
    uint64_t end = nowMs() + (uint64_t)ms;

    while (nowMs() < end) {
        step(w);
    }
}

static void test_discovery(World& w, const uint8_t* rep_key)
{
    mcd_discover_state st;
    mcd_discovered list[MCD_DISCOVER_MAX];
    int sent_before;

    printf("-- zero-hop discovery\n");
    sent_before = g_control_tx;
    check("a round starts", mcd_runtime_discover(w.rt, &st) == MCD_DISCOVER_STARTED &&
                                st.round == 1 && st.open);
    check("pressing again while it is open sends nothing and keeps the round",
          mcd_runtime_discover(w.rt, &st) == MCD_DISCOVER_BUSY && st.round == 1 &&
              mcd_runtime_discover(w.rt, &st) == MCD_DISCOVER_BUSY);
    check("the repeater answers", until(w, [&] { return g_seen.discover_replies >= 1; }));
    check("exactly one request went on the air for three presses",
          g_control_tx - sent_before == 1 && w.rep.discover_requests == 1);
    check("the repeater is listed with its whole key, both SNRs and our RSSI",
          mcd_runtime_discovered(w.rt, list, MCD_DISCOVER_MAX) == 1 &&
              memcmp(list[0].public_key, rep_key, 32) == 0 && list[0].snr_known &&
              list[0].snr_db == 8.25 && list[0].rssi_known && list[0].round == 1);

    /* A DISCOVER_RESP that came through a relay, or by flood, is never handed
     * to the runtime's control handler by mesh::Mesh: built here by hand with
     * this round's tag, it would be accepted if it were. */
    {
        uint8_t frame[2 + 1 + 6 + 32];
        uint8_t payload[6 + 32];
        uint32_t tag = 0;
        int before = g_seen.discover_replies;
        uint8_t other[32];
        mcd_discover_state now;

        mcd_runtime_discover_state(w.rt, &now);
        /* The round's tag is not exposed; a relayed answer cannot be built
         * with it, and that is beside the point - the frame is refused
         * before its tag is read. Any tag shows it. */
        memset(other, 0x5A, sizeof(other));
        payload[0] = 0x92;
        payload[1] = 40;
        memcpy(payload + 2, &tag, 4);
        memcpy(payload + 6, other, 32);
        frame[0] = (uint8_t)(ROUTE_TYPE_DIRECT | (PAYLOAD_TYPE_CONTROL << PH_TYPE_SHIFT));
        frame[1] = 1;      /* one relay hash in the path */
        frame[2] = 0xEE;
        memcpy(frame + 3, payload, sizeof(payload));
        {
            mcd_rx_meta meta;

            memset(&meta, 0, sizeof(meta));
            meta.mono_ms = nowMs();
            mcd_runtime_deliver_rx(w.rt, frame, 3 + (int)sizeof(payload), &meta);
            frame[0] = (uint8_t)(ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_CONTROL << PH_TYPE_SHIFT));
            frame[1] = 0;
            memcpy(frame + 2, payload, sizeof(payload));
            mcd_runtime_deliver_rx(w.rt, frame, 2 + (int)sizeof(payload), &meta);
        }
        settle(w, 100);
        check("an answer that came through a relay, or by flood, is not listed",
              g_seen.discover_replies == before &&
                  mcd_runtime_discovered(w.rt, list, MCD_DISCOVER_MAX) == 1);
    }

    check("the window closes on its own (driven)", mcd_runtime_discover_expire(
                                                       w.rt, nowMs() + MCD_DISCOVER_WINDOW_MS) &&
                                                       g_seen.discover_closed == 1);
    /* Round 2, with the repeater out of range. */
    g_rep_hears = false;
    check("a new round starts once the window has closed",
          mcd_runtime_discover(w.rt, &st) == MCD_DISCOVER_STARTED && st.round == 2);
    settle(w, 300);
    g_rep_hears = true;
    check("a repeater that did not answer this round is kept, as round 1, not current",
          mcd_runtime_discovered(w.rt, list, MCD_DISCOVER_MAX) == 1 && list[0].round == 1);
    mcd_runtime_discover_expire(w.rt, nowMs() + MCD_DISCOVER_WINDOW_MS);
}

static void test_login(World& w, const uint8_t* rep_key)
{
    uint64_t id = 0;
    uint32_t wait = 0;
    mcd_remote_session s;
    int events;

    printf("-- login\n");
    check("a password longer than upstream's 15 is refused, not cut",
          mcd_runtime_remote_login(w.rt, rep_key, "0123456789abcdefg", &id, &wait) ==
              MCD_REMOTE_BAD_TEXT);
    check("as is one with a control character",
          mcd_runtime_remote_login(w.rt, rep_key, "a\tb", &id, &wait) == MCD_REMOTE_BAD_TEXT);
    {
        uint8_t stranger[32];

        memset(stranger, 0x33, sizeof(stranger));
        check("a node that is not a contact cannot be logged in to",
              mcd_runtime_remote_login(w.rt, stranger, "hunter2", &id, &wait) ==
                  MCD_REMOTE_NO_CONTACT);
    }
    check("status before a login is refused",
          mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_STATUS, &id, &wait) ==
              MCD_REMOTE_NOT_LOGGED_IN);

    /* Wrong password: upstream sends nothing, so it ends as a timeout. */
    events = g_seen.remote_events;
    check("a login goes out", mcd_runtime_remote_login(w.rt, rep_key, "wrong", &id, &wait) ==
                                  MCD_REMOTE_ACCEPTED_FLOOD && wait >= MCD_REMOTE_MIN_WAIT_MS);
    check("a second request while it waits is refused",
          mcd_runtime_remote_login(w.rt, rep_key, "hunter2", &id, &wait) == MCD_REMOTE_BUSY);
    check("the repeater heard a wrong password",
          until(w, [&] { return w.rep.logins_bad >= 1; }));
    settle(w, 300);
    mcd_runtime_remote_session(w.rt, &s);
    check("and answered nothing: still waiting", s.login == MCD_LOGIN_WAITING &&
                                                     g_seen.remote_events == events);
    check("the wait ends as a timeout", mcd_runtime_remote_expire(w.rt, s.deadline_mono_ms) &&
                                            g_seen.last.kind == MCD_REMOTE_LOGIN &&
                                            g_seen.last.outcome == MCD_REMOTE_TIMED_OUT &&
                                            g_seen.session.login == MCD_LOGIN_TIMEOUT);

    /* An explicit refusal (not what upstream's repeater does; what the
     * companion firmware treats as a failed login). */
    w.rep.refuse_login = true;
    mcd_runtime_remote_login(w.rt, rep_key, "nope", &id, &wait);
    check("an answer that is not an OK is a refused login",
          until(w, [&] { return g_seen.session.login == MCD_LOGIN_REFUSED; }) &&
              g_seen.last.outcome == MCD_REMOTE_REFUSED);
    w.rep.refuse_login = false;

    /* The right one. */
    check("the admin password is sent",
          mcd_runtime_remote_login(w.rt, rep_key, "hunter2", &id, &wait) <=
              MCD_REMOTE_ACCEPTED_DIRECT);
    check("and answered OK",
          until(w, [&] { return g_seen.session.login == MCD_LOGIN_OK; }) &&
              g_seen.last.kind == MCD_REMOTE_LOGIN && g_seen.last.request_id == id &&
              !g_seen.last.late);
    mcd_runtime_remote_session(w.rt, &s);
    check("as admin, with the repeater's firmware level and clock",
          s.admin && !s.legacy && s.fw_level == 2 && s.server_clock_known && s.acl == 3);
    check("the answer carried the signal it was heard at",
          g_seen.last.snr_known && g_seen.last.rssi_known);
}

static void test_requests(World& w, const uint8_t* rep_key)
{
    uint64_t id = 0;
    uint32_t wait = 0;
    mcd_remote_session s;

    printf("-- status, neighbours, owner, command\n");
    check("status goes out", mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_STATUS, &id,
                                                    &wait) <= MCD_REMOTE_ACCEPTED_DIRECT);
    check("a second request while it waits is refused",
          mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_OWNER, &id, &wait) == MCD_REMOTE_BUSY);
    check("status is answered", until(w, [&] {
              return g_seen.last.kind == MCD_REMOTE_STATUS &&
                     g_seen.last.outcome == MCD_REMOTE_REPLIED;
          }));
    check("with the repeater's numbers", g_seen.last.stats.batt_milli_volts == 3987 &&
                                             g_seen.last.stats.up_time_secs == 4242 &&
                                             g_seen.last.stats.have_rx_air);

    mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_NEIGHBOURS, &id, &wait);
    check("neighbours are answered", until(w, [&] {
              return g_seen.last.kind == MCD_REMOTE_NEIGHBOURS &&
                     g_seen.last.outcome == MCD_REMOTE_REPLIED;
          }));
    check("two of two, with prefix, age and SNR",
          g_seen.last.neighbours_total == 2 && g_seen.last.neighbour_count == 2 &&
              g_seen.last.neighbours[1].prefix[0] == 0xB1 &&
              g_seen.last.neighbours[1].heard_secs_ago == 31 &&
              g_seen.last.neighbours[1].snr_x4 == 9);

    mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_OWNER, &id, &wait);
    check("owner information is answered", until(w, [&] {
              return g_seen.last.kind == MCD_REMOTE_OWNER &&
                     g_seen.last.outcome == MCD_REMOTE_REPLIED;
          }) && strcmp(g_seen.last.text, "v1.9.0\nTestRep\nbench owner") == 0);

    check("a command of two lines is refused",
          mcd_runtime_remote_cli(w.rt, rep_key, "ver\nreboot", &id, &wait) == MCD_REMOTE_BAD_TEXT);
    check("a command goes out", mcd_runtime_remote_cli(w.rt, rep_key, "ver", &id, &wait) <=
                                    MCD_REMOTE_ACCEPTED_DIRECT);
    check("and its answer comes back as the command's", until(w, [&] {
              return g_seen.last.kind == MCD_REMOTE_CLI &&
                     g_seen.last.outcome == MCD_REMOTE_REPLIED;
          }) && strcmp(g_seen.last.text, "-> ver") == 0 && strcmp(w.rep.last_cli, "ver") == 0);

    /* A reply too short to read is ignored and the request times out. */
    w.rep.short_status = true;
    mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_STATUS, &id, &wait);
    check("a malformed status reply is counted and ignored", until(w, [&] {
              mcd_remote_session t;

              mcd_runtime_remote_session(w.rt, &t);
              return t.malformed_replies >= 1;
          }));
    mcd_runtime_remote_session(w.rt, &s);
    check("and the request is still waiting", s.pending == MCD_REMOTE_STATUS);
    mcd_runtime_remote_expire(w.rt, s.deadline_mono_ms);
    check("until it times out", g_seen.last.outcome == MCD_REMOTE_TIMED_OUT &&
                                    g_seen.last.kind == MCD_REMOTE_STATUS);
    w.rep.short_status = false;

    /* A reply that arrives after its request timed out is stale. */
    g_hold_replies = true;
    mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_STATUS, &id, &wait);
    until(w, [&] { return w.rep.requests >= 5; }, 3000);
    settle(w, 200);
    mcd_runtime_remote_session(w.rt, &s);
    mcd_runtime_remote_expire(w.rt, s.deadline_mono_ms);
    {
        int events = g_seen.remote_events;
        uint64_t stale_before;

        mcd_runtime_remote_session(w.rt, &s);
        stale_before = s.stale_replies;
        g_hold_replies = false;
        check("a reply to a request that already timed out is stale",
              until(w, [&] {
                  mcd_remote_session t;

                  mcd_runtime_remote_session(w.rt, &t);
                  return t.stale_replies > stale_before;
              }) && g_seen.remote_events == events);
    }
}

static void test_late_login(World& w, const uint8_t* rep_key)
{
    uint64_t id = 0;
    uint32_t wait = 0;
    mcd_remote_session s;

    printf("-- a late login answer\n");
    g_hold_replies = true;
    mcd_runtime_remote_login(w.rt, rep_key, "hunter2", &id, &wait);
    until(w, [&] { return w.rep.logins_ok >= 2; }, 3000);
    settle(w, 200);
    mcd_runtime_remote_session(w.rt, &s);
    mcd_runtime_remote_expire(w.rt, s.deadline_mono_ms);
    check("a login with no answer in its wait times out",
          g_seen.session.login == MCD_LOGIN_TIMEOUT);
    g_hold_replies = false;
    check("its OK arriving later is still taken, and marked late",
          until(w, [&] { return g_seen.session.login == MCD_LOGIN_OK; }) && g_seen.last.late);
}

static void test_logout_and_endings(World& w, const uint8_t* rep_key)
{
    uint64_t id = 0;
    uint32_t wait = 0;
    mcd_remote_session s;

    printf("-- logout, forgetting, a restart\n");
    mcd_runtime_remote_login(w.rt, rep_key, "hunter2", &id, &wait);
    until(w, [&] { return g_seen.session.login == MCD_LOGIN_OK && g_seen.last.request_id == id; });
    mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_STATUS, &id, &wait);
    check("logout ends the session", mcd_runtime_remote_logout(w.rt, rep_key));
    check("and answers the request it had waiting as cancelled",
          g_seen.last.request_id == id && g_seen.last.outcome == MCD_REMOTE_CANCELLED);
    mcd_runtime_remote_session(w.rt, &s);
    check("nothing of the session is left", !s.active && s.login == MCD_LOGIN_NONE);
    check("a second logout has nothing to end", !mcd_runtime_remote_logout(w.rt, NULL));
    check("and status is refused again",
          mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_STATUS, &id, &wait) ==
              MCD_REMOTE_NOT_LOGGED_IN);
    settle(w, 300); /* the status reply, now stale, drains */

    mcd_runtime_remote_login(w.rt, rep_key, "hunter2", &id, &wait);
    until(w, [&] { return g_seen.session.login == MCD_LOGIN_OK && g_seen.last.request_id == id; });
    mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_OWNER, &id, &wait);
    check("forgetting the repeater's node ends the session",
          mcd_runtime_node_remove(w.rt, rep_key, NULL, NULL) &&
              g_seen.last.request_id == id && g_seen.last.outcome == MCD_REMOTE_CANCELLED);
    mcd_runtime_remote_session(w.rt, &s);
    check("and leaves no session behind", !s.active);
    settle(w, 300);

    /* The repeater adverts again; log in, then restart the service. */
    w.rep.advert();
    until(w, [&] {
        struct mcd_node n;

        return mcd_runtime_node_by_prefix(w.rt, rep_key, 32, &n) == 1;
    });
    mcd_runtime_remote_login(w.rt, rep_key, "hunter2", &id, &wait);
    check("logged in again", until(w, [&] { return g_seen.session.login == MCD_LOGIN_OK; }));
    mcd_runtime_persist(w.rt);
    mcd_runtime_destroy(w.rt);
    w.rt = NULL;
    check("the service restarts", startRuntime(w));
    mcd_runtime_remote_session(w.rt, &s);
    check("with no session: a session is never written anywhere", !s.active);
    check("and status is refused until a new login",
          mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_STATUS, &id, &wait) ==
              MCD_REMOTE_NOT_LOGGED_IN);
}

static void test_guest(World& w, const uint8_t* rep_key)
{
    uint64_t id = 0;
    uint32_t wait = 0;

    printf("-- a guest login\n");
    mcd_runtime_remote_login(w.rt, rep_key, "guest", &id, &wait);
    check("the guest password logs in",
          until(w, [&] { return g_seen.session.login == MCD_LOGIN_OK && g_seen.last.request_id == id; }));
    check("not as admin", !g_seen.session.admin);
    check("a guest may ask for status",
          mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_STATUS, &id, &wait) <=
              MCD_REMOTE_ACCEPTED_DIRECT);
    until(w, [&] { return g_seen.last.kind == MCD_REMOTE_STATUS; });
    check("but a command is refused here, since the repeater would never answer it",
          mcd_runtime_remote_cli(w.rt, rep_key, "ver", &id, &wait) == MCD_REMOTE_NOT_ADMIN);
    mcd_runtime_remote_logout(w.rt, NULL);
}

/* CLOCK on a mesh with 2-byte path hashes, as unit B's (2026-10-05): the
 * command is CLI data, answered by CLI data with no tag. Answered, lost,
 * answered late; and the next request after a lost one. */
static void test_clock(World& w, const uint8_t* rep_key)
{
    uint64_t id = 0;
    uint32_t wait = 0;
    bool persisted = false;
    mcd_remote_session s;
    struct mcd_node n;
    int events;

    printf("-- CLOCK on a 2-byte path hash route\n");
    check("this node floods with 2-byte path hashes",
          mcd_runtime_set_path_hash_bytes(w.rt, 2, &persisted));
    mcd_runtime_node_reset_path(w.rt, rep_key, NULL);
    w.rep.forget_clients();
    /* The runtime was restarted by the case before, and its unique clock
     * started again: a login built in the same second as the last run's
     * would be byte for byte that login, and the repeater's duplicate table
     * would drop it. A few seconds on, it is a new packet. */
    settle(w, 3000);
    mcd_runtime_remote_login(w.rt, rep_key, "hunter2", &id, &wait);
    check("the flooded login is answered",
          until(w, [&] { return g_seen.session.login == MCD_LOGIN_OK && g_seen.last.request_id == id; }));
    check("and the route learned from it is zero hops in 2-byte hashes (0x40)",
          mcd_runtime_node_by_prefix(w.rt, rep_key, 32, &n) == 1 && n.path_known &&
              n.path_len == 0x40 && n.path_hops == 0);

    check("CLOCK goes direct",
          mcd_runtime_remote_cli(w.rt, rep_key, "clock", &id, &wait) == MCD_REMOTE_ACCEPTED_DIRECT);
    /* Upstream counts hops as path_len & 63. Read as the whole byte, 0x40
     * was 65 hops, and the wait ran to minutes. */
    check("its wait is sized for zero hops, not for the packed byte",
          wait >= MCD_REMOTE_MIN_WAIT_MS && wait < 30000);
    check("CLOCK is answered with the repeater's time", until(w, [&] {
              return g_seen.last.request_id == id && g_seen.last.outcome == MCD_REMOTE_REPLIED;
          }) && g_seen.last.kind == MCD_REMOTE_CLI &&
              strcmp(g_seen.last.text, "12:34 - 5/10/2026 UTC") == 0);
    mcd_runtime_remote_session(w.rt, &s);
    check("and nothing is waiting after it", s.pending == MCD_REMOTE_NONE);

    /* The answer lost on the air. */
    w.rep.cli_silent = true;
    mcd_runtime_remote_cli(w.rt, rep_key, "clock", &id, &wait);
    check("a lost CLOCK keeps waiting until its deadline, and no longer",
          until(w, [&] { return w.rep.cli_commands >= 3; }, 3000) &&
              !mcd_runtime_remote_expire(w.rt, mcport::monotonicMillis()));
    mcd_runtime_remote_session(w.rt, &s);
    check("its deadline is the wait it was given",
          s.pending == MCD_REMOTE_CLI && s.deadline_mono_ms > 0);
    check("then it ends as a timeout", mcd_runtime_remote_expire(w.rt, s.deadline_mono_ms) &&
                                           g_seen.last.request_id == id &&
                                           g_seen.last.outcome == MCD_REMOTE_TIMED_OUT);
    mcd_runtime_remote_session(w.rt, &s);
    check("and the session is free again, still logged in",
          s.pending == MCD_REMOTE_NONE && s.login == MCD_LOGIN_OK);
    w.rep.cli_silent = false;

    /* Answered after its wait: stale, and no event revives it. */
    g_hold_replies = true;
    {
        int sent_before = g_rep_tx;

        mcd_runtime_remote_cli(w.rt, rep_key, "clock", &id, &wait);
        /* Until the repeater has heard it AND put its answer on the air -
         * its dispatcher may hold the answer back for airtime - so the wait
         * below ends with the answer already in flight. */
        check("the repeater hears the next CLOCK and answers it (held on the air)",
              until(w, [&] { return w.rep.cli_commands >= 4 && g_rep_tx > sent_before; }));
    }
    mcd_runtime_remote_session(w.rt, &s);
    mcd_runtime_remote_expire(w.rt, s.deadline_mono_ms);
    events = g_seen.remote_events;
    g_hold_replies = false;
    check("an answer after the timeout is counted stale and raises nothing", until(w, [&] {
              mcd_remote_session t;

              mcd_runtime_remote_session(w.rt, &t);
              return t.stale_replies >= 1;
          }) && g_seen.remote_events == events);
    mcd_runtime_remote_session(w.rt, &s);
    printf("     (commands heard %d, stale %llu, events %d -> %d, rep tx %d, delivered %d, "
           "parsed %d, text %d)\n", w.rep.cli_commands, (unsigned long long)s.stale_replies,
           events, g_seen.remote_events, g_rep_tx, g_delivered, g_node_frames, g_node_text);

    check("STATUS after the lost and late CLOCKs is taken",
          mcd_runtime_remote_ask(w.rt, rep_key, MCD_REMOTE_STATUS, &id, &wait) ==
              MCD_REMOTE_ACCEPTED_DIRECT && wait < 30000);
    check("and answered", until(w, [&] {
              return g_seen.last.request_id == id && g_seen.last.outcome == MCD_REMOTE_REPLIED;
          }));
    mcd_runtime_remote_logout(w.rt, NULL);
}

int main(void)
{
    static World w;
    char tmpl[] = "/tmp/mcd-repeater-XXXXXX";
    char* root;
    uint8_t rep_key[32];

    setvbuf(stdout, NULL, _IOLBF, 0);
    test_decode_stats();
    test_decode_neighbours();
    test_session_rules();
    test_discovery_rules();

    root = mkdtemp(tmpl);
    if (!root) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(w.dir, sizeof(w.dir), "%s/node", root);
    mkdir(w.dir, 0700);
    w.rep.self_id = mesh::LocalIdentity(&w.rng);
    w.rep.begin();
    memcpy(rep_key, w.rep.self_id.pub_key, 32);
    if (!startRuntime(w)) {
        return 1;
    }
    w.rep.advert();
    check("the runtime learns the repeater from its advert", until(w, [&] {
              struct mcd_node n;

              return mcd_runtime_node_by_prefix(w.rt, rep_key, 32, &n) == 1 && n.type == 2;
          }));
    test_discovery(w, rep_key);
    test_login(w, rep_key);
    test_requests(w, rep_key);
    test_late_login(w, rep_key);
    test_guest(w, rep_key);
    test_logout_and_endings(w, rep_key);
    test_clock(w, rep_key);
    mcd_runtime_destroy(w.rt);

    printf("meshcored_repeater_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
