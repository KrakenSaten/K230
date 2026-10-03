/*
 * RIFT's model: what this app knows about the mesh, and how sure it is.
 *
 * meshcored's mesh.* API is protocol-oriented (docs/api/mesh.md): it names
 * what the service knows and says nothing about screens. This is the other
 * half - one bounded cache of nodes and raw activity, fed by snapshots and
 * events, which the screens read and never write.
 *
 * Three rules hold throughout, and they are the reason this file exists
 * separately from the LVGL that draws it:
 *
 *   - A value that is not known is absent, exactly as the API leaves it.
 *     Nothing here turns a missing RSSI into 0 dBm or a missing hop count
 *     into 0 hops. Every optional field travels with its own have_ flag.
 *   - A node is keyed by its public key, never by its position. mesh.nodes
 *     may reorder between snapshots and a mesh.node event names whichever
 *     node changed, so a duplicate updates the row it belongs to rather
 *     than adding a second one.
 *   - Malformed input is refused and counted, never half-applied. A hostile
 *     or buggy peer on the air reaches this app as an event; one bad event
 *     must cost one event.
 *
 * No LVGL and no sockets: the parsing and the cache are host-tested by
 * tests/rift_model_test.c with no display and no service.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_MODEL_H
#define RIFT_MODEL_H

#include <cjson/cJSON.h>

#include <stdint.h>

#include "rift_traffic.h"

/* A public key is 64 hex characters (docs/api/mesh.md, mesh.identity).
 *
 * The same field width holds a CONVERSATION key, which is either such a
 * public key or a channel's key, which starts with '#'. The two can never be
 * confused: a public key is 64 hex characters and nothing else, and no hex
 * character is '#'. Using one field for both is what lets a channel be an
 * ordinary conversation everywhere below - one list, one thread, one read
 * mark, one unread count - instead of a second copy of all of it.
 *
 * A channel's key is "#<slot>:<hash>:<name>" - the slot, the one-byte
 * channel hash and an FNV-1a fingerprint of this node's local name for it
 * (8 hex). The slot alone is NOT an identity: leaving a channel empties its
 * slot, and the next channel added takes the lowest empty one (mesh.md,
 * mesh.channels), while the old channel's messages are kept. Keyed by slot,
 * those messages would be shown under the new channel's name and a reply
 * would go to the new channel's audience. The hash tells channels with
 * different keys apart, and the name - fixed when the channel is added,
 * there is no rename - tells apart most of the rest; both travel with every
 * channel message (captured when meshcored recorded it) and with every
 * channel in mesh.channels, and neither is key material. A message missing
 * either one gets "#<slot>:?", a conversation of its own that no channel
 * ever matches and nothing can be sent to: separated rather than guessed.
 * What stays indistinguishable is a different channel re-added into the same
 * slot under the same local name with the same one-byte hash; telling that
 * apart needs a per-channel identity the service does not report.
 * rift_channel_key() and rift_key_is_channel() are the two ends of it, and
 * rift_model_key_channel() says whether a key still names a joined channel. */
#define RIFT_KEY_HEX 65
/* The node hash is its first byte, two hex characters. */
#define RIFT_HASH_HEX 3
/* How much of a remote name is kept. Names are chosen by whoever is on the
 * air; meshcored has already made them well-formed UTF-8 with no controls,
 * and this keeps a bounded prefix of that, cut on a character boundary. */
#define RIFT_NAME_MAX 48
/* MeshCore's path is at most 64 bytes (MCD_MAX_PATH), so 128 hex + NUL. */
#define RIFT_PATH_HEX_MAX 129
/* The cache above meshcored's node table. mesh.nodes lists most recently
 * heard first and a snapshot fills the cache from the head of that list; a
 * mesh.node event for a node not held evicts the stalest one. Either way
 * anything past the bound is counted, not kept. It was held at 64 while the
 * NODES list built a row per cached node; the list builds rows only for
 * what is on screen (ui/rift_nodes.c), so the cache is sized for the mesh
 * rather than the screen: 1000, which is what the service holds
 * (MCD_MAX_NODES) and what a Norwegian-scale mesh may reach. A node
 * is about 830 bytes, so this is about 830 KB of the shell's heap, measured
 * on the host by tests/rift_app_test.c (docs/apps/RIFT.md, Scale). */
#define RIFT_MAX_NODES 1000
/* The raw feed is a window, not a log: the newest entries, bounded. */
#define RIFT_MAX_ACTIVITY 48
/* mesh.send takes 1 to 160 bytes (docs/api/mesh.md); this holds one of
 * those and its NUL, with room for a body meshcored may have replaced
 * bytes in on the way out. A longer body is refused before it is sent,
 * with a reason, rather than silently cut. */
#define RIFT_MSG_TEXT_MAX 176
#define RIFT_SEND_TEXT_MAX 160
/* The message history this app keeps. meshcored's own store does not
 * survive its restart (mesh.messages, "persistent": false), so this is a
 * window on a window: the newest messages, bounded, oldest dropped and
 * counted. Everything derived from it - a conversation's unread count, its
 * preview, the delivery tally - is derived from what is still in here and
 * says so rather than implying a complete history. 512 messages of about
 * 610 bytes: a session's worth on a busy channel, for about 310 KB. */
#define RIFT_MAX_MESSAGES 512
/* Distinct conversations tracked: the rows COMMS can list and the read
 * marks kept. Part of the message history's bounds. 256: a read mark each
 * here (about 100 bytes), and on screen a list that builds rows only for
 * what is visible (ui/rift_conv_list.c), so the count costs no objects. */
#define RIFT_MAX_CONVERSATIONS 256
/* How many recent direct-message arrivals are remembered to recognise a
 * retransmission (rift_model_apply_live_message). */
#define RIFT_DM_RECENT 8
/* The same, for channel-message arrivals, and how many of the newest of
 * them keep the conversation they arrived in - which channel, so a muted one
 * can be told apart (rift_notify.h). One poll reads them all; more than this
 * many in one poll is still at most one sound. */
