/*
 * meshcored: the MeshCore runtime and the radiod adapter. See mesh_runtime.h.
 *
 * Two classes and a struct.
 *
 *   RadiodRadio   implements mesh::Radio over radiod's IPC. It is the only
 *                 place that knows a frame arrived from somewhere other than
 *                 a transceiver, and it knows nothing about JSON: the daemon
 *                 hands it bytes and metadata, and it hands the daemon bytes
 *                 back through one hook. protocols/meshcore deliberately
 *                 left mesh::Radio unimplemented for exactly this.
 *
 *   Node          a BaseChatMesh. It decides nothing about presentation - it
 *                 records what happened and tells the daemon - which is the
 *                 boundary this phase is establishing: the protocol runtime
 *                 outlives any client, and no client's absence stops it.
 *
 *   mcd_runtime   the two of them plus the packet pool, the mesh tables, the
 *                 clocks, the RNG and the persistence.
 *
 * The crypto is real throughout: real Ed25519 signatures, a real X25519
 * agreement and real AES-128 with encrypt-then-MAC, all from the pinned
 * vendored sources through protocols/meshcore. There is no mock cipher
 * anywhere in this service.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "mesh_runtime.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <helpers/AdvertDataHelpers.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>

#include "airtime.h"
#include "mc_port.h"
#include "mesh_store.h"

/* The sizes this service repeats in its C header must be MeshCore's own. */
static_assert(MCD_PUB_KEY_LEN == PUB_KEY_SIZE, "public key size drifted from MeshCore");
static_assert(MCD_MAX_PATH == MAX_PATH_SIZE, "path size drifted from MeshCore");
static_assert(MCD_MAX_TEXT == MAX_TEXT_LEN, "text length drifted from MeshCore");
static_assert(MCD_MAX_FRAME == MAX_TRANS_UNIT, "frame size drifted from MeshCore");
static_assert(MCD_NODE_NAME_LEN == sizeof(((ContactInfo*)0)->name),
              "contact name size drifted from MeshCore");

#ifndef MESHCORE_RIFT_COMMIT
#define MESHCORE_RIFT_COMMIT ""
#endif
#ifndef MESHCORE_CRYPTO_COMMIT
#define MESHCORE_CRYPTO_COMMIT ""
#endif

namespace {

/* How many received frames wait between one turn of the loop and the next.
 * A burst larger than this is dropped and counted rather than making the
 * daemon allocate without bound because somebody is transmitting quickly. */
const int RX_QUEUE_DEPTH = 32;

/* The last N messages, in memory. Messages are not persisted in this phase -
 * see docs/services/MESHCORED.md, "What is persistent". */
const int MSG_RING = 64;

/* How many messages may be waiting for an ACK at once. MeshCore itself
 * tracks one timeout (BaseChatMesh::txt_send_timeout), so this is about
 * matching an ACK that arrives to the message it answers, not about running
 * several timers. */
const int OUTBOX_SLOTS = 8;

/* ---- the radio --------------------------------------------------------- */

struct RxFrame {
    uint8_t bytes[MAX_TRANS_UNIT];
    int len;
    mcd_rx_meta meta;
};

class RadiodRadio : public mesh::Radio {
public:
    explicit RadiodRadio(const mcd_runtime_hooks& hooks)
        : _hooks(hooks), _head(0), _tail(0), _count(0), _dropped(0),
          _queued(0), _handed(0), _online(false), _rx_mode(false),
          _tx_busy(false), _tx_submit_id(0), _tx_len(0), _tx_transmitted(false),
          _profile_known(false), _sf(MCD_PROFILE_SF_UNSET), _bw_khz(0.0), _cr(5),
          _preamble(8), _crc(true)
    {
        memset(&_cur, 0, sizeof(_cur));
        _cur_valid = false;
    }

    /* ---- fed by the daemon ---- */

    bool push(const uint8_t* bytes, int len, const mcd_rx_meta& meta)
    {
        if (len <= 0 || len > MAX_TRANS_UNIT) {
            return false;
        }
        if (_count >= RX_QUEUE_DEPTH) {
            _dropped++;
            return false;
        }
        RxFrame& f = _queue[_tail];

        memcpy(f.bytes, bytes, (size_t)len);
        f.len = len;
        f.meta = meta;
        _tail = (_tail + 1) % RX_QUEUE_DEPTH;
        _count++;
        _queued++;
        return true;
    }

    bool pending() const { return _count > 0; }
    uint64_t dropped() const { return _dropped; }
    uint64_t queued() const { return _queued; }
    uint64_t handed() const { return _handed; }

    void setOnline(bool on)
    {
        _online = on;
        if (!on) {
            _rx_mode = false;
        }
    }
    bool online() const { return _online; }
    void setRxMode(bool on) { _rx_mode = on; }

    /* The profile radiod applied, so the airtime this radio reports is the
     * airtime the packet will really take. Until it is known the estimate
     * falls back to the service default, which is the profile meshcored
     * asks for anyway. */
    void setProfile(int sf, double bw_khz, int cr, int preamble, bool crc)
    {
        _sf = sf;
        _bw_khz = bw_khz;
        _cr = cr;
        _preamble = preamble;
        _crc = crc;
        _profile_known = true;
    }

