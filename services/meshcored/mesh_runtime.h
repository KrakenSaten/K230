/*
 * The one door between meshcored's C daemon and the C++ MeshCore runtime.
 *
 * Everything above this header speaks pocketipc JSON and knows nothing about
 * MeshCore. Everything below it is the protocol core from
 * protocols/meshcore - mesh::Mesh, BaseChatMesh, the packet pool, the
 * dispatcher, real Ed25519 and real AES - and knows nothing about JSON,
 * sockets or radiod. The structs here are plain C by-value views; no MeshCore
 * object, pointer or buffer crosses the line.
 *
 * The seam exists for two reasons. The obvious one is language: the protocol
 * core is C++ and the service conventions (pocketipc, pocketlog, cJSON) are
 * C. The one that matters more is the boundary protocols/meshcore's README
 * asks for - "Do not make MeshCore protocol code know IPC JSON" - and
 * tests/meshcored_lint.sh checks it by inspection: no cJSON or pocketipc
 * include below this line, no MeshCore include above it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef MCD_MESH_RUNTIME_H
#define MCD_MESH_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MeshCore's own sizes, repeated here so this header stands alone. Checked
 * against the vendored values by a static assertion in mesh_runtime.cpp, so
 * they cannot drift apart silently. */
#define MCD_PUB_KEY_LEN 32
#define MCD_MAX_PATH 64
#define MCD_MAX_TEXT 160
#define MCD_MAX_FRAME 255
#define MCD_NODE_NAME_LEN 32
/* MeshCore's MAX_GROUP_CHANNELS and ChannelDetails::name. Static-asserted
 * against the vendored values in mesh_runtime.cpp, like the five above. */
#define MCD_MAX_CHANNELS 8
#define MCD_CHANNEL_NAME_LEN 32
/* MeshCore's MAX_CONTACTS (protocols/meshcore/compat/mc_contacts.h): the
 * most nodes the table holds and mesh.nodes can list. Static-asserted like
 * the rest. */
#define MCD_MAX_NODES 1000

/* ---- what came off the air --------------------------------------------
 *
 * radiod's radio.rx carries RSSI, SNR and frequency error as plain numbers,
 * and a future backend - or a malformed event - may not carry them at all.
 * Each value therefore travels with its own known flag, and an unknown one is
 * absent from everything meshcored reports. "Unknown" and "zero" are
 * different answers, and a UI drawing a signal bar from a fabricated 0 would
 * be showing a measurement nobody made (docs/api/radio.md, radio.channel).
 */
struct mcd_rx_meta {
    uint64_t mono_ms;        /* CLOCK_MONOTONIC, always present (radiod stamps it) */
    bool rssi_known;
    double rssi_dbm;
    bool snr_known;
    double snr_db;
    bool freq_error_known;
    double freq_error_hz;
};

/* ---- one reception, as the receive log sees it --------------------------
 *
 * The receive log (docs/api/mesh.md, "The receive log") reports every frame
 * this node took off the air, once per reception, duplicates included. It is
 * observational: it is filled from the dispatcher's own logging hooks and a
 * wrapper round MeshCore's packet handler, and nothing in it changes what
 * MeshCore does with the frame - the deduplication, the routing and the
 * decoding are exactly what they were.
 *
 * The packet is read when the dispatcher logs it, which is before MeshCore
 * has looked at it at all: before its seen-table, before a flood waits out
 * its receive delay, and before a relay rewrites its path. What MeshCore then
 * made of it - the verdict, the relay, what it could read - is filled in when
 * the handler returns, and the observation is reported then. A flood frame
 * MeshCore holds back for its receive delay is therefore reported up to that
 * delay late; seq and the receive time are the reception's own.
 */
#define MCD_RX_HASH_LEN 8   /* MeshCore's MAX_HASH_SIZE: the packet hash */

enum mcd_rx_verdict {
    MCD_RX_NEW = 0,       /* MeshCore handled it: it was not in its seen-table */
    MCD_RX_DUPLICATE,     /* MeshCore's seen-table matched it and it went no further */
    MCD_RX_REJECTED,      /* it never reached MeshCore's handler (mcd_rx_reject says why) */
    MCD_RX_UNRESOLVED,    /* parsed, but the log lost track of it before MeshCore was done */
};

enum mcd_rx_reject {
    MCD_RX_REJECT_NONE = 0,
    MCD_RX_REJECT_QUEUE_FULL,  /* the receive queue was full; dropped before MeshCore */
    MCD_RX_REJECT_UNPARSED,    /* the dispatcher's own parse refused the bytes */
    MCD_RX_REJECT_NO_BUFFER,   /* the packet pool was empty when it arrived */
};

/* What MeshCore could read of it. Only what it decoded for its own use is
 * reported: nothing is decrypted for the log. */
enum mcd_rx_decode {
    MCD_RX_DECODE_NONE = 0,    /* nothing read: not for us, no key, a duplicate, or no content */
    MCD_RX_DECODE_DIRECT,      /* a direct text message to this node */
    MCD_RX_DECODE_CHANNEL,     /* a text message on a channel this node holds */
    MCD_RX_DECODE_ADVERT,      /* an advert from a node the table holds */
};

