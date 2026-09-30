/*
 * PocketFleet multiplayer: one match between two devices, as a state
 * machine (docs/apps/FLEET_MULTIPLAYER.md).
 *
 * It is driven, never driving. Everything that happens to it is a call with
 * the time in it - a user action, a packet, a tick, a word from the
 * transport - and everything it wants done comes out of two doors:
 *
 *   - the outbox, packets to hand to the transport, and
 *   - the dirty flag, state to save before any of those packets may leave.
 *
 * The outbox is shut while there are unsaved changes: fleet_match_pop()
 * returns nothing until fleet_match_saved() (or fleet_match_save_failed())
 * says the state is on disk. That is how "nothing is transmitted that is not
 * already persisted" is a property of the code rather than of every caller.
 *
 * No clock is read and no random number is drawn here except through the
 * caller's `now_ms` (CLOCK_MONOTONIC) and the seed given at init, so a
 * match replays exactly, which is what lets tests/fleet_mp_sim_test run
 * thousands of them over a fake network.
 *
 * Pure C: no LVGL, no IPC, no filesystem (tests/fleet_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_MATCH_H
#define POCKETFLEET_MATCH_H

#include "fleet_chat.h"
#include "fleet_proto.h"

#include "../engine/fleet_rng.h"
#include "../engine/fleet_rules.h"

#include <stddef.h>
#include <stdint.h>

#define FLEET_MATCH_TOMBSTONES 4
#define FLEET_MATCH_OUTBOX 8
#define FLEET_MATCH_NAME_MAX 32
#define FLEET_MATCH_PEER_PREFIX 8

/* ---- timing (docs/apps/FLEET_MULTIPLAYER.md, "Obligations and retries") -- */
#define FLEET_RETRY_BASE_MS 7000u
#define FLEET_RETRY_CAP_MS 60000u
#define FLEET_RETRY_MAX 6
#define FLEET_BUSY_RETRY_MS 1500u
#define FLEET_PROBE_MS 120000u
#define FLEET_PROBE_MAX 15
#define FLEET_SILENCE_PROBE_MS 300000u
#define FLEET_DUP_REPLY_MS 2000u
#define FLEET_TOMB_REPLY_MS 30000u
/* How many times the fleets are asked for after the game is over (tries plus
 * probes), however often the peer answers something else. */
#define FLEET_REVEAL_SENDS (FLEET_RETRY_MAX + FLEET_PROBE_MAX)
/* Probes, one per silence period, on the opponent's turn. */
#define FLEET_SILENCE_PROBES 6
/* airtime governor */
#define FLEET_GOV_BURST 6
#define FLEET_GOV_REFILL_MS 5000u
#define FLEET_GOV_HOUR_MS 90000u
/* Chat ("Chat" in the protocol doc) goes second to the game, always: a line
 * waits while anything of the game's is due, held back or unanswered, and it
 * spends the same governor on stricter terms - never the last two tokens of
 * the burst, only while Fleet's airtime in the rolling hour is under 50 s,
 * and never once chat has used 20 s of that hour. So chat stops first when
 * the game is busy. A line is tried three times and then shown as not
 * delivered. Receipts, one per line at most, are held to the whole hour only
 * (fleet_match.c, chat_enqueue). */
#define FLEET_CHAT_TRIES 3
#define FLEET_CHAT_TOKEN_RESERVE 2
#define FLEET_CHAT_HOUR_MS 50000u
#define FLEET_CHAT_SHARE_MS 20000u

enum fleet_mp_phase {
    FLEET_MP_IDLE = 0,
    FLEET_MP_INVITING,      /* host, waiting for an answer (not saved) */
    FLEET_MP_INVITED,       /* guest, deciding (not saved) */
    FLEET_MP_ACCEPTING,     /* guest, accepted, waiting for START (not saved) */
    FLEET_MP_DEPLOY,        /* session live, own fleet not confirmed */
    FLEET_MP_COMMITTED,     /* own fleet committed, peer's not yet held */
    FLEET_MP_BATTLE,
    FLEET_MP_REVEAL,        /* the game is over; exchanging fleets */
    FLEET_MP_DONE,
    FLEET_MP_PHASE_COUNT
};

enum fleet_mp_role { FLEET_ROLE_NONE = 0, FLEET_ROLE_HOST, FLEET_ROLE_GUEST };

