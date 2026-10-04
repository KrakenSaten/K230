/*
 * meshcored: the repeater session and the discovery round. See mesh_remote.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "mesh_remote.h"

#include <stdio.h>
#include <string.h>

namespace mcdremote {

namespace {

uint16_t rd16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* The two login answers upstream accepts (companion MyMesh.cpp
 * onContactResponse), each stating the length it reads. */
bool isLoginOk(const uint8_t* data, int len)
{
    return len >= 13 && data[4] == LOGIN_OK_BYTE;
}

bool isLegacyOk(const uint8_t* data, int len)
{
    return len >= 6 && data[4] == 'O' && data[5] == 'K';
}

}  // namespace

bool decodeStats(const uint8_t* data, int len, mcd_repeater_stats* out)
{
    const uint8_t* s;
    int n;

    if (data == NULL || out == NULL || len < STATS_TAG + STATS_MIN) {
        return false;
    }
    s = data + STATS_TAG;
    n = len - STATS_TAG;
    memset(out, 0, sizeof(*out));
    /* Little-endian, as every MeshCore target memcpys its packed struct. */
    out->batt_milli_volts = rd16(s + 0);
    out->tx_queue_len = rd16(s + 2);
    out->noise_floor = (int16_t)rd16(s + 4);
    out->last_rssi = (int16_t)rd16(s + 6);
    out->packets_recv = rd32(s + 8);
    out->packets_sent = rd32(s + 12);
    out->air_time_secs = rd32(s + 16);
    out->up_time_secs = rd32(s + 20);
    out->sent_flood = rd32(s + 24);
    out->sent_direct = rd32(s + 28);
    out->recv_flood = rd32(s + 32);
    out->recv_direct = rd32(s + 36);
    out->err_events = rd16(s + 40);
    out->last_snr_x4 = (int16_t)rd16(s + 42);
    if (n >= STATS_DUPS) {
        out->direct_dups = rd16(s + 44);
        out->flood_dups = rd16(s + 46);
        out->have_dups = true;
    }
    if (n >= STATS_FULL) {
        out->rx_air_time_secs = rd32(s + 48);
        out->recv_errors = rd32(s + 52);
        out->have_rx_air = true;
    }
    return true;
}

bool decodeNeighbours(const uint8_t* data, int len, int prefix_len, int asked,
                      mcd_remote_reply* out)
{
    int entry = prefix_len + 4 + 1;
    int total;
    int count;
    int i;

    if (data == NULL || out == NULL || prefix_len <= 0 ||
        prefix_len > MCD_REMOTE_NEIGHBOUR_PREFIX || len < 8) {
        return false;
    }
    total = (int16_t)rd16(data + 4);
    count = (int16_t)rd16(data + 6);
    /* Every number here came from the air. A count larger than was asked
     * for, or than the reply holds, is not read at all: the entries would be
     * whatever followed them in the buffer. */
    if (total < 0 || count < 0 || count > asked || count > MCD_REMOTE_NEIGHBOURS_MAX ||
        count > total || 8 + count * entry > len) {
        return false;
    }
    out->neighbours_total = total;
    out->neighbour_count = count;
    for (i = 0; i < count; i++) {
        const uint8_t* e = data + 8 + i * entry;
        mcd_neighbour* nb = &out->neighbours[i];

        memset(nb, 0, sizeof(*nb));
        memcpy(nb->prefix, e, (size_t)prefix_len);
        nb->heard_secs_ago = rd32(e + prefix_len);
        nb->snr_x4 = (int8_t)e[prefix_len + 4];
    }
    return true;
}