#define RIFT_CH_RECENT 8
#define RIFT_CH_ARRIVAL_RING 8
/* Channels the service will hold (mesh.channels, "max"). A service that
 * grows its table past this shows its first RIFT_MAX_CHANNELS here and says
 * so rather than silently listing some of them. */
#define RIFT_MAX_CHANNELS 8
/* A channel name is chosen locally and meshcored keeps 31 bytes of it; this
 * holds that and a little, cut on a character boundary like every other
 * remote string here. */
#define RIFT_CHANNEL_NAME_MAX 40
/* Path changes this app has seen for one node, newest first. Only what was
 * observed while RIFT was running; there is no history before that. */
#define RIFT_PATH_HISTORY 3
#define RIFT_REASON_MAX 72
#define RIFT_TEXT_MAX 96

/* How far the start of meshcored's current run may appear to move without
 * the service having restarted. mesh.status reports its uptime in whole
 * seconds, so a start derived from it is up to a second late, and the reply
 * is read some time after the service stamped it. Two seconds covers both
 * with room to spare, and is short enough that a restart is noticed while
 * the message ids that restarted with it are still only a handful. See
 * "which run of the service this is" below. */
#define RIFT_SVC_RESTART_SLACK_MS 2000

/* The seven service states of docs/api/mesh.md, plus the two answers that
 * are about this app's connection rather than about the service:
 * RIFT_SVC_ABSENT (meshcored is not answering) and RIFT_SVC_UNKNOWN (it is,
 * but has not said yet). They are kept apart because "the radio service is
 * not there" and "it has not told me yet" are different things to show. */
enum rift_svc_state {
    RIFT_SVC_UNKNOWN = 0,
    RIFT_SVC_ABSENT,
    RIFT_SVC_STARTING,
    RIFT_SVC_WAITING_RADIOD,
    RIFT_SVC_WAITING_LEASE,
    RIFT_SVC_CONFIGURING,
    RIFT_SVC_ONLINE,
    RIFT_SVC_DEGRADED,
    RIFT_SVC_ERROR,
};

/* How a node was last heard. UNKNOWN is "no route back is known", which is
 * not the same as "relayed by nobody". */
enum rift_link {
    RIFT_LINK_UNKNOWN = 0,
    RIFT_LINK_DIRECT,
    RIFT_LINK_RELAYED,
};

/* One observed path for a node: what this app saw, and when it saw it. */
struct rift_path_obs {
    int have_mono;
    int64_t mono_ms;                     /* meshcored's CLOCK_MONOTONIC */
    int path_known;
    int hops;
    char path_hex[RIFT_PATH_HEX_MAX];
};

struct rift_node {
    char key[RIFT_KEY_HEX];              /* the identity; never empty */
    char hash[RIFT_HASH_HEX];
    char name[RIFT_NAME_MAX];
    int have_name;
    int type;                            /* MeshCore ADV_TYPE_*: 1..4 */
    int have_type;

    int path_known;
    int hops;                            /* relay hops; only when path_known */
    int direct;                          /* zero relays */
    char path_hex[RIFT_PATH_HEX_MAX];    /* "" when the service sent none */

    int have_advert;
    int64_t advert_timestamp;            /* their clock */
    int have_heard;
    int64_t heard_mono_ms;               /* ours */
    int have_snr;
    double snr_db;
    int have_rssi;
    double rssi_dbm;
    /* How many relays the node's last advert passed through before this
     * device heard it (mesh.nodes, advert_hops): 0 is heard straight from
     * the node - a zero-hop neighbour. Not the route back above, which is
     * learned separately. Absent until an advert was heard in the
     * service's current run. */
    int have_advert_hops;
    int advert_hops;
    int have_advert_mono;
    int64_t advert_mono_ms;              /* ours */
    /* Where the node's adverts say it is (mesh.nodes lat/lon, degrees): a
     * claim by the node, absent unless both came, finite and in range, and
     * not MeshCore's 0,0 for "none". What MAP plots, and nothing else. */
    int have_location;
    double lat;
    double lon;

    /* How many adverts this app has seen name this node, and its path
     * history since RIFT opened. Both are this app's own observations and
     * are never presented as the service's. */
    unsigned observations;
    struct rift_path_obs hist[RIFT_PATH_HISTORY];
    int hist_count;

    uint32_t seq;                        /* update order, newest highest */
};

/* One channel, as mesh.channels reports it.
 *
 * There is no key here and no way to ask for one: meshcored does not report
 * it through any method (docs/api/mesh.md). What this holds is what a screen
 * needs - the slot it is named by, the local name, the one-byte hash that
 * actually goes on the air, and how long a body may be on it. */
struct rift_channel {
    int slot;
    char name[RIFT_CHANNEL_NAME_MAX];
    int have_name;
    char hash[RIFT_HASH_HEX];
    int have_hash;
    int key_bits;
    int have_key_bits;
    /* MeshCore's well-known Public channel, by the service's word
     * (mesh.channels "well_known": "public"): it holds the key and compared
     * it. Never inferred here from the name or the one-byte hash. */
    int is_public;
    /* The longest body mesh.send will take on this channel. Smaller than a
     * direct message's 160, because this node's name is sent inside a
     * channel payload. */
    int text_limit;
    int have_text_limit;
};

enum rift_act_kind {
    RIFT_ACT_RX = 0,
    RIFT_ACT_TX,
};

struct rift_activity {
    enum rift_act_kind kind;
    char word[24];                       /* rx: payload_type; tx: result */
    int have_bytes;
    int bytes;
    int have_mono;
    int64_t mono_ms;
    int have_rssi;
    double rssi_dbm;
    int have_snr;
    double snr_db;
};

/* A message's direction, as the API words it. */
enum rift_msg_dir {
    RIFT_MSG_IN = 0,
    RIFT_MSG_OUT,
};