struct mcd_rx_obs {
    uint64_t seq;                 /* 1 upwards, per run, in the order frames were received */
    struct mcd_rx_meta meta;      /* the reception's own time and signal */
    int bytes;                    /* the frame's length on the air */
    bool parsed;                  /* false: only bytes, meta, verdict and reject are meaningful */
    uint8_t header;               /* the raw header byte (also set for an unparsed frame of 1+ byte) */
    bool header_known;
    uint8_t payload_type;         /* MeshCore's PAYLOAD_TYPE_*, 0..15 */
    uint8_t route_type;           /* MeshCore's ROUTE_TYPE_*, 0..3 */
    uint8_t payload_ver;
    /* The path as received. For every type but TRACE it is hops of
     * path_hash_size bytes each: on a flood the relays it came through, first
     * one first; on a direct packet the hops it still has to take. A TRACE
     * carries the SNR each hop measured instead (path_is_snr), one byte each,
     * a quarter dB. */
    bool path_is_snr;
    uint8_t path_hash_size;       /* 1..3; 1 for TRACE */
    uint8_t path_hops;
    uint8_t path_bytes;
    uint8_t path[MCD_MAX_PATH];
    bool hash_known;
    uint8_t hash[MCD_RX_HASH_LEN];
    /* The one-byte hashes the payload is addressed with, where its type has
     * them: a group frame's channel, a peer packet's destination and source,
     * an advert's sender (the first byte of its key). */
    bool has_channel_hash;
    uint8_t channel_hash;
    bool channel_known;           /* this node holds a channel with that hash */
    bool has_dest_hash;
    uint8_t dest_hash;
    bool for_us;                  /* the destination hash is this node's */
    bool has_src_hash;
    uint8_t src_hash;
    /* A CONTROL payload's first byte: its control type in the high nibble
     * (0x80 a node-discovery request, 0x90 its response, upstream's
     * CTL_TYPE_NODE_DISCOVER_*) and its flags in the low one. */
    bool has_control_flags;
    uint8_t control_flags;
    /* This node transmitted this very packet - sent it, or relayed it - and
     * is hearing it again from somebody who repeated it. */
    bool own;
    /* How many times this receive log has seen this packet hash, this one
     * included, among the last MCD_RX_SEEN_HASHES different hashes: 1 for
     * the first reception. Counted by the log, not by MeshCore. */
    uint32_t dup;
    enum mcd_rx_verdict verdict;
    enum mcd_rx_reject reject;
    bool relayed;                 /* MeshCore queued it to be transmitted again */
    enum mcd_rx_decode decode;
    char sender[MCD_NODE_NAME_LEN];       /* DIRECT: the contact; CHANNEL: the claimed name; ADVERT: the node */
    bool has_sender_key;                  /* DIRECT and ADVERT: who, by key (a channel names nobody) */
    uint8_t sender_key[MCD_PUB_KEY_LEN];
    char recipient[MCD_NODE_NAME_LEN];    /* DIRECT: this node's name */
    char channel_name[MCD_CHANNEL_NAME_LEN]; /* CHANNEL: our local name for it */
    char text[MCD_MAX_TEXT + 1];          /* DIRECT/CHANNEL: the body, without the sender prefix */
};
/* The distinct packet hashes the receive log remembers for counting
 * repeats. MeshCore's own seen-table holds 160. */
#define MCD_RX_SEEN_HASHES 256

/* ---- how a transmit ended ---------------------------------------------
 *
 * The four outcomes radiod distinguishes, kept apart on purpose. A daemon
 * asking "must I send this again?" reads whether the bytes went out, and
 * collapsing a failed receive re-entry into a failed transmit would answer
 * "yes" for a packet that was transmitted perfectly well - twice the airtime,
 * for a fault in the receive path (docs/api/radio.md, radio.send_async).
 */
enum mcd_tx_outcome {
    MCD_TX_OK = 0,            /* transmitted, and the radio is receiving again */
    MCD_TX_RX_RESUME_FAILED,  /* transmitted, but the radio is not receiving */
    MCD_TX_FAILED,            /* not transmitted */
    MCD_TX_UNKNOWN            /* the connection went away; the outcome cannot be known */
};

const char *mcd_tx_outcome_name(enum mcd_tx_outcome o);

/* ---- a node ------------------------------------------------------------ */

struct mcd_node {
    uint8_t public_key[MCD_PUB_KEY_LEN];
    char name[MCD_NODE_NAME_LEN];
    uint8_t type;                 /* MeshCore ADV_TYPE_*: 1 chat, 2 repeater, ... */
    bool path_known;
    uint8_t path_len;             /* MeshCore's packed path_len byte */
    uint8_t path_hops;            /* hop count decoded from it */
    uint8_t path[MCD_MAX_PATH];
    uint8_t path_bytes;
    uint32_t last_advert_timestamp; /* by THEIR clock, 0 when never seen */
    bool last_heard_known;
    uint64_t last_heard_mono_ms;  /* by ours */
    bool last_snr_known;
    double last_snr_db;
    bool last_rssi_known;
    double last_rssi_dbm;
    /* How many relays the node's last advert passed through on its way
     * here: MeshCore's own hop count of the advert packet's path, 0 when it
     * was heard with nothing in between (a zero-hop advert, or a flood heard
     * before any repeater took it up). Observed this run only, like the
     * signal, and only when an advert from the node was heard. Not the
     * route back (path_*), which is learned separately and may differ. */
    bool advert_hops_known;
    uint8_t advert_hops;
    uint64_t advert_mono_ms;      /* when that advert was heard, by ours */
    /* Where the node says it is: the latitude and longitude its adverts
     * carried (MeshCore ADV_LATLON_MASK, degrees x 1e6), as MeshCore keeps
     * them in the contact and state.v1 stores them. Known only when an
     * advert carried one inside the valid range: MeshCore keeps 0,0 for "no
     * location ever", so exactly 0,0 is not a location. Claimed by the node,
     * never measured here. */
    bool location_known;
    int32_t lat_e6;
    int32_t lon_e6;
};

/* ---- a channel ---------------------------------------------------------
 *
 * A MeshCore group channel is a pre-shared key and nothing else. Two nodes
 * are on the same channel when they hold the same bytes; the name is local
 * and never goes on the air; and the one-byte hash the frame carries is
 * derived from the key, so it is a routing hint rather than an identity -
 * collisions are ordinary and the MAC is what decides
 * (protocols/meshcore/README.md, "Group channels").
 *
 * The KEY IS NOT IN THIS STRUCT, and there is no accessor for it anywhere
 * above the runtime. It is written to channels.v1 at 0600 and read back, and
 * that is the whole of its travel. A client names a channel by its slot.
 *
 * The slot is the identity a client uses, and it is stable: removing a
 * channel empties its slot rather than compacting the table, so no channel
 * ever changes slot while it exists. A slot that has been emptied may later
 * be taken by a different channel, which is why the hash and the name travel
 * with it and a client re-reads on a mesh.channel event rather than assuming
 * a cached slot still means what it did.
 */
struct mcd_channel {
    int slot;
    char name[MCD_CHANNEL_NAME_LEN];
    uint8_t hash;     /* SHA-256(key)[0] - what MeshCore puts on the air */
    int key_bits;     /* 128 or 256 */
    /* The key is MeshCore's well-known Public channel key (upstream
     * PUBLIC_GROUP_PSK, 8b3387e9c5cdea6ac9e5edbaa115cd72): decided here, from
     * the key itself, because a client is never given the key and a name or
     * a one-byte hash would be a guess. */
    bool is_public;
    /* The longest body this node can send on this channel. MeshCore puts
     * "<our name>: " inside the encrypted payload (BaseChatMesh.cpp:492) and
     * silently TRUNCATES the text to make it fit MAX_TEXT_LEN; this service
     * refuses instead, so a client needs the real number rather than 160. */
    int text_limit;
};

/* ---- a message --------------------------------------------------------- */