void decodeText(const uint8_t* data, int len, int offset, char* out, size_t out_len)
{
    size_t n = 0;

    if (out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (data == NULL || len <= offset) {
        return;
    }
    /* Up to the first NUL: the decrypted payload is padded to the AES block
     * with zeroes, and the padding is not text. */
    while (offset + (int)n < len && data[offset + n] != 0 && n + 1 < out_len) {
        n++;
    }
    memcpy(out, data + offset, n);
    out[n] = '\0';
}

uint32_t waitFor(uint32_t est_ms)
{
    uint64_t wait = (uint64_t)est_ms * 2 + 8000;

    if (wait < MCD_REMOTE_MIN_WAIT_MS) {
        wait = MCD_REMOTE_MIN_WAIT_MS;
    }
    if (wait > 600000) {
        wait = 600000; /* an estimate this long is a fault; ten minutes is plenty */
    }
    return (uint32_t)wait;
}

/* ---- the session ------------------------------------------------------- */

Session::Session() : _tag(0), _last_tag(0), _next_id(0), _login_sent_ms(0)
{
    memset(&_s, 0, sizeof(_s));
}

void Session::state(mcd_remote_session* out) const
{
    if (out) {
        *out = _s;
    }
}

bool Session::isTarget(const uint8_t* key) const
{
    return _s.active && key != NULL && memcmp(_s.key, key, MCD_PUB_KEY_LEN) == 0;
}

bool Session::loggedIn(const uint8_t* key) const
{
    return isTarget(key) && _s.login == MCD_LOGIN_OK;
}

bool Session::knownGuest() const
{
    /* Only a modern answer says; the legacy "OK" carries no permissions, so
     * it is not called a guest - the repeater decides, as it always does. */
    return _s.login == MCD_LOGIN_OK && !_s.legacy && !_s.admin;
}

void Session::finishInto(mcd_remote_reply* r, mcd_remote_outcome o, uint64_t now_ms)
{
    memset(r, 0, sizeof(*r));
    r->request_id = _s.pending_id;
    r->kind = _s.pending;
    r->outcome = o;
    memcpy(r->key, _s.key, MCD_PUB_KEY_LEN);
    r->mono_ms = now_ms;
    _s.pending = MCD_REMOTE_NONE;
    _s.deadline_mono_ms = 0;
    _tag = 0;
}

bool Session::retarget(const uint8_t* key, mcd_remote_reply* cancelled, uint64_t now_ms)
{
    bool had = false;

    if (isTarget(key)) {
        return false;
    }
    if (_s.active) {
        had = end(cancelled, now_ms);
    }
    _s.active = true;
    memcpy(_s.key, key, MCD_PUB_KEY_LEN);
    return had;
}

uint64_t Session::begin(mcd_remote_kind kind, uint32_t tag, uint32_t wait_ms, uint64_t now_ms)
{
    _s.pending = kind;
    _s.pending_id = ++_next_id;
    _s.deadline_mono_ms = now_ms + wait_ms;
    _tag = tag;
    if (kind == MCD_REMOTE_LOGIN) {
        _s.login = MCD_LOGIN_WAITING;
        _s.legacy = false;
        _s.admin = false;
        _s.permissions = 0;
        _s.acl = 0;
        _s.fw_level = 0;
        _s.login_mono_ms = now_ms;
        _login_sent_ms = now_ms ? now_ms : 1;
    }
    return _s.pending_id;
}

void Session::loginOk(const uint8_t* data, int len, uint64_t now_ms)
{
    _s.login = MCD_LOGIN_OK;
    _s.login_mono_ms = now_ms;
    _login_sent_ms = 0;
    /* A login answer starts with the repeater's own clock rather than a tag
     * (simple_repeater handleLoginReq), which is the one time this node
     * learns it without asking. */
    _s.server_clock_known = true;
    _s.server_clock = rd32(data);
    if (isLoginOk(data, len)) {
        _s.legacy = false;
        _s.admin = (data[6] & 1) != 0;
        _s.permissions = data[6];
        _s.acl = data[7];
        _s.fw_level = data[12];
    } else {
        _s.legacy = true;
    }
}

Verdict Session::onResponse(const uint8_t* key, const uint8_t* data, int len, uint64_t now_ms,
                            mcd_remote_reply* reply)
{
    uint32_t tag;

    if (!isTarget(key)) {
        return V_IGNORED;
    }
    if (data == NULL || len < 4) {
        _s.malformed_replies++;
        return V_MALFORMED;
    }
    tag = rd32(data);
    if (_s.pending == MCD_REMOTE_LOGIN) {
        /* An answer to the request before this login, whose tag we still
         * know, is late - and is tested FIRST, because a login answer is
         * recognised by its shape alone and a status reply whose battery
         * reading has a zero low byte has exactly that shape. */
        if (lastTagIs(tag)) {
            _s.stale_replies++;
            return V_STALE;
        }
        if (isLoginOk(data, len) || isLegacyOk(data, len)) {
            mcd_remote_reply r;

            loginOk(data, len, now_ms);
            finishInto(&r, MCD_REMOTE_REPLIED, now_ms);
            *reply = r;
            return V_ANSWER;
        }
        /* A login answer carries no tag of ours, so anything else from the
         * target while a login waits is - by upstream's companion rule - the
         * refusal. */
        {
            mcd_remote_reply r;

            _s.login = MCD_LOGIN_REFUSED;
            _login_sent_ms = 0;
            finishInto(&r, MCD_REMOTE_REFUSED, now_ms);
            *reply = r;
            return V_ANSWER;
        }
    }
    if (_s.pending == MCD_REMOTE_NONE) {
        /* Upstream RIFT's late login: seen on a device, a login answered at
         * 28 s on a seven-hop route, after the wait had said no answer. Only
         * an OK, only from the node the login went to, only while nothing
         * else waits, and only for a while. */
        if (_login_sent_ms != 0 && _s.login != MCD_LOGIN_OK && !lastTagIs(tag) &&
            now_ms - _login_sent_ms < MCD_REMOTE_LATE_LOGIN_MS &&
            (isLoginOk(data, len) || isLegacyOk(data, len))) {
            memset(reply, 0, sizeof(*reply));
            reply->request_id = _s.pending_id;
            reply->kind = MCD_REMOTE_LOGIN;
            reply->outcome = MCD_REMOTE_REPLIED;
            reply->late = true;
            memcpy(reply->key, _s.key, MCD_PUB_KEY_LEN);
            reply->mono_ms = now_ms;
            loginOk(data, len, now_ms);
            return V_LATE_LOGIN;
        }
        _s.stale_replies++;
        return V_STALE;
    }
    if (_s.pending == MCD_REMOTE_CLI || tag != _tag) {
        /* A CLI answer is command data, never a RESPONSE; and a tag that is
         * not the outstanding one answers something that already ended. */
        _s.stale_replies++;
        return V_STALE;
    }
    {
        mcd_remote_reply r;
        bool ok = false;

        memset(&r, 0, sizeof(r));
        switch (_s.pending) {
        case MCD_REMOTE_STATUS:
            ok = decodeStats(data, len, &r.stats);
            break;
        case MCD_REMOTE_NEIGHBOURS:
            ok = decodeNeighbours(data, len, MCD_REMOTE_NEIGHBOUR_PREFIX,
                                  MCD_REMOTE_NEIGHBOURS_MAX, &r);
            break;
        case MCD_REMOTE_OWNER:
            decodeText(data, len, 4, r.text, sizeof(r.text));
            ok = true;
            break;
        default:
            break;
        }
        if (!ok) {
            /* Matched and unreadable: ignored, and the request keeps
             * waiting - a good answer may still come, and if not, the
             * timeout says so. */
            _s.malformed_replies++;
            return V_MALFORMED;
        }
        {
            mcd_remote_reply done;

            finishInto(&done, MCD_REMOTE_REPLIED, now_ms);
            r.request_id = done.request_id;
            r.kind = done.kind;
            r.outcome = done.outcome;
            memcpy(r.key, done.key, MCD_PUB_KEY_LEN);
            r.mono_ms = now_ms;
            *reply = r;
        }
        _last_tag = tag;
        return V_ANSWER;
    }
}

Verdict Session::onCommandData(const uint8_t* key, const char* text, uint64_t now_ms,
                               mcd_remote_reply* reply)
{
    mcd_remote_reply r;

    if (!isTarget(key)) {
        return V_IGNORED;
    }
    if (_s.pending != MCD_REMOTE_CLI) {
        /* No tag to match on, so a reply with no command waiting is one this
         * session cannot place - a late one, or somebody else's. */
        _s.stale_replies++;
        return V_STALE;
    }
    finishInto(&r, MCD_REMOTE_REPLIED, now_ms);
    snprintf(r.text, sizeof(r.text), "%s", text ? text : "");
    *reply = r;
    return V_ANSWER;
}

bool Session::expire(uint64_t now_ms, mcd_remote_reply* reply)
{
    if (_s.pending == MCD_REMOTE_NONE || now_ms < _s.deadline_mono_ms) {
        return false;
    }
    if (_s.pending == MCD_REMOTE_LOGIN) {
        /* _login_sent_ms stays: a late OK may still be taken. */
        _s.login = MCD_LOGIN_TIMEOUT;
    } else if (_tag != 0) {
        _last_tag = _tag;
    }
    finishInto(reply, MCD_REMOTE_TIMED_OUT, now_ms);
    return true;
}

bool Session::end(mcd_remote_reply* cancelled, uint64_t now_ms)
{
    bool had = _s.pending != MCD_REMOTE_NONE;

    if (had && cancelled) {
        finishInto(cancelled, MCD_REMOTE_CANCELLED, now_ms);
    }
    memset(&_s, 0, sizeof(_s));
    _tag = 0;
    _last_tag = 0;
    _login_sent_ms = 0;
    return had && cancelled;
}

/* ---- the discovery round ---------------------------------------------- */

Discovery::Discovery() : _count(0), _tag(0)
{
    memset(_seen, 0, sizeof(_seen));
    memset(&_st, 0, sizeof(_st));
}

void Discovery::state(mcd_discover_state* out) const
{
    if (out) {
        *out = _st;
    }
}

bool Discovery::open(uint64_t now_ms) const
{
    return _st.open && now_ms < _st.until_mono_ms;
}

void Discovery::begin(uint32_t tag, uint64_t now_ms)
{
    _tag = tag;
    _st.round++;
    _st.open = true;
    _st.started_mono_ms = now_ms;
    _st.until_mono_ms = now_ms + MCD_DISCOVER_WINDOW_MS;
}

bool Discovery::onResponse(const uint8_t* payload, int len, const uint8_t* self_key,
                           const mcd_rx_meta* meta, bool meta_known, uint64_t now_ms,
                           mcd_discovered* entry)
{
    const uint8_t* key;
    int slot = -1;
    int i;

    if (!open(now_ms) || payload == NULL || len < 6 + MCD_PUB_KEY_LEN) {
        return false;
    }
    /* The responder's own layout (simple_repeater onControlDataRecv): type
     * in the high nibble, node type in the low, then its SNR of our request
     * times four, our tag, and its whole key. Repeaters only: the request
     * asked for them, and anything else answering is not this round's. */
    if ((payload[0] & 0xF0) != CTL_DISCOVER_RESP ||
        (payload[0] & 0x0F) != ADV_TYPE_REPEATER_BYTE || rd32(payload + 2) != _tag) {
        return false;
    }
    key = payload + 6;
    if (self_key != NULL && memcmp(key, self_key, MCD_PUB_KEY_LEN) == 0) {
        return false;
    }
    /* One row per node: a repeater can answer twice if it heard the request
     * twice, and two rows for one node read as two repeaters. */
    for (i = 0; i < _count; i++) {
        if (memcmp(_seen[i].public_key, key, MCD_PUB_KEY_LEN) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && _count < MCD_DISCOVER_MAX) {
        slot = _count++;
    }
    if (slot < 0) {
        /* Full: the oldest answer from an EARLIER round gives way. Answers to
         * this round are never evicted by more answers to it. */
        for (i = 0; i < _count; i++) {
            if (_seen[i].round != _st.round &&
                (slot < 0 || _seen[i].mono_ms < _seen[slot].mono_ms)) {
                slot = i;
            }
        }
        if (slot < 0) {
            return false;
        }
    }
    memset(&_seen[slot], 0, sizeof(_seen[slot]));
    memcpy(_seen[slot].public_key, key, MCD_PUB_KEY_LEN);
    _seen[slot].round = _st.round;
    _seen[slot].mono_ms = now_ms;
    _seen[slot].their_snr_db = (double)(int8_t)payload[1] / 4.0;
    if (meta_known && meta != NULL) {
        _seen[slot].snr_known = meta->snr_known;
        _seen[slot].snr_db = meta->snr_db;
        _seen[slot].rssi_known = meta->rssi_known;
        _seen[slot].rssi_dbm = meta->rssi_dbm;
        if (meta->mono_ms != 0) {
            _seen[slot].mono_ms = meta->mono_ms;
        }
    }
    if (entry) {
        *entry = _seen[slot];
    }
    return true;
}

bool Discovery::expire(uint64_t now_ms)
{
    if (!_st.open || now_ms < _st.until_mono_ms) {
        return false;
    }
    _st.open = false;
    _tag = 0;
    return true;
}

int Discovery::list(mcd_discovered* out, int max) const
{
    int order[MCD_DISCOVER_MAX];
    int n = 0;
    int i;
    int j;

    if (out == NULL || max <= 0) {
        return 0;
    }
    for (i = 0; i < _count; i++) {
        order[i] = i;
    }
    /* Newest answer first; sixteen at most, so a plain insertion sort. */
    for (i = 1; i < _count; i++) {
        int k = order[i];

        for (j = i; j > 0 && _seen[order[j - 1]].mono_ms < _seen[k].mono_ms; j--) {
            order[j] = order[j - 1];
        }
        order[j] = k;
    }
    for (i = 0; i < _count && n < max; i++) {
        out[n++] = _seen[order[i]];
    }
    return n;
}

}  // namespace mcdremote