/* The six states of docs/api/mesh.md, plus the two this app owns:
 *
 *   RIFT_MSG_SENDING   this app has written a mesh.send and has not been
 *                      answered yet. It is not a state meshcored reports
 *                      and is never shown as delivered - it says the
 *                      request is in flight and nothing more.
 *   RIFT_MSG_UNKNOWN   a state word this build does not know. The message
 *                      is kept and shown with its word rather than being
 *                      mapped onto the nearest state this build happens to
 *                      have; a v0 API may grow one.
 *
 * "sent" and "delivered" are deliberately not the same answer. The API is
 * explicit that accepted is not transmitted and that an ACK is what makes
 * it delivered, so nothing here collapses sent_flood or sent_direct into
 * success. */
enum rift_msg_state {
    RIFT_MSG_STATE_UNKNOWN = 0,
    RIFT_MSG_SENDING,
    RIFT_MSG_RECEIVED,
    RIFT_MSG_SENT_FLOOD,
    RIFT_MSG_SENT_DIRECT,
    RIFT_MSG_ACKED,
    RIFT_MSG_NO_ACK,
    RIFT_MSG_FAILED,
};

struct rift_message {
    /* meshcored's own id, 1 upwards, never reused while it runs - and note
     * "while it runs": the next run starts again at 1, which is why the
     * cache is emptied when the run changes (msg_generation). It is the
     * identity a duplicate event is matched on, which is why a message
     * without one is not kept. */
    int64_t id;
    enum rift_msg_dir dir;
    enum rift_msg_state state;
    char state_word[24]; /* what the service called it, for an unknown state */

    /* The conversation this belongs to: a peer's public key, or "#<slot>"
     * for a channel. It is what the list, the thread and the read mark are
     * all keyed on. */
    char conv_key[RIFT_KEY_HEX];

    /* Exactly one of the two halves below is filled.
     *
     * A direct message has a peer. A channel message has no peer at all -
     * a MeshCore group frame carries no public key and nothing signs it - so
     * peer_key stays empty and what it has instead is a channel and a name
     * somebody CLAIMED inside the payload. The two are kept apart here
     * because they are not the same kind of fact. */
    int is_channel;

    char peer_key[RIFT_KEY_HEX];
    char peer_name[RIFT_NAME_MAX];
    int have_peer_name;

    int channel_slot;
    char channel_name[RIFT_CHANNEL_NAME_MAX];
    int have_channel_name;
    /* The one byte that was on the air, as meshcored recorded it. With the
     * slot and the name it is the channel's identity (conv_key). */
    char channel_hash[RIFT_HASH_HEX];
    int have_channel_hash;
    /* The sender's claimed name, which meshcored parsed back out of the
     * payload prefix. Not authenticated, and never shown as if it were: see
     * rift_fmt_msg_caption. `text` still holds the whole payload including
     * the prefix. */
    char sender_name[RIFT_NAME_MAX];
    int have_sender_name;

    /* Whether an acknowledgement can ever arrive for this message. False for
     * everything on a channel: a group frame is flooded and unacknowledged,
     * so sent_flood is where an outgoing channel message ends. Nothing in
     * this app may draw "delivered" or "no ack" where this is 0. */
    int ack_expected;

    char text[RIFT_MSG_TEXT_MAX];

    int have_timestamp;
    int64_t timestamp; /* the sender's clock, MeshCore's own stamp */
    int have_mono;
    int64_t mono_ms; /* when the service saw it: ours, CLOCK_MONOTONIC */
    int have_ack_mono;
    int64_t ack_mono_ms;
    int have_snr;
    double snr_db;
    int have_rssi;
    double rssi_dbm;

    uint32_t seq; /* arrival order in this cache, newest highest */
};

/* One conversation, built on demand from the messages that are still held.
 * It is a view and not a record: nothing is stored per conversation except
 * how far the screen has read (see rift_model_mark_read). */
struct rift_conv {
    char key[RIFT_KEY_HEX];
    char name[RIFT_NAME_MAX];
    int have_name;
    /* A channel conversation. Its key is the channel's (rift_channel_key)
     * and there is no peer behind it. */
    int is_channel;
    int channel_slot;
    int unread;   /* incoming messages newer than the read mark */
    int total;    /* messages held for this conversation */
    int outgoing; /* of which sent by us */
    int acked;    /* of which acknowledged */
    int no_ack;   /* of which timed out */
    int failed;
    /* Of the outgoing ones, how many could never be acknowledged at all
     * because they went to a channel. Counted apart from acked/no_ack so a
     * tally can say "3 SENT, NO ACK ON CHANNELS" rather than implying three
     * deliveries were expected and did not arrive. */
    int unacknowledgeable;
    int have_newest_mono;
    int64_t newest_mono_ms;
    /* The newest message the OTHER side said, on the service's clock: what
     * the conversation's activity is measured from (rift_pulse_of). A
     * message this device sent says nothing about whether anyone is there. */
    int have_last_in_mono;
    int64_t last_in_mono_ms;
    const struct rift_message *newest; /* for the preview line */
};

/* A mesh.send this app has written and not yet been answered.
 *
 * It exists so the thread can say "sending" without saying "sent". The
 * message itself is not created here: meshcored raises mesh.message for it
 * and answers mesh.send with its id, and whichever arrives first creates
 * the one message, keyed by that id. Inventing a local copy and then
 * reconciling it would be the way one message became two. */
struct rift_outbox {
    int active;
    char conv_key[RIFT_KEY_HEX];
    char text[RIFT_MSG_TEXT_MAX];
    int have_submitted;
    int64_t submitted_mono_ms;
    int64_t message_id; /* 0 until the reply names it */
    char route[8];      /* "flood" or "direct", when the reply said */
    int failed;
    int unknown;        /* of failed: the service went before it answered */
    char error[RIFT_TEXT_MAX];
};

/* The requests a reader makes that are not a message: an advert, forgetting
 * a node, forgetting a route. The same shape as the outbox - written, then
 * answered or refused - and for the same reason: nothing here decides that a
 * request happened. An advert that was answered was ACCEPTED by the service,
 * which is not the same as transmitted; how the transmit went is in the
 * activity feed, in the service's own words. */