enum mcd_msg_state {
    MCD_MSG_RECEIVED = 0,
    MCD_MSG_SENT_FLOOD,
    MCD_MSG_SENT_DIRECT,
    MCD_MSG_ACKED,
    MCD_MSG_NO_ACK,
    MCD_MSG_FAILED
};

const char *mcd_msg_state_name(enum mcd_msg_state s);

struct mcd_message {
    uint64_t id;                  /* meshcored's own, 1 upwards, never reused */
    bool outgoing;
    /* ---- who it was with ----
     *
     * Exactly one of these two is meaningful. A direct message has a peer;
     * a channel message has a channel and no peer at all - peer_key stays
     * zero and peer_name empty, because a group frame names no node.
     */
    bool is_channel;
    uint8_t peer_key[MCD_PUB_KEY_LEN];
    char peer_name[MCD_NODE_NAME_LEN];
    int channel_slot;             /* only when is_channel */
    uint8_t channel_hash;         /* only when is_channel */
    char channel_name[MCD_CHANNEL_NAME_LEN]; /* our local name for it */
    /* The name the sender CLAIMED, parsed back out of the payload prefix
     * MeshCore writes there. Empty when the text does not begin with
     * "<something>: ". It is not authenticated - nothing signs a group frame
     * - and `text` still holds the whole payload including the prefix, so a
     * caller that wants the body alone skips strlen(sender_name) + 2. */
    char sender_name[MCD_NODE_NAME_LEN];
    char text[MCD_MAX_TEXT + 1];
    uint32_t timestamp;           /* the sender's clock, MeshCore's own stamp */
    uint64_t mono_ms;
    enum mcd_msg_state state;
    /* Whether an acknowledgement can ever arrive for this message. True for
     * an outgoing direct message; false for everything on a channel, because
     * PAYLOAD_TYPE_GRP_TXT is flooded and unacknowledged - there is no
     * expected_ack, no timeout and no delivery report in the protocol. A
     * client must not draw "delivered" or "no ack" where this is false: it
     * would be inventing a guarantee MeshCore does not offer. */
    bool ack_expected;
    bool ack_known;
    uint64_t ack_mono_ms;
    /* MeshCore's attempt number of the last send of this message: 0 for the
     * first, one more for each resend (mcd_runtime_resend). Upstream writes
     * it into the payload's flags byte, so each attempt has an expected_ack
     * of its own. Only meaningful for an outgoing direct message. */
    uint8_t attempt;
    bool snr_known;
    double snr_db;
    bool rssi_known;
    double rssi_dbm;
};

/* ---- an app datagram (docs/api/mesh.md, "App datagrams") ----------------
 *
 * A packet for an application on another Doors node, carried as a MeshCore
 * PAYLOAD_TYPE_REQ whose data is `0xD0 | port`, the payload's length, and the
 * payload. The first byte is outside every request type upstream defines
 * (0x00-0x07 in the pinned tree), so no MeshCore firmware mistakes one for a
 * request it serves. The length is there because the decrypted REQ is padded
 * to the AES block and MeshCore records no length of its own.
 *
 * Unacknowledged at this layer, like a channel message: reliability is the
 * application's, end to end (ADR-008). One arriving by flood is answered with
 * a small RESPONSE riding MeshCore's own return path, so the sender learns a
 * direct route; one arriving direct is not answered at all.
 */
#define MCD_APP_MARKER 0xD0
#define MCD_APP_PORT_MIN 1
#define MCD_APP_PORT_MAX 15
#define MCD_APP_PAYLOAD_MAX 160
/* Held for clients that were not listening when they arrived: a bounded
 * ring, this run only, like the message list. */
#define MCD_APP_INBOX 32

struct mcd_app_datagram {
    uint64_t id;                  /* 1 upwards, never reused while the service runs */
    uint8_t port;
    uint8_t from[MCD_PUB_KEY_LEN];
    uint8_t len;
    uint8_t payload[MCD_APP_PAYLOAD_MAX];
    bool flood;                   /* it arrived by flood rather than on a known path */
    uint64_t mono_ms;
    bool snr_known;
    double snr_db;
    bool rssi_known;
    double rssi_dbm;
};

/* ---- repeaters heard directly (docs/api/mesh.md, "Repeater control") ----
 *
 * Which repeaters can hear THIS node, right now, with nothing in between.
 * Upstream's own question, asked its own way: a CONTROL packet
 * CTL_TYPE_NODE_DISCOVER_REQ (0x80) with a repeater-only type filter and a
 * random tag, sent zero-hop, which every repeater in direct range answers
 * with a zero-hop CTL_TYPE_NODE_DISCOVER_RESP (0x90 | ADV_TYPE_REPEATER)
 * carrying the tag, its whole public key and the SNR it heard the request at
 * (vendor/RIFT examples/simple_repeater/MyMesh.cpp onControlDataRecv; the
 * T-Deck RIFT firmware's DISCOVER 0-HOP sends the same bytes).
 *
 * The proof that an answer came straight from the repeater is upstream's:
 * mesh::Mesh hands a control packet to onControlDataRecv only when it is
 * direct-routed with a path of zero hashes (Mesh.cpp:70-75), so an answer
 * nobody relayed is the only kind that can arrive here. Nothing is inferred
 * from a name, a cached route or a hop guess.
 *
 * Answers trickle in: repeaters answer after a random delay widened four
 * times, because many answer at once, so a round stays open for
 * MCD_DISCOVER_WINDOW_MS (upstream RIFT's RIFT_DISCOVER_WINDOW_MS). One round
 * at a time: a second request while one is open is refused, never re-armed,
 * because re-arming would change the tag and drop the answers already in. A
 * repeater answers at most four requests in two minutes (its
 * discover_limiter), which one round per 30 s respects.
 *
 * Kept for this run only, newest answer first, at most MCD_DISCOVER_MAX
 * repeaters; one that answered an earlier round keeps that round's number,
 * so a client can show it as older rather than as heard now. */
#define MCD_DISCOVER_WINDOW_MS 30000
#define MCD_DISCOVER_MAX 16

struct mcd_discovered {
    uint8_t public_key[MCD_PUB_KEY_LEN];
    uint32_t round;           /* the round it last answered */
    uint64_t mono_ms;         /* when that answer was heard, by ours */
    double their_snr_db;      /* how IT heard our request: its own reading */
    bool snr_known;           /* how WE heard its answer, when radiod said */
    double snr_db;
    bool rssi_known;
    double rssi_dbm;
};

struct mcd_discover_state {
    uint32_t round;           /* the latest round started this run; 0 none */
    bool open;                /* that round is still collecting answers */
    uint64_t started_mono_ms;
    uint64_t until_mono_ms;
};