enum fleet_mp_outcome {
    FLEET_OUTCOME_NONE = 0,
    FLEET_OUTCOME_WIN,
    FLEET_OUTCOME_LOSS,
    FLEET_OUTCOME_VOID
};

enum fleet_mp_verify {
    FLEET_VERIFY_PENDING = 0,
    FLEET_VERIFY_OK,        /* the fleet matched its commitment and every answer */
    FLEET_VERIFY_MISMATCH,  /* it did not */
    FLEET_VERIFY_NONE       /* no fleet was revealed */
};

enum fleet_mp_link {
    FLEET_LINK_OK = 0,
    FLEET_LINK_RETRYING,
    FLEET_LINK_LOST,
    FLEET_LINK_RESYNCING,
    FLEET_LINK_THROTTLED        /* waiting for the airtime governor */
};

/* Something to tell the player in the lobby, once. */
enum fleet_mp_notice {
    FLEET_NOTICE_NONE = 0,
    FLEET_NOTICE_DECLINED,
    FLEET_NOTICE_BUSY,
    FLEET_NOTICE_INCOMPATIBLE,
    FLEET_NOTICE_NO_ANSWER,
    FLEET_NOTICE_CANCELLED,     /* the host withdrew the invite */
    FLEET_NOTICE_CROSSED,       /* both invited; joined the peer's game */
};

/* What just happened, for a screen that animates it. */
enum fleet_mp_event {
    FLEET_EV_NONE = 0,
    FLEET_EV_INCOMING,          /* the opponent fired at cell */
    FLEET_EV_ANSWER,            /* the answer to our shot at cell arrived */
    FLEET_EV_PHASE,
};

enum fleet_mp_obligation {
    FLEET_OB_NONE = 0,
    FLEET_OB_END,
    FLEET_OB_SYNC,
    FLEET_OB_INVITE,
    FLEET_OB_ACCEPT,
    FLEET_OB_CANCEL,
    FLEET_OB_COMMIT,
    FLEET_OB_SHOT,
    FLEET_OB_REVEAL,
};

enum fleet_tomb_kind {
    FLEET_TOMB_EMPTY = 0,
    FLEET_TOMB_DECLINED,
    FLEET_TOMB_CANCELLED,
    FLEET_TOMB_ENDED,
};

struct fleet_tombstone {
    uint32_t sid;
    uint8_t peer[FLEET_MATCH_PEER_PREFIX];
    uint8_t kind;                           /* enum fleet_tomb_kind */
    uint8_t reason;                         /* enum fleet_end_reason for ENDED */
};

struct fleet_match_out {
    uint8_t to[FLEET_KEY_BYTES];
    uint8_t len;
    uint8_t bytes[FLEET_PROTO_MAX];
    uint8_t obligation;                     /* enum fleet_mp_obligation, or 0 for a reply */
};

struct fleet_match_stats {
    unsigned rx;
    unsigned rx_bad;           /* malformed */
    unsigned rx_foreign;       /* right session, wrong sender */
    unsigned rx_stale;         /* old ply, old or unknown session */
    unsigned tx;
    unsigned tx_obligation;
    unsigned tx_airtime_ms;
    unsigned dup_replies;
    unsigned gov_waits;        /* an obligation waited for the governor */
    unsigned gov_drops;        /* a reply the governor dropped */
    unsigned busy;             /* the transport refused a frame */
    unsigned violations;
    unsigned resyncs;
    unsigned chat_tx;          /* CHAT frames, retries included */
    unsigned chat_rx;          /* new lines of theirs */
    unsigned chat_dup;         /* copies of theirs, not shown again */
    unsigned chat_failed;      /* lines of ours given up */
    unsigned chat_held;        /* a line waited for the game or the governor */
};