enum rift_action {
    RIFT_ACTION_NONE = 0,
    RIFT_ACTION_ADVERT_NEAR,  /* mesh.advert, zero_hop: direct range only */
    RIFT_ACTION_ADVERT_MESH,  /* mesh.advert, flooded */
    RIFT_ACTION_FORGET,       /* mesh.node_remove */
    RIFT_ACTION_RESET_PATH,   /* mesh.node_reset_path */
    /* Managing this node (ui/rift_manage.c), one at a time in m->manage_op.
     * None transmits: a channel is a key held here, a name reaches peers at
     * the next advert, and a path hash size applies to the next flood. */
    RIFT_ACTION_CHANNEL_ADD,     /* mesh.channel_add */
    RIFT_ACTION_CHANNEL_REMOVE,  /* mesh.channel_remove */
    RIFT_ACTION_RENAME,          /* mesh.set_name */
    RIFT_ACTION_PATH_HASH,       /* mesh.set_path_hash */
};

struct rift_action_state {
    enum rift_action kind;
    int active;  /* written, not answered */
    int done;    /* the service said yes */
    int failed;  /* it said no, or nobody knows */
    int unknown; /* of failed: nobody knows - the service went before answering */
    /* Of done, for a rename: the service took the name and said it could
     * not write it ("persisted": false) - it is in use now and the old one
     * returns at the service's next start. */
    int unsaved;
    char key[RIFT_KEY_HEX];     /* the node, for FORGET and RESET_PATH */
    char label[RIFT_NAME_MAX];  /* what the node was called when asked */
    int value;                  /* the slot, or the path hash size, asked about */
    int have_mono;
    int64_t mono_ms;            /* written, and then answered */
    char error[RIFT_TEXT_MAX];
};

/* How far a conversation has been read. Kept per peer because it is the one
 * piece of conversation state that is this app's and cannot be derived. */
struct rift_read_mark {
    char key[RIFT_KEY_HEX];
    int64_t last_read_id;
};

struct rift_model {
    /* ---- the service ---------------------------------------------- */
    enum rift_svc_state state;
    char reason[RIFT_REASON_MAX];
    int have_state_mono;
    int64_t state_mono_ms;

    int have_info;
    char version[24];
    char build[24];
    char protocol[16];

    int have_status;
    int radio_connected;
    int radio_lease_held;
    int radio_online;
    int have_radio_state;
    char radio_state[16];
    int have_nodes_reported;
    int nodes_reported;                  /* the service's own node count */
    int have_state_fault;
    char state_fault[RIFT_TEXT_MAX];
    /* The service could not read its stored channels. Reported apart from
     * state_fault because the two losses are not comparable: the mesh
     * re-advertises a forgotten node, and nothing gives back a channel key. */
    int have_channel_fault;
    char channel_fault[RIFT_TEXT_MAX];
    int have_counters;
    unsigned rx_events;
    unsigned rx_delivered;
    unsigned nodes_unretained;
    /* The transmit side, in the service's own words: went out and the radio
     * came back (tx_ok), went out and it did not (tx_rx_resume_failed), did
     * not go out (tx_failed), nobody knows (tx_unknown). And what the
     * protocol core sent and received, flooded or direct. */
    int have_traffic;
    unsigned tx_ok;
    unsigned tx_failed;
    unsigned tx_unknown;
    unsigned tx_rx_resume_failed;
    unsigned sent_flood;
    unsigned sent_direct;
    unsigned recv_flood;
    unsigned recv_direct;
    /* How often MeshCore reported its contact table full. Above zero, a new
     * node's advert could not be kept and a message to that node cannot be
     * sent until a node is forgotten. */
    int have_contacts_full;
    unsigned contacts_full;
    /* nodes_unretained as it stood when this app last saw a node forgotten
     * (or when the counter last went backwards, which is a new run of the
     * service). The counter only grows, so "the table is full" is said only
     * while adverts are still being turned away SINCE then - not for the rest
     * of the service's run once room has been made. */
    unsigned unretained_baseline;

    /* ---- which run of the service this is --------------------------- */
    /* meshcored hands out message ids from 1 on every run and keeps no
     * messages across one (mesh.messages, "persistent": false), so an id
     * identifies a message only within a single run of the service. This
     * is how a new run is told from the one before it.
     *
     * What is held is the run's start on the shared monotonic clock - the
     * moment a status was read, less the uptime it reported - which is one
     * value per run rather than a number that keeps moving. The lowest seen
     * is kept, because the truncation to whole seconds can only make it
     * look later than it is.
     *
     * Two things say the run has changed, and both are needed:
     *
     *   - uptime_s going backwards. Within one run it cannot, so this is
     *     certain, but it misses the case below.
     *   - the start moving forward by more than RIFT_SVC_RESTART_SLACK_MS.
     *     This catches the restart the first test misses: a service that
     *     had been up 3 s when it died, replaced ten seconds later by one
     *     whose uptime is already larger than the 3 s last seen.
     *
     * The residual gap is a run that lived, and was replaced, inside the
     * slack - a couple of seconds. Nothing else this app holds is affected:
     * nodes are keyed by public key and replaced by every snapshot, and the
     * activity ring is stamped on the board's clock, not the service's. */
    int have_uptime;
    int64_t uptime_s;                    /* the service's own, as last read */
    int have_svc_start;
    int64_t svc_start_ms;                /* this run's start, ours */
    unsigned svc_restarts;               /* runs of it this app has seen end */

    int have_identity;
    char self_key[RIFT_KEY_HEX];
    char self_hash[RIFT_HASH_HEX];
    char self_name[RIFT_NAME_MAX];
    /* Where the name came from (mesh.identity name_source): RIFT_NAME_SOURCE_*.
     * CONFIG is the operator's --name, which the service will not rename. */
    int self_name_source;
    int self_name_max; /* bytes; 0 when the service did not say */

