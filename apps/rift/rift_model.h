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

/* A public key is 64 hex characters (docs/api/mesh.md, mesh.identity). */
#define RIFT_KEY_HEX 65
/* The node hash is its first byte, two hex characters. */
#define RIFT_HASH_HEX 3
/* How much of a remote name is kept. Names are chosen by whoever is on the
 * air; meshcored has already made them well-formed UTF-8 with no controls,
 * and this keeps a bounded prefix of that, cut on a character boundary. */
#define RIFT_NAME_MAX 48
/* MeshCore's path is at most 64 bytes (MCD_MAX_PATH), so 128 hex + NUL. */
#define RIFT_PATH_HEX_MAX 129
/* MeshCore's contact table holds 32; this is the cache above it, with room
 * for a service whose table grows without this app being rebuilt. A full
 * cache drops the stalest node and counts it rather than growing. */
#define RIFT_MAX_NODES 64
/* The raw feed is a window, not a log: the newest entries, bounded. */
#define RIFT_MAX_ACTIVITY 48
/* Path changes this app has seen for one node, newest first. Only what was
 * observed while RIFT was running; there is no history before that. */
#define RIFT_PATH_HISTORY 3
#define RIFT_REASON_MAX 72
#define RIFT_TEXT_MAX 96

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

    /* How many adverts this app has seen name this node, and its path
     * history since RIFT opened. Both are this app's own observations and
     * are never presented as the service's. */
    unsigned observations;
    struct rift_path_obs hist[RIFT_PATH_HISTORY];
    int hist_count;

    uint32_t seq;                        /* update order, newest highest */
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
    int have_counters;
    unsigned rx_events;
    unsigned rx_delivered;
    unsigned nodes_unretained;

    int have_identity;
    char self_key[RIFT_KEY_HEX];
    char self_hash[RIFT_HASH_HEX];
    char self_name[RIFT_NAME_MAX];

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
};

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
int rift_model_apply_status(struct rift_model *m, const cJSON *result);
int rift_model_apply_identity(struct rift_model *m, const cJSON *result);
/* A whole mesh.nodes result: the cache becomes exactly this list, in the
 * order the service gave, and the snapshot becomes valid. */
int rift_model_apply_nodes(struct rift_model *m, const cJSON *result);

/* One event: "mesh.state", "mesh.node" or "mesh.activity". Anything else -
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

/* The name for a hop hash, or NULL: the only nodes whose hash can be
 * resolved are the ones in this cache, and an ambiguous hash resolves to
 * nothing rather than to a guess (two nodes can share a first byte). */
const char *rift_model_name_for_hash(const struct rift_model *m, const char *hash_hex);

const char *rift_svc_state_word(enum rift_svc_state s);

/* The one clock an interval may be measured on here: this board starts at
 * 1970 on every boot and jumps by decades when the network comes up, so
 * ages come from CLOCK_MONOTONIC and never from the wall clock. */
int64_t rift_mono_ms(void);

#endif