enum mcd_discover_result {
    MCD_DISCOVER_STARTED = 0,
    MCD_DISCOVER_BUSY,        /* a round is open; nothing was sent */
    MCD_DISCOVER_NO_RADIO,
    MCD_DISCOVER_FAILED       /* no packet free */
};

/* ---- one repeater session (docs/api/mesh.md, "Repeater control") --------
 *
 * Logging in to a repeater and reading it back, with upstream's own calls
 * and upstream's own matching:
 *
 *   login       BaseChatMesh::sendLogin - an ANON_REQ carrying our clock and
 *               the password; answered RESP_SERVER_LOGIN_OK (13 bytes: the
 *               repeater's clock, 0, is-admin, permissions, firmware level)
 *               or the legacy "OK". A WRONG PASSWORD IS NOT ANSWERED AT ALL
 *               (simple_repeater handleLoginReq returns 0), so it ends as a
 *               timeout, and nothing here can tell it from a request the
 *               repeater never heard.
 *   status      sendRequest(REQ_TYPE_GET_STATUS); the reply reflects our tag
 *               and carries RepeaterStats.
 *   neighbours  sendRequest(REQ_TYPE_GET_NEIGHBOURS, version 0); the
 *               repeater's own list of repeaters it heard zero-hop.
 *   owner       sendRequest(REQ_TYPE_GET_OWNER_INFO): firmware version,
 *               name, owner text.
 *   cli         sendCommandData (TXT_TYPE_CLI_DATA); answered by command
 *               data from the same node, with no tag. Admin only, on the
 *               repeater's side.
 *
 * ONE TARGET AND ONE REQUEST AT A TIME, as upstream's panel has: a request
 * while one is outstanding is refused, so rapid presses cannot stack
 * packets. A reply is matched by sender and, where upstream reflects one, by
 * tag; anything else is counted as stale or malformed and ignored. A wait
 * ends at max(20 s, 2 x MeshCore's estimate + 8 s), upstream RIFT's rule. A
 * login OK that arrives within MCD_REMOTE_LATE_LOGIN_MS of the login while
 * nothing else is waiting is still taken, as upstream RIFT does.
 *
 * There is NO LOGOUT ON THE AIR in MeshCore: upstream's CMD_LOGOUT is
 * BaseChatMesh::stopConnection, which drops local keep-alive state and
 * transmits nothing. Logout here does the same and forgets the session; the
 * repeater keeps its ACL entry for us. The session is memory only - never
 * written - and ends with the service, with a logout, and when the target's
 * contact is forgotten. The password is copied, handed to sendLogin and
 * wiped; it is never stored or logged. */
#define MCD_REMOTE_PASSWORD_MAX 15   /* upstream sendLogin's cut; longer is refused */
#define MCD_REMOTE_MIN_WAIT_MS 20000
#define MCD_REMOTE_LATE_LOGIN_MS 300000
#define MCD_REMOTE_NEIGHBOUR_PREFIX 6
#define MCD_REMOTE_NEIGHBOURS_MAX 11 /* what fits upstream's 130-byte result buffer at a 6-byte prefix */

enum mcd_remote_kind {
    MCD_REMOTE_NONE = 0,
    MCD_REMOTE_LOGIN,
    MCD_REMOTE_STATUS,
    MCD_REMOTE_NEIGHBOURS,
    MCD_REMOTE_OWNER,
    MCD_REMOTE_CLI
};
const char *mcd_remote_kind_name(enum mcd_remote_kind k);

enum mcd_login_state {
    MCD_LOGIN_NONE = 0,
    MCD_LOGIN_WAITING,
    MCD_LOGIN_OK,
    MCD_LOGIN_REFUSED,        /* the repeater answered, and not with an OK */
    MCD_LOGIN_TIMEOUT         /* no answer: a wrong password looks exactly like this */
};
const char *mcd_login_state_name(enum mcd_login_state s);

enum mcd_remote_outcome {
    MCD_REMOTE_REPLIED = 0,
    MCD_REMOTE_REFUSED,
    MCD_REMOTE_TIMED_OUT,
    MCD_REMOTE_CANCELLED      /* logout, the contact forgotten, a new target */
};
const char *mcd_remote_outcome_name(enum mcd_remote_outcome o);

/* RepeaterStats, decoded field by field against the length received: the
 * struct has grown across firmware versions (44, 48, 56 bytes), and a struct
 * copy would read an older repeater's short reply past its end. */
struct mcd_repeater_stats {
    uint16_t batt_milli_volts;
    uint16_t tx_queue_len;
    int16_t noise_floor;
    int16_t last_rssi;
    uint32_t packets_recv;
    uint32_t packets_sent;
    uint32_t air_time_secs;
    uint32_t up_time_secs;
    uint32_t sent_flood;
    uint32_t sent_direct;
    uint32_t recv_flood;
    uint32_t recv_direct;
    uint16_t err_events;
    int16_t last_snr_x4;
    bool have_dups;
    uint16_t direct_dups;
    uint16_t flood_dups;
    bool have_rx_air;
    uint32_t rx_air_time_secs;
    uint32_t recv_errors;
};

struct mcd_neighbour {
    uint8_t prefix[MCD_REMOTE_NEIGHBOUR_PREFIX];
    uint32_t heard_secs_ago;
    int8_t snr_x4;
};

struct mcd_remote_session {
    bool active;              /* a target is set */
    uint8_t key[MCD_PUB_KEY_LEN];
    enum mcd_login_state login;
    bool legacy;              /* answered with the legacy "OK": no permissions sent */
    bool admin;
    uint8_t permissions;
    uint8_t acl;
    uint8_t fw_level;
    bool server_clock_known;  /* the repeater's clock, from its login answer */
    uint32_t server_clock;
    uint64_t login_mono_ms;   /* when the login was sent, or answered */
    enum mcd_remote_kind pending;
    uint64_t pending_id;
    uint64_t deadline_mono_ms;
    uint64_t stale_replies;     /* for no request outstanding, or another one */
    uint64_t malformed_replies; /* matched, and too short or inconsistent to read */
};

struct mcd_remote_reply {
    uint64_t request_id;
    enum mcd_remote_kind kind;
    enum mcd_remote_outcome outcome;
    uint8_t key[MCD_PUB_KEY_LEN];
    uint64_t mono_ms;
    bool late;                /* a login OK taken after its wait had ended */
    struct mcd_repeater_stats stats;          /* STATUS */
    int neighbours_total;                     /* NEIGHBOURS: how many it holds */
    int neighbour_count;                      /* and how many came back */
    struct mcd_neighbour neighbours[MCD_REMOTE_NEIGHBOURS_MAX];
    char text[MCD_MAX_TEXT + 1];              /* OWNER, CLI: as sent, unsanitised */
    bool snr_known;
    double snr_db;
    bool rssi_known;
    double rssi_dbm;
};