    /* The path hash size this node's floods ask for (mesh.path_hash): bytes
     * of each relay's hash a repeater writes, 1 to 3. have_path_hash is 0
     * until it is read; path_hash_unsupported is set when the service
     * answered that it has no such method (a build older than it). */
    int have_path_hash;
    int path_hash_bytes;
    unsigned path_hash_allowed; /* bit n set: n bytes is allowed */
    int path_hash_unsupported;

    /* ---- what is shown, and how sure it is ------------------------- */
    /* A snapshot has been read since the last connection was made. Until
     * then the node list is whatever the previous connection left. */
    int snapshot_valid;
    int have_snapshot_mono;
    int64_t snapshot_mono_ms;
    /* The service is not answering and what is on screen is cached. DS §9
     * and the handoff §5 both want that said, not merely implied. */
    int stale;

    struct rift_node nodes[RIFT_MAX_NODES];
    int node_count;
    uint32_t seq;
    unsigned nodes_dropped;              /* the cache was full */
    /* Input this model would not take: a malformed event, and a node inside
     * an otherwise good snapshot that carries no usable key. Refused whole,
     * never half-applied, and counted so a screen can say the feed is
     * carrying rubbish rather than merely going quiet. */
    unsigned events_malformed;
    unsigned events_applied;

    struct rift_activity activity[RIFT_MAX_ACTIVITY]; /* ring */
    int activity_head;                   /* index of the newest */
    int activity_count;
    unsigned activity_total;
    /* The same feed counted by the minute, for the last twenty
     * (rift_traffic.h): received frames only, by what they were. */
    struct rift_traffic traffic;

    /* ---- messages --------------------------------------------------- */
    /* Oldest first, which is the order mesh.messages gives and the order a
     * thread is read in. A full cache drops the oldest and counts it. */
    struct rift_message msg[RIFT_MAX_MESSAGES];
    int msg_count;
    uint32_t msg_seq;
    unsigned msgs_dropped;     /* the cache was full */
    unsigned msgs_applied;     /* created or updated */
    unsigned msgs_duplicate;   /* an id already held: updated, not added */
    unsigned msgs_forgotten;   /* the service restarted under them */
    /* Which run of the service the ids in here belong to: svc_restarts as
     * it stood when the cache was last filled. When it falls behind, the
     * cache is emptied before anything new is merged into it - see
     * rift_messages.c. */
    unsigned msg_generation;
    /* The service has answered mesh.messages on this connection. Until it
     * has, an empty thread is "not read yet" rather than "nothing said". */
    int messages_valid;
    /* The first snapshot of a session has been taken in. What it carried
     * happened before this app was watching, so it is marked read: "unread"
     * here means "arrived while RIFT was open and has not been drawn", and
     * this app has no way to know what was read before it started. A later
     * snapshot - after a reconnect - is *not* treated this way, because
     * messages that arrived while the connection was down are unread. */
    int messages_seeded;
    /* meshcored's own total, which may be larger than what is held here. */
    int have_messages_reported;
    int messages_reported;
    int messages_persistent;

    /* ---- channels ---------------------------------------------------- */
    struct rift_channel channels[RIFT_MAX_CHANNELS];
    int channel_count;
    /* The service has answered mesh.channels on this connection. Until it
     * has, no channels is "not read yet" rather than "none joined". */
    int channels_valid;
    /* What the service said its table holds in total, which may be more than
     * RIFT_MAX_CHANNELS. */
    int have_channels_reported;
    int channels_reported;
    int channels_max;
    unsigned channels_dropped; /* the service listed more than this app holds */

    struct rift_read_mark read_mark[RIFT_MAX_CONVERSATIONS];
    int read_mark_count;

    /* ---- new direct messages (the DM notification) -------------------- */
    /* What rift_model_apply_live_message decided was a direct message that
     * genuinely just arrived - the one thing the DM sound is for. See
     * "new direct messages" below for the five conditions. dm_arrivals only
     * grows; a listener remembers how far it has read it. */
    unsigned dm_arrivals;
    /* Live incoming direct messages that were NOT new: a state update or a
     * repeat of the event for an id already held, an id at or below one
     * already seen this run, or a retransmission the service filed under a
     * fresh id. Counted so a test (and a curious reader) can see them go by. */
    unsigned dm_repeats;
    char dm_last_key[RIFT_KEY_HEX]; /* who the newest arrival was from */
    /* The highest incoming direct message id this run has shown, live or
     * in a snapshot. Emptied with the message window when the run changes,
     * because ids start again from 1. */
    int64_t dm_high_id;
    /* Fingerprints of the last few arrivals: peer, sender's timestamp and
     * text. A sender that retries a message nobody acknowledged sends the
     * same three again, which the service may record as a new message with a
     * new id; this is what keeps that from sounding twice. Kept across a
     * run change on purpose - the retry does not know the service restarted. */
    uint32_t dm_recent_fp[RIFT_DM_RECENT];
    int dm_recent_count;
    int dm_recent_at;

    /* ---- new channel messages (the channel notification) -------------- */
    /* The same five conditions as a direct message (below), over incoming
     * channel messages: a live event, new to the window, above every channel
     * id this run has shown, and not a retransmission of one of the last
     * few (channel, claimed sender, sender's timestamp, text). ch_arrivals
     * only grows; the newest RIFT_CH_ARRIVAL_RING arrivals keep their
     * conversation key, at ch_arrival_conv[(n - 1) % RING] for the n-th. */
    unsigned ch_arrivals;
    unsigned ch_repeats;
    char ch_arrival_conv[RIFT_CH_ARRIVAL_RING][RIFT_KEY_HEX];
    int64_t ch_high_id;
    uint32_t ch_recent_fp[RIFT_CH_RECENT];
    int ch_recent_count;
    int ch_recent_at;

    struct rift_outbox outbox;

    /* ---- actions (rift_actions.c) ------------------------------------- */
    /* Two slots, so an advert and a change to one node are not each other's
     * business: the last advert is what ACTIVITY reports, the last node
     * change is what NODES reports. Each holds one request at a time. */
    struct rift_action_state advert;
    struct rift_action_state node_op;
    /* The third: managing this node - a channel joined or left, a rename, a
     * path hash size. ACTIVITY reports it. */
    struct rift_action_state manage_op;
};

