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
    uint8_t peer_key[MCD_PUB_KEY_LEN];
    char peer_name[MCD_NODE_NAME_LEN];
    char text[MCD_MAX_TEXT + 1];
    uint32_t timestamp;           /* the sender's clock, MeshCore's own stamp */
    uint64_t mono_ms;
    enum mcd_msg_state state;
    bool ack_known;
    uint64_t ack_mono_ms;
    bool snr_known;
    double snr_db;
    bool rssi_known;
    double rssi_dbm;
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
    /* A frame that was not a node or a message: someone else's traffic, or
     * one of ours that could not be opened. outcome is a short word. */
    void (*on_frame)(void *user, const struct mcd_rx_meta *meta, int bytes,
                     const char *outcome);
    void *user;
};

struct mcd_runtime_config {
    const char *state_dir;   /* where identity.id and contacts.v1 live */
    const char *node_name;   /* NULL or empty: keep the stored one, else derive */
    bool verbose;
};

struct mcd_runtime;

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

int mcd_runtime_node_count(const struct mcd_runtime *rt);
/* Copy node idx (0-based, stable within one call sequence) into n. */
bool mcd_runtime_node_at(const struct mcd_runtime *rt, int idx, struct mcd_node *n);
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
    MCD_SEND_FAILED          /* MeshCore refused it (no free packet, encode failure) */
};

/* Send text to the node whose public key starts with prefix. On acceptance
 * *msg_id is the message this created and *est_timeout_ms is how long
 * MeshCore will wait for the ACK. */
enum mcd_send_result mcd_runtime_send_text(struct mcd_runtime *rt, const uint8_t *prefix,
                                           size_t prefix_len, const char *text,
                                           uint64_t *msg_id, uint32_t *est_timeout_ms);

/* Build and flood one self-advert. This is the only way meshcored ever
 * transmits without having been sent something first: there is no periodic
 * advert, by decision - see docs/services/MESHCORED.md. */
bool mcd_runtime_send_advert(struct mcd_runtime *rt);

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
};
void mcd_runtime_stats(const struct mcd_runtime *rt, struct mcd_runtime_stats *out);

/* Write the contact table out. Called when it changed and at shutdown; safe
 * to call when nothing changed (it does nothing). Returns 0 or -1. */
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