enum mcd_remote_result {
    MCD_REMOTE_ACCEPTED_FLOOD = 0,
    MCD_REMOTE_ACCEPTED_DIRECT,
    MCD_REMOTE_BUSY,          /* a request is outstanding */
    MCD_REMOTE_NO_RADIO,
    MCD_REMOTE_NO_CONTACT,    /* no contact with that whole key */
    MCD_REMOTE_NOT_LOGGED_IN, /* no login OK for that node in this session */
    MCD_REMOTE_NOT_ADMIN,     /* the login said guest; the repeater ignores its commands */
    MCD_REMOTE_BAD_TEXT,      /* password or command empty, too long, or not one line */
    MCD_REMOTE_FAILED         /* MeshCore could not build it */
};

/* ---- what the runtime asks of the daemon -------------------------------
 *
 * One outbound call and three notifications. tx_submit is the whole of the
 * radio relationship seen from below: the runtime hands over bytes and is
 * told later how they went.
 */
struct mcd_runtime_hooks {
    /* Submit len bytes for transmission.
     *
     * Returns 0 when the daemon has taken the frame and will report exactly
     * one outcome through mcd_runtime_tx_done(), writing the submission's id
     * to *submit_id. Returns -1 when it cannot take it at all - no radio, a
     * transmit already outstanding, the write failed - in which case no
     * outcome follows and nothing was sent.
     *
     * It never blocks waiting for radiod to accept: acceptance is itself
     * asynchronous (radio.send_async answers before the radio is touched),
     * and a refusal arrives as MCD_TX_FAILED. */
    int (*tx_submit)(void *user, const uint8_t *bytes, int len, uint64_t *submit_id);

    void (*on_node)(void *user, const struct mcd_node *n, const char *reason);
    void (*on_message)(void *user, const struct mcd_message *m);
    /* A channel was added or removed. reason is a short word. */
    void (*on_channel)(void *user, const struct mcd_channel *c, const char *reason);
    /* A frame that was not a node or a message: someone else's traffic, or
     * one of ours that could not be opened. outcome is a short word. */
    void (*on_frame)(void *user, const struct mcd_rx_meta *meta, int bytes,
                     const char *outcome);
    void *user;
    /* An app datagram arrived. Appended, so a hooks table that does not set
     * it is unchanged; may be NULL. */
    void (*on_app)(void *user, const struct mcd_app_datagram *d);
    /* A repeater answered the open discovery round (d set), or the round's
     * window closed (d NULL). May be NULL. */
    void (*on_discover)(void *user, const struct mcd_discovered *d,
                        const struct mcd_discover_state *s);
    /* A repeater request ended: answered, refused, timed out or cancelled.
     * s is the session afterwards. May be NULL. */
    void (*on_remote)(void *user, const struct mcd_remote_reply *r,
                      const struct mcd_remote_session *s);
    /* One reception for the receive log (struct mcd_rx_obs). Appended like
     * on_app; may be NULL, and when it is the runtime does no receive-log
     * work at all. Called whether or not anyone is listening, so the repeat
     * count stays true across a client coming and going; skipping the
     * report is the daemon's business. */
    void (*on_rx_obs)(void *user, const struct mcd_rx_obs *o);
};

struct mcd_runtime_config {
    const char *state_dir;   /* where identity.id and contacts.v1 live */
    const char *node_name;   /* NULL or empty: keep the stored one, else derive */
    bool verbose;
};

struct mcd_runtime;

/* Claim the state directory for this process: create it if it is not there
 * (parents 0755, the directory itself 0700, as the runtime does) and take an
 * exclusive, non-blocking flock on it. Returns the descriptor that holds the
 * lock - keep it open for the life of the process; the kernel releases it on
 * exit however the process ends, so a lock is never stale - or -1 with why in
 * err. *busy is set when another process holds it.
 *
 * One state directory is one MeshCore node: its identity, its contacts, its
 * channel keys. Two processes on it would be two radios claiming one identity,
 * each rewriting the other's node table, and the second one's socket would
 * replace the first one's (pocketipc unlinks before it binds). The init script
 * prevents that for the processes it starts; this refuses it for all of them,
 * a hand-started one included. Call it before mcd_runtime_create and before
 * listening. */
int mcd_runtime_lock_state_dir(const char *state_dir, bool *busy, char *err, size_t errlen);

/* Create the runtime: load or generate the identity, load the contacts, and
 * bring the MeshCore node up. Returns NULL with why in err on a failure that
 * must stop the service - a corrupt identity file above all, which is never
 * replaced silently. */
struct mcd_runtime *mcd_runtime_create(const struct mcd_runtime_config *cfg,
                                       const struct mcd_runtime_hooks *hooks,
                                       char *err, size_t errlen);
void mcd_runtime_destroy(struct mcd_runtime *rt);

/* One turn of the MeshCore event loop. Never blocks. */
void mcd_runtime_tick(struct mcd_runtime *rt);
/* Is there a received frame waiting to be handed to MeshCore? The daemon
 * polls with no timeout while this is true, so a burst is drained one frame
 * per turn without starving its own IPC clients. */
bool mcd_runtime_rx_pending(const struct mcd_runtime *rt);

/* Hand over a received frame. Returns true when it was queued, false when
 * the receive queue was full (the frame is dropped and counted). The bytes
 * are copied. */
bool mcd_runtime_deliver_rx(struct mcd_runtime *rt, const uint8_t *bytes, int len,
                            const struct mcd_rx_meta *meta);
uint64_t mcd_runtime_rx_dropped(const struct mcd_runtime *rt);

/* The outcome of a submission the runtime made. Exactly one per tx_submit
 * that returned 0; a second one for the same id is ignored. */
void mcd_runtime_tx_done(struct mcd_runtime *rt, uint64_t submit_id,
                         enum mcd_tx_outcome outcome);

/* Whether the radio is usable. While false the runtime keeps running - it
 * still expires timeouts and serves clients - but tx_submit is not called,
 * so MeshCore is told the send failed rather than being left waiting for a
 * transmit that cannot happen. */
void mcd_runtime_set_radio_online(struct mcd_runtime *rt, bool online);
bool mcd_runtime_radio_online(const struct mcd_runtime *rt);

