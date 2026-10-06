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
#include "mcd_util.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include <base64.hpp>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>

#include "airtime.h"
#include "mc_port.h"
#include "mesh_remote.h"
#include "mesh_rxlog.h"
#include "mesh_store.h"

/* The sizes this service repeats in its C header must be MeshCore's own. */
static_assert(MCD_PUB_KEY_LEN == PUB_KEY_SIZE, "public key size drifted from MeshCore");
static_assert(MCD_MAX_PATH == MAX_PATH_SIZE, "path size drifted from MeshCore");
static_assert(MCD_MAX_TEXT == MAX_TEXT_LEN, "text length drifted from MeshCore");
/* An app datagram is sendRequest()'s data: the app byte, the length and the
 * payload, which upstream allows up to MAX_PACKET_PAYLOAD - 16 bytes of. */
static_assert(2 + MCD_APP_PAYLOAD_MAX <= MAX_PACKET_PAYLOAD - 16, "app payload too large for a REQ");
static_assert(MCD_APP_MARKER > 0x07, "the app byte collides with an upstream request type");
static_assert(MCD_MAX_FRAME == MAX_TRANS_UNIT, "frame size drifted from MeshCore");
static_assert(MCD_NODE_NAME_LEN == sizeof(((ContactInfo*)0)->name),
              "contact name size drifted from MeshCore");
/* The channel table's size is decided by protocols/meshcore/compat/mc_channels.h
 * and reached through <Arduino.h>, which BaseChatMesh.h includes before it
 * declares channels[MAX_GROUP_CHANNELS]. This service repeats the number in
 * its C header so that header stands alone; the assertion is what stops the
 * two from ever being different values for the same array. */
static_assert(MCD_MAX_CHANNELS == MAX_GROUP_CHANNELS,
              "channel table size drifted from MeshCore");
static_assert(MCD_CHANNEL_NAME_LEN == sizeof(((ChannelDetails*)0)->name),
              "channel name size drifted from MeshCore");
static_assert(MCD_MAX_CHANNELS == mcdstore::MAX_CHANNELS,
              "the channel table and channels.v1 disagree about how many channels there are");
/* The contact table, the same way: sized by compat/mc_contacts.h, repeated
 * in the C header, persisted by mesh_store.h. */
static_assert(MCD_MAX_NODES == MAX_CONTACTS, "contact table size drifted from MeshCore");
static_assert(mcdstore::MAX_NODES == MAX_CONTACTS,
              "the contact table and state.v1 disagree about how many nodes there are");

#ifndef MESHCORE_RIFT_COMMIT
#define MESHCORE_RIFT_COMMIT ""
#endif
#ifndef MESHCORE_CRYPTO_COMMIT
#define MESHCORE_CRYPTO_COMMIT ""
#endif

namespace {

/* MeshCore's well-known Public channel key: upstream's PUBLIC_GROUP_PSK
 * ("izOH6cXN6mrJ5e26oRXNcg==", examples/companion_radio/MyMesh.cpp), the
 * 128-bit key every stock MeshCore node joins as its first channel. It is
 * public by design; it is here so the service can say which of the channels
 * it holds is that one (mcd_channel.is_public). */
const uint8_t kPublicChannelKey[16] = { 0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                        0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72 };

/* How many received frames wait between one turn of the loop and the next.
 * A burst larger than this is dropped and counted rather than making the
 * daemon allocate without bound because somebody is transmitting quickly. */
const int RX_QUEUE_DEPTH = 32;

/* The last N messages, in memory. Messages are not persisted - see
 * docs/services/MESHCORED.md, "What is persistent".
 *
 * TWO rings, not one, and that is deliberate. A channel can be busy in a way
 * a direct conversation never is: one chatty group would otherwise push every
 * direct message out of a shared ring within seconds, and "channels were
 * added" would read to a user as "my messages disappeared". Separate rings
 * mean channel traffic cannot evict direct history and direct traffic cannot
 * evict a channel's, which is what keeps the existing direct behaviour
 * exactly as it was. The two are merged by message id - one id space, handed
 * out in arrival order - wherever a caller wants them as one list. */
const int MSG_RING = 64;
const int CHAN_MSG_RING = 64;

/* How many messages may be waiting for an ACK at once.
 *
 * Each one carries its OWN deadline, and that is the point of this table.
 * MeshCore tracks a single timeout (BaseChatMesh::txt_send_timeout): every
 * sendMessage() overwrites it and any matched ACK zeroes it
 * (BaseChatMesh.cpp:339, :350, :451, :455). With two messages in flight that
 * one timer belongs to whichever was sent last, so an ACK for the first
 * cancelled the second's timeout and left it `sent_*` for ever, and a timeout
 * that did fire was pinned on the oldest message rather than the one it was
 * for. So upstream's timer is not used here at all (onSendTimeout does
 * nothing) and expireAcks() gives each message the answer that belongs to it.
 *
 * A full table refuses the next send (MCD_SEND_BUSY) rather than overwriting
 * a slot: an overwritten message would never hear its ACK matched and would
 * never time out either, which is the same fault by another route. */
const int OUTBOX_SLOTS = 8;

/* How many expected ACKs are remembered after their wait is over.
 *
 * A deadline is an estimate (calcFlood/DirectTimeoutMillisFor), and an ACK
 * can come back after it - a busy mesh, a long flood. Upstream's companion
 * firmware keeps every expected ACK in a circular table that a timeout does
 * not clear (examples/companion_radio/MyMesh.cpp, expected_ack_table,
 * processAck), so a late ACK there still confirms the message; its own UI
 * then marks a "no ack" line delivered (ui-rift/RiftMsgLog.h, markDelivered).
 * This keeps the same promise: a message that went no_ack becomes acked when
 * its ACK arrives late, and so does one that was resent, by an ACK for any of
 * its attempts. Twice upstream's eight, because each resend leaves one more
 * attempt behind. Only this run: nothing here survives a restart, so an ACK
 * for a message of an earlier run matches nothing and is ignored. */
const int LATE_ACK_SLOTS = 16;

/* How many of this node's own channel sends are watched: for the radio's
 * outcome and for copies relayed back to it (mcd_message, echo_tracked).
 * Upstream gives a group frame no ACK and no tag (BaseChatMesh.cpp:487-506,
 * companion CMD_SEND_CHANNEL_TXT_MSG answers only OK), but it does mark its
 * own flood as seen "in case it is rebroadcast back to us" (Mesh.cpp:651):
 * a copy coming back is expected, and its packet hash is the one sent. The
 * newest sends are watched; an older one simply stops counting. */
const int CHAN_ECHO_SLOTS = 16;

struct ChanEchoSlot {
    bool used;
    uint8_t hash[MAX_HASH_SIZE];
    uint64_t msg_id;
};

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
    /* When this message stops waiting: sent_ms plus the timeout MeshCore
     * computed for this very packet (flood or direct, from its airtime and
     * the path length). */
    uint64_t deadline_ms;
};

class Node : public BaseChatMesh {
public:
    Node(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc,
         mesh::PacketManager& mgr, SimpleMeshTables& tables, RadiodRadio& adapter,
         const mcd_runtime_hooks& hooks)
        : BaseChatMesh(radio, ms, rng, rtc, mgr, tables), _adapter(adapter), _hooks(hooks),
          _seen_tables(tables),
          _dirty(false), _channels_dirty(false), _msg_count(0), _msg_head(0),
          _chan_count(0), _chan_head(0), _next_msg_id(1),
          _path_refused(0), _unparsed(0), _rx_logged(0), _unretained(0),
          _contacts_full(0), _chan_unmatched(0), _app_count(0), _app_head(0), _next_app_id(1),
          _rx_flood(false), _app_rx(0), _app_tx(0), _app_receipts(0), _app_refused(0),
          _path_hash_bytes(1)
    {
        memset(_app, 0, sizeof(_app));
        memset(_outbox, 0, sizeof(_outbox));
        memset(_late, 0, sizeof(_late));
        _late_next = 0;
        memset(_echo, 0, sizeof(_echo));
        _echo_next = 0;
        _chan_sent = false;
        memset(_chan_sent_hash, 0, sizeof(_chan_sent_hash));
        memset(_messages, 0, sizeof(_messages));
        memset(_chan_messages, 0, sizeof(_chan_messages));
        memset(_occupied, 0, sizeof(_occupied));
        memset(_key_len, 0, sizeof(_key_len));
        _name[0] = '\0';
    }

    void setName(const char* n)
    {
        snprintf(_name, sizeof(_name), "%s", n ? n : "");
    }
    const char* name() const { return _name; }
    /* A rename is a change to what state.v1 holds (its header carries the
     * name), so the table is marked for writing like any other change. */
    void rename(const char* n)
    {
        setName(n);
        _dirty = true;
    }

    /* How many bytes of each relay's hash a flood this node starts asks the
     * repeaters to write into its path: MeshCore's path hash size, 1 to 3
     * (upstream's path_hash_mode 0..2 plus one; mode 3 is reserved there).
     * 1 is what every MeshCore build has always done and stays the default. */
    void setPathHashBytes(uint8_t bytes) { _path_hash_bytes = bytes; }
    uint8_t pathHashBytes() const { return _path_hash_bytes; }

    /* Every flood this node starts goes through these two - a message or a
     * returned path to a node, a message to a channel (BaseChatMesh) - and
     * the advert (mcd_runtime_send_advert). They are upstream's
     * sendFloodScoped with no scope, as the companion firmware's own
     * (examples/companion_radio/MyMesh.cpp, sendFloodScoped with a null
     * TransportKey): sendFlood with the configured path hash size. No
     * transport codes are written; this service has no flood scopes. */
    void sendFloodScoped(const ContactInfo&, mesh::Packet* pkt, uint32_t delay_millis) override
    {
        sendFlood(pkt, delay_millis, _path_hash_bytes);
    }
    void sendFloodScoped(const mesh::GroupChannel&, mesh::Packet* pkt,
                         uint32_t delay_millis) override
    {
        /* The packet's hash, for the channel message sendChannelText() is
         * about to record (CHAN_ECHO_SLOTS). The path is not in it. */
        if (pkt) {
            pkt->calculatePacketHash(_chan_sent_hash);
            _chan_sent = true;
        }
        sendFlood(pkt, delay_millis, _path_hash_bytes);
    }

    bool dirty() const { return _dirty; }
    void clearDirty() { _dirty = false; }
    bool channelsDirty() const { return _channels_dirty; }
    void clearChannelsDirty() { _channels_dirty = false; }
    uint64_t channelFramesUnmatched() const { return _chan_unmatched; }
    uint64_t pathRefused() const { return _path_refused; }
    uint64_t unparsed() const { return _unparsed; }
    uint64_t unretained() const { return _unretained; }
    uint64_t contactsFull() const { return _contacts_full; }

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

    /* The receive log's two points outside the dispatcher: a raw frame it
     * took off the air and never parsed (after the loop's turn), and one the
     * receive queue turned away before the dispatcher saw it. */
    void flushRxRaw()
    {
        mcd_rx_obs obs;

        if (_hooks.on_rx_obs && _rxlog.takeRaw(obs)) {
            _hooks.on_rx_obs(_hooks.user, &obs);
        }
    }
    void observeTurnedAway(const uint8_t* bytes, int len, const mcd_rx_meta& meta,
                           enum mcd_rx_reject why)
    {
        mcd_rx_obs obs;

        if (_hooks.on_rx_obs) {
            _rxlog.rejected(bytes, len, meta, why, obs);
            _hooks.on_rx_obs(_hooks.user, &obs);
        }
    }

    /* ---- messages ---- */

    int messageCount() const { return _msg_count + _chan_count; }

    /* The two rings as one list, oldest first.
     *
     * Both are already in ascending id order - one counter hands ids out in
     * arrival order and each ring is appended to - so this is a merge, and
     * the result is every message in the order it happened regardless of
     * which ring it is in. O(idx) per call and at most 128 entries, which is
     * not worth an index for. */
    bool messageAt(int idx, mcd_message& out) const
    {
        int total = _msg_count + _chan_count;
        int i = 0;
        int j = 0;
        int k;

        if (idx < 0 || idx >= total) {
            return false;
        }
        for (k = 0; k <= idx; k++) {
            const mcd_message* a = (i < _msg_count) ? &_messages[directAt(i)] : NULL;
            const mcd_message* b = (j < _chan_count) ? &_chan_messages[channelAt(j)] : NULL;
            const mcd_message* take;

            if (a != NULL && (b == NULL || a->id < b->id)) {
                take = a;
                i++;
            } else {
                take = b;
                j++;
            }
            if (k == idx) {
                out = *take;
                return true;
            }
        }
        return false;
    }

    uint64_t recordOutgoing(const ContactInfo& to, const char* text, uint32_t timestamp,
                            mcd_msg_state state, uint32_t expected_ack, uint32_t timeout_ms)
    {
        mcd_message m;

        memset(&m, 0, sizeof(m));
        m.id = _next_msg_id++;
        m.outgoing = true;
        m.is_channel = false;
        memcpy(m.peer_key, to.id.pub_key, PUB_KEY_SIZE);
        snprintf(m.peer_name, sizeof(m.peer_name), "%s", to.name);
        snprintf(m.text, sizeof(m.text), "%s", text);
        m.timestamp = timestamp;
        m.mono_ms = mcport::monotonicMillis();
        m.state = state;
        /* A directed message is acknowledged: MeshCore produced an
         * expected_ack for it and will time it out if none arrives. */
        m.ack_expected = true;
        push(m);
        addToOutbox(expected_ack, m.id, to.id.pub_key, m.mono_ms, m.mono_ms + timeout_ms);
        emitMessage(m.id);
        return m.id;
    }