struct fleet_match {
    /* ---- saved (fleet_match_save.c) ------------------------------------ */
    uint8_t self_key[FLEET_KEY_BYTES];
    uint8_t role;                           /* enum fleet_mp_role */
    uint8_t phase;                          /* enum fleet_mp_phase */
    uint32_t sid;
    uint8_t peer_key[FLEET_KEY_BYTES];
    char peer_name[FLEET_MATCH_NAME_MAX];
    uint8_t rules;
    uint8_t committed;
    uint8_t own_layout[FLEET_LAYOUT_BYTES];
    uint8_t own_salt[FLEET_SALT_BYTES];
    uint8_t own_commit[FLEET_COMMIT_BYTES];
    uint8_t have_peer_commit;
    uint8_t peer_commit[FLEET_COMMIT_BYTES];
    uint8_t peer_has_commit;                /* evidence the peer holds ours */
    uint8_t resolved;                       /* R: plies resolved on this device */
    uint8_t log_cell[FLEET_PROTO_PLY_MAX + 1];  /* 1-based */
    uint8_t log_res[FLEET_PROTO_PLY_MAX + 1];
    uint8_t pending;                        /* our unanswered shot, or FLEET_NO_CELL */
    uint8_t have_peer_reveal;
    uint8_t peer_layout[FLEET_LAYOUT_BYTES];
    uint8_t peer_salt[FLEET_SALT_BYTES];
    uint8_t peer_has_reveal;
    uint8_t outcome;                        /* enum fleet_mp_outcome */
    uint8_t verify;                         /* enum fleet_mp_verify */
    uint8_t end_reason;                     /* enum fleet_end_reason */
    uint8_t end_by_me;
    uint8_t end_unacked;
    struct fleet_tombstone tomb[FLEET_MATCH_TOMBSTONES];

    /* ---- derived from the saved state ---------------------------------- */
    struct fleet_board own;                 /* our fleet and the fire at it */
    struct fleet_board target;              /* what we know of theirs */
    struct fleet_board peer_board;          /* theirs, once revealed and legal */

    /* ---- runtime -------------------------------------------------------- */
    struct fleet_rng rng;
    int64_t now;
    uint8_t ob;                             /* enum fleet_mp_obligation in force */
    uint8_t ob_ply;
    uint8_t attempts;
    uint8_t busy_run;
    int64_t next_retry;
    uint32_t retry_base;                    /* the transport's estimate */
    uint8_t lost;
    uint8_t probes;
    uint8_t reveal_sends;
    int64_t next_probe;
    uint8_t resyncing;
    uint8_t cancel_left;
    uint32_t cancel_sid;
    uint8_t cancel_to[FLEET_KEY_BYTES];
    int64_t last_heard;
    uint8_t silence_probed;
    int64_t last_reply[FLEET_MSG_TYPE_COUNT];
    int64_t tomb_reply[FLEET_MATCH_TOMBSTONES];
    uint32_t unknown_sid;
    int64_t unknown_reply;
    /* governor */
    uint8_t tokens;
    uint8_t throttled;                      /* an obligation is waiting for airtime */
    int64_t refill_at;
    uint32_t minute_ms[60];
    uint32_t chat_minute_ms[60];            /* the chat within minute_ms */
    int64_t minute_of[60];
    /* chat: never saved; a new session starts with none */
    struct fleet_chat chat;
    /* outbox */
    struct fleet_match_out out[FLEET_MATCH_OUTBOX];
    uint8_t out_head;
    uint8_t out_len;
    /* persistence */
    uint8_t dirty;
    uint8_t save_off;                       /* saving failed: the gate is open */
    uint8_t store_fault;                    /* the saved match could not be read */
    /* for the screens */
    unsigned revision;
    uint8_t notice;                         /* enum fleet_mp_notice */
    uint8_t event;                          /* enum fleet_mp_event */
    uint8_t event_cell;
    uint8_t event_res;
    struct fleet_match_stats stats;
};

/* ---- lifecycle ---------------------------------------------------------- */

/* A match with nothing in it, for this node's key. seed drives jitter only. */
void fleet_match_init(struct fleet_match *m, const uint8_t self_key[FLEET_KEY_BYTES],
                      uint32_t seed);
/* Rebuild the derived boards after the saved fields were loaded, and arm
 * the runtime for time now. Returns 0, or -1 when the saved state does not
 * hold together (the caller then treats the match as lost). */
int fleet_match_restore(struct fleet_match *m, int64_t now);

/* ---- user actions: 0 when done, -1 when not possible now ----------------- */

/* sid_entropy: random bits from the OS; 24 of them become the session id. */
int fleet_match_invite(struct fleet_match *m, const uint8_t peer[FLEET_KEY_BYTES],
                       const char *peer_name, uint32_t sid_entropy, int64_t now);
