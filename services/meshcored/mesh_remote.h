/*
 * meshcored: the repeater session and the discovery round, as bookkeeping.
 *
 * Everything here is a decision about bytes that already arrived or a wait
 * that is already running: which reply belongs to which request, what a
 * RepeaterStats reply says, when a round or a request has ended. Nothing
 * here builds or sends a packet - the Node in mesh_runtime.cpp does that with
 * upstream's own calls (sendLogin, sendRequest, sendCommandData,
 * createControlData + sendZeroHop) and hands the answers to these classes.
 *
 * Kept apart from mesh_runtime.cpp so the rules can be read in one place and
 * so the file that owns the MeshCore node does not grow without bound. It
 * includes no MeshCore header: the types are the seam's (mesh_runtime.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MCD_MESH_REMOTE_H
#define MCD_MESH_REMOTE_H

#include "mesh_runtime.h"

#include <stdint.h>
#include <stddef.h>

namespace mcdremote {

/* Upstream's request and response bytes (vendor/RIFT examples/simple_repeater
 * MyMesh.cpp and src/helpers/BaseChatMesh.h), named once. */
const uint8_t REQ_GET_STATUS = 0x01;
const uint8_t REQ_GET_NEIGHBOURS = 0x06;
const uint8_t REQ_GET_OWNER_INFO = 0x07;
const uint8_t LOGIN_OK_BYTE = 0;  /* upstream RESP_SERVER_LOGIN_OK */
const uint8_t CTL_DISCOVER_REQ = 0x80;
const uint8_t CTL_DISCOVER_RESP = 0x90;
const uint8_t ADV_TYPE_REPEATER_BYTE = 2;

/* The RepeaterStats tiers, after the four-byte tag (upstream RIFT's
 * RIFT_STATS_MIN / _DUPS / _FULL). */
const int STATS_TAG = 4;
const int STATS_MIN = 44;
const int STATS_DUPS = 48;
const int STATS_FULL = 56;

/* data/len are the whole response as onContactResponse gets it, tag
 * included. False for a reply too short to hold the oldest tier. */
bool decodeStats(const uint8_t* data, int len, mcd_repeater_stats* out);

/* REQ_TYPE_GET_NEIGHBOURS version 0's reply: tag, total (int16), count
 * (int16), then count entries of prefix_len + 4 + 1 bytes. False when the
 * count does not fit what arrived or exceeds what was asked for. */
bool decodeNeighbours(const uint8_t* data, int len, int prefix_len, int asked,
                      mcd_remote_reply* out);

/* A text reply after the tag (OWNER), up to its NUL or its length. */
void decodeText(const uint8_t* data, int len, int offset, char* out, size_t out_len);

/* The wait upstream RIFT gives a request: twice MeshCore's estimate plus
 * 8 s, never under MCD_REMOTE_MIN_WAIT_MS. */
uint32_t waitFor(uint32_t est_ms);

/* What a response is, once a session has looked at it. */
enum Verdict {
    V_IGNORED = 0,    /* not about this session: another node, or no session */
    V_STALE,          /* from the target, for nothing outstanding or a different tag */
    V_MALFORMED,      /* matched, and could not be read */
    V_ANSWER,         /* fills `reply`; the request is over */
    V_LATE_LOGIN      /* a login OK after its wait; fills `reply` */
};

class Session {
public:
    Session();

    void state(mcd_remote_session* out) const;
    bool active() const { return _s.active; }
    bool isTarget(const uint8_t* key) const;
    bool busy() const { return _s.pending != MCD_REMOTE_NONE; }
    bool loggedIn(const uint8_t* key) const;
    bool knownGuest() const;

    /* The target becomes key; a different previous target is forgotten,
     * and a request outstanding for it is answered CANCELLED into *cancelled
     * (returns true when it did). */
    bool retarget(const uint8_t* key, mcd_remote_reply* cancelled, uint64_t now_ms);

    /* A request went out. tag is what the reply will reflect (0 for a login,
     * whose answer carries the repeater's clock instead, and for CLI, which
     * has none). Returns the request id. */
    uint64_t begin(mcd_remote_kind kind, uint32_t tag, uint32_t wait_ms, uint64_t now_ms);

    /* A RESPONSE from `key`. Fills reply on V_ANSWER / V_LATE_LOGIN. */
    Verdict onResponse(const uint8_t* key, const uint8_t* data, int len, uint64_t now_ms,
                       mcd_remote_reply* reply);
    /* CLI data (TXT_TYPE_CLI_DATA) from `key`. */
    Verdict onCommandData(const uint8_t* key, const char* text, uint64_t now_ms,
                          mcd_remote_reply* reply);

    /* The outstanding request's wait ended: fills reply TIMED_OUT. */
    bool expire(uint64_t now_ms, mcd_remote_reply* reply);

    /* Forget everything. Fills *cancelled for an outstanding request and
     * returns true when there was one. */
    bool end(mcd_remote_reply* cancelled, uint64_t now_ms);

private:
    void finishInto(mcd_remote_reply* r, mcd_remote_outcome o, uint64_t now_ms);
    void loginOk(const uint8_t* data, int len, uint64_t now_ms);
    /* The tag of the last request that ended, answered or not: a reply
     * carrying it arrived late and is stale, whatever is waiting now. */
    bool lastTagIs(uint32_t tag) const { return _last_tag != 0 && tag == _last_tag; }

    mcd_remote_session _s;
    uint32_t _tag;
    uint32_t _last_tag;
    uint64_t _next_id;
    uint64_t _login_sent_ms; /* for the late login; 0 when none is owed */
};

/* The discovery round. */
class Discovery {
public:
    Discovery();

    void state(mcd_discover_state* out) const;
    bool open(uint64_t now_ms) const;
    /* A round started with this tag. */
    void begin(uint32_t tag, uint64_t now_ms);
    /* A DISCOVER_RESP payload from the air, already known to be zero-hop.
     * Fills *entry and returns true when it answered the open round. */
    bool onResponse(const uint8_t* payload, int len, const uint8_t* self_key,
                    const mcd_rx_meta* meta, bool meta_known, uint64_t now_ms,
                    mcd_discovered* entry);
    /* Closes the round whose window ended; true when it did. */
    bool expire(uint64_t now_ms);
    int list(mcd_discovered* out, int max) const;

private:
    mcd_discovered _seen[MCD_DISCOVER_MAX];
    int _count;
    uint32_t _tag;
    mcd_discover_state _st;
};

}  // namespace mcdremote

#endif /* MCD_MESH_REMOTE_H */