/* The profile radiod applied, so the airtime the protocol core budgets with
 * and the deadline it gives an outbound packet are the ones this link really
 * has. Until it is told, the service default (the MeshCore profile it asks
 * for) is assumed. */
void mcd_runtime_set_profile(struct mcd_runtime *rt, int spreading_factor, double bandwidth_khz,
                             int coding_rate, int preamble_length, bool crc);

/* ---- what the daemon asks of the runtime ------------------------------- */

/* The local identity. name is this node's advert name. */
void mcd_runtime_identity(const struct mcd_runtime *rt, uint8_t pub_key[MCD_PUB_KEY_LEN],
                          char *name, size_t name_len);

/* ---- the node's own name ------------------------------------------------
 *
 * Where the name in use came from: the command line (--name, from
 * MESHCORED_NAME in /etc/default/meshcored), which replaces the stored one at
 * a start unless the node was renamed over that very configured name since
 * (settings.v1, renamed_over); state.v1; or derived from the key on a first
 * start. */
enum mcd_name_source {
    MCD_NAME_CONFIG = 0,
    MCD_NAME_STORED,
    MCD_NAME_DERIVED,
};
enum mcd_name_source mcd_runtime_name_source(const struct mcd_runtime *rt);

/* Rename this node: 1 to MCD_NODE_NAME_LEN - 1 bytes of well-formed UTF-8,
 * one line, not only spaces. The name is what this node's adverts carry and
 * what it writes in front of every channel message ("<name>: "), so a peer
 * learns it at this node's next advert; nothing is transmitted here. Written
 * to state.v1 straight away, the one place a name is kept.
 *
 * A node whose name came from the command line is renamed too. So that the
 * next start does not put the configured name back, settings.v1 records
 * which configured name the rename replaced (not the name: its mark); a
 * configured name changed after that wins again, so the operator keeps the
 * last word.
 *
 * *persisted is true only when everything a restart needs was written: the
 * state file, and that mark when the name was configured. When it is false
 * the node runs under the new name now and comes back under the old one. */
enum mcd_rename_result {
    MCD_RENAME_OK = 0,
    MCD_RENAME_BAD_NAME,
};
enum mcd_rename_result mcd_runtime_set_name(struct mcd_runtime *rt, const char *name,
                                            bool *persisted);

/* ---- the path hash size --------------------------------------------------
 *
 * How many bytes of each relay's public key a flood this node starts asks
 * repeaters to write into its path - MeshCore's path hash size, set upstream
 * by the companion's CMD_SET_PATH_HASH_MODE (mode 0..2 is 1..3 bytes; mode 3
 * is reserved). 1 is the default and what every MeshCore node understands;
 * 2 and 3 tell more relays apart in a large mesh, and need repeaters whose
 * firmware reads the size bits of the path length. Kept in settings.v1. */
#define MCD_PATH_HASH_MIN 1
#define MCD_PATH_HASH_MAX 3
int mcd_runtime_path_hash_bytes(const struct mcd_runtime *rt);
/* Returns false, changing nothing, for a size outside 1..3. *persisted says
 * whether settings.v1 was written; the size applies either way. */
bool mcd_runtime_set_path_hash_bytes(struct mcd_runtime *rt, int bytes, bool *persisted);

int mcd_runtime_node_count(const struct mcd_runtime *rt);
/* Copy node idx (0-based, stable within one call sequence) into n. */
bool mcd_runtime_node_at(const struct mcd_runtime *rt, int idx, struct mcd_node *n);
/* Every node, most recently heard first, into out[0..max). Returns how many
 * were written; with max at MCD_MAX_NODES that is all of them, and a
 * smaller max gets the most recent ones.
 *
 * The order is what mesh.nodes promises (docs/api/mesh.md): nodes heard
 * during this run first, newest last_heard_mono_ms first; then the nodes
 * not heard since the service started, newest first by the time MeshCore
 * last updated the contact (its lastmod, kept in state.v1), so after a
 * restart the list still leads with what was heard last; ties in table
 * order. A client that keeps fewer nodes than the service holds keeps the
 * head of the list and so keeps the ones that matter. */
int mcd_runtime_nodes_recent(const struct mcd_runtime *rt, struct mcd_node *out, int max);
/* Look a node up by a public-key prefix of prefix_len bytes. Returns 1 on a
 * unique match (copied into n), 0 for no match, and -1 when the prefix
 * matches more than one node - which is a question the caller must ask more
 * precisely, not one to answer with a guess. */
int mcd_runtime_node_by_prefix(const struct mcd_runtime *rt, const uint8_t *prefix,
                               size_t prefix_len, struct mcd_node *n);

int mcd_runtime_message_count(const struct mcd_runtime *rt);
bool mcd_runtime_message_at(const struct mcd_runtime *rt, int idx, struct mcd_message *m);

enum mcd_send_result {
    MCD_SEND_ACCEPTED_FLOOD = 0,
    MCD_SEND_ACCEPTED_DIRECT,
    MCD_SEND_NO_RADIO,       /* the radio is not online */
    MCD_SEND_NO_CONTACT,     /* no such node */
    MCD_SEND_TOO_LONG,       /* the text does not fit a MeshCore message */
    MCD_SEND_NO_CHANNEL,     /* no channel in that slot */
    MCD_SEND_FAILED,         /* MeshCore refused it (no free packet, encode failure) */
    /* Every slot that watches a message for its ACK is in use. The message
     * is not built and nothing is sent: one this service could not watch
     * would never be answered delivered or not. */
    MCD_SEND_BUSY,
    /* mcd_runtime_resend only: no outgoing direct message has that id in
     * this run, or it was delivered, or it is still waiting for its ACK. */
    MCD_SEND_NOT_RESENDABLE
};

/* ---- channels -----------------------------------------------------------
 *
 * The table is fixed at MCD_MAX_CHANNELS slots and starts empty. Nothing
 * here joins a channel on its own: a channel exists because somebody asked
 * for it with a key, which is the only way the key can arrive.
 */
int mcd_runtime_channel_count(const struct mcd_runtime *rt);
/* Copy the idx'th occupied channel (0-based, in slot order) into c. */
bool mcd_runtime_channel_at(const struct mcd_runtime *rt, int idx, struct mcd_channel *c);
/* By slot. False when that slot holds no channel. */
bool mcd_runtime_channel_by_slot(const struct mcd_runtime *rt, int slot, struct mcd_channel *c);