    /* Called once per turn, before MeshCore runs, so a callback can tell
     * "this happened while handling a frame we just took off the air" from
     * "this happened later", and report telemetry only in the first case. */
    void beginTurn() { _cur_valid = false; }
    bool currentMeta(mcd_rx_meta& out) const
    {
        if (!_cur_valid) {
            return false;
        }
        out = _cur;
        return true;
    }

    /* ---- transmit bookkeeping ---- */

    bool txBusy() const { return _tx_busy; }
    uint64_t txSubmitId() const { return _tx_busy ? _tx_submit_id : 0; }

    void txDone(uint64_t submit_id, mcd_tx_outcome outcome)
    {
        if (!_tx_busy || submit_id != _tx_submit_id) {
            /* A completion for a transmit this radio has already given up on
             * (the dispatcher expired it) or never had. The daemon counts it;
             * there is nothing here to finish. */
            return;
        }
        /* Complete means the bytes went out. A transmit that did not go out
         * is NOT reported as complete: mesh::Dispatcher has exactly one
         * success path, and taking it would charge airtime for a packet that
         * never left and count it as sent. The packet is expired by the
         * dispatcher's own outbound deadline instead, which logs the failure
         * and charges nothing. */
        if (outcome == MCD_TX_OK || outcome == MCD_TX_RX_RESUME_FAILED) {
            _tx_transmitted = true;
        }
        if (outcome == MCD_TX_RX_RESUME_FAILED) {
            _rx_mode = false;
        }
    }

    /* ---- mesh::Radio ---- */

    int recvRaw(uint8_t* bytes, int sz) override
    {
        if (_count == 0) {
            return 0;
        }
        RxFrame& f = _queue[_head];
        int n = f.len;

        if (n > sz) {
            n = sz;
        }
        memcpy(bytes, f.bytes, (size_t)n);
        _cur = f.meta;
        _cur_valid = true;
        _head = (_head + 1) % RX_QUEUE_DEPTH;
        _count--;
        _handed++;
        return n;
    }

    uint32_t getEstAirtimeFor(int len_bytes) override
    {
        double ms;

        if (len_bytes <= 0) {
            len_bytes = 1;
        }
        if (len_bytes > MAX_TRANS_UNIT) {
            len_bytes = MAX_TRANS_UNIT;
        }
        ms = lora_airtime_ms(_profile_known ? _sf : MCD_PROFILE_SF_DEFAULT,
                             _profile_known ? _bw_khz : MCD_PROFILE_BW_DEFAULT,
                             _cr, _preamble, (size_t)len_bytes, _crc, false);
        if (!(ms > 0.0)) {
            return 1;
        }
        return (uint32_t)(ms + 0.5);
    }

    /* Upstream's own scoring, arithmetic for arithmetic
     * (vendor/RIFT/src/helpers/radiolib/RadioLibWrappers.cpp
     * packetScoreInt), so a Doors node delays a relayed flood packet by the
     * same rule a T-Deck does. It is reimplemented rather than linked because
     * it lives in the RadioLib wrapper, which protocols/meshcore leaves
     * outside its boundary.
     *
     * When radiod reported no SNR the score is 1.0 - "process it now" -
     * rather than a number derived from an SNR nobody measured. */
    float packetScore(float snr, int packet_len) override
    {
        static const float threshold[] = { -7.5f, -10.0f, -12.5f, -15.0f, -17.5f, -20.0f };
        int sf = _profile_known ? _sf : MCD_PROFILE_SF_DEFAULT;
        float t;
        double rate;
        double penalty;
        double score;

        if (!_cur_valid || !_cur.snr_known) {
            return 1.0f;
        }
        if (sf < 7) {
            return 0.0f;
        }
        if (sf > 12) {
            sf = 12;
        }
        t = threshold[sf - 7];
        if (snr < t) {
            return 0.0f;
        }
        rate = ((double)snr - (double)t) / 10.0;
        penalty = 1.0 - ((double)packet_len / 256.0);
        score = rate * penalty;
        if (score < 0.0) {
            score = 0.0;
        }
        if (score > 1.0) {
            score = 1.0;
        }
        return (float)score;
    }

    bool startSendRaw(const uint8_t* bytes, int len) override
    {
        uint64_t id = 0;

        if (!_online || _tx_busy || len <= 0 || len > MAX_TRANS_UNIT) {
            return false;
        }
        if (_hooks.tx_submit == NULL) {
            return false;
        }
        if (_hooks.tx_submit(_hooks.user, bytes, len, &id) != 0) {
            return false;
        }
        _tx_busy = true;
        _tx_submit_id = id;
        _tx_len = len;
        _tx_transmitted = false;
        return true;
    }

    bool isSendComplete() override { return _tx_busy && _tx_transmitted; }

    void onSendFinished() override
    {
        /* Reached on both of the dispatcher's exits: the completion above,
         * and its own deadline. After the deadline a completion may still
         * arrive from radiod; txDone() above ignores it, because the
         * transmit it belonged to is no longer this radio's business. */
        _tx_busy = false;
        _tx_submit_id = 0;
        _tx_len = 0;
        _tx_transmitted = false;
    }

    bool isInRecvMode() const override { return _online && _rx_mode; }