    /* Is there room to watch one more message for its ACK? Asked BEFORE the
     * message is built and handed to the dispatcher, so a refusal puts
     * nothing on the air. */
    bool outboxFull() const
    {
        for (int i = 0; i < OUTBOX_SLOTS; i++) {
            if (!_outbox[i].used) {
                return false;
            }
        }
        return true;
    }

    int outboxWaiting() const
    {
        int n = 0;

        for (int i = 0; i < OUTBOX_SLOTS; i++) {
            n += _outbox[i].used ? 1 : 0;
        }
        return n;
    }

    /* Every message whose own deadline has passed without an ACK becomes
     * no_ack, earliest deadline first so the events come out in the order
     * the deadlines fell. Each message is judged against its own deadline
     * and nobody else's - see OUTBOX_SLOTS. */
    int expireAcks(uint64_t now_ms)
    {
        int expired = 0;

        for (;;) {
            int due = -1;

            for (int i = 0; i < OUTBOX_SLOTS; i++) {
                if (!_outbox[i].used || _outbox[i].deadline_ms > now_ms) {
                    continue;
                }
                if (due < 0 || _outbox[i].deadline_ms < _outbox[due].deadline_ms) {
                    due = i;
                }
            }
            if (due < 0) {
                return expired;
            }
            /* Its wait is over; its ACK is still recognised if it comes. */
            _late[_late_next] = _outbox[due];
            _late_next = (_late_next + 1) % LATE_ACK_SLOTS;
            _outbox[due].used = false;
            markState(_outbox[due].msg_id, MCD_MSG_NO_ACK);
            expired++;
        }
    }

    /* Send message `id` again: its text and its timestamp, attempt + 1. The
     * caller has checked the radio and the outbox; see mcd_runtime_resend. */
    int resend(uint64_t id, uint32_t& est)
    {
        mcd_message* m = find(id);
        uint32_t expected_ack = 0;
        int rc;

        /* A message waiting for its ACK is sent_*, and becomes no_ack only
         * when its slot is let go, so the state alone keeps two attempts of
         * one message from being in flight. The attempt is one byte on the
         * air (composeMsgPacket); it does not wrap round to 0. */
        if (m == NULL || !m->outgoing || m->is_channel || !m->ack_expected ||
            (m->state != MCD_MSG_NO_ACK && m->state != MCD_MSG_FAILED) || m->attempt == 255) {
            return -1;
        }
        ContactInfo* c = lookupContactByPubKey(m->peer_key, PUB_KEY_SIZE);

        if (c == NULL || c->type == ADV_TYPE_NONE) {
            return -2;
        }
        rc = sendMessage(*c, m->timestamp, (uint8_t)(m->attempt + 1), m->text, expected_ack, est);
        if (rc == MSG_SEND_FAILED) {
            return -3;
        }
        m->attempt++;
        m->state = (rc == MSG_SEND_SENT_DIRECT) ? MCD_MSG_SENT_DIRECT : MCD_MSG_SENT_FLOOD;
        m->ack_known = false;
        m->ack_mono_ms = 0;
        {
            uint64_t now = mcport::monotonicMillis();

            addToOutbox(expected_ack, id, m->peer_key, now, now + est);
        }
        emitMessage(id);
        return rc;
    }

    /* ---- contacts --------------------------------------------------------
     *
     * The contact table holds MAX_CONTACTS and MeshCore does not evict from
     * it (shouldOverwriteWhenFull() is false), so once it is full an advert
     * from a new node is counted as unretained and a direct message to that
     * node cannot be sent: there is no contact to encrypt to. Forgetting a
     * node is what makes room. It is the node's entry here that goes - its
     * learned route and its last advert - not the node: it is added back the
     * next time it adverts. */
    bool forgetNode(const uint8_t* key, mcd_node& was)
    {
        ContactInfo* c = lookupContactByPubKey(key, PUB_KEY_SIZE);

        if (c == NULL || c->type == ADV_TYPE_NONE) {
            return false;
        }
        fill(*c, was);
        if (!removeContact(*c)) {
            return false;
        }
        /* The repeater the session is with has gone from the table: there is
         * no contact to encrypt to or match a reply against, so the session
         * ends, and a request it had outstanding is answered cancelled. */
        if (_remote.isTarget(key)) {
            mcd_remote_reply r;

            if (_remote.end(&r, mcport::monotonicMillis())) {
                emitRemote(r);
            }
        }
        /* What this service kept beside the contact goes with it, so a node
         * that adverts again is heard afresh rather than inheriting the
         * signal of a node that was forgotten. A message still waiting for
         * its ACK keeps waiting: an ACK names the message, not the contact. */
        for (int i = 0; i < MAX_CONTACTS; i++) {
            if (_telemetry[i].used && memcmp(_telemetry[i].key, key, PUB_KEY_SIZE) == 0) {
                memset(&_telemetry[i], 0, sizeof(_telemetry[i]));
            }
        }
        _dirty = true;
        if (_hooks.on_node) {
            _hooks.on_node(_hooks.user, &was, "removed");
        }
        return true;
    }

    /* Forget the learned route to a node, so the next message to it floods
     * and the reply teaches a fresh one. The remedy when a node has moved and
     * the stored route no longer reaches it, which shows up as direct
     * messages that are never acknowledged. */
    bool resetPath(const uint8_t* key, mcd_node& now)
    {
        ContactInfo* c = lookupContactByPubKey(key, PUB_KEY_SIZE);

        if (c == NULL || c->type == ADV_TYPE_NONE) {
            return false;
        }
        resetPathTo(*c);
        _dirty = true;
        fill(*c, now);
        emitNode(*c, "path");
        return true;
    }

    /* ---- channels ------------------------------------------------------
     *
     * MeshCore holds the table; this class holds which of its slots are
     * real. The two are separate because BaseChatMesh has no idea: its
     * `channels` array is a fixed array of ChannelDetails that starts zeroed,
     * and a zeroed slot is indistinguishable from a channel whose key is 32
     * zero bytes. That distinction is the whole of the guard below.
     */

    int channelCount() const
    {
        int n = 0;

        for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
            if (_occupied[i]) {
                n++;
            }
        }
        return n;
    }

    bool channelBySlot(int slot, mcd_channel& out) const
    {
        ChannelDetails ch;

        if (slot < 0 || slot >= MAX_GROUP_CHANNELS || !_occupied[slot]) {
            return false;
        }
        if (!const_cast<Node*>(this)->getChannel(slot, ch)) {
            return false;
        }
        memset(&out, 0, sizeof(out));
        out.slot = slot;
        snprintf(out.name, sizeof(out.name), "%s", ch.name);
        out.hash = ch.channel.hash[0];
        out.key_bits = _key_len[slot] * 8;
        out.is_public = _key_len[slot] == (int)sizeof(kPublicChannelKey) &&
                        memcmp(ch.channel.secret, kPublicChannelKey,
                               sizeof(kPublicChannelKey)) == 0;
        out.text_limit = channelTextLimit();
        return true;
    }

    bool channelAt(int idx, mcd_channel& out) const
    {
        int seen = 0;

        for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
            if (!_occupied[i]) {
                continue;
            }
            if (seen == idx) {
                return channelBySlot(i, out);
            }
            seen++;
        }
        return false;
    }

    /* The longest body this node can put on a channel.
     *
     * sendGroupMessage() writes "<our name>: " into the payload and then
     * SILENTLY TRUNCATES the caller's text to make the whole thing fit
     * MAX_TEXT_LEN (BaseChatMesh.cpp:496). Truncating a message somebody
     * typed and reporting success is not something this service will do, so
     * it refuses instead - and a composer needs this number rather than the
     * 160 that applies to a direct message. */
    int channelTextLimit() const
    {
        int prefix = (int)strlen(_name) + 2;
        int limit = MAX_TEXT_LEN - prefix;

        return limit > 0 ? limit : 0;
    }

    /* Install a channel. key_len is 16 or 32; the caller has already
     * validated the key, because the reasons a key is refused are things a
     * client needs told apart. */
    bool installChannel(int slot, const char* name, const uint8_t* key, int key_len)
    {
        ChannelDetails ch;

        if (slot < 0 || slot >= MAX_GROUP_CHANNELS) {
            return false;
        }
        memset(&ch, 0, sizeof(ch));
        snprintf(ch.name, sizeof(ch.name), "%s", name ? name : "");
        memcpy(ch.channel.secret, key, (size_t)key_len);
        /* setChannel() is upstream's own installer: it derives the channel
         * hash from the key exactly as every other MeshCore node does. The
         * derivation is not reimplemented here, because a channel whose hash
         * this service computed differently would be a channel nobody else
         * can route to. */
        if (!setChannel(slot, ch)) {
            return false;
        }
        _occupied[slot] = true;
        _key_len[slot] = key_len;
        _channels_dirty = true;
        emitChannel(slot, "added");
        return true;
    }

    bool removeChannel(int slot)
    {
        ChannelDetails blank;
        mcd_channel gone;

        if (slot < 0 || slot >= MAX_GROUP_CHANNELS || !_occupied[slot]) {
            return false;
        }
        (void)channelBySlot(slot, gone);
        memset(&blank, 0, sizeof(blank));
        /* The key is overwritten in MeshCore's own table, not merely marked
         * unused: leaving it there would keep a secret this node has been
         * told to forget. */
        setChannel(slot, blank);
        _occupied[slot] = false;
        _key_len[slot] = 0;
        _channels_dirty = true;
        if (_hooks.on_channel) {
            _hooks.on_channel(_hooks.user, &gone, "removed");
        }
        return true;
    }

    int freeChannelSlot() const
    {
        for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
            if (!_occupied[i]) {
                return i;
            }
        }
        return -1;
    }

    /* Is this key already in the table? The key is the channel, so a second
     * copy under another name would be a channel that can never be routed
     * to: MeshCore's scan would find whichever came first and stop. */
    bool holdsKey(const uint8_t* key, int key_len) const
    {
        ChannelDetails ch;

        for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
            if (!_occupied[i] || _key_len[i] != key_len) {
                continue;
            }
            if (!const_cast<Node*>(this)->getChannel(i, ch)) {
                continue;
            }
            if (memcmp(ch.channel.secret, key, PUB_KEY_SIZE) == 0) {
                return true;
            }
        }
        return false;
    }

    bool channelKeyAt(int slot, uint8_t* key, int* key_len) const
    {
        ChannelDetails ch;

        if (slot < 0 || slot >= MAX_GROUP_CHANNELS || !_occupied[slot]) {
            return false;
        }
        if (!const_cast<Node*>(this)->getChannel(slot, ch)) {
            return false;
        }
        memcpy(key, ch.channel.secret, PUB_KEY_SIZE);
        *key_len = _key_len[slot];
        return true;
    }

    /* Send on a channel. Flood, unacknowledged, and recorded as such. */
    mcd_send_result sendChannelText(int slot, const char* text, uint64_t* msg_id)
    {
        ChannelDetails ch;
        mcd_message m;
        uint32_t timestamp;

        if (slot < 0 || slot >= MAX_GROUP_CHANNELS || !_occupied[slot]) {
            return MCD_SEND_NO_CHANNEL;
        }
        if (text == NULL || text[0] == '\0') {
            return MCD_SEND_TOO_LONG;
        }
        if ((int)strlen(text) > channelTextLimit()) {
            return MCD_SEND_TOO_LONG;
        }
        if (!getChannel(slot, ch)) {
            return MCD_SEND_NO_CHANNEL;
        }
        timestamp = getRTCClock()->getCurrentTimeUnique();
        _chan_sent = false;
        if (!sendGroupMessage(timestamp, ch.channel, _name, text, (int)strlen(text))) {
            return MCD_SEND_FAILED;
        }

        memset(&m, 0, sizeof(m));
        m.id = _next_msg_id++;
        m.outgoing = true;
        m.is_channel = true;
        m.channel_slot = slot;
        m.channel_hash = ch.channel.hash[0];
        snprintf(m.channel_name, sizeof(m.channel_name), "%s", ch.name);
        snprintf(m.sender_name, sizeof(m.sender_name), "%s", _name);
        /* The text as it went on the air, prefix and all, so a sender and a
         * receiver hold the same bytes for the same message. It is built the
         * same way BaseChatMesh.cpp:492 builds it. */
        snprintf(m.text, sizeof(m.text), "%s: %s", _name, text);
        m.timestamp = timestamp;
        m.mono_ms = mcport::monotonicMillis();
        /* sent_flood is where this ends. There is no ACK for a group frame -
         * no expected_ack, no timeout, no delivery report - so nothing will
         * ever move it to acked or to no_ack, and ack_expected says so
         * rather than leaving a client to infer it from a state that never
         * changes. */
        m.state = MCD_MSG_SENT_FLOOD;
        m.ack_expected = false;
        if (_chan_sent) {
            ChanEchoSlot& e = _echo[_echo_next];

            e.used = true;
            memcpy(e.hash, _chan_sent_hash, sizeof(e.hash));
            e.msg_id = m.id;
            _echo_next = (_echo_next + 1) % CHAN_ECHO_SLOTS;
            m.echo_tracked = true;
            _chan_sent = false;
        }
        pushChannel(m);
        emitMessage(m.id);
        if (msg_id) {
            *msg_id = m.id;
        }
        return MCD_SEND_ACCEPTED_FLOOD;
    }

    /* ---- BaseChatMesh, the presentation side ---- */