#define RIFT_NAME_SOURCE_UNKNOWN 0
#define RIFT_NAME_SOURCE_CONFIG 1
#define RIFT_NAME_SOURCE_STORED 2
#define RIFT_NAME_SOURCE_DERIVED 3

/* An empty model: no service, no identity, no nodes, nothing known. */
void rift_model_init(struct rift_model *m);

/* The service has gone away. The nodes are kept - they are the last thing
 * anybody told us and throwing them away would leave the screen emptier
 * than the truth - but they are marked stale, and the snapshot is no longer
 * valid, so the next connection re-reads it. */
void rift_model_service_lost(struct rift_model *m, const char *reason);
/* A connection was made: what is on screen is live again, but no snapshot
 * has been read yet. */
void rift_model_service_found(struct rift_model *m);

/* Each returns 0 when the result was applied, -1 when it was not a result
 * this model will take. A refusal changes nothing and is counted. */
int rift_model_apply_info(struct rift_model *m, const cJSON *result);
/* A mesh.status result. now_ms is CLOCK_MONOTONIC as this app reads it
 * (rift_mono_ms), and it is taken rather than read here so that the run the
 * service is on - which is derived from it and the reported uptime - can be
 * tested without waiting for a real clock. */
int rift_model_apply_status(struct rift_model *m, const cJSON *result, int64_t now_ms);
int rift_model_apply_identity(struct rift_model *m, const cJSON *result);
/* A mesh.path_hash (or mesh.set_path_hash) result: bytes and allowed. */
int rift_model_apply_path_hash(struct rift_model *m, const cJSON *result);
/* A whole mesh.nodes result: the cache becomes exactly this list, in the
 * order the service gave, and the snapshot becomes valid. */
int rift_model_apply_nodes(struct rift_model *m, const cJSON *result);
/* A whole mesh.channels result: the list becomes exactly what the service
 * reported, in the order it gave, and channels_valid becomes true. */
int rift_model_apply_channels(struct rift_model *m, const cJSON *result);

/* ---- channels ----------------------------------------------------------
 *
 * The conversation key for a channel, and its inverse (the identity is
 * described at RIFT_KEY_HEX). rift_channel_key writes "#<slot>:<hash>:<name>"
 * when both hash (two hex characters) and name (non-empty) are given, and
 * "#<slot>:?" when either is missing; an out-of-range slot writes "".
 * rift_channel_conv_key is the same for a channel in the list.
 * rift_key_is_channel returns the slot of either form, or -1 when the key is
 * not a channel's - including the bare "#<slot>" of builds before this one,
 * which named a slot and not a channel. */
void rift_channel_key(int slot, const char *hash, const char *name, char *out, size_t out_len);
void rift_channel_conv_key(const struct rift_channel *ch, char *out, size_t out_len);
int rift_key_is_channel(const char *key);

/* The channel in that slot, or NULL. */
const struct rift_channel *rift_model_channel(const struct rift_model *m, int slot);
/* The joined channel a conversation key names, or NULL: the channel in the
 * key's slot, only while it is still the same channel (its own key is this
 * one). NULL for a channel that has been left, for a slot another channel
 * has taken since, for "#<slot>:?", and for anything that is not a channel
 * key. This, and never the slot alone, is what may be written to. */
const struct rift_channel *rift_model_key_channel(const struct rift_model *m, const char *key);
/* COMMS' one fixed place: the row of MeshCore's well-known Public channel
 * (rift_channel.is_public) is moved to the front of list[0..count). Every
 * other row keeps the order it came in. Nothing is added: a node that does
 * not hold the channel has no such row, and one that does has exactly one.
 * Returns the index the row was found at, or -1. */
int rift_conv_public_first(const struct rift_model *m, struct rift_conv *list, int count);

/* One event: "mesh.state", "mesh.node", "mesh.channel" or "mesh.activity".
 * Anything else -
 * an unknown name, a data that is not an object, a node with no usable key -
 * is ignored and counted in events_malformed. Returns 0 or -1. */
int rift_model_apply_event(struct rift_model *m, const char *name, const cJSON *data);

/* Lookup by public key (full, lowercase). NULL when it is not held. */
const struct rift_node *rift_model_find(const struct rift_model *m, const char *key);

/* The newest activity first; index 0 is the newest. NULL past the end. */
const struct rift_activity *rift_model_activity_at(const struct rift_model *m, int i);

/* The list order (handoff §11.1, decided here): heard first, most recently
 * heard at the top, then everything not heard for more than the stale
 * boundary, then nodes never heard at all. Within a group, ties break on
 * the identity so the order is total and does not flicker.
 *
 * now_ms is meshcored's CLOCK_MONOTONIC as this app reads it (the same
 * clock: one board, one monotonic clock). Writes at most max pointers and
 * returns how many. */
#define RIFT_STALE_MS (12 * 60 * 60 * 1000LL)
int rift_model_order(const struct rift_model *m, int64_t now_ms, const struct rift_node **out,
                     int max);
/* How many of the ordered nodes are fresh (heard within RIFT_STALE_MS).
 * The rest of the order is the stale group and then the never-heard. */
int rift_model_fresh_count(const struct rift_model *m, int64_t now_ms);

/* ---- finding nodes (rift_order.c) -----------------------------------------
 *
 * A search is a question asked of the list, never a change to it: nothing
 * here writes a node.
 *
 * rift_node_matches: whether a node answers what a reader typed. Case does
 * not matter (ASCII, and the Latin-1 letters Æ Ø Å and their kin); the
 * spaces around the query do not count. A node matches when its name holds
 * the query, or when the query is two or more hex characters its key starts
 * with - the node hash, which is how a hop is written. An empty query
 * matches everything.
 *
 * rift_node_zero_hop: heard straight from it - its last advert came through
 * no relay (advert_hops 0), or the learned route back has none (direct).
 *
 * rift_node_filter keeps, in place and in order, the nodes that match the
 * query and - when zero_hop_repeaters is set - are repeaters heard zero-hop.
 * Returns how many are kept. */