    /* meshcored cannot answer this honestly. radiod reports
     * `activity_known: false` on the SX1262 - CAD detects a preamble at the
     * configured modulation and takes the radio out of receive, so it is not
     * a busy signal, and an RSSI threshold for this board has never been
     * measured (docs/api/radio.md, radio.channel). Polling radiod once per
     * packet would also put an IPC round trip inside the transmit path. So
     * this says "not receiving", which is what a node with no channel sense
     * does, and the daemon does not pretend otherwise. */
    bool isReceiving() override { return false; }

    float getLastRSSI() const override
    {
        return (_cur_valid && _cur.rssi_known) ? (float)_cur.rssi_dbm : 0.0f;
    }
    float getLastSNR() const override
    {
        return (_cur_valid && _cur.snr_known) ? (float)_cur.snr_db : 0.0f;
    }

    /* The two profile fallbacks, named rather than written twice. */
    static const int MCD_PROFILE_SF_UNSET = 0;
    static const int MCD_PROFILE_SF_DEFAULT = 8;
    static constexpr double MCD_PROFILE_BW_DEFAULT = 62.5;

private:
    mcd_runtime_hooks _hooks;

    RxFrame _queue[RX_QUEUE_DEPTH];
    int _head, _tail, _count;
    uint64_t _dropped, _queued, _handed;

    mcd_rx_meta _cur;
    bool _cur_valid;

    bool _online;
    bool _rx_mode;

    bool _tx_busy;
    uint64_t _tx_submit_id;
    int _tx_len;
    bool _tx_transmitted;

    bool _profile_known;
    int _sf;
    double _bw_khz;
    int _cr;
    int _preamble;
    bool _crc;
};

/* ---- the node ---------------------------------------------------------- */

struct OutboxSlot {
    bool used;
    uint32_t expected_ack;
    uint64_t msg_id;
    uint8_t peer_key[PUB_KEY_SIZE];
    uint64_t sent_ms;
};

class Node : public BaseChatMesh {
public:
    Node(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
         mesh::PacketManager& mgr, mesh::MeshTables& tables, RadiodRadio& adapter,
         const mcd_runtime_hooks& hooks)
        : BaseChatMesh(radio, ms, rng, rtc, mgr, tables), _adapter(adapter), _hooks(hooks),
          _dirty(false), _msg_count(0), _msg_head(0), _next_msg_id(1),
          _path_refused(0), _unparsed(0), _rx_logged(0)
    {
        memset(_outbox, 0, sizeof(_outbox));
        memset(_messages, 0, sizeof(_messages));
        _name[0] = '\0';
    }

    void setName(const char* n)
    {
        snprintf(_name, sizeof(_name), "%s", n ? n : "");
    }
    const char* name() const { return _name; }

    bool dirty() const { return _dirty; }
    void clearDirty() { _dirty = false; }
    uint64_t pathRefused() const { return _path_refused; }
    uint64_t unparsed() const { return _unparsed; }

    void noteHanded(uint64_t handed)
    {
        /* Frames taken off the queue that the dispatcher never logged as a
         * packet were refused by its own bounds-checked parse. Counting the
         * difference is the only way to see them from here: tryParsePacket()
         * frees the packet and says nothing. */
        if (handed > _rx_logged) {
            _unparsed = handed - _rx_logged;
        }
    }

    /* ---- messages ---- */

    int messageCount() const { return _msg_count; }
    bool messageAt(int idx, mcd_message& out) const
    {
        if (idx < 0 || idx >= _msg_count) {
            return false;
        }
        int pos = (_msg_head - _msg_count + idx + MSG_RING * 2) % MSG_RING;

        out = _messages[pos];
        return true;
    }

    uint64_t recordOutgoing(const ContactInfo& to, const char* text, uint32_t timestamp,
                            mcd_msg_state state, uint32_t expected_ack)
    {
        mcd_message m;

        memset(&m, 0, sizeof(m));
        m.id = _next_msg_id++;
        m.outgoing = true;
        memcpy(m.peer_key, to.id.pub_key, PUB_KEY_SIZE);
        snprintf(m.peer_name, sizeof(m.peer_name), "%s", to.name);
        snprintf(m.text, sizeof(m.text), "%s", text);
        m.timestamp = timestamp;
        m.mono_ms = mcport::monotonicMillis();
        m.state = state;
        push(m);
        addToOutbox(expected_ack, m.id, to.id.pub_key, m.mono_ms);
        emitMessage(m.id);
        return m.id;
    }

    /* ---- BaseChatMesh, the presentation side ---- */

protected:
    void onDiscoveredContact(ContactInfo& contact, bool, uint8_t, const uint8_t*) override
    {
        /* is_new is deliberately ignored: BaseChatMesh declares it false and
         * never assigns it (vendor/RIFT/src/helpers/BaseChatMesh.cpp:154 and
         * 198), so a contact added for the first time is reported as not new.
         * Upstream's to decide; nothing here is built on the flag. */
        _dirty = true;
        stamp(contact.id.pub_key);
        emitNode(contact, "discovered");
    }

    void onContactPathUpdated(const ContactInfo& contact) override
    {
        _dirty = true;
        emitNode(contact, "path");
    }