int fleet_match_cancel(struct fleet_match *m, int64_t now);
int fleet_match_accept(struct fleet_match *m, int64_t now);
int fleet_match_decline(struct fleet_match *m, int64_t now);
/* Commit to the fleet on `board` (all five placed). salt: 16 random bytes. */
int fleet_match_deploy(struct fleet_match *m, const struct fleet_board *board,
                       const uint8_t salt[FLEET_SALT_BYTES], int64_t now);
int fleet_match_fire(struct fleet_match *m, int row, int col, int64_t now);
int fleet_match_forfeit(struct fleet_match *m, int64_t now);
/* Ask the peer where it stands (SYNC). Used on reopen, and by Check link. */
int fleet_match_resume(struct fleet_match *m, int64_t now);
/* Put a finished match away: it becomes a tombstone and the match is idle. */
void fleet_match_dismiss(struct fleet_match *m);
void fleet_match_set_peer_name(struct fleet_match *m, const char *name);

/* ---- chat ------------------------------------------------------------------ */

/* Whether lines can be exchanged now: from Deploy until the match is put
 * away, unless it ended by forfeit, void or abandon - then the peer answers
 * anything but the ending with END, and a line would only cost airtime. */
int fleet_match_chat_open(const struct fleet_match *m);
/* Say something to the opponent. Returns 0 when the line is queued; -1 when
 * chat is not open or the line is empty, too long or not text; -2 when
 * FLEET_CHAT_OUTGOING lines are still waiting. */
int fleet_match_chat_send(struct fleet_match *m, const char *text, int64_t now);
/* The player has read every line. */
void fleet_match_chat_seen(struct fleet_match *m);

/* ---- from the transport -------------------------------------------------- */

void fleet_match_receive(struct fleet_match *m, const uint8_t from[FLEET_KEY_BYTES],
                         const uint8_t *buf, size_t n, int64_t now);
void fleet_match_tick(struct fleet_match *m, int64_t now);
/* The transport refused this frame (busy): nothing went out. */
void fleet_match_tx_refused(struct fleet_match *m, const struct fleet_match_out *o,
                            int64_t now);
/* The transport's own timeout estimate for the route, in ms (0: default). */
void fleet_match_set_retry_base(struct fleet_match *m, uint32_t ms);

/* ---- outputs ------------------------------------------------------------- */

/* Take the next packet to send. Returns 1 and fills *o, or 0 when there is
 * none or unsaved state is holding the outbox shut. */
int fleet_match_pop(struct fleet_match *m, struct fleet_match_out *o);
int fleet_match_dirty(const struct fleet_match *m);
void fleet_match_saved(struct fleet_match *m);
/* Saving failed: play on for the session, with the gate open. */
void fleet_match_save_failed(struct fleet_match *m);
/* The saved match could not be read: answer strangers' packets with
 * END(abandon) rather than END(unknown). */
void fleet_match_set_store_fault(struct fleet_match *m, int fault);

/* ---- reading it ---------------------------------------------------------- */

int fleet_match_active(const struct fleet_match *m);        /* DEPLOY..REVEAL */
int fleet_match_saved_phase(const struct fleet_match *m);   /* DEPLOY..DONE */
int fleet_match_my_turn(const struct fleet_match *m);
/* The side that fires ply k. */
enum fleet_mp_role fleet_match_shooter(int ply);
enum fleet_mp_link fleet_match_link(const struct fleet_match *m);
/* Shots each side has fired. */
int fleet_match_shots_by(const struct fleet_match *m, enum fleet_mp_role side);
/* FNV-1a over sid, both commits and plies 1..n (the SYNC digest). */
uint32_t fleet_match_digest(const struct fleet_match *m, int n);
const char *fleet_match_phase_name(enum fleet_mp_phase phase);

/* ---- the commitment (exposed for tests and for a peer's reveal) ------------ */

int fleet_layout_encode(const struct fleet_board *board, uint8_t out[FLEET_LAYOUT_BYTES]);
/* Place the five ships a layout names. Returns 0, or -1 when it is not a
 * legal fleet. */
int fleet_layout_decode(const uint8_t in[FLEET_LAYOUT_BYTES], struct fleet_board *board);
void fleet_commit_compute(uint32_t sid, const uint8_t owner[FLEET_KEY_BYTES],
                          const uint8_t layout[FLEET_LAYOUT_BYTES],
                          const uint8_t salt[FLEET_SALT_BYTES],
                          uint8_t out[FLEET_COMMIT_BYTES]);

#endif