enum mcd_channel_result {
    MCD_CHANNEL_OK = 0,
    MCD_CHANNEL_BAD_KEY,     /* not base64, or not 16/32 bytes, or all zero */
    MCD_CHANNEL_AMBIGUOUS_KEY, /* 32 bytes whose upper half is zero: see below */
    MCD_CHANNEL_BAD_NAME,
    MCD_CHANNEL_FULL,
    MCD_CHANNEL_DUPLICATE,   /* that key is already in the table */
    MCD_CHANNEL_NOT_FOUND,
    MCD_CHANNEL_FAILED,      /* MeshCore refused the slot */
    MCD_CHANNEL_MANDATORY    /* the standard Public channel cannot be left */
};

const char *mcd_channel_result_name(enum mcd_channel_result r);

/* Join a channel from a base64 pre-shared key, into the lowest free slot.
 *
 * MCD_CHANNEL_AMBIGUOUS_KEY is the one refusal that needs explaining. MeshCore's
 * setChannel() decides a key's length by looking at its upper 16 bytes and
 * hashes over 16 when they are all zero (BaseChatMesh.cpp:896-906), while its
 * addChannel() uses the decoded length instead - so a 32-byte key whose upper
 * half happens to be zero derives two different channel hashes depending on
 * which path a node took. Joining on one of them would put this node on a
 * channel its peers may hash differently, and the symptom would be silence
 * rather than an error. It is refused instead.
 *
 * On MCD_CHANNEL_OK, *out (when given) is the channel that was created. */
enum mcd_channel_result mcd_runtime_channel_add(struct mcd_runtime *rt, const char *name,
                                                const char *psk_base64,
                                                struct mcd_channel *out);
/* Leave a channel: its slot is emptied, not compacted, so every other
 * channel keeps the slot a client already knows it by. The standard Public
 * channel is mandatory in Doors and is refused (MCD_CHANNEL_MANDATORY).
 *
 * THE PUBLIC CHANNEL IS MANDATORY. MeshCore's well-known Public key
 * (PUBLIC_GROUP_PSK) is held by every Doors node: when the runtime starts
 * and the stored channels hold no slot with that exact key, it is joined
 * into the lowest free slot as "Public" and written at once - so a store
 * that predates the rule, or none at all, gets it, and one that has it is
 * left alone. Identified by the key only: a hashtag channel named "#public"
 * is a different channel and is untouched. With all slots taken by other
 * channels nothing is evicted; the service logs that Public could not be
 * added. Joining transmits nothing. */
enum mcd_channel_result mcd_runtime_channel_remove(struct mcd_runtime *rt, int slot);

/* Send text on a channel. There is no ACK, no timeout and no delivery
 * report: the message's state goes to sent_flood and stays there, and its
 * ack_expected is false. Refuses rather than truncating when the text plus
 * this node's name prefix does not fit; mcd_channel::text_limit is the
 * number a composer should be shown. */
enum mcd_send_result mcd_runtime_send_channel_text(struct mcd_runtime *rt, int slot,
                                                   const char *text, uint64_t *msg_id);

/* Send text to the node whose public key starts with prefix. On acceptance
 * *msg_id is the message this created and *est_timeout_ms is how long this
 * service will wait for its ACK before calling it no_ack.
 *
 * Every accepted message is watched for its ACK against its own deadline, up
 * to a small fixed number at once; MCD_SEND_BUSY is the answer when that many
 * are already waiting, and nothing is sent. */
enum mcd_send_result mcd_runtime_send_text(struct mcd_runtime *rt, const uint8_t *prefix,
                                           size_t prefix_len, const char *text,
                                           uint64_t *msg_id, uint32_t *est_timeout_ms);

/* Send an outgoing direct message of this run again, as upstream's
 * companion clients retry one (examples/companion_radio/MyMesh.cpp,
 * CMD_SEND_TXT_MSG): the SAME text and the SAME sender timestamp, with
 * MeshCore's attempt number one higher. The attempt is part of what the
 * recipient hashes into its ACK (BaseChatMesh::composeMsgPacket), so the new
 * attempt is watched under a fresh expected_ack and deadline; an ACK for an
 * earlier attempt that arrives late still marks the message delivered.
 *
 * Only a message that is no_ack (or failed) may be resent: one that was
 * delivered needs no second copy, and one still waiting would put two
 * attempts in flight. The message keeps its id - there is still one message,
 * sent more than once - and goes back to sent_flood or sent_direct.
 * MCD_SEND_NOT_RESENDABLE for anything else; NO_CONTACT when the peer has been
 * forgotten since; BUSY and NO_RADIO as mcd_runtime_send_text. */
enum mcd_send_result mcd_runtime_resend(struct mcd_runtime *rt, uint64_t msg_id,
                                        uint32_t *est_timeout_ms);

/* Answer no_ack for every watched message whose deadline is at or before
 * now_ms (this service's CLOCK_MONOTONIC milliseconds). mcd_runtime_tick()
 * calls it with the clock; it is exposed so a caller holding its own notion
 * of the time - a test - can drive the deadlines without waiting for them.
 * Returns how many messages it answered. */
int mcd_runtime_expire_acks(struct mcd_runtime *rt, uint64_t now_ms);
/* How many sent messages are waiting for their ACK now. */
int mcd_runtime_acks_waiting(const struct mcd_runtime *rt);

/* Send an app datagram to the node whose WHOLE public key is key. On
 * acceptance *est_timeout_ms is MeshCore's own estimate of how long an answer
 * to it could take on the route it went (flood or direct). The result is
 * ACCEPTED_FLOOD or ACCEPTED_DIRECT; NO_RADIO; NO_CONTACT when no such node is
 * held; TOO_LONG for a payload of 0 or more than MCD_APP_PAYLOAD_MAX bytes or
 * a port outside 1..15; FAILED when MeshCore could not build it. */
enum mcd_send_result mcd_runtime_send_app(struct mcd_runtime *rt,
                                          const uint8_t key[MCD_PUB_KEY_LEN], int port,
                                          const uint8_t *payload, size_t len,
                                          uint32_t *est_timeout_ms);
/* The held datagrams for a port with an id above after_id, oldest first, into
 * out[0..max). Returns how many. */
int mcd_runtime_app_inbox(const struct mcd_runtime *rt, int port, uint64_t after_id,
                          struct mcd_app_datagram *out, int max);

/* Build and flood one self-advert. This and the zero-hop one below are the
 * only ways meshcored ever transmits without having been sent something
 * first: there is no periodic advert, by decision - see
 * docs/services/MESHCORED.md. */
bool mcd_runtime_send_advert(struct mcd_runtime *rt);
/* The same advert, sent zero-hop: heard by the nodes in direct range and
 * repeated by none of them. */
bool mcd_runtime_send_advert_zero_hop(struct mcd_runtime *rt);