    /* ---- the PATH guard --------------------------------------------------
     *
     * mesh::Mesh computes the length of a PATH payload's trailing "extra"
     * field as `extra_len = len - k`, where k is how far it has read and len
     * is the decrypted length, and nothing checks that k <= len
     * (vendor/RIFT/src/Mesh.cpp:172; known debt 2 in
     * protocols/meshcore/README.md). A payload that declares a longer path
     * than it carries makes the subtraction negative, and the uint8_t
     * truncation turns it into a large positive length handed on with a
     * pointer near the end of a 184-byte stack buffer.
     *
     * Running the MeshCore receive path in a daemon is what makes that
     * reachable from the air. It needs a valid MAC, so the sender must be a
     * contact - but MeshCore adds contacts from adverts on its own, so any
     * node that adverts can become one. The default handler reads four bytes
     * of `extra` for an ACK and hands the whole claimed length to
     * onContactResponse() for a RESPONSE.
     *
     * So the payload is refused here, before anything reads through the
     * pointer, and the vendored tree is not touched. The test is exact, not
     * a heuristic: in a well-formed payload k + extra_len is the decrypted
     * length, which cannot exceed MAX_PACKET_PAYLOAD; in the underflow case
     * extra_len is 256 + len - k, so k + extra_len is 256 + len, always more.
     * No well-formed payload is refused and no malformed one is accepted.
     */
    bool onContactPathRecv(ContactInfo& from, uint8_t* in_path, uint8_t in_path_len,
                           uint8_t* out_path, uint8_t out_path_len,
                           uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override
    {
        size_t hash_size = mesh::Packet::pathHashSize(out_path_len);
        size_t hash_count = mesh::Packet::pathHashCount(out_path_len);
        size_t k = 2 + hash_size * hash_count;

        if (k + (size_t)extra_len > (size_t)MAX_PACKET_PAYLOAD) {
            _path_refused++;
            mcport::logWrite(mcport::LOG_WARN,
                             "meshcore: refused a PATH payload claiming %u extra bytes after "
                             "%zu of path; the decrypted payload cannot hold both",
                             (unsigned)extra_len, k);
            return false;  /* and no reciprocal path is sent */
        }
        return BaseChatMesh::onContactPathRecv(from, in_path, in_path_len, out_path,
                                               out_path_len, extra_type, extra, extra_len);
    }

    ContactInfo* processAck(const uint8_t* data) override
    {
        uint32_t crc;

        memcpy(&crc, data, 4);
        for (int i = 0; i < OUTBOX_SLOTS; i++) {
            OutboxSlot& s = _outbox[i];

            if (!s.used || s.expected_ack != crc) {
                continue;
            }
            markAcked(s.msg_id);
            ContactInfo* c = lookupContactByPubKey(s.peer_key, PUB_KEY_SIZE);

            s.used = false;
            return c;
        }
        return NULL;
    }

    void onMessageRecv(const ContactInfo& contact, mesh::Packet*, uint32_t sender_timestamp,
                       const char* text) override
    {
        mcd_message m;
        mcd_rx_meta meta;

        memset(&m, 0, sizeof(m));
        m.id = _next_msg_id++;
        m.outgoing = false;
        memcpy(m.peer_key, contact.id.pub_key, PUB_KEY_SIZE);
        snprintf(m.peer_name, sizeof(m.peer_name), "%s", contact.name);
        snprintf(m.text, sizeof(m.text), "%s", text);
        m.timestamp = sender_timestamp;
        m.mono_ms = mcport::monotonicMillis();
        m.state = MCD_MSG_RECEIVED;
        if (_adapter.currentMeta(meta)) {
            m.snr_known = meta.snr_known;
            m.snr_db = meta.snr_db;
            m.rssi_known = meta.rssi_known;
            m.rssi_dbm = meta.rssi_dbm;
        }
        stamp(contact.id.pub_key);
        push(m);
        emitMessage(m.id);
    }

    /* A CLI-data message and a signed message are MeshCore payloads this
     * service does not serve. They are counted as frames and dropped rather
     * than shown as chat text they are not. */
    void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override { }
    void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*,
                             const char*) override { }
    void onChannelMessageRecv(const mesh::GroupChannel&, mesh::Packet*, uint32_t,
                              const char*) override { }

    /* This node serves no requests. Returning 0 means "no reply", and
     * `data`/`len` are deliberately not read: for a RESPONSE carried in a
     * PATH payload they would be the pointer and length the guard above
     * exists to refuse, and a handler that read them would be the way that
     * defect became an out-of-bounds read. */
    uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t,
                             uint8_t*) override
    {
        return 0;
    }
    void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override { }

    /* The same shape the MeshCore firmwares use: a multiple of the airtime
     * plus a fixed allowance. */
    uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override
    {
        return 12000 + pkt_airtime_millis * 8;
    }
    uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis,
                                        uint8_t path_len) const override
    {
        return 6000 + (pkt_airtime_millis * 2) * (path_len + 1);
    }

    void onSendTimeout() override
    {
        /* MeshCore keeps one timeout, so this says "the message it was
         * watching did not get an ACK". The oldest unanswered one is that
         * message. */
        int oldest = -1;

        for (int i = 0; i < OUTBOX_SLOTS; i++) {
            if (!_outbox[i].used) {
                continue;
            }
            if (oldest < 0 || _outbox[i].sent_ms < _outbox[oldest].sent_ms) {
                oldest = i;
            }
        }
        if (oldest < 0) {
            return;
        }
        markState(_outbox[oldest].msg_id, MCD_MSG_NO_ACK);
        _outbox[oldest].used = false;
    }

    /* ---- dispatcher logging hooks ---- */

    void logRx(mesh::Packet* packet, int len, float) override
    {
        _rx_logged++;
        frameFor(packet, len);
    }
    void logTx(mesh::Packet*, int len) override
    {
        mcport::logWrite(mcport::LOG_DEBUG, "meshcore: tx %d bytes", len);
    }
    void logTxFail(mesh::Packet*, int len) override
    {
        mcport::logWrite(mcport::LOG_WARN, "meshcore: transmit of %d bytes did not complete", len);
    }