protected:
    /* A discovered contact that the table did not keep.
     *
     * Once MeshCore's fixed MAX_CONTACTS table is full, allocateContactSlot()
     * returns NULL and BaseChatMesh calls this anyway, with a ContactInfo
     * built on its own stack purely so a UI can say "somebody adverted and I
     * could not keep them" (vendor/RIFT/src/helpers/BaseChatMesh.cpp:172-179).
     * It is not in the table, it will not come back from mesh.nodes, and it
     * will be gone the moment this returns.
     *
     * Treating it as stored - which is what happens if you just use it - gives
     * a mesh.node event for a node mesh.node cannot then find, marks the state
     * dirty so state.v1 is rewritten with nothing changed, and evicts a real
     * node's telemetry slot to hold readings for a node nobody kept. On a busy
     * mesh with a full table that repeats for every advert from every stranger.
     *
     * The test is exact rather than a guess. When the contact IS stored,
     * upstream passes *from, which is the table entry itself, so looking its
     * key up returns that same address. When it is transient there is no entry
     * with that key at all - the transient path is only reached when the
     * lookup upstream already did came back empty - so the lookup returns
     * NULL. No well-formed case is misread either way.
     *
     * The table-full policy itself is unchanged: MeshCore still refuses the
     * new contact and keeps the ones it has. */
    bool isRetained(const ContactInfo& contact)
    {
        return lookupContactByPubKey(contact.id.pub_key, PUB_KEY_SIZE) == &contact;
    }

    void onDiscoveredContact(ContactInfo& contact, bool, uint8_t path_len, const uint8_t*) override
    {
        /* is_new is deliberately ignored: BaseChatMesh declares it false and
         * never assigns it (vendor/RIFT/src/helpers/BaseChatMesh.cpp:154 and
         * 198), so a contact added for the first time is reported as not new.
         * Upstream's to decide; nothing here is built on the flag. */
        if (!isRetained(contact)) {
            _unretained++;
            return;
        }
        _dirty = true;
        stamp(contact.id.pub_key);
        /* path_len is the advert packet's own (BaseChatMesh::onAdvertRecv
         * passes packet->path_len), so its hop count is how many relays the
         * advert came through: 0 when it was heard straight from the node.
         * Read, not inferred, and kept beside the signal for this run. */
        noteAdvertHops(contact.id.pub_key, mesh::Packet::pathHashCount(path_len));
        emitNode(contact, "discovered");
    }

    void noteAdvertHops(const uint8_t* key, uint8_t hops)
    {
        Telemetry& t = slotFor(key);

        if (!t.used || memcmp(t.key, key, PUB_KEY_SIZE) != 0) {
            return; /* stamp() has just made the slot; nothing else may */
        }
        t.advert_hops_known = true;
        t.advert_hops = hops;
        t.advert_ms = t.heard_ms;
    }

    /* Counted rather than logged per advert: on a full table this fires for
     * every stranger that adverts, and a log line each would be the noise. */
    void onContactsFull() override { _contacts_full++; }

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
        /* Waiting first, then the ones whose wait is over (LATE_ACK_SLOTS).
         * Whichever matches, every slot of that message goes - an ACK for any
         * attempt is delivery of the message - so a second copy of the same
         * ACK, or an ACK for another attempt of it, matches nothing and
         * changes nothing. A crc nobody is waiting for is ignored. */
        for (int pass = 0; pass < 2; pass++) {
            OutboxSlot* table = pass == 0 ? _outbox : _late;
            int n = pass == 0 ? OUTBOX_SLOTS : LATE_ACK_SLOTS;

            for (int i = 0; i < n; i++) {
                OutboxSlot& s = table[i];

                if (!s.used || s.expected_ack != crc) {
                    continue;
                }
                uint64_t id = s.msg_id;
                ContactInfo* c = lookupContactByPubKey(s.peer_key, PUB_KEY_SIZE);

                forgetAcks(id);
                markAcked(id);
                return c;
            }
        }
        return NULL;
    }

    void forgetAcks(uint64_t id)
    {
        for (int i = 0; i < OUTBOX_SLOTS; i++) {
            if (_outbox[i].used && _outbox[i].msg_id == id) {
                _outbox[i].used = false;
            }
        }
        for (int i = 0; i < LATE_ACK_SLOTS; i++) {
            if (_late[i].used && _late[i].msg_id == id) {
                _late[i].used = false;
            }
        }
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

    /* CLI data is a repeater's answer to a command this node sent it
     * (mesh_remote.h). Taken only as the answer to the one command the
     * session has outstanding, from the node it went to; anything else is
     * counted stale and dropped rather than shown as chat text it is not. A
     * signed message is a payload this service does not serve. */
    void onCommandDataRecv(const ContactInfo& contact, mesh::Packet*, uint32_t,
                           const char* text) override
    {
        mcd_remote_reply r;

        if (_remote.onCommandData(contact.id.pub_key, text, mcport::monotonicMillis(), &r) ==
            mcdremote::V_ANSWER) {
            emitRemote(r);
        }
    }
    void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*,
                             const char*) override { }
    /* ---- the empty-slot guard ------------------------------------------
     *
     * BaseChatMesh::searchChannelsByHash() walks all MAX_GROUP_CHANNELS slots
     * and offers every one whose hash byte matches the frame's
     * (vendor/RIFT/src/helpers/BaseChatMesh.cpp:367-376; known debt 5 in
     * protocols/meshcore/README.md). A slot with no channel in it is all
     * zeroes - hash byte 0, key 32 zero bytes - so a frame whose channel-hash
     * byte is 0 matches EVERY free slot and is handed to an all-zero key.
     * That key is not a secret. Anyone can encrypt to it, and an unguarded
     * node then accepts the message on a channel it never joined, with a
     * MAC that verifies.
     *
     * Upstream's own firmware never meets this: its slot 0 always holds the
     * public channel and its screens skip empty slots. A service whose table
     * starts empty meets it on the very first group frame.
     *
     * So the table is filtered here, before any key is consulted, by the one
     * thing MeshCore does not record: which slots actually hold a channel.
     * The test is exact rather than a heuristic - a slot is occupied because
     * installChannel() put something in it - so no real channel is hidden and
     * no empty slot is offered. tests/meshcore_smoke_test.cpp demonstrates
     * both halves: the fault on an unguarded node, the refusal on a guarded
     * one.
     */
    int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel dest[],
                             int max_matches) override
    {
        ChannelDetails ch;
        int n = 0;

        for (int i = 0; i < MAX_GROUP_CHANNELS && n < max_matches; i++) {
            if (!_occupied[i] || !getChannel(i, ch)) {
                continue;
            }
            if (ch.channel.hash[0] == hash[0]) {
                dest[n++] = ch.channel;
            }
        }
        if (n == 0) {
            /* A group frame for a channel this node does not hold. Ordinary
             * on any mesh with more than one channel on it, and counted
             * rather than logged for exactly that reason. */
            _chan_unmatched++;
        }
        return n;
    }

    /* A message on one of our channels.
     *
     * `channel` is the entry whose key actually decrypted it, which is how a
     * node holding two channels that share the one-byte hash learns which of
     * them this was - the hash cannot say, and the MAC already has. */
    void onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet*,
                              uint32_t timestamp, const char* text) override
    {
        mcd_message m;
        mcd_rx_meta meta;
        ChannelDetails ch;
        int slot = slotForChannel(channel);

        if (slot < 0) {
            /* Unreachable through the guard above, which only ever offers a
             * slot that is occupied. Counted rather than asserted: this is a
             * daemon, and the honest answer to "that cannot happen" is to
             * drop the frame and say so in a number. */
            _chan_unmatched++;
            return;
        }
        if (!getChannel(slot, ch)) {
            _chan_unmatched++;
            return;
        }
        memset(&m, 0, sizeof(m));
        m.id = _next_msg_id++;
        m.outgoing = false;
        m.is_channel = true;
        m.channel_slot = slot;
        m.channel_hash = channel.hash[0];
        snprintf(m.channel_name, sizeof(m.channel_name), "%s", ch.name);
        snprintf(m.text, sizeof(m.text), "%s", text);
        claimedSender(text, m.sender_name, sizeof(m.sender_name));
        m.timestamp = timestamp;
        m.mono_ms = mcport::monotonicMillis();
        m.state = MCD_MSG_RECEIVED;
        m.ack_expected = false;
        if (_adapter.currentMeta(meta)) {
            m.snr_known = meta.snr_known;
            m.snr_db = meta.snr_db;
            m.rssi_known = meta.rssi_known;
            m.rssi_dbm = meta.rssi_dbm;
        }
        /* No stamp() call, and no contact is touched. A group frame names no
         * node: there is no public key in it, nothing signs it, and the name
         * in the text is a claim. Recording it against a contact would be
         * this service deciding who sent it. */
        pushChannel(m);
        emitMessage(m.id);
    }

    /* ---- app datagrams (mesh_runtime.h, docs/api/mesh.md) ----
     *
     * Upstream hands onContactRequest() the contact but not the packet, and
     * whether a datagram came by flood is what decides if it is answered. So
     * the route is noted on the way in and upstream does the rest exactly as
     * it always has: the MAC, the contact lookup, the decryption. */
    void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret,
                        uint8_t* data, size_t len) override
    {
        _rx_flood = packet && packet->isRouteFlood();
        BaseChatMesh::onPeerDataRecv(packet, type, sender_idx, secret, data, len);
    }

    /* A REQ from a contact. Everything but a Doors app datagram is refused
     * with "no reply", as before: this node serves no MeshCore requests.
     *
     * `data` is the decrypted REQ after its 4-byte tag, `len` its length
     * padded to the AES block - which is why the datagram carries its own
     * length, and why that length is checked against what arrived. It is
     * only ever reached from a REQ (BaseChatMesh::onPeerDataRecv); a RESPONSE
     * carried in a PATH payload goes to onContactResponse(), which reads
     * nothing, behind the guard above. */
    uint8_t onContactRequest(const ContactInfo& contact, uint32_t sender_timestamp,
                             const uint8_t* data, uint8_t len, uint8_t* reply) override
    {
        mcd_app_datagram d;
        mcd_rx_meta meta;
        int port;

        if (!data || len < 3 || (data[0] & 0xF0) != MCD_APP_MARKER) {
            return 0;
        }
        port = data[0] & 0x0F;
        if (port < MCD_APP_PORT_MIN || data[1] == 0 || data[1] > MCD_APP_PAYLOAD_MAX ||
            data[1] > len - 2) {
            _app_refused++;
            return 0;
        }
        memset(&d, 0, sizeof(d));
        d.id = _next_app_id++;
        d.port = (uint8_t)port;
        memcpy(d.from, contact.id.pub_key, PUB_KEY_SIZE);
        d.len = data[1];
        memcpy(d.payload, data + 2, d.len);
        d.flood = _rx_flood;
        d.mono_ms = mcport::monotonicMillis();
        if (_adapter.currentMeta(meta)) {
            d.snr_known = meta.snr_known;
            d.snr_db = meta.snr_db;
            d.rssi_known = meta.rssi_known;
            d.rssi_dbm = meta.rssi_dbm;
        }
        _app[(_app_head + _app_count) % MCD_APP_INBOX] = d;
        if (_app_count < MCD_APP_INBOX) {
            _app_count++;
        } else {
            _app_head = (_app_head + 1) % MCD_APP_INBOX;
        }
        _app_rx++;
        stamp(contact.id.pub_key);
        if (_hooks.on_app) {
            _hooks.on_app(_hooks.user, &d);
        }
        if (!_rx_flood) {
            return 0;
        }
        /* The receipt: the request's tag, as upstream's replies begin, and
         * the app byte. Upstream sends it back on the path the flood came
         * by, which is what teaches the sender a direct route. */
        memcpy(reply, &sender_timestamp, 4);
        reply[4] = data[0];
        _app_receipts++;
        return 5;
    }
    /* A RESPONSE: a repeater answering the session's login or request, or a
     * Doors peer's app-datagram receipt, which carries nothing to read.
     * Matched by mesh_remote.h's rules; everything else is ignored. A
     * RESPONSE riding a PATH payload reaches here only past the PATH guard
     * above. */
    void onContactResponse(const ContactInfo& contact, const uint8_t* data, uint8_t len) override
    {
        mcd_remote_reply r;
        mcdremote::Verdict v =
            _remote.onResponse(contact.id.pub_key, data, len, mcport::monotonicMillis(), &r);

        if (v == mcdremote::V_ANSWER || v == mcdremote::V_LATE_LOGIN) {
            emitRemote(r);
        }
    }

    /* A zero-hop CONTROL packet: mesh::Mesh delivers one here only when it
     * came with no relay in its path (Mesh.cpp:70-75), so a discovery answer
     * is proof the repeater was heard directly. This node serves no
     * discovery requests of its own - it is not a repeater. */
    void onControlDataRecv(mesh::Packet* packet) override
    {
        mcd_discovered d;
        mcd_discover_state st;
        mcd_rx_meta meta;
        bool known = _adapter.currentMeta(meta);

        if (packet == NULL ||
            !_discovery.onResponse(packet->payload, packet->payload_len, self_id.pub_key, &meta,
                                   known, mcport::monotonicMillis(), &d)) {
            return;
        }
        _discovery.state(&st);
        if (_hooks.on_discover) {
            _hooks.on_discover(_hooks.user, &d, &st);
        }
    }