/* ---- repeater discovery and control (the structs above) ------------------ */

/* Start a zero-hop discovery round. *state (when given) is the round after
 * the call: the one just started, or the one still open when BUSY. */
enum mcd_discover_result mcd_runtime_discover(struct mcd_runtime *rt,
                                              struct mcd_discover_state *state);
void mcd_runtime_discover_state(const struct mcd_runtime *rt, struct mcd_discover_state *out);
/* The repeaters that answered this run, newest answer first. Returns how many. */
int mcd_runtime_discovered(const struct mcd_runtime *rt, struct mcd_discovered *out, int max);
/* Close the open round if its window ended at or before now_ms; returns
 * whether it did. mcd_runtime_tick calls it with the clock. */
bool mcd_runtime_discover_expire(struct mcd_runtime *rt, uint64_t now_ms);

/* Log in to the node with this WHOLE key. Copies the password, sends it, and
 * wipes the copy. Another target's session is ended first (CANCELLED). */
enum mcd_remote_result mcd_runtime_remote_login(struct mcd_runtime *rt,
                                                const uint8_t key[MCD_PUB_KEY_LEN],
                                                const char *password, uint64_t *request_id,
                                                uint32_t *wait_ms);
/* STATUS, NEIGHBOURS or OWNER of the node logged in to. */
enum mcd_remote_result mcd_runtime_remote_ask(struct mcd_runtime *rt,
                                              const uint8_t key[MCD_PUB_KEY_LEN],
                                              enum mcd_remote_kind kind, uint64_t *request_id,
                                              uint32_t *wait_ms);
/* One CLI command line to the node logged in to. */
enum mcd_remote_result mcd_runtime_remote_cli(struct mcd_runtime *rt,
                                              const uint8_t key[MCD_PUB_KEY_LEN],
                                              const char *command, uint64_t *request_id,
                                              uint32_t *wait_ms);
/* End the session with this node, or with whichever node when key is NULL:
 * local only, as upstream's logout is. False when there was none. */
bool mcd_runtime_remote_logout(struct mcd_runtime *rt, const uint8_t *key);
void mcd_runtime_remote_session(const struct mcd_runtime *rt, struct mcd_remote_session *out);
/* End the outstanding request if its wait ended at or before now_ms. Called
 * by mcd_runtime_tick; exposed for tests. Returns whether one ended. */
bool mcd_runtime_remote_expire(struct mcd_runtime *rt, uint64_t now_ms);

/* ---- the contact table ---------------------------------------------------
 *
 * Both take a WHOLE public key, never a prefix: they change what this node
 * holds, and doing that to whichever node a short prefix happened to match
 * would be acting on a guess.
 */

/* Forget a node: its contact, its learned route and the signal this service
 * recorded for it. It is added back when it next adverts. The table is
 * written out before this returns, and *persisted (when given) says whether
 * that write happened: false when it failed, or when an unreadable state.v1
 * is being kept as evidence and nothing is written - the node is then
 * forgotten for this run only. *was (when given) is the node as it was;
 * on_node is raised with it and the reason "removed". False when no such
 * node is held. */
bool mcd_runtime_node_remove(struct mcd_runtime *rt, const uint8_t key[MCD_PUB_KEY_LEN],
                             struct mcd_node *was, bool *persisted);
/* Forget the route to a node, so the next message to it floods. *now (when
 * given) is the node afterwards; on_node is raised with the reason "path".
 * False when no such node is held. */
bool mcd_runtime_node_reset_path(struct mcd_runtime *rt, const uint8_t key[MCD_PUB_KEY_LEN],
                                 struct mcd_node *now);

/* Protocol-level statistics, from the MeshCore dispatcher itself. */
struct mcd_runtime_stats {
    uint32_t sent_flood;
    uint32_t sent_direct;
    uint32_t recv_flood;
    uint32_t recv_direct;
    uint64_t rx_queued;
    uint64_t rx_dropped;
    int packets_free;
    int packets_total;
    int contacts;
    uint64_t path_payloads_refused; /* see mesh_runtime.cpp, the PATH guard */
    /* Adverts from nodes the contact table had no room for. MeshCore reports
     * them anyway, with a ContactInfo it is about to throw away; meshcored
     * counts them and does nothing else with them. */
    uint64_t nodes_unretained;
    uint64_t contacts_full;
    int channels;
    /* Group frames whose one-byte channel hash matched a slot this node does
     * not hold a channel in. Counted rather than logged: on a busy mesh
     * every channel anybody else uses lands here, and it is the number that
     * says whether the hash space is crowded. */
    uint64_t channel_frames_unmatched;
    /* App datagrams: received, sent, and flood receipts answered. */
    uint64_t app_rx;
    uint64_t app_tx;
    uint64_t app_receipts;
};
void mcd_runtime_stats(const struct mcd_runtime *rt, struct mcd_runtime_stats *out);

/* Was the stored node state unusable at start-up, and what happened to it?
 *
 * A corrupt or incompatible state.v1 does not stop the service - it is a
 * cache the mesh will refill, not an identity - so the node starts with an
 * empty table and this says so. Returns true when there was a fault, with a
 * description in buf. False, and an empty buf, on an ordinary start. */
bool mcd_runtime_state_fault(const struct mcd_runtime *rt, char *buf, size_t buf_len);

/* The same question for channels.v1, answered separately because the two
 * files have different consequences. A lost node table costs a rediscovery
 * the mesh performs on its own; a lost channel table costs every key an
 * operator typed in by hand, and nothing on the air will bring those back. */
bool mcd_runtime_channel_fault(const struct mcd_runtime *rt, char *buf, size_t buf_len);

/* Write the contact table out. Called when it changed and at shutdown; safe
 * to call when nothing changed (it does nothing). Returns 0 or -1.
 *
 * Does nothing, successfully, when an unusable state file could not be moved
 * aside: that file is the only evidence of the fault and is not written over. */
int mcd_runtime_persist(struct mcd_runtime *rt);
bool mcd_runtime_dirty(const struct mcd_runtime *rt);

/* Route the protocol core's logging into pocketlog. Called once at start-up;
 * the sink takes pocketlog's own level values, which mcport's enum mirrors. */
void mcd_runtime_set_log_sink(void (*sink)(int level, const char *line));

/* The protocol revision this was built from, for mesh.info. Either may be
 * an empty string if the build did not define it. */
const char *mcd_runtime_rift_commit(void);
const char *mcd_runtime_crypto_commit(void);

#ifdef __cplusplus
}
#endif

#endif /* MCD_MESH_RUNTIME_H */