private:
    void push(const mcd_message& m)
    {
        _messages[_msg_head] = m;
        _msg_head = (_msg_head + 1) % MSG_RING;
        if (_msg_count < MSG_RING) {
            _msg_count++;
        }
    }

    mcd_message* find(uint64_t id)
    {
        for (int i = 0; i < _msg_count; i++) {
            int pos = (_msg_head - _msg_count + i + MSG_RING * 2) % MSG_RING;

            if (_messages[pos].id == id) {
                return &_messages[pos];
            }
        }
        return NULL;
    }

    void markAcked(uint64_t id)
    {
        mcd_message* m = find(id);

        if (m == NULL) {
            return;
        }
        m->state = MCD_MSG_ACKED;
        m->ack_known = true;
        m->ack_mono_ms = mcport::monotonicMillis();
        emitMessage(id);
    }

    void markState(uint64_t id, mcd_msg_state st)
    {
        mcd_message* m = find(id);

        if (m == NULL) {
            return;
        }
        m->state = st;
        emitMessage(id);
    }

    void addToOutbox(uint32_t expected_ack, uint64_t msg_id, const uint8_t* key, uint64_t now)
    {
        int oldest = 0;

        for (int i = 0; i < OUTBOX_SLOTS; i++) {
            if (!_outbox[i].used) {
                oldest = i;
                goto take;
            }
            if (_outbox[i].sent_ms < _outbox[oldest].sent_ms) {
                oldest = i;
            }
        }
    take:
        _outbox[oldest].used = true;
        _outbox[oldest].expected_ack = expected_ack;
        _outbox[oldest].msg_id = msg_id;
        memcpy(_outbox[oldest].peer_key, key, PUB_KEY_SIZE);
        _outbox[oldest].sent_ms = now;
    }

    /* Per-node telemetry. It is recorded only while the frame that caused
     * the callback is the one this turn took off the queue; a packet held in
     * the delayed inbound queue and processed later arrives with no metadata
     * attached, and is then recorded as heard with the signal unknown rather
     * than with the signal of whatever came next. */
    void stamp(const uint8_t* key)
    {
        mcd_rx_meta meta;
        Telemetry& t = slotFor(key);

        t.used = true;
        memcpy(t.key, key, PUB_KEY_SIZE);
        t.heard_known = true;
        t.heard_ms = mcport::monotonicMillis();
        if (_adapter.currentMeta(meta)) {
            t.heard_ms = meta.mono_ms;
            t.snr_known = meta.snr_known;
            t.snr_db = meta.snr_db;
            t.rssi_known = meta.rssi_known;
            t.rssi_dbm = meta.rssi_dbm;
        } else {
            t.snr_known = false;
            t.rssi_known = false;
        }
    }

public:
    struct Telemetry {
        bool used;
        uint8_t key[PUB_KEY_SIZE];
        bool heard_known;
        uint64_t heard_ms;
        bool snr_known;
        double snr_db;
        bool rssi_known;
        double rssi_dbm;
    };

    const Telemetry* telemetryFor(const uint8_t* key) const
    {
        for (int i = 0; i < MAX_CONTACTS; i++) {
            if (_telemetry[i].used && memcmp(_telemetry[i].key, key, PUB_KEY_SIZE) == 0) {
                return &_telemetry[i];
            }
        }
        return NULL;
    }

    void emitNode(const ContactInfo& c, const char* reason)
    {
        mcd_node n;

        fill(c, n);
        if (_hooks.on_node) {
            _hooks.on_node(_hooks.user, &n, reason);
        }
    }

    void fill(const ContactInfo& c, mcd_node& n) const
    {
        memset(&n, 0, sizeof(n));
        memcpy(n.public_key, c.id.pub_key, PUB_KEY_SIZE);
        snprintf(n.name, sizeof(n.name), "%s", c.name);
        n.type = c.type;
        n.last_advert_timestamp = c.last_advert_timestamp;
        if (c.out_path_len != OUT_PATH_UNKNOWN) {
            uint8_t bytes = (uint8_t)(mesh::Packet::pathHashSize(c.out_path_len) *
                                      mesh::Packet::pathHashCount(c.out_path_len));

            n.path_known = true;
            n.path_len = c.out_path_len;
            n.path_hops = mesh::Packet::pathHashCount(c.out_path_len);
            if (bytes > MAX_PATH_SIZE) {
                bytes = MAX_PATH_SIZE;
            }
            n.path_bytes = bytes;
            memcpy(n.path, c.out_path, bytes);
        }
        const Telemetry* t = telemetryFor(c.id.pub_key);

        if (t) {
            n.last_heard_known = t->heard_known;
            n.last_heard_mono_ms = t->heard_ms;
            n.last_snr_known = t->snr_known;
            n.last_snr_db = t->snr_db;
            n.last_rssi_known = t->rssi_known;
            n.last_rssi_dbm = t->rssi_dbm;
        }
    }