#define RIFT_QUERY_MAX 40
#define RIFT_NODE_TYPE_REPEATER 2
#define RIFT_NODE_TYPE_SENSOR 4

/* Whether a node takes a normal direct message, by its advertised type
 * (MeshCore ADV_TYPE_*), never its name. Upstream MeshCore's repeater
 * (examples/simple_repeater, onPeerDataRecv) and sensor (simple_sensor) read
 * a text message only from a client logged in as admin, and then run it as a
 * CLI command; from anyone else it is not even decrypted, and no ACK comes.
 * So a repeater (2) and a sensor (4) are never offered a conversation and
 * nothing is sent to one. A chat node (1) is; so is a room server (3),
 * whose posts upstream also need a login - that is said in the thread, not
 * refused here. A node whose type is not known yet is not refused on a
 * guess. NULL is a node nobody has heard: not refused. */
int rift_node_can_message(const struct rift_node *n);
/* Whether lat/lon are a location this app will plot: both finite, inside
 * -90..90 and -180..180, and not exactly 0,0 (MeshCore's "never set"). */
int rift_location_valid(double lat, double lon);
/* Why not, in a reader's words, or NULL when it can. */
const char *rift_node_no_message_why(const struct rift_node *n);
int rift_node_matches(const struct rift_node *n, const char *query);
int rift_node_is_repeater(const struct rift_node *n);
int rift_node_zero_hop(const struct rift_node *n);
int rift_node_filter(const struct rift_node **list, int count, const char *query,
                     int zero_hop_repeaters);

/* ---- activity (rift_format.h, rift_pulse_of, for the words) ----------
 *
 * When a conversation was last heard from, on the service's clock: for a
 * direct conversation the later of its peer's last_heard_mono_ms (when the
 * node is held) and the newest incoming message; for a channel the newest
 * incoming message on it. Returns 1 and sets *ms, or 0 when nothing has ever
 * been heard from it. c is a row of rift_model_conversations, or one a
 * screen made for a conversation with nothing in it yet. */
int rift_model_conv_heard(const struct rift_model *m, const struct rift_conv *c, int64_t *ms);

/* Frames in the activity ring stamped within window_ms before now_ms, by
 * direction. The ring holds the newest RIFT_MAX_ACTIVITY only, so when it is
 * full and even its oldest entry is inside the window the true count may be
 * higher: *at_least is then 1 and a screen says "48+" rather than "48".
 * Entries with no stamp are not counted. */
void rift_model_recent_frames(const struct rift_model *m, int64_t now_ms, int64_t window_ms,
                              int *rx, int *tx, int *at_least);

/* The name for a hop hash, or NULL: the only nodes whose hash can be
 * resolved are the ones in this cache, and an ambiguous hash resolves to
 * nothing rather than to a guess (two nodes can share a first byte). */
const char *rift_model_name_for_hash(const struct rift_model *m, const char *hash_hex);

/* ---- messages ----------------------------------------------------------- */

/* Read which run of the service a mesh.status result came from, and count a
 * restart when it is not the run the last one came from. now_ms is when the
 * result was read, on this app's own CLOCK_MONOTONIC.
 *
 * rift_model_apply_status calls this and nothing else does; it lives with
 * the message cache because the cache is the only thing the answer is for.
 * A result that says nothing about the run leaves what is held alone. */
void rift_model_note_service_run(struct rift_model *m, const cJSON *result, int64_t now_ms);

/* One message object, in the shape docs/api/mesh.md gives it. Both the
 * mesh.message event and every entry of a mesh.messages snapshot go through
 * here, so all three kinds of arrival - new, sent, state changed - are one
 * path keyed on the message id. Returns 0, or -1 for a message this model
 * will not hold (no id, no peer key, no direction, no text). */
int rift_model_apply_message(struct rift_model *m, const cJSON *message);

/* The filing itself, for the model's own translation units: as
 * rift_model_apply_message, and on 0 *out is the message as filed and
 * *fresh whether its id was new to the window. Either may be NULL. */
int rift_model_file_message(struct rift_model *m, const cJSON *message,
                            struct rift_message **out, int *fresh);

/* The same, for a message that arrived as a live mesh.message EVENT, which
 * is the only way a new direct message can announce itself. On top of what
 * rift_model_apply_message does, it decides whether this is a direct
 * message that genuinely just arrived, and counts it in dm_arrivals when
 * all five hold:
 *
 *   1. it came as an event - never from a mesh.messages snapshot, which is
 *      history however recent (a snapshot only raises dm_high_id);
 *   2. it is incoming, and direct - not this device's own, not a channel's;
 *   3. its id is new to the window - an id already held is a state change
 *      or the same event again;
 *   4. its id is above dm_high_id - anything at or below the highest this
 *      run has shown is history coming round again (an evicted message, a
 *      replay);
 *   5. its peer, sender timestamp and text are not those of one of the last
 *      RIFT_DM_RECENT arrivals - a retransmission filed under a fresh id.
 *      Only when the sender's timestamp is known: without it two messages
 *      that happen to say the same thing are not the same message.
 *
 * An incoming CHANNEL message that meets the same five conditions - over
 * ch_high_id, with the channel and the claimed sender name in place of the
 * peer - is counted in ch_arrivals instead, with its conversation key.
 *
 * Returns what rift_model_apply_message returns. */
int rift_model_apply_live_message(struct rift_model *m, const cJSON *message);
/* The conversation of the n-th channel arrival (1-based, as ch_arrivals
 * counts), or NULL when it is older than the ring keeps. */
const char *rift_model_ch_arrival_conv(const struct rift_model *m, unsigned n);

/* A whole mesh.messages result. The messages are merged by id, so a
 * snapshot taken after events have already delivered some of the same
 * messages updates them rather than doubling them - unless the service has
 * restarted since the cache was filled, in which case the cache is emptied
 * first, because the ids have started again and merging would land a new
 * id 1 on top of an old one. Returns 0 or -1. */