public:
    /* ---- repeater discovery and the session (mesh_remote.h) ---- */
    mcdremote::Session& remote() { return _remote; }
    const mcdremote::Session& remote() const { return _remote; }
    mcdremote::Discovery& discovery() { return _discovery; }
    const mcdremote::Discovery& discovery() const { return _discovery; }
    /* Upstream's logout (companion CMD_LOGOUT): keep-alive state dropped,
     * nothing transmitted. Protected upstream, so reached through here. */
    void dropConnection(const uint8_t* key) { stopConnection(key); }

    /* The answer, with the signal of the frame that carried it when this
     * turn's frame is the one, and the session afterwards. */
    void emitRemote(mcd_remote_reply& r)
    {
        mcd_remote_session s;
        mcd_rx_meta meta;

        if (r.outcome == MCD_REMOTE_REPLIED && _adapter.currentMeta(meta)) {
            r.snr_known = meta.snr_known;
            r.snr_db = meta.snr_db;
            r.rssi_known = meta.rssi_known;
            r.rssi_dbm = meta.rssi_dbm;
        }
        _remote.state(&s);
        if (_hooks.on_remote) {
            _hooks.on_remote(_hooks.user, &r, &s);
        }
    }

    void emitDiscoverClosed()
    {
        mcd_discover_state st;

        _discovery.state(&st);
        if (_hooks.on_discover) {
            _hooks.on_discover(_hooks.user, NULL, &st);
        }
    }

    int appInbox(int port, uint64_t after_id, mcd_app_datagram* out, int max) const
    {
        int n = 0;
        int i;

        for (i = 0; i < _app_count && n < max; i++) {
            const mcd_app_datagram& d = _app[(_app_head + i) % MCD_APP_INBOX];

            if (d.port == port && d.id > after_id) {
                out[n++] = d;
            }
        }
        return n;
    }
    void noteAppTx() { _app_tx++; }
    uint64_t appRx() const { return _app_rx; }
    uint64_t appTx() const { return _app_tx; }
    uint64_t appReceipts() const { return _app_receipts; }

protected:

    /* The same shape the MeshCore firmwares use: a multiple of the airtime
     * plus a fixed allowance. */
    uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override
    {
        return 12000 + pkt_airtime_millis * 8;
    }
    /* path_len is MeshCore's PACKED byte: the hop count in the low six bits
     * and the path hash size in the top two. Per hop means per hop, as
     * upstream's companion reads it (`path_len & 63`): on a mesh using
     * 2-byte path hashes a zero-hop route is 0x40, and reading that as 65
     * hops made a direct request to a repeater beside this node wait over
     * two minutes (unit B, 2026-10-05). */
    uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis,
                                        uint8_t path_len) const override
    {
        return 6000 + (pkt_airtime_millis * 2) * (mesh::Packet::pathHashCount(path_len) + 1);
    }

    void onSendTimeout() override
    {
        /* Deliberately nothing. MeshCore's one timer belongs to whichever
         * message was sent last and is cancelled by an ACK for any of them,
         * so it cannot say which message went unanswered - and pinning it on
         * the oldest, as this used to, marked the wrong one. Every message
         * in the outbox has its own deadline instead, and expireAcks() is
         * what reads them (see OUTBOX_SLOTS). */
    }

    /* ---- dispatcher logging hooks ---- */

    void logRx(mesh::Packet* packet, int len, float) override
    {
        _rx_logged++;
        frameFor(packet, len);
        observeRx(packet, len);
    }
    void logTx(mesh::Packet* packet, int len) override
    {
        mcport::logWrite(mcport::LOG_DEBUG, "meshcore: tx %d bytes", len);
        noteChannelTx(packet, true);
        if (_hooks.on_rx_obs && packet) {
            _rxlog.noteOwn(packet);
        }
    }

    /* ---- the receive log (mesh_runtime.h, mesh_rxlog.h) ----
     *
     * Three observation points, none of which changes what MeshCore does:
     * the raw bytes (logRxRaw), the parsed packet (logRx, just above), and
     * MeshCore's handler (onRecvPacket, wrapped). The dispatcher calls the
     * first two in that order inside one checkRecv(), before the handler
     * has seen the packet - so before its seen-table, before a flood waits
     * out its receive delay, and before a relay rewrites the path. */
    void logRxRaw(float, float, const uint8_t raw[], int len) override
    {
        mcd_rx_meta meta;

        if (!_hooks.on_rx_obs) {
            return;
        }
        if (!_adapter.currentMeta(meta)) {
            memset(&meta, 0, sizeof(meta));
            meta.mono_ms = mcport::monotonicMillis();
        }
        /* An empty pool means the dispatcher will not even try to parse
         * these bytes (Dispatcher::checkRecv, allocNew). */
        _rxlog.noteRaw(len, len > 0, len > 0 ? raw[0] : 0, _mgr->getFreeCount() == 0, meta);
    }

    /* MeshCore's handler, unchanged: the wrapper only reads what changed
     * around the call - its seen-table's duplicate counters, the message
     * ids, the action it returned - and reports. A packet with no
     * observation held (the log is off) goes straight through. */
    mesh::DispatcherAction onRecvPacket(mesh::Packet* pkt) override
    {
        mcd_rx_obs obs;

        /* Before MeshCore's seen-table, which drops a copy of this node's
         * own flood without a word (Mesh.cpp:651, :233). */
        noteChannelEcho(pkt);

        if (!_hooks.on_rx_obs || !_rxlog.take(pkt, obs)) {
            return BaseChatMesh::onRecvPacket(pkt);
        }
        uint64_t dups = seenDuplicates();
        uint64_t next_id = _next_msg_id;
        mesh::DispatcherAction action = BaseChatMesh::onRecvPacket(pkt);

        obs.verdict = seenDuplicates() != dups ? MCD_RX_DUPLICATE : MCD_RX_NEW;
        obs.relayed = action != ACTION_RELEASE;
        describeDecoded(obs, pkt, next_id);
        _hooks.on_rx_obs(_hooks.user, &obs);
        return action;
    }
    void logTxFail(mesh::Packet* packet, int len) override
    {
        mcport::logWrite(mcport::LOG_WARN, "meshcore: transmit of %d bytes did not complete", len);
        noteChannelTx(packet, false);
    }

    /* The watched channel message this packet is, or NULL: a GRP_TXT whose
     * hash is one of this node's own recent sends, still in the ring. */
    mcd_message* ownChannelMessage(const mesh::Packet* pkt)
    {
        uint8_t hash[MAX_HASH_SIZE];

        if (pkt == NULL || pkt->getPayloadType() != PAYLOAD_TYPE_GRP_TXT) {
            return NULL;
        }
        pkt->calculatePacketHash(hash);
        for (int i = 0; i < CHAN_ECHO_SLOTS; i++) {
            if (_echo[i].used && memcmp(_echo[i].hash, hash, sizeof(hash)) == 0) {
                return find(_echo[i].msg_id);
            }
        }
        return NULL;
    }

    /* The radio's word on this node's own channel frame: sent, or not. */
    void noteChannelTx(const mesh::Packet* pkt, bool sent)
    {
        mcd_message* m = ownChannelMessage(pkt);

        if (m == NULL || !m->outgoing || (m->tx_known && m->transmitted == sent)) {
            return;
        }
        m->tx_known = true;
        m->transmitted = sent;
        emitMessage(m->id);
    }

    /* A copy of this node's own channel frame, relayed back: counted, with
     * the fewest relays any copy came through. A copy with no relay in its
     * path is not one a repeater sent on, and is not counted. */
    void noteChannelEcho(const mesh::Packet* pkt)
    {
        mcd_message* m;
        uint8_t hops;

        if (pkt == NULL || !pkt->isRouteFlood()) {
            return;
        }
        hops = (uint8_t)pkt->getPathHashCount();
        if (hops == 0 || (m = ownChannelMessage(pkt)) == NULL || !m->outgoing) {
            return;
        }
        if (m->heard_back == 0 || hops < m->heard_back_hops) {
            m->heard_back_hops = hops;
        }
        if (m->heard_back == 0) {
            m->heard_back_mono_ms = mcport::monotonicMillis();
        }
        if (m->heard_back < UINT16_MAX) {
            m->heard_back++;
        }
        emitMessage(m->id);
    }