private:
    Telemetry& slotFor(const uint8_t* key)
    {
        int oldest = 0;

        for (int i = 0; i < MAX_CONTACTS; i++) {
            if (_telemetry[i].used && memcmp(_telemetry[i].key, key, PUB_KEY_SIZE) == 0) {
                return _telemetry[i];
            }
            if (!_telemetry[i].used) {
                return _telemetry[i];
            }
            if (_telemetry[i].heard_ms < _telemetry[oldest].heard_ms) {
                oldest = i;
            }
        }
        return _telemetry[oldest];
    }

    void emitMessage(uint64_t id)
    {
        mcd_message* m = find(id);

        if (m && _hooks.on_message) {
            _hooks.on_message(_hooks.user, m);
        }
    }

    void frameFor(mesh::Packet* packet, int len)
    {
        static const char* names[16] = {
            "req", "response", "text", "ack", "advert", "group_text", "group_data",
            "anon_req", "path", "trace", "multipart", "control", "type12", "type13",
            "type14", "raw_custom"
        };
        mcd_rx_meta meta;
        const char* what = packet ? names[packet->getPayloadType() & 0x0F] : "unknown";

        if (!_hooks.on_frame) {
            return;
        }
        if (!_adapter.currentMeta(meta)) {
            memset(&meta, 0, sizeof(meta));
            meta.mono_ms = mcport::monotonicMillis();
        }
        _hooks.on_frame(_hooks.user, &meta, len, what);
    }

    RadiodRadio& _adapter;
    mcd_runtime_hooks _hooks;
    bool _dirty;

    mcd_message _messages[MSG_RING];
    int _msg_count;
    int _msg_head;
    uint64_t _next_msg_id;

    OutboxSlot _outbox[OUTBOX_SLOTS];
    Telemetry _telemetry[MAX_CONTACTS];

    uint64_t _path_refused;
    uint64_t _unparsed;
    uint64_t _rx_logged;

    char _name[MCD_NODE_NAME_LEN];
};

/* The C log sink, behind a C++ one, so the daemon can point the protocol
 * core at pocketlog without this file including pocketlog. */
void (*g_c_sink)(int level, const char* line);

void cxx_sink(mcport::LogLevel level, const char* line)
{
    if (g_c_sink) {
        g_c_sink((int)level, line);
    }
}

}  // namespace

/* ---- the runtime -------------------------------------------------------- */

struct mcd_runtime {
    mcport::MonotonicClock clock;
    mcport::SystemRTCClock rtc;
    mcport::HostRNG rng;
    StaticPoolPacketManager mgr;
    SimpleMeshTables tables;
    RadiodRadio radio;
    Node node;
    char state_dir[512];

    explicit mcd_runtime(const mcd_runtime_hooks& hooks)
        : mgr(32), radio(hooks),
          node(radio, clock, rng, rtc, mgr, tables, radio, hooks)
    {
        state_dir[0] = '\0';
    }
};