int rift_model_apply_messages(struct rift_model *m, const cJSON *result);

/* The conversations, newest message first. A peer with no message held is
 * not a conversation and is not returned: the contacts a screen offers to
 * start one with come from the node list, which is a different question.
 * Writes at most max and returns how many. */
int rift_model_conversations(const struct rift_model *m, struct rift_conv *out, int max);

/* One conversation's messages, oldest first - the order they happened and
 * the order a thread is read in. Writes at most max pointers, and when
 * there are more than max it writes the *newest* max: a thread that does
 * not fit is read from its end. Returns how many were written, and sets
 * *older to how many were left off the front. */
int rift_model_thread(const struct rift_model *m, const char *peer_key,
                      const struct rift_message **out, int max, int *older);

/* Everything the screen has now drawn for this peer is read. Returns the
 * number of messages that stopped being unread. Marking a conversation
 * that is not held does nothing.
 *
 * The mark is an id and not a count, so a message arriving between the draw
 * and the mark is still unread afterwards. */
int rift_model_mark_read(struct rift_model *m, const char *peer_key);
int rift_model_unread(const struct rift_model *m, const char *peer_key);
int rift_model_unread_total(const struct rift_model *m);

/* The peer's display name from the newest message that carried one, else
 * the node cache, else NULL. */
/* A conversation's display name: for a peer, the newest message that
 * carried one, else the node cache; for a channel, this node's own name for
 * it. NULL when there is none, which a caller shows as the node hash or the
 * slot rather than as an empty row. */
const char *rift_model_conv_name(const struct rift_model *m, const char *conv_key);

/* ---- sending ------------------------------------------------------------ */

/* Take a submission. Fails (-1) when one is already in flight, when the
 * destination or text is not one this app will send, or when the model is
 * not in a state to send at all. conv_key is a peer's public key or a
 * channel's "#<slot>". The text is not put on the air here - this only
 * records that a request is about to be written, so the thread can say so. */
int rift_model_send_begin(struct rift_model *m, const char *conv_key, const char *text,
                          int64_t now_ms);

/* The longest body this conversation will take, or 0 when it is not known.
 * A channel's limit is shorter than a direct message's and comes from the
 * service (mesh.channels, text_limit); asking here keeps the composer from
 * having to know which kind it is looking at. */
int rift_model_text_limit(const struct rift_model *m, const char *conv_key);
/* meshcored answered mesh.send. route may be NULL. */
void rift_model_send_accepted(struct rift_model *m, int64_t message_id, const char *route);
/* meshcored refused it, or the connection went away under it. */
void rift_model_send_failed(struct rift_model *m, const char *error);
/* Forget the last failure, so the caption goes when the composer is used
 * again. */
void rift_model_send_clear(struct rift_model *m);
/* A submission is in flight: the composer's SEND is disabled while one is,
 * because this radio sends one message at a time and a queue would be this
 * app's fiction rather than the service's. */
int rift_model_sending(const struct rift_model *m);

/* ---- actions (rift_actions.c) --------------------------------------------
 *
 * The slot an action kind lives in: m->advert for the two adverts, m->node_op
 * for FORGET and RESET_PATH. NULL for anything else. */
struct rift_action_state *rift_model_action_slot(struct rift_model *m, enum rift_action kind);
const struct rift_action_state *rift_model_action_of(const struct rift_model *m,
                                                     enum rift_action kind);
/* Record that a request is about to be written. Fails (-1) when that slot
 * already has one in flight, when a node action names no whole public key,
 * or when kind is not an action. label is what the node is called, for a
 * screen to say later what was done to whom; it may be NULL. */
int rift_model_action_begin(struct rift_model *m, enum rift_action kind, const char *key,
                            const char *label, int64_t now_ms);
/* The service answered the request in flight for this kind. */
void rift_model_action_done(struct rift_model *m, enum rift_action kind, int64_t now_ms);
/* It refused, or the request could not be written; why is for the reader. */
void rift_model_action_failed(struct rift_model *m, enum rift_action kind, const char *why,
                              int64_t now_ms);
/* Forget what happened to the last request of this kind's slot. */
void rift_model_action_clear(struct rift_model *m, enum rift_action kind);
/* A request of this kind's slot is in flight. */
int rift_model_action_busy(const struct rift_model *m, enum rift_action kind);

/* A node, parsed as mesh.node answers it - one node in the shape of a
 * snapshot entry - and filed as an update, never a second row. Unlike a
 * mesh.node EVENT it is not counted among the events that named the node:
 * it is the service answering RIFT, not the mesh speaking. Returns 0 or -1. */
int rift_model_apply_node_reply(struct rift_model *m, const cJSON *result);
/* The service has forgotten this node: it leaves the cache, with the path
 * history RIFT kept for it. Returns 1 when it was held. */
int rift_model_drop_node(struct rift_model *m, const char *key);

/* Adverts the service's full node table has turned away since this app last
 * saw a node forgotten - the number worth saying "the table is full" about.
 * 0 when room has been made since and nothing has been turned away. */
unsigned rift_model_unretained_recent(const struct rift_model *m);

/* One mesh.channel event's data (rift_channels.c). Returns 0, or -1 for one
 * without a usable slot or reason; the caller counts the refusal. */
int rift_model_apply_channel_event(struct rift_model *m, const cJSON *data);

const char *rift_svc_state_word(enum rift_svc_state s);

/* What the activity panel calls the service's state: the state word, except
 * that a service degraded only because the owner switched the radio off
 * (radiod state `off`, docs/api/mesh.md) says "radio off" - a choice, not a
 * fault. */
const char *rift_model_state_label(const struct rift_model *m);
/* Whether the radio is off by the owner's choice, as last reported. */
int rift_model_radio_off(const struct rift_model *m);

/* The one clock an interval may be measured on here: this board starts at
 * 1970 on every boot and jumps by decades when the network comes up, so
 * ages come from CLOCK_MONOTONIC and never from the wall clock. */
int64_t rift_mono_ms(void);

#endif