private:
    /* Where the i'th oldest entry of each ring lives. */
    int directAt(int i) const { return (_msg_head - _msg_count + i + MSG_RING * 2) % MSG_RING; }
    int channelAt(int i) const
    {
        return (_chan_head - _chan_count + i + CHAN_MSG_RING * 2) % CHAN_MSG_RING;
    }

    void push(const mcd_message& m)
    {
        _messages[_msg_head] = m;
        _msg_head = (_msg_head + 1) % MSG_RING;
        if (_msg_count < MSG_RING) {
            _msg_count++;
        }
    }

    void pushChannel(const mcd_message& m)
    {
        _chan_messages[_chan_head] = m;
        _chan_head = (_chan_head + 1) % CHAN_MSG_RING;
        if (_chan_count < CHAN_MSG_RING) {
            _chan_count++;
        }
    }

    mcd_message* find(uint64_t id)
    {
        for (int i = 0; i < _msg_count; i++) {
            if (_messages[directAt(i)].id == id) {
                return &_messages[directAt(i)];
            }
        }
        for (int i = 0; i < _chan_count; i++) {
            if (_chan_messages[channelAt(i)].id == id) {
                return &_chan_messages[channelAt(i)];
            }
        }
        return NULL;
    }

    /* Which of our channels this GroupChannel is. The key is the identity -
     * the one-byte hash is not, because two channels can share it - so the
     * comparison is over the whole secret. */
    int slotForChannel(const mesh::GroupChannel& channel) const
    {
        ChannelDetails ch;

        for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
            if (!_occupied[i] || !const_cast<Node*>(this)->getChannel(i, ch)) {
                continue;
            }
            if (memcmp(ch.channel.secret, channel.secret, PUB_KEY_SIZE) == 0) {
                return i;
            }
        }
        return -1;
    }

    /* The sender's CLAIMED name, parsed back out of the payload.
     *
     * MeshCore's sendGroupMessage() writes "<name>: " ahead of the body
     * inside the encrypted payload (BaseChatMesh.cpp:492), and that is the
     * only sender identity a group frame has. Nothing signs it, so it is a
     * claim - and this parse is its inverse, with the same ambiguity: the
     * rule is the FIRST ": ", because a name containing one, or a body whose
     * first words do, cannot be told apart from the prefix by anything in
     * the frame. `text` keeps the whole payload either way, so a caller that
     * disagrees with the split still has the bytes.
     *
     * Empty when the text does not begin with a prefix at all, which is what
     * a sender that is not MeshCore's own chat client would produce. */
    static void claimedSender(const char* text, char* out, size_t out_len)
    {
        const char* sep;
        size_t n;

        out[0] = '\0';
        if (text == NULL) {
            return;
        }
        sep = strstr(text, ": ");
        if (sep == NULL || sep == text) {
            return;
        }
        n = (size_t)(sep - text);
        if (n >= out_len) {
            /* Longer than any name MeshCore could have written, so it is not
             * a prefix - it is a body with a colon in it. */
            return;
        }
        memcpy(out, text, n);
        out[n] = '\0';
    }

    void emitChannel(int slot, const char* reason)
    {
        mcd_channel c;

        if (_hooks.on_channel && channelBySlot(slot, c)) {
            _hooks.on_channel(_hooks.user, &c, reason);
        }
    }

    void markAcked(uint64_t id)
    {
        mcd_message* m = find(id);

        /* Once: a message already delivered is not delivered again, and its
         * first ACK's time is the one that is kept. processAck already lets go
         * of every slot of a message it matches, so a second ACK finds none;
         * this holds even if a later change reaches here another way. */
        if (m == NULL || m->state == MCD_MSG_ACKED) {
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

    /* The caller has already checked outboxFull(): a send that could not be
     * watched is refused before it is built. Should a slot nevertheless not
     * be found, the message is answered no_ack at once rather than left
     * `sent_*` with nothing watching it - a message nothing will ever time
     * out is the fault this table exists to prevent. */
    void addToOutbox(uint32_t expected_ack, uint64_t msg_id, const uint8_t* key, uint64_t now,
                     uint64_t deadline)
    {
        for (int i = 0; i < OUTBOX_SLOTS; i++) {
            if (_outbox[i].used) {
                continue;
            }
            _outbox[i].used = true;
            _outbox[i].expected_ack = expected_ack;
            _outbox[i].msg_id = msg_id;
            memcpy(_outbox[i].peer_key, key, PUB_KEY_SIZE);
            _outbox[i].sent_ms = now;
            _outbox[i].deadline_ms = deadline;
            return;
        }
        markState(msg_id, MCD_MSG_NO_ACK);
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

        /* A slot taken over from another node carries nothing of it over:
         * its advert's hop count was that node's. */
        if (!t.used || memcmp(t.key, key, PUB_KEY_SIZE) != 0) {
            t.advert_hops_known = false;
            t.advert_hops = 0;
            t.advert_ms = 0;
        }
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
        /* The hop count of the node's last advert, as the packet carried it
         * (mcd_node.advert_hops). */
        bool advert_hops_known;
        uint8_t advert_hops;
        uint64_t advert_ms;
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
        if ((c.gps_lat != 0 || c.gps_lon != 0) && c.gps_lat >= -90000000 &&
            c.gps_lat <= 90000000 && c.gps_lon >= -180000000 && c.gps_lon <= 180000000) {
            n.location_known = true;
            n.lat_e6 = c.gps_lat;
            n.lon_e6 = c.gps_lon;
        }
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
            n.advert_hops_known = t->advert_hops_known;
            n.advert_hops = t->advert_hops;
            n.advert_mono_ms = t->advert_ms;
        }
    }

private:
    /* The node's own slot wherever it is; else a free one; else the one heard
     * longest ago. In that order, and the first search over the whole table:
     * forgetting a node (forgetNode) frees a slot in the MIDDLE, and a search
     * that stopped at the first free slot would give a node already held
     * further on a second slot - after which a later eviction could wipe a
     * real node's last-heard time to make room for a duplicate. */
    Telemetry& slotFor(const uint8_t* key)
    {
        int oldest = 0;
        int free_slot = -1;

        for (int i = 0; i < MAX_CONTACTS; i++) {
            if (_telemetry[i].used && memcmp(_telemetry[i].key, key, PUB_KEY_SIZE) == 0) {
                return _telemetry[i];
            }
            if (!_telemetry[i].used) {
                if (free_slot < 0) {
                    free_slot = i;
                }
                continue;
            }
            if (_telemetry[i].heard_ms < _telemetry[oldest].heard_ms) {
                oldest = i;
            }
        }
        return _telemetry[free_slot >= 0 ? free_slot : oldest];
    }

    void emitMessage(uint64_t id)
    {
        mcd_message* m = find(id);

        if (m && _hooks.on_message) {
            _hooks.on_message(_hooks.user, m);
        }
    }

    void observeRx(mesh::Packet* packet, int len)
    {
        mcd_rx_obs obs;
        mcd_rx_obs evicted;
        mcd_rx_meta meta;

        if (!_hooks.on_rx_obs || !packet) {
            return;
        }
        _rxlog.clearRaw();
        if (!_adapter.currentMeta(meta)) {
            memset(&meta, 0, sizeof(meta));
            meta.mono_ms = mcport::monotonicMillis();
        }
        _rxlog.capture(packet, len, meta, obs);
        if (obs.has_channel_hash) {
            obs.channel_known = holdsChannelHash(obs.channel_hash);
        }
        if (obs.has_dest_hash) {
            obs.for_us = self_id.isHashMatch(&obs.dest_hash, 1);
        }
        if (_rxlog.hold(packet, obs, &evicted)) {
            _hooks.on_rx_obs(_hooks.user, &evicted);
        }
    }

    bool holdsChannelHash(uint8_t hash)
    {
        ChannelDetails ch;

        for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
            if (_occupied[i] && getChannel(i, ch) && ch.channel.hash[0] == hash) {
                return true;
            }
        }
        return false;
    }

    uint64_t seenDuplicates() const
    {
        return (uint64_t)_seen_tables.getNumFloodDups() + _seen_tables.getNumDirectDups();
    }

    /* What MeshCore read of the packet, from what it recorded: a message it
     * filed while handling this one, or the node table's name for an
     * advert's key. Nothing is decrypted here. */
    void describeDecoded(mcd_rx_obs& obs, const mesh::Packet* pkt, uint64_t next_id)
    {
        if (_next_msg_id > next_id) {
            const mcd_message* m = find(_next_msg_id - 1);

            if (m && !m->outgoing) {
                if (m->is_channel) {
                    const char* body = m->text;
                    size_t claimed = strlen(m->sender_name);

                    /* text holds the payload whole; the claimed name and its
                     * ": " are the prefix (mcd_message.sender_name). */
                    if (claimed > 0 && strncmp(body, m->sender_name, claimed) == 0 &&
                        body[claimed] == ':' && body[claimed + 1] == ' ') {
                        body += claimed + 2;
                    }
                    obs.decode = MCD_RX_DECODE_CHANNEL;
                    snprintf(obs.sender, sizeof(obs.sender), "%s", m->sender_name);
                    snprintf(obs.channel_name, sizeof(obs.channel_name), "%s", m->channel_name);
                    snprintf(obs.text, sizeof(obs.text), "%s", body);
                } else {
                    obs.decode = MCD_RX_DECODE_DIRECT;
                    obs.has_sender_key = true;
                    memcpy(obs.sender_key, m->peer_key, PUB_KEY_SIZE);
                    snprintf(obs.sender, sizeof(obs.sender), "%s", m->peer_name);
                    snprintf(obs.recipient, sizeof(obs.recipient), "%s", _name);
                    snprintf(obs.text, sizeof(obs.text), "%s", m->text);
                }
                return;
            }
        }
        if (obs.payload_type == PAYLOAD_TYPE_ADVERT && pkt->payload_len >= PUB_KEY_SIZE) {
            ContactInfo* c = lookupContactByPubKey(pkt->payload, PUB_KEY_SIZE);

            if (c && c->name[0]) {
                obs.decode = MCD_RX_DECODE_ADVERT;
                obs.has_sender_key = true;
                memcpy(obs.sender_key, c->id.pub_key, PUB_KEY_SIZE);
                snprintf(obs.sender, sizeof(obs.sender), "%s", c->name);
            }
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
    SimpleMeshTables& _seen_tables;  /* read only: its duplicate counters */
    mcdrx::Log _rxlog;
    bool _dirty;
    bool _channels_dirty;

    mcd_message _messages[MSG_RING];
    int _msg_count;
    int _msg_head;
    mcd_message _chan_messages[CHAN_MSG_RING];
    int _chan_count;
    int _chan_head;
    uint64_t _next_msg_id;

    /* Which of MeshCore's channel slots hold a channel, and how long each
     * key is. MeshCore records neither: a zeroed slot and a channel keyed
     * with zeroes are one thing to it, and setChannel() infers the key
     * length from the key's own content. Both are kept here so the guard
     * above is exact and so channels.v1 stores what it was told rather than
     * what can be guessed back. */
    bool _occupied[MAX_GROUP_CHANNELS];
    int _key_len[MAX_GROUP_CHANNELS];

    OutboxSlot _outbox[OUTBOX_SLOTS];
    /* Expected ACKs whose wait is over, newest overwriting oldest: see
     * LATE_ACK_SLOTS. Only `used`, `expected_ack`, `msg_id` and `peer_key`
     * mean anything here. */
    OutboxSlot _late[LATE_ACK_SLOTS];
    int _late_next;
    /* This node's own channel packets, by hash (CHAN_ECHO_SLOTS). The hash of
     * the one being sent is caught in sendFloodScoped(GroupChannel) - which
     * sendGroupMessage() calls with the packet it built - and filed against
     * the message once the message has an id. */
    ChanEchoSlot _echo[CHAN_ECHO_SLOTS];
    int _echo_next;
    bool _chan_sent;
    uint8_t _chan_sent_hash[MAX_HASH_SIZE];
    Telemetry _telemetry[MAX_CONTACTS];

    uint64_t _path_refused;
    uint64_t _unparsed;
    uint64_t _rx_logged;
    uint64_t _unretained;
    uint64_t _contacts_full;
    uint64_t _chan_unmatched;

    /* App datagrams held for clients that were not listening: a ring, this
     * run only, and the route the frame being handled came by. */
    mcd_app_datagram _app[MCD_APP_INBOX];
    int _app_count;
    int _app_head;
    uint64_t _next_app_id;
    bool _rx_flood;
    uint64_t _app_rx;
    uint64_t _app_tx;
    uint64_t _app_receipts;
    uint64_t _app_refused;
    uint8_t _path_hash_bytes; /* setPathHashBytes */

    char _name[MCD_NODE_NAME_LEN];

    mcdremote::Session _remote;
    mcdremote::Discovery _discovery;
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
    /* What was wrong with the stored node state, when something was. Empty on
     * an ordinary start; reported through mesh.status so a client can see
     * that this node forgot what it knew, rather than wondering why its
     * table is empty. */
    char state_fault[192];
    /* The same for channels.v1, kept apart because the two losses are not
     * comparable. A lost node table costs a rediscovery the mesh performs by
     * itself; a lost channel table costs every key an operator typed in, and
     * nothing on the air will bring one back. */
    char channel_fault[192];
    /* Set when the unusable file could not be moved aside. Nothing is written
     * for the rest of this run: the file is the only evidence there is. */
    bool persist_blocked;
    bool channels_blocked;
    /* Where the name in use came from (mcd_runtime_name_source). */
    enum mcd_name_source name_source;
    /* The mark of the name this start was configured with (--name), "" when
     * none was; and settings.v1's renamed_over as it stands. Both are needed
     * by whatever writes settings.v1 next, which writes the whole file. */
    char config_mark[mcdstore::RENAMED_OVER_HEX + 1];
    char renamed_over[mcdstore::RENAMED_OVER_HEX + 1];

    explicit mcd_runtime(const mcd_runtime_hooks& hooks)
        : mgr(32), radio(hooks),
          node(radio, clock, rng, rtc, mgr, tables, radio, hooks),
          persist_blocked(false)  /* channels_blocked is set in the body */
    {
        state_dir[0] = '\0';
        state_fault[0] = '\0';
        channel_fault[0] = '\0';
        channels_blocked = false;
        name_source = MCD_NAME_DERIVED;
        config_mark[0] = '\0';
        renamed_over[0] = '\0';
    }
};

/* The mark of a configured name: FNV-1a over its bytes, as 16 hex. It tells
 * one configured name from another and nothing more - it is not a secret and
 * nothing is protected by it. */
static void nameMark(const char* name, char out[mcdstore::RENAMED_OVER_HEX + 1])
{
    uint64_t h = 14695981039346656037ULL;

    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    snprintf(out, mcdstore::RENAMED_OVER_HEX + 1, "%016llx", (unsigned long long)h);
}

/* settings.v1 is written whole, from what the runtime holds. */
static bool saveSettings(struct mcd_runtime* rt)
{
    mcdstore::Settings set;
    char err[mcdstore::ERR_SIZE] = "";

    set.path_hash_bytes = rt->node.pathHashBytes();
    snprintf(set.renamed_over, sizeof(set.renamed_over), "%s", rt->renamed_over);
    if (!mcdstore::settingsSave(set, rt->state_dir, err)) {
        mcport::logWrite(mcport::LOG_ERROR, "meshcored: %s", err);
        return false;
    }
    return true;
}

extern "C" {

/* Defined with the rest of the persistence, below. Declared here because
 * joining or leaving a channel writes the table straight away rather than
 * waiting for the daemon's timer - see mcd_runtime_channel_add. */
static int persistChannels(struct mcd_runtime* rt);

/* The standard Public channel, joined if no slot holds its key (see
 * mesh_runtime.h, "THE PUBLIC CHANNEL IS MANDATORY"). Called once, after the
 * stored channels are restored. */
static void ensurePublicChannel(struct mcd_runtime* rt)
{
    uint8_t key[PUB_KEY_SIZE];
    int slot;

    /* holdsKey compares the whole secret buffer, which MeshCore keeps
     * zero-padded past a 16-byte key. */
    memset(key, 0, sizeof(key));
    memcpy(key, kPublicChannelKey, sizeof(kPublicChannelKey));
    if (rt->node.holdsKey(key, (int)sizeof(kPublicChannelKey))) {
        return;
    }
    slot = rt->node.freeChannelSlot();
    if (slot < 0) {
        mcport::logWrite(mcport::LOG_WARN,
                         "meshcored: every channel slot is taken, so the standard Public "
                         "channel could not be added; no other channel was removed");
        return;
    }
    if (!rt->node.installChannel(slot, "Public", key, (int)sizeof(kPublicChannelKey))) {
        mcport::logWrite(mcport::LOG_WARN,
                         "meshcored: the standard Public channel could not be installed in "
                         "slot %d", slot);
        return;
    }
    mcport::logWrite(mcport::LOG_INFO,
                     "meshcored: joined the standard Public channel into slot %d", slot);
    /* Written at once, like any join: the next start finds it stored. */
    (void)persistChannels(rt);
}

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

const char* mcd_channel_result_name(enum mcd_channel_result r)
{
    switch (r) {
    case MCD_CHANNEL_OK: return "ok";
    case MCD_CHANNEL_BAD_KEY: return "bad_key";
    case MCD_CHANNEL_AMBIGUOUS_KEY: return "ambiguous_key";
    case MCD_CHANNEL_BAD_NAME: return "bad_name";
    case MCD_CHANNEL_FULL: return "full";
    case MCD_CHANNEL_DUPLICATE: return "duplicate";
    case MCD_CHANNEL_NOT_FOUND: return "not_found";
    case MCD_CHANNEL_FAILED: return "failed";
    case MCD_CHANNEL_MANDATORY: return "mandatory";
    }
    return "failed";
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

int mcd_runtime_lock_state_dir(const char* state_dir, bool* busy, char* err, size_t errlen)
{
    char store_err[mcdstore::ERR_SIZE] = "";
    int fd;

    if (busy) {
        *busy = false;
    }
    if (state_dir == NULL || !mcdstore::ensureDir(state_dir, store_err)) {
        snprintf(err, errlen, "%s", state_dir ? store_err : "no state directory");
        return -1;
    }
    /* The directory itself, not a file inside it: nothing new appears in a
     * directory whose listing the gates read, and there is no lock file to
     * leave behind. flock on a directory descriptor is ordinary Linux. */
    fd = open(state_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, errlen, "cannot open %s: %s", state_dir, strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;

        close(fd);
        if (e == EWOULDBLOCK) {
            if (busy) {
                *busy = true;
            }
            snprintf(err, errlen,
                     "another meshcored is already running on %s; refusing to start a "
                     "second node with the same identity",
                     state_dir);
        } else {
            snprintf(err, errlen, "cannot lock %s: %s", state_dir, strerror(e));
        }
        return -1;
    }
    return fd;
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

    /* A state.v1 this build will not read is NOT fatal, and the difference
     * from the identity above is the whole point.
     *
     * The identity cannot be reconstructed: lose it and every peer holds a
     * contact for a node that no longer exists. state.v1 is a cache - names,
     * advert types and return paths that the mesh will say again - so a
     * corrupt one costs a rediscovery, not an identity. Treating the two the
     * same way is what made one bad file fatal: the runtime refused to start,
     * meshcored exited, the supervisor restarted it, it read the same file
     * and exited again, five times, and then gave up with a crash-loop marker
     * - a node taken off the air for the rest of the session by a cache.
     *
     * So the file is moved aside (renamed, never rewritten: it is the only
     * evidence of what went wrong), the node starts with an empty table, and
     * what happened is recorded where a client can read it. */
    char state_fault[192] = "";
    char quarantined[288] = "";
    bool persist_blocked = false;

    rc = mcdstore::stateLoad(st, cfg->state_dir, store_err);
    if (rc < 0) {
        char q_err[mcdstore::ERR_SIZE] = "";

        mcport::logWrite(mcport::LOG_ERROR, "meshcored: the stored node state is unusable: %s",
                         store_err);
        if (mcdstore::stateQuarantine(cfg->state_dir, quarantined, sizeof(quarantined), q_err)) {
            mcport::logWrite(mcport::LOG_WARN,
                             "meshcored: it has been kept as %s; starting with no known nodes",
                             quarantined);
            snprintf(state_fault, sizeof(state_fault), "%s (kept as %s)", store_err, quarantined);
        } else {
            /* It could not be moved. Then it is not written over either:
             * whatever is in it is the only record of the fault, and a new
             * table on top of it would be the second mistake. This run keeps
             * nothing, and says so. */
            mcport::logWrite(mcport::LOG_ERROR,
                             "meshcored: it could not be moved aside (%s); this run will not "
                             "write node state, so the file is left for inspection", q_err);
            snprintf(state_fault, sizeof(state_fault),
                     "%s (left in place; node state is not being written)", store_err);
            persist_blocked = true;
        }
        st = mcdstore::NodeState();
        rc = 0;  /* as if there had been no file: a first start, with a note */
    }

    rt = new mcd_runtime(*hooks);
    snprintf(rt->state_fault, sizeof(rt->state_fault), "%s", state_fault);
    rt->persist_blocked = persist_blocked;
    snprintf(rt->state_dir, sizeof(rt->state_dir), "%s", cfg->state_dir);
    rt->node.self_id = id;

    /* The service's own settings, read before the name is chosen: one of
     * them says whether the stored name is a rename made over the configured
     * one. A file that cannot be read leaves the defaults (path hash 1, what
     * MeshCore has always sent; no rename) and says so; it holds nothing that
     * cannot be set again. */
    mcdstore::Settings set;
    bool stale_mark = false;
    {
        char set_err[mcdstore::ERR_SIZE] = "";

        if (mcdstore::settingsLoad(set, cfg->state_dir, set_err) < 0) {
            mcport::logWrite(mcport::LOG_WARN,
                             "meshcored: %s; path hash size stays 1 byte, and a configured "
                             "name is used as given",
                             set_err);
            set = mcdstore::Settings();
        }
    }
    snprintf(rt->renamed_over, sizeof(rt->renamed_over), "%s", set.renamed_over);
    if (cfg->node_name && cfg->node_name[0]) {
        nameMark(cfg->node_name, rt->config_mark);
    }

    /* The name: what the operator gave wins and is then persisted - unless
     * the node was renamed (mesh.set_name) while that same configured name
     * was in force, in which case the rename in state.v1 stands until the
     * operator configures a different one; otherwise the stored one;
     * otherwise one derived from the key, which is what the node hash is
     * anyway. */
    if (rt->config_mark[0] && st.name[0] && strcmp(rt->config_mark, rt->renamed_over) == 0) {
        rt->node.setName(st.name);
        rt->name_source = MCD_NAME_STORED;
    } else if (cfg->node_name && cfg->node_name[0]) {
        rt->node.setName(cfg->node_name);
        rt->name_source = MCD_NAME_CONFIG;
        /* The operator configured a different name since the rename, and it
         * has won: the mark is spent. Left in place it would hand the name
         * back to state.v1 the day the old configured name was put back. */
        stale_mark = rt->renamed_over[0] != '\0';
        rt->renamed_over[0] = '\0';
    } else if (st.name[0]) {
        rt->node.setName(st.name);
        rt->name_source = MCD_NAME_STORED;
    } else {
        char derived[MCD_NODE_NAME_LEN];

        snprintf(derived, sizeof(derived), "Doors-%02x%02x",
                 (unsigned)id.pub_key[0], (unsigned)id.pub_key[1]);
        rt->node.setName(derived);
    }

    rt->node.setPathHashBytes((uint8_t)set.path_hash_bytes);
    if (stale_mark && !saveSettings(rt)) {
        mcport::logWrite(mcport::LOG_WARN, "meshcored: could not clear renamed_over in "
                                           "settings.v1");
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
    /* ---- the channels --------------------------------------------------
     *
     * Treated like state.v1 and not like identity.id: an unreadable file is
     * moved aside and the node starts with no channels rather than refusing
     * to run, because a daemon that will not start is a node off the air.
     * The difference from state.v1 is what it costs - the mesh cannot give a
     * channel key back - so the fault is reported separately and said
     * plainly rather than folded in with the node table's. */
    {
        mcdstore::ChannelState cs;
        char q_err[mcdstore::ERR_SIZE] = "";
        char kept[288] = "";
        int crc;

        crc = mcdstore::channelsLoad(cs, cfg->state_dir, store_err);
        if (crc < 0) {
            mcport::logWrite(mcport::LOG_ERROR,
                             "meshcored: the stored channels are unusable: %s", store_err);
            if (mcdstore::channelsQuarantine(cfg->state_dir, kept, sizeof(kept), q_err)) {
                mcport::logWrite(mcport::LOG_WARN,
                                 "meshcored: they have been kept as %s; starting with no "
                                 "channels. The keys cannot be recovered from the mesh and "
                                 "must be entered again.", kept);
                snprintf(rt->channel_fault, sizeof(rt->channel_fault), "%s (kept as %s)",
                         store_err, kept);
            } else {
                mcport::logWrite(mcport::LOG_ERROR,
                                 "meshcored: they could not be moved aside (%s); this run will "
                                 "not write channels, so the file is left for inspection", q_err);
                snprintf(rt->channel_fault, sizeof(rt->channel_fault),
                         "%s (left in place; channels are not being written)", store_err);
                rt->channels_blocked = true;
            }
            cs = mcdstore::ChannelState();
        }
        for (int i = 0; i < cs.count; i++) {
            const mcdstore::ChannelRecord& r = cs.channels[i];

            if (!rt->node.installChannel(r.slot, r.name, r.secret, r.key_len)) {
                mcport::logWrite(mcport::LOG_WARN,
                                 "meshcored: channel %s could not be restored into slot %d",
                                 r.name, r.slot);
            }
        }
        /* Restoring is not a change. Leaving the flag set would rewrite a
         * file identical to the one just read, on every start. */
        rt->node.clearChannelsDirty();
        ensurePublicChannel(rt);
    }

    mcport::logWrite(mcport::LOG_INFO, "meshcored: node %s, %d known node(s), %d channel(s)",
                     rt->node.name(), rt->node.getNumContacts(), rt->node.channelCount());
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
    rt->node.flushRxRaw();
    rt->node.noteHanded(rt->radio.handed());
    /* After the loop, and only once every frame already received has been
     * handed to the protocol core: the daemon hands over one frame a turn,
     * and an ACK that arrived in time but is queued behind others must be
     * matched before its message's deadline is judged. */
    if (!rt->radio.pending()) {
        uint64_t now = mcport::monotonicMillis();

        mcd_runtime_expire_acks(rt, now);
        /* The same rule for a repeater request's wait and a discovery
         * round's window: an answer queued behind other frames is taken
         * before the wait is called over. */
        mcd_runtime_remote_expire(rt, now);
        mcd_runtime_discover_expire(rt, now);
    }
}

int mcd_runtime_expire_acks(struct mcd_runtime* rt, uint64_t now_ms)
{
    return rt->node.expireAcks(now_ms);
}

int mcd_runtime_acks_waiting(const struct mcd_runtime* rt)
{
    return rt->node.outboxWaiting();
}

bool mcd_runtime_rx_pending(const struct mcd_runtime* rt)
{
    return rt->radio.pending();
}

bool mcd_runtime_deliver_rx(struct mcd_runtime* rt, const uint8_t* bytes, int len,
                            const struct mcd_rx_meta* meta)
{
    uint64_t dropped = rt->radio.dropped();

    if (rt->radio.push(bytes, len, *meta)) {
        return true;
    }
    /* Turned away before MeshCore: a full queue (counted as dropped), or a
     * length no frame can have. Both are receptions the log reports. */
    rt->node.observeTurnedAway(bytes, len, *meta,
                               rt->radio.dropped() != dropped ? MCD_RX_REJECT_QUEUE_FULL
                                                              : MCD_RX_REJECT_UNPARSED);
    return false;
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

enum mcd_name_source mcd_runtime_name_source(const struct mcd_runtime* rt)
{
    return rt->name_source;
}

enum mcd_rename_result mcd_runtime_set_name(struct mcd_runtime* rt, const char* name,
                                            bool* persisted)
{
    char clean[MCD_SANITIZED_SIZE(MCD_NODE_NAME_LEN)];
    size_t len;
    size_t i;
    bool blank = true;
    bool mark_written = true;
    bool state_written;

    if (persisted) {
        *persisted = false;
    }
    if (!name || !mcd_text_acceptable(name, MCD_NODE_NAME_LEN - 1)) {
        return MCD_RENAME_BAD_NAME;
    }
    len = strlen(name);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        /* A name is one line: no newline or tab, which a message may hold. */
        if (c == '\n' || c == '\t') {
            return MCD_RENAME_BAD_NAME;
        }
        if (c != ' ') {
            blank = false;
        }
    }
    /* Well-formed UTF-8 only: the name goes on the air in every advert and
     * into every channel message, and a peer shows what it receives. What
     * mcd_text_sanitize would change is what a peer could not show. */
    mcd_text_sanitize(name, clean, sizeof(clean));
    if (blank || strcmp(clean, name) != 0) {
        return MCD_RENAME_BAD_NAME;
    }
    /* A name configured on the command line (--name, which S65meshcored
     * passes from MESHCORED_NAME) replaces the stored one at a start. So
     * that this rename is not undone at the next one, settings.v1 records
     * which configured name it replaced - before the name itself is
     * written, so that a failure between the two leaves the old name on
     * both sides of a restart rather than a rename that silently goes. */
    if (rt->config_mark[0] && strcmp(rt->renamed_over, rt->config_mark) != 0) {
        char was[sizeof(rt->renamed_over)];

        memcpy(was, rt->renamed_over, sizeof(was));
        memcpy(rt->renamed_over, rt->config_mark, sizeof(rt->renamed_over));
        if (!saveSettings(rt)) {
            memcpy(rt->renamed_over, was, sizeof(was));
            mark_written = false;
        }
    }
    rt->node.rename(name);
    rt->name_source = MCD_NAME_STORED;
    /* Written now, as forgetting a node is: a rename answered and then lost
     * to a restart ten seconds later would make the answer untrue. */
    state_written = !rt->persist_blocked && mcd_runtime_persist(rt) == 0 && !rt->node.dirty();
    if (persisted) {
        *persisted = mark_written && state_written;
    }
    if (!mark_written || !state_written) {
        mcport::logWrite(mcport::LOG_WARN,
                         "meshcored: renamed for this run only; the name was not saved and "
                         "the old one returns at the next start");
    }
    return MCD_RENAME_OK;
}

int mcd_runtime_path_hash_bytes(const struct mcd_runtime* rt)
{
    return rt->node.pathHashBytes();
}

bool mcd_runtime_set_path_hash_bytes(struct mcd_runtime* rt, int bytes, bool* persisted)
{
    bool written;

    if (bytes < MCD_PATH_HASH_MIN || bytes > MCD_PATH_HASH_MAX) {
        return false;
    }
    rt->node.setPathHashBytes((uint8_t)bytes);
    written = saveSettings(rt);
    if (persisted) {
        *persisted = written;
    }
    return true;
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

/* One contact's place in mcd_runtime_nodes_recent's order. */
struct NodeRank {
    int slot;          /* MeshCore's table index, for the copy and the tie */
    bool heard;        /* heard during this run */
    uint64_t heard_ms;
    uint32_t lastmod;  /* MeshCore's, by our wall clock; survives a restart */
};

static int newestFirst(const void* pa, const void* pb)
{
    const NodeRank* a = (const NodeRank*)pa;
    const NodeRank* b = (const NodeRank*)pb;

    if (a->heard != b->heard) {
        return a->heard ? -1 : 1;
    }
    if (a->heard && a->heard_ms != b->heard_ms) {
        return a->heard_ms > b->heard_ms ? -1 : 1;
    }
    if (a->lastmod != b->lastmod) {
        return a->lastmod > b->lastmod ? -1 : 1;
    }
    return a->slot - b->slot;
}

int mcd_runtime_nodes_recent(const struct mcd_runtime* rt, struct mcd_node* out, int max)
{
    NodeRank rank[MAX_CONTACTS];
    Node& node = const_cast<Node&>(rt->node);
    ContactInfo c;
    int n = 0;

    if (out == NULL || max <= 0) {
        return 0;
    }
    /* From the first real slot, as ContactsIterator walks: the eight before
     * it are MeshCore's reserved anonymous ones. */
    for (int slot = MAX_ANON_CONTACTS; slot < node.getTotalContactSlots() && n < MAX_CONTACTS;
         slot++) {
        if (!node.getContactByIdx((uint32_t)slot, c) || c.type == ADV_TYPE_NONE) {
            continue;
        }
        const Node::Telemetry* t = node.telemetryFor(c.id.pub_key);

        rank[n].slot = slot;
        rank[n].heard = t && t->heard_known;
        rank[n].heard_ms = rank[n].heard ? t->heard_ms : 0;
        rank[n].lastmod = c.lastmod;
        n++;
    }
    qsort(rank, (size_t)n, sizeof(rank[0]), newestFirst);
    if (n > max) {
        n = max;
    }
    for (int i = 0; i < n; i++) {
        node.getContactByIdx((uint32_t)rank[i].slot, c);
        node.fill(c, out[i]);
    }
    return n;
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
    /* Before anything is built: a message this service could not watch for
     * its ACK would never be answered either way, so it is not sent. Any
     * message already past its deadline is answered first - requests are
     * served between ticks, and a slot that was due must not turn a send
     * away as busy. Not while received frames are still queued, for the
     * reason mcd_runtime_tick gives. */
    if (!rt->radio.pending()) {
        mcd_runtime_expire_acks(rt, mcport::monotonicMillis());
    }
    if (rt->node.outboxFull()) {
        return MCD_SEND_BUSY;
    }

    timestamp = rt->rtc.getCurrentTimeUnique();
    rc = rt->node.sendMessage(found, timestamp, 0, text, expected_ack, est);
    if (rc == MSG_SEND_FAILED) {
        return MCD_SEND_FAILED;
    }
    {
        mcd_msg_state st = (rc == MSG_SEND_SENT_DIRECT) ? MCD_MSG_SENT_DIRECT : MCD_MSG_SENT_FLOOD;
        uint64_t id = rt->node.recordOutgoing(found, text, timestamp, st, expected_ack, est);

        if (msg_id) {
            *msg_id = id;
        }
    }
    if (est_timeout_ms) {
        *est_timeout_ms = est;
    }
    return (rc == MSG_SEND_SENT_DIRECT) ? MCD_SEND_ACCEPTED_DIRECT : MCD_SEND_ACCEPTED_FLOOD;
}

enum mcd_send_result mcd_runtime_resend(struct mcd_runtime* rt, uint64_t msg_id,
                                        uint32_t* est_timeout_ms)
{
    uint32_t est = 0;
    int rc;

    if (!rt->radio.online()) {
        return MCD_SEND_NO_RADIO;
    }
    /* As a first send: anything past its deadline is answered first, which
     * is also what lets a message that has just run out be resent at once. */
    if (!rt->radio.pending()) {
        mcd_runtime_expire_acks(rt, mcport::monotonicMillis());
    }
    if (rt->node.outboxFull()) {
        return MCD_SEND_BUSY;
    }
    rc = rt->node.resend(msg_id, est);
    if (rc == -1) {
        return MCD_SEND_NOT_RESENDABLE;
    }
    if (rc == -2) {
        return MCD_SEND_NO_CONTACT;
    }
    if (rc < 0) {
        return MCD_SEND_FAILED;
    }
    if (est_timeout_ms) {
        *est_timeout_ms = est;
    }
    return (rc == MSG_SEND_SENT_DIRECT) ? MCD_SEND_ACCEPTED_DIRECT : MCD_SEND_ACCEPTED_FLOOD;
}

int mcd_runtime_channel_count(const struct mcd_runtime* rt)
{
    return rt->node.channelCount();
}

bool mcd_runtime_channel_at(const struct mcd_runtime* rt, int idx, struct mcd_channel* c)
{
    return rt->node.channelAt(idx, *c);
}

bool mcd_runtime_channel_by_slot(const struct mcd_runtime* rt, int slot, struct mcd_channel* c)
{
    return rt->node.channelBySlot(slot, *c);
}

enum mcd_channel_result mcd_runtime_channel_add(struct mcd_runtime* rt, const char* name,
                                                const char* psk_base64, struct mcd_channel* out)
{
    uint8_t key[PUB_KEY_SIZE];
    size_t klen;
    int slot;
    bool all_zero = true;

    if (name == NULL || name[0] == '\0' || strlen(name) >= MCD_CHANNEL_NAME_LEN) {
        return MCD_CHANNEL_BAD_NAME;
    }
    if (psk_base64 == NULL) {
        return MCD_CHANNEL_BAD_KEY;
    }
    memset(key, 0, sizeof(key));
    klen = mcport::base64Decode((const unsigned char*)psk_base64, strlen(psk_base64), key,
                                sizeof(key));
    if (klen != 16 && klen != 32) {
        return MCD_CHANNEL_BAD_KEY;
    }
    for (size_t i = 0; i < klen; i++) {
        if (key[i] != 0) {
            all_zero = false;
        }
    }
    /* An all-zero key is what an unused MeshCore slot holds. Installing one
     * would put a real channel where the receive-path guard expects nothing,
     * and its key is public by construction. */
    if (all_zero) {
        return MCD_CHANNEL_BAD_KEY;
    }
    if (klen == 32) {
        bool upper_zero = true;

        for (int i = 16; i < 32; i++) {
            if (key[i] != 0) {
                upper_zero = false;
            }
        }
        /* MeshCore's setChannel() reads a 32-byte key whose upper half is
         * zero as a 128-bit key and hashes it over 16 bytes, while its
         * addChannel() would use the decoded length and hash it over 32. The
         * same key therefore derives two different channel hashes depending
         * on which path a peer took, and this node would sit on a channel
         * some of its peers cannot route to it on. The symptom would be
         * silence, so it is refused with a reason instead. */
        if (upper_zero) {
            return MCD_CHANNEL_AMBIGUOUS_KEY;
        }
    }
    if (rt->node.holdsKey(key, (int)klen)) {
        return MCD_CHANNEL_DUPLICATE;
    }
    slot = rt->node.freeChannelSlot();
    if (slot < 0) {
        return MCD_CHANNEL_FULL;
    }
    if (!rt->node.installChannel(slot, name, key, (int)klen)) {
        return MCD_CHANNEL_FAILED;
    }
    /* Written now, not on the daemon's ten-second persist timer.
     *
     * A channel key is the one thing this service holds that nothing can give
     * back. The node table is a cache the mesh refills, so a power cut inside
     * its write window costs a rediscovery; a key inside this window is gone,
     * and the operator typed it by hand and may not have it any more. Joining
     * and leaving are operator actions and happen a handful of times in a
     * node's life, so writing on each one costs nothing the timer was
     * protecting against - unlike the node table, which changes on every
     * advert and would churn the flash if it were written the same way.
     *
     * A failed write does not fail the join: the channel is installed and
     * usable either way, and persistChannels has already logged it. */
    (void)persistChannels(rt);
    if (out && !rt->node.channelBySlot(slot, *out)) {
        return MCD_CHANNEL_FAILED;
    }
    return MCD_CHANNEL_OK;
}

enum mcd_channel_result mcd_runtime_channel_remove(struct mcd_runtime* rt, int slot)
{
    mcd_channel c;

    /* The standard Public channel is mandatory (mesh_runtime.h). */
    if (rt->node.channelBySlot(slot, c) && c.is_public) {
        return MCD_CHANNEL_MANDATORY;
    }
    if (!rt->node.removeChannel(slot)) {
        return MCD_CHANNEL_NOT_FOUND;
    }
    /* And forgotten now, for the mirror of the reason above: mesh.channel_remove
     * answers `key_forgotten`, and a key still on the disk ten seconds after
     * that answer would make it untrue. */
    (void)persistChannels(rt);
    return MCD_CHANNEL_OK;
}

enum mcd_send_result mcd_runtime_send_channel_text(struct mcd_runtime* rt, int slot,
                                                   const char* text, uint64_t* msg_id)
{
    if (!rt->radio.online()) {
        return MCD_SEND_NO_RADIO;
    }
    return rt->node.sendChannelText(slot, text, msg_id);
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
    /* With the configured path hash size, as every other flood this node
     * starts (Node::sendFloodScoped). */
    rt->node.sendFlood(pkt, 0, rt->node.pathHashBytes());
    return true;
}

bool mcd_runtime_send_advert_zero_hop(struct mcd_runtime* rt)
{
    mesh::Packet* pkt;

    if (!rt->radio.online()) {
        return false;
    }
    pkt = rt->node.createSelfAdvert(rt->node.name());
    if (pkt == NULL) {
        return false;
    }
    /* Heard by the nodes in direct range and repeated by none of them: the
     * same signed advert, at the airtime cost of one packet rather than of a
     * flood across the whole mesh. */
    rt->node.sendZeroHop(pkt);
    return true;
}

/* ---- repeater discovery and control ------------------------------------- */

const char* mcd_remote_kind_name(enum mcd_remote_kind k)
{
    switch (k) {
    case MCD_REMOTE_LOGIN:
        return "login";
    case MCD_REMOTE_STATUS:
        return "status";
    case MCD_REMOTE_NEIGHBOURS:
        return "neighbours";
    case MCD_REMOTE_OWNER:
        return "owner";
    case MCD_REMOTE_CLI:
        return "cli";
    case MCD_REMOTE_NONE:
    default:
        return "none";
    }
}

const char* mcd_login_state_name(enum mcd_login_state s)
{
    switch (s) {
    case MCD_LOGIN_WAITING:
        return "waiting";
    case MCD_LOGIN_OK:
        return "ok";
    case MCD_LOGIN_REFUSED:
        return "refused";
    case MCD_LOGIN_TIMEOUT:
        return "timeout";
    case MCD_LOGIN_NONE:
    default:
        return "none";
    }
}

const char* mcd_remote_outcome_name(enum mcd_remote_outcome o)
{
    switch (o) {
    case MCD_REMOTE_REPLIED:
        return "replied";
    case MCD_REMOTE_REFUSED:
        return "refused";
    case MCD_REMOTE_TIMED_OUT:
        return "timeout";
    case MCD_REMOTE_CANCELLED:
    default:
        return "cancelled";
    }
}

enum mcd_discover_result mcd_runtime_discover(struct mcd_runtime* rt,
                                              struct mcd_discover_state* state)
{
    uint64_t now = mcport::monotonicMillis();
    uint8_t data[10];
    uint32_t tag = 0;
    uint32_t since = 0;
    mesh::Packet* pkt;

    (void)mcd_runtime_discover_expire(rt, now);
    if (rt->node.discovery().open(now)) {
        /* Never re-armed: a new tag would orphan every answer the open round
         * is still collecting. */
        rt->node.discovery().state(state);
        return MCD_DISCOVER_BUSY;
    }
    if (!rt->radio.online()) {
        rt->node.discovery().state(state);
        return MCD_DISCOVER_NO_RADIO;
    }
    /* Byte for byte the request simple_repeater answers and upstream RIFT's
     * DISCOVER 0-HOP sends: prefix_only 0 (whole keys back), a repeater-only
     * type filter, a random tag that is never 0, and no "changed since". */
    rt->rng.random((uint8_t*)&tag, sizeof(tag));
    if (tag == 0) {
        tag = 1;
    }
    data[0] = mcdremote::CTL_DISCOVER_REQ;
    data[1] = (uint8_t)(1u << ADV_TYPE_REPEATER);
    memcpy(&data[2], &tag, 4);
    memcpy(&data[6], &since, 4);
    pkt = rt->node.createControlData(data, sizeof(data));
    if (pkt == NULL) {
        rt->node.discovery().state(state);
        return MCD_DISCOVER_FAILED;
    }
    rt->node.discovery().begin(tag, now);
    rt->node.sendZeroHop(pkt);
    rt->node.discovery().state(state);
    return MCD_DISCOVER_STARTED;
}

void mcd_runtime_discover_state(const struct mcd_runtime* rt, struct mcd_discover_state* out)
{
    rt->node.discovery().state(out);
}

int mcd_runtime_discovered(const struct mcd_runtime* rt, struct mcd_discovered* out, int max)
{
    return rt->node.discovery().list(out, max);
}

bool mcd_runtime_discover_expire(struct mcd_runtime* rt, uint64_t now_ms)
{
    if (!rt->node.discovery().expire(now_ms)) {
        return false;
    }
    rt->node.emitDiscoverClosed();
    return true;
}

/* Overwrite a buffer that held a password in a way the compiler may not
 * drop as a dead store. */
static void wipe(void* p, size_t n)
{
    volatile uint8_t* v = (volatile uint8_t*)p;

    while (n--) {
        *v++ = 0;
    }
}

/* One line of printable text: no control character, no newline. A login
 * password must not start with a byte below a space either way - upstream's
 * repeater reads such a first byte as another anonymous request type. */
static bool oneLine(const char* s, size_t max, bool allow_empty)
{
    size_t n;

    if (s == NULL) {
        return false;
    }
    n = strnlen(s, max + 1);
    if (n > max || (n == 0 && !allow_empty)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];

        if (ch < 0x20 || ch == 0x7F) {
            return false;
        }
    }
    return true;
}

static ContactInfo* contactFor(struct mcd_runtime* rt, const uint8_t key[MCD_PUB_KEY_LEN])
{
    ContactInfo* c = rt->node.lookupContactByPubKey(key, PUB_KEY_SIZE);

    return (c != NULL && c->type != ADV_TYPE_NONE) ? c : NULL;
}

static enum mcd_remote_result accepted(int rc)
{
    return rc == MSG_SEND_SENT_DIRECT ? MCD_REMOTE_ACCEPTED_DIRECT : MCD_REMOTE_ACCEPTED_FLOOD;
}

enum mcd_remote_result mcd_runtime_remote_login(struct mcd_runtime* rt,
                                                const uint8_t key[MCD_PUB_KEY_LEN],
                                                const char* password, uint64_t* request_id,
                                                uint32_t* wait_ms)
{
    char pw[MCD_REMOTE_PASSWORD_MAX + 1];
    uint64_t now = mcport::monotonicMillis();
    mcd_remote_reply cancelled;
    ContactInfo* c;
    uint32_t est = 0;
    uint32_t wait;
    int rc;

    if (!rt->radio.online()) {
        return MCD_REMOTE_NO_RADIO;
    }
    /* Empty is upstream's own case: a blank password logs in only a node the
     * repeater already holds in its access list. Longer than upstream's cut
     * is refused rather than sent shortened - a password somebody typed and
     * this node silently changed would fail for a reason nobody could see. */
    if (!oneLine(password, MCD_REMOTE_PASSWORD_MAX, true)) {
        return MCD_REMOTE_BAD_TEXT;
    }
    c = contactFor(rt, key);
    if (c == NULL) {
        return MCD_REMOTE_NO_CONTACT;
    }
    if (rt->node.remote().busy()) {
        return MCD_REMOTE_BUSY;
    }
    if (rt->node.remote().retarget(key, &cancelled, now)) {
        rt->node.emitRemote(cancelled);
    }
    snprintf(pw, sizeof(pw), "%s", password);
    rc = rt->node.sendLogin(*c, pw, est);
    wipe(pw, sizeof(pw));
    if (rc == MSG_SEND_FAILED) {
        return MCD_REMOTE_FAILED;
    }
    wait = mcdremote::waitFor(est);
    if (request_id) {
        *request_id = rt->node.remote().begin(MCD_REMOTE_LOGIN, 0, wait, now);
    } else {
        (void)rt->node.remote().begin(MCD_REMOTE_LOGIN, 0, wait, now);
    }
    if (wait_ms) {
        *wait_ms = wait;
    }
    return accepted(rc);
}

/* The checks every request after a login shares. */
static enum mcd_remote_result readyFor(struct mcd_runtime* rt, const uint8_t key[MCD_PUB_KEY_LEN],
                                       ContactInfo** c)
{
    if (!rt->radio.online()) {
        return MCD_REMOTE_NO_RADIO;
    }
    *c = contactFor(rt, key);
    if (*c == NULL) {
        return MCD_REMOTE_NO_CONTACT;
    }
    if (!rt->node.remote().loggedIn(key)) {
        return MCD_REMOTE_NOT_LOGGED_IN;
    }
    if (rt->node.remote().busy()) {
        return MCD_REMOTE_BUSY;
    }
    return MCD_REMOTE_ACCEPTED_FLOOD;
}

enum mcd_remote_result mcd_runtime_remote_ask(struct mcd_runtime* rt,
                                              const uint8_t key[MCD_PUB_KEY_LEN],
                                              enum mcd_remote_kind kind, uint64_t* request_id,
                                              uint32_t* wait_ms)
{
    uint64_t now = mcport::monotonicMillis();
    enum mcd_remote_result ready;
    ContactInfo* c = NULL;
    uint32_t tag = 0;
    uint32_t est = 0;
    uint32_t wait;
    uint64_t id;
    int rc;

    if (kind != MCD_REMOTE_STATUS && kind != MCD_REMOTE_NEIGHBOURS && kind != MCD_REMOTE_OWNER) {
        return MCD_REMOTE_BAD_TEXT;
    }
    ready = readyFor(rt, key, &c);
    if (ready != MCD_REMOTE_ACCEPTED_FLOOD) {
        return ready;
    }
    if (kind == MCD_REMOTE_STATUS) {
        /* Upstream's own 13-byte status request: tag, type, four reserved
         * bytes, four random ones. */
        rc = rt->node.sendRequest(*c, mcdremote::REQ_GET_STATUS, tag, est);
    } else if (kind == MCD_REMOTE_NEIGHBOURS) {
        /* Version 0: how many, from where, newest first, how much of each
         * key, and four random bytes for the packet hash. */
        uint8_t req[11];

        req[0] = mcdremote::REQ_GET_NEIGHBOURS;
        req[1] = 0;
        req[2] = MCD_REMOTE_NEIGHBOURS_MAX;
        req[3] = 0;
        req[4] = 0;
        req[5] = 0;
        req[6] = MCD_REMOTE_NEIGHBOUR_PREFIX;
        rt->rng.random(&req[7], 4);
        rc = rt->node.sendRequest(*c, req, sizeof(req), tag, est);
    } else {
        uint8_t req[1] = { mcdremote::REQ_GET_OWNER_INFO };

        rc = rt->node.sendRequest(*c, req, sizeof(req), tag, est);
    }
    if (rc == MSG_SEND_FAILED) {
        return MCD_REMOTE_FAILED;
    }
    wait = mcdremote::waitFor(est);
    id = rt->node.remote().begin(kind, tag, wait, now);
    if (request_id) {
        *request_id = id;
    }
    if (wait_ms) {
        *wait_ms = wait;
    }
    return accepted(rc);
}

enum mcd_remote_result mcd_runtime_remote_cli(struct mcd_runtime* rt,
                                              const uint8_t key[MCD_PUB_KEY_LEN],
                                              const char* command, uint64_t* request_id,
                                              uint32_t* wait_ms)
{
    uint64_t now = mcport::monotonicMillis();
    enum mcd_remote_result ready;
    ContactInfo* c = NULL;
    uint32_t est = 0;
    uint32_t wait;
    uint64_t id;
    int rc;

    if (!oneLine(command, MAX_TEXT_LEN, false)) {
        return MCD_REMOTE_BAD_TEXT;
    }
    ready = readyFor(rt, key, &c);
    if (ready != MCD_REMOTE_ACCEPTED_FLOOD) {
        return ready;
    }
    /* A guest's commands are dropped by the repeater without an answer
     * (simple_repeater onPeerDataRecv: client->isAdmin()), so sending one
     * would only ever end as a timeout. */
    if (rt->node.remote().knownGuest()) {
        return MCD_REMOTE_NOT_ADMIN;
    }
    /* This node's own unique clock, as upstream RIFT sends it: the repeater
     * refuses a timestamp older than the last it saw from us. */
    rc = rt->node.sendCommandData(*c, rt->rtc.getCurrentTimeUnique(), 0, command, est);
    if (rc == MSG_SEND_FAILED) {
        return MCD_REMOTE_FAILED;
    }
    wait = mcdremote::waitFor(est);
    id = rt->node.remote().begin(MCD_REMOTE_CLI, 0, wait, now);
    if (request_id) {
        *request_id = id;
    }
    if (wait_ms) {
        *wait_ms = wait;
    }
    return accepted(rc);
}

bool mcd_runtime_remote_logout(struct mcd_runtime* rt, const uint8_t* key)
{
    mcd_remote_session s;
    mcd_remote_reply cancelled;

    rt->node.remote().state(&s);
    if (!s.active || (key != NULL && !rt->node.remote().isTarget(key))) {
        return false;
    }
    /* Upstream's logout, and all of it: keep-alive state dropped, nothing
     * transmitted. */
    rt->node.dropConnection(s.key);
    if (rt->node.remote().end(&cancelled, mcport::monotonicMillis())) {
        rt->node.emitRemote(cancelled);
    }
    return true;
}

void mcd_runtime_remote_session(const struct mcd_runtime* rt, struct mcd_remote_session* out)
{
    rt->node.remote().state(out);
}

bool mcd_runtime_remote_expire(struct mcd_runtime* rt, uint64_t now_ms)
{
    mcd_remote_reply r;

    if (!rt->node.remote().expire(now_ms, &r)) {
        return false;
    }
    rt->node.emitRemote(r);
    return true;
}

bool mcd_runtime_node_remove(struct mcd_runtime* rt, const uint8_t key[MCD_PUB_KEY_LEN],
                             struct mcd_node* was, bool* persisted)
{
    struct mcd_node n;
    bool written;

    if (!rt->node.forgetNode(key, n)) {
        return false;
    }
    if (was) {
        *was = n;
    }
    /* Written now rather than at the next persist interval: a node answered
     * as forgotten that came back after a restart ten seconds later would
     * make the answer untrue. The same reasoning as a channel's key. And
     * whether it was written is part of the answer: a table that cannot be
     * written (a failed write, or an unreadable state.v1 kept as evidence)
     * forgets the node for this run only. */
    written = !rt->persist_blocked && mcd_runtime_persist(rt) == 0 && !rt->node.dirty();
    if (persisted) {
        *persisted = written;
    }
    return true;
}

bool mcd_runtime_node_reset_path(struct mcd_runtime* rt, const uint8_t key[MCD_PUB_KEY_LEN],
                                 struct mcd_node* now)
{
    struct mcd_node n;

    if (!rt->node.resetPath(key, n)) {
        return false;
    }
    if (now) {
        *now = n;
    }
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
    out->nodes_unretained = rt->node.unretained();
    out->contacts_full = rt->node.contactsFull();
    out->channels = rt->node.channelCount();
    out->channel_frames_unmatched = rt->node.channelFramesUnmatched();
    out->app_rx = rt->node.appRx();
    out->app_tx = rt->node.appTx();
    out->app_receipts = rt->node.appReceipts();
}

enum mcd_send_result mcd_runtime_send_app(struct mcd_runtime* rt, const uint8_t key[MCD_PUB_KEY_LEN],
                                          int port, const uint8_t* payload, size_t len,
                                          uint32_t* est_timeout_ms)
{
    uint8_t req[2 + MCD_APP_PAYLOAD_MAX];
    ContactInfo* c;
    uint32_t tag = 0;
    uint32_t est = 0;
    int rc;

    if (!rt->radio.online()) {
        return MCD_SEND_NO_RADIO;
    }
    if (port < MCD_APP_PORT_MIN || port > MCD_APP_PORT_MAX || !payload || len == 0 ||
        len > MCD_APP_PAYLOAD_MAX) {
        return MCD_SEND_TOO_LONG;
    }
    c = rt->node.lookupContactByPubKey(key, PUB_KEY_SIZE);
    if (!c || c->type == ADV_TYPE_NONE) {
        return MCD_SEND_NO_CONTACT;
    }
    req[0] = (uint8_t)(MCD_APP_MARKER | port);
    req[1] = (uint8_t)len;
    memcpy(req + 2, payload, len);
    rc = rt->node.sendRequest(*c, req, (uint8_t)(2 + len), tag, est);
    if (rc == MSG_SEND_FAILED) {
        return MCD_SEND_FAILED;
    }
    rt->node.noteAppTx();
    if (est_timeout_ms) {
        *est_timeout_ms = est;
    }
    return rc == MSG_SEND_SENT_DIRECT ? MCD_SEND_ACCEPTED_DIRECT : MCD_SEND_ACCEPTED_FLOOD;
}

int mcd_runtime_app_inbox(const struct mcd_runtime* rt, int port, uint64_t after_id,
                          struct mcd_app_datagram* out, int max)
{
    if (!rt || !out || max <= 0 || port < MCD_APP_PORT_MIN || port > MCD_APP_PORT_MAX) {
        return 0;
    }
    return rt->node.appInbox(port, after_id, out, max);
}

bool mcd_runtime_dirty(const struct mcd_runtime* rt)
{
    return rt->node.dirty() || rt->node.channelsDirty();
}

bool mcd_runtime_state_fault(const struct mcd_runtime* rt, char* buf, size_t buf_len)
{
    snprintf(buf, buf_len, "%s", rt->state_fault);
    return rt->state_fault[0] != '\0';
}

bool mcd_runtime_channel_fault(const struct mcd_runtime* rt, char* buf, size_t buf_len)
{
    snprintf(buf, buf_len, "%s", rt->channel_fault);
    return rt->channel_fault[0] != '\0';
}

/* Write the channel table out. Separate from the node table because the two
 * files are separate, and because one of them failing must not stop the
 * other: a node that cannot write its contacts should still not forget the
 * channel somebody just joined. */
static int persistChannels(struct mcd_runtime* rt)
{
    char err[mcdstore::ERR_SIZE] = "";
    mcdstore::ChannelState cs;

    if (rt->channels_blocked) {
        rt->node.clearChannelsDirty();
        return 0;
    }
    cs = mcdstore::ChannelState();
    for (int slot = 0; slot < MAX_GROUP_CHANNELS; slot++) {
        mcd_channel c;
        mcdstore::ChannelRecord& r = cs.channels[cs.count];
        int key_len = 0;

        if (!rt->node.channelBySlot(slot, c)) {
            continue;
        }
        r = mcdstore::ChannelRecord();
        if (!rt->node.channelKeyAt(slot, r.secret, &key_len)) {
            continue;
        }
        r.slot = slot;
        r.key_len = key_len;
        snprintf(r.name, sizeof(r.name), "%s", c.name);
        cs.count++;
    }
    if (!mcdstore::channelsSave(cs, rt->state_dir, err)) {
        mcport::logWrite(mcport::LOG_ERROR, "meshcored: %s", err);
        return -1;
    }
    rt->node.clearChannelsDirty();
    return 0;
}

int mcd_runtime_persist(struct mcd_runtime* rt)
{
    char err[mcdstore::ERR_SIZE] = "";
    mcdstore::NodeState st;
    ContactInfo c;
    ContactsIterator it = rt->node.startContactsIterator();
    int rc = 0;

    if (rt->node.channelsDirty()) {
        rc = persistChannels(rt);
    }

    if (rt->persist_blocked) {
        /* An unusable state.v1 that could not be moved aside is still there,
         * and it is the only record of whatever went wrong. Writing a fresh
         * table over it would destroy that before anybody had looked. The
         * node runs perfectly well without persisting; it simply starts empty
         * again next time, which is the cheaper of the two losses. */
        rt->node.clearDirty();
        return rc;
    }
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
    return rc;
}

}  /* extern "C" */