extern "C" {

const char* mcd_tx_outcome_name(enum mcd_tx_outcome o)
{
    switch (o) {
    case MCD_TX_OK: return "ok";
    case MCD_TX_RX_RESUME_FAILED: return "rx_resume_failed";
    case MCD_TX_FAILED: return "tx_failed";
    case MCD_TX_UNKNOWN: return "unknown";
    }
    return "unknown";
}

const char* mcd_msg_state_name(enum mcd_msg_state s)
{
    switch (s) {
    case MCD_MSG_RECEIVED: return "received";
    case MCD_MSG_SENT_FLOOD: return "sent_flood";
    case MCD_MSG_SENT_DIRECT: return "sent_direct";
    case MCD_MSG_ACKED: return "acked";
    case MCD_MSG_NO_ACK: return "no_ack";
    case MCD_MSG_FAILED: return "failed";
    }
    return "unknown";
}

void mcd_runtime_set_log_sink(void (*sink)(int level, const char* line))
{
    g_c_sink = sink;
    mcport::setLogSink(sink ? cxx_sink : NULL);
}

const char* mcd_runtime_rift_commit(void) { return MESHCORE_RIFT_COMMIT; }
const char* mcd_runtime_crypto_commit(void) { return MESHCORE_CRYPTO_COMMIT; }

struct mcd_runtime* mcd_runtime_create(const struct mcd_runtime_config* cfg,
                                       const struct mcd_runtime_hooks* hooks,
                                       char* err, size_t errlen)
{
    char store_err[mcdstore::ERR_SIZE] = "";
    mcd_runtime* rt;
    mesh::LocalIdentity id;
    mcdstore::NodeState st;
    int rc;

    if (cfg == NULL || cfg->state_dir == NULL || hooks == NULL) {
        snprintf(err, errlen, "meshcored runtime: missing configuration");
        return NULL;
    }
    if (!mcdstore::ensureDir(cfg->state_dir, store_err)) {
        snprintf(err, errlen, "%s", store_err);
        return NULL;
    }

    rc = mcdstore::identityLoad(id, cfg->state_dir, store_err);
    if (rc < 0) {
        /* A file that is there and wrong stops the service. Generating a
         * replacement would silently make this a different node to every
         * peer that knows it, and would destroy the only copy of a key
         * nothing else holds. */
        snprintf(err, errlen, "%s", store_err);
        return NULL;
    }
    if (rc == 0) {
        if (!mcdstore::identityCreate(id, store_err) ||
            !mcdstore::identitySave(id, cfg->state_dir, store_err)) {
            snprintf(err, errlen, "%s", store_err);
            return NULL;
        }
        mcport::logWrite(mcport::LOG_INFO, "meshcored: generated a new MeshCore identity");
    }

    rc = mcdstore::stateLoad(st, cfg->state_dir, store_err);
    if (rc < 0) {
        snprintf(err, errlen, "%s", store_err);
        return NULL;
    }

    rt = new mcd_runtime(*hooks);
    snprintf(rt->state_dir, sizeof(rt->state_dir), "%s", cfg->state_dir);
    rt->node.self_id = id;

    /* The name: what the operator gave wins and is then persisted; otherwise
     * the stored one; otherwise one derived from the key, which is what the
     * node hash is anyway. */
    if (cfg->node_name && cfg->node_name[0]) {
        rt->node.setName(cfg->node_name);
    } else if (st.name[0]) {
        rt->node.setName(st.name);
    } else {
        char derived[MCD_NODE_NAME_LEN];

        snprintf(derived, sizeof(derived), "Doors-%02x%02x",
                 (unsigned)id.pub_key[0], (unsigned)id.pub_key[1]);
        rt->node.setName(derived);
    }

    rt->node.begin();

    for (int i = 0; i < st.count; i++) {
        if (!rt->node.addContact(st.nodes[i])) {
            mcport::logWrite(mcport::LOG_WARN,
                             "meshcored: the node table is full; %d stored nodes were not loaded",
                             st.count - i);
            break;
        }
    }
    if (rc == 0 || strcmp(st.name, rt->node.name()) != 0) {
        /* A first start, or a name that changed: write the state out now so
         * the name survives even if nothing else happens this session. */
        rt->node.clearDirty();
        if (mcd_runtime_persist(rt) != 0) {
            mcport::logWrite(mcport::LOG_WARN, "meshcored: could not write the node state");
        }
    }
    mcport::logWrite(mcport::LOG_INFO, "meshcored: node %s, %d known node(s)",
                     rt->node.name(), rt->node.getNumContacts());
    return rt;
}

void mcd_runtime_destroy(struct mcd_runtime* rt)
{
    if (rt == NULL) {
        return;
    }
    delete rt;
}

void mcd_runtime_tick(struct mcd_runtime* rt)
{
    rt->radio.beginTurn();
    rt->node.loop();
    rt->node.noteHanded(rt->radio.handed());
}

bool mcd_runtime_rx_pending(const struct mcd_runtime* rt)
{
    return rt->radio.pending();
}

bool mcd_runtime_deliver_rx(struct mcd_runtime* rt, const uint8_t* bytes, int len,
                            const struct mcd_rx_meta* meta)
{
    return rt->radio.push(bytes, len, *meta);
}

uint64_t mcd_runtime_rx_dropped(const struct mcd_runtime* rt)
{
    return rt->radio.dropped();
}

void mcd_runtime_tx_done(struct mcd_runtime* rt, uint64_t submit_id, enum mcd_tx_outcome outcome)
{
    rt->radio.txDone(submit_id, outcome);
}

void mcd_runtime_set_radio_online(struct mcd_runtime* rt, bool online)
{
    rt->radio.setOnline(online);
    rt->radio.setRxMode(online);
}

bool mcd_runtime_radio_online(const struct mcd_runtime* rt)
{
    return rt->radio.online();
}

void mcd_runtime_set_profile(struct mcd_runtime* rt, int sf, double bw_khz, int cr,
                             int preamble, bool crc)
{
    rt->radio.setProfile(sf, bw_khz, cr, preamble, crc);
}

void mcd_runtime_identity(const struct mcd_runtime* rt, uint8_t pub_key[MCD_PUB_KEY_LEN],
                          char* name, size_t name_len)
{
    memcpy(pub_key, rt->node.self_id.pub_key, PUB_KEY_SIZE);
    snprintf(name, name_len, "%s", rt->node.name());
}

int mcd_runtime_node_count(const struct mcd_runtime* rt)
{
    ContactInfo c;
    ContactsIterator it = const_cast<Node&>(rt->node).startContactsIterator();
    int n = 0;

    while (it.hasNext(&rt->node, c)) {
        if (c.type != ADV_TYPE_NONE) {
            n++;
        }
    }
    return n;
}

bool mcd_runtime_node_at(const struct mcd_runtime* rt, int idx, struct mcd_node* n)
{
    ContactInfo c;
    ContactsIterator it = const_cast<Node&>(rt->node).startContactsIterator();
    int seen = 0;

    if (idx < 0) {
        return false;
    }
    while (it.hasNext(&rt->node, c)) {
        if (c.type == ADV_TYPE_NONE) {
            continue;
        }
        if (seen == idx) {
            rt->node.fill(c, *n);
            return true;
        }
        seen++;
    }
    return false;
}

int mcd_runtime_node_by_prefix(const struct mcd_runtime* rt, const uint8_t* prefix,
                               size_t prefix_len, struct mcd_node* n)
{
    ContactInfo c;
    ContactInfo found;
    ContactsIterator it = const_cast<Node&>(rt->node).startContactsIterator();
    int matches = 0;

    if (prefix_len == 0 || prefix_len > PUB_KEY_SIZE) {
        return 0;
    }
    while (it.hasNext(&rt->node, c)) {
        if (c.type == ADV_TYPE_NONE) {
            continue;
        }
        if (memcmp(c.id.pub_key, prefix, prefix_len) != 0) {
            continue;
        }
        if (matches == 0) {
            found = c;
        }
        matches++;
    }
    if (matches == 0) {
        return 0;
    }
    if (matches > 1) {
        return -1;
    }
    if (n) {
        rt->node.fill(found, *n);
    }
    return 1;
}

int mcd_runtime_message_count(const struct mcd_runtime* rt)
{
    return rt->node.messageCount();
}

bool mcd_runtime_message_at(const struct mcd_runtime* rt, int idx, struct mcd_message* m)
{
    return rt->node.messageAt(idx, *m);
}

enum mcd_send_result mcd_runtime_send_text(struct mcd_runtime* rt, const uint8_t* prefix,
                                           size_t prefix_len, const char* text,
                                           uint64_t* msg_id, uint32_t* est_timeout_ms)
{
    ContactInfo c;
    ContactInfo found;
    ContactsIterator it = rt->node.startContactsIterator();
    int matches = 0;
    uint32_t expected_ack = 0;
    uint32_t est = 0;
    uint32_t timestamp;
    int rc;

    if (!rt->radio.online()) {
        return MCD_SEND_NO_RADIO;
    }
    if (text == NULL || text[0] == '\0') {
        return MCD_SEND_TOO_LONG;
    }
    if (strlen(text) > MAX_TEXT_LEN) {
        return MCD_SEND_TOO_LONG;
    }
    if (prefix_len == 0 || prefix_len > PUB_KEY_SIZE) {
        return MCD_SEND_NO_CONTACT;
    }
    while (it.hasNext(&rt->node, c)) {
        if (c.type == ADV_TYPE_NONE) {
            continue;
        }
        if (memcmp(c.id.pub_key, prefix, prefix_len) != 0) {
            continue;
        }
        if (matches == 0) {
            found = c;
        }
        matches++;
    }
    if (matches != 1) {
        return MCD_SEND_NO_CONTACT;
    }

    timestamp = rt->rtc.getCurrentTimeUnique();
    rc = rt->node.sendMessage(found, timestamp, 0, text, expected_ack, est);
    if (rc == MSG_SEND_FAILED) {
        return MCD_SEND_FAILED;
    }
    {
        mcd_msg_state st = (rc == MSG_SEND_SENT_DIRECT) ? MCD_MSG_SENT_DIRECT : MCD_MSG_SENT_FLOOD;
        uint64_t id = rt->node.recordOutgoing(found, text, timestamp, st, expected_ack);

        if (msg_id) {
            *msg_id = id;
        }
    }
    if (est_timeout_ms) {
        *est_timeout_ms = est;
    }
    return (rc == MSG_SEND_SENT_DIRECT) ? MCD_SEND_ACCEPTED_DIRECT : MCD_SEND_ACCEPTED_FLOOD;
}

bool mcd_runtime_send_advert(struct mcd_runtime* rt)
{
    mesh::Packet* pkt;

    if (!rt->radio.online()) {
        return false;
    }
    pkt = rt->node.createSelfAdvert(rt->node.name());
    if (pkt == NULL) {
        return false;
    }
    rt->node.sendFlood(pkt);
    return true;
}

void mcd_runtime_stats(const struct mcd_runtime* rt, struct mcd_runtime_stats* out)
{
    memset(out, 0, sizeof(*out));
    out->sent_flood = rt->node.getNumSentFlood();
    out->sent_direct = rt->node.getNumSentDirect();
    out->recv_flood = rt->node.getNumRecvFlood();
    out->recv_direct = rt->node.getNumRecvDirect();
    out->rx_queued = rt->radio.queued();
    out->rx_dropped = rt->radio.dropped();
    out->packets_free = rt->mgr.getFreeCount();
    out->packets_total = 32;
    out->contacts = mcd_runtime_node_count(rt);
    out->path_payloads_refused = rt->node.pathRefused();
}

bool mcd_runtime_dirty(const struct mcd_runtime* rt)
{
    return rt->node.dirty();
}

int mcd_runtime_persist(struct mcd_runtime* rt)
{
    char err[mcdstore::ERR_SIZE] = "";
    mcdstore::NodeState st;
    ContactInfo c;
    ContactsIterator it = rt->node.startContactsIterator();

    st = mcdstore::NodeState();
    snprintf(st.name, sizeof(st.name), "%s", rt->node.name());
    while (it.hasNext(&rt->node, c) && st.count < mcdstore::MAX_NODES) {
        if (c.type == ADV_TYPE_NONE) {
            continue;
        }
        st.nodes[st.count++] = c;
    }
    if (!mcdstore::stateSave(st, rt->state_dir, err)) {
        mcport::logWrite(mcport::LOG_ERROR, "meshcored: %s", err);
        return -1;
    }
    rt->node.clearDirty();
    return 0;
}

}  /* extern "C" */
