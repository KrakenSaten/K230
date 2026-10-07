/*
 * PocketFleet multiplayer view model. See fleet_view_mp.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_view_mp.h"

#include "fleet_view.h"

#include <stdio.h>
#include <string.h>

#define DOT "\xc2\xb7"
#define ELLIPSIS "\xe2\x80\xa6"

static int fail(char *buf, size_t n)
{
    if (buf && n > 0) {
        buf[0] = '\0';
    }
    return -1;
}

static const char *who(const char *peer)
{
    return peer && peer[0] ? peer : "Your opponent";
}

static void upper_into(char *dst, size_t n, const char *src)
{
    size_t i;

    for (i = 0; i + 1 < n && src && src[i]; i++) {
        char c = src[i];

        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    if (n) {
        dst[i < n ? i : n - 1] = '\0';
    }
}

/* The header has room for a short name: cut a long one at a whole character
 * and say so. */
#define STATUS_NAME_MAX 16

static void short_name(char *dst, size_t n, const char *src)
{
    char up[FLEET_MATCH_NAME_MAX];
    size_t len;

    upper_into(up, sizeof(up), src);
    len = strlen(up);
    if (len <= STATUS_NAME_MAX) {
        snprintf(dst, n, "%s", up);
        return;
    }
    len = STATUS_NAME_MAX;
    while (len > 0 && ((unsigned char)up[len] & 0xC0) == 0x80) {
        len--;      /* not in the middle of a character */
    }
    snprintf(dst, n, "%.*s" ELLIPSIS, (int)len, up);
}

enum fleet_mp_status fleet_view_mp_state(const struct fleet_match *m, enum fleet_link_state link)
{
    int reachable = link == FLEET_LINK_UP || link == FLEET_LINK_CONNECTING;

    if (!m) {
        return reachable ? FLEET_STATUS_CONNECTING : FLEET_STATUS_OFFLINE;
    }
    if (m->phase == FLEET_MP_REVEAL || m->phase == FLEET_MP_DONE) {
        return FLEET_STATUS_GAME_OVER;
    }
    if (m->phase == FLEET_MP_IDLE || m->phase == FLEET_MP_INVITED) {
        return FLEET_STATUS_IDLE;
    }
    if (!reachable) {
        return FLEET_STATUS_OFFLINE;
    }
    if (link == FLEET_LINK_CONNECTING || m->phase == FLEET_MP_INVITING ||
        m->phase == FLEET_MP_ACCEPTING) {
        return FLEET_STATUS_CONNECTING;
    }
    switch (fleet_match_link(m)) {
    case FLEET_LINK_LOST:
        return FLEET_STATUS_DISCONNECTED;
    case FLEET_LINK_RESYNCING:
        return FLEET_STATUS_SYNCING;
    case FLEET_LINK_RETRYING:
        return FLEET_STATUS_RECONNECTING;
    default:
        break;
    }
    if (m->phase == FLEET_MP_DEPLOY && !m->committed) {
        return FLEET_STATUS_DEPLOYING;
    }
    return fleet_match_my_turn(m) ? FLEET_STATUS_YOUR_TURN : FLEET_STATUS_WAITING;
}

int fleet_view_mp_status(const struct fleet_match *m, enum fleet_link_state link,
                         const char *peer, char *buf, size_t n)
{
    char name[FLEET_MATCH_NAME_MAX + 4];

    if (!buf || n == 0) {
        return fail(buf, n);
    }
    switch (fleet_view_mp_state(m, link)) {
    case FLEET_STATUS_CONNECTING:
        snprintf(buf, n, "CONNECTING");
        break;
    case FLEET_STATUS_OFFLINE:
        snprintf(buf, n, "MESH OFFLINE");
        break;
    case FLEET_STATUS_DEPLOYING:
        snprintf(buf, n, "DEPLOY YOUR FLEET");
        break;
    case FLEET_STATUS_SYNCING:
        snprintf(buf, n, "SYNCING");
        break;
    case FLEET_STATUS_RECONNECTING:
        snprintf(buf, n, "RECONNECTING");
        break;
    case FLEET_STATUS_DISCONNECTED:
        snprintf(buf, n, "OPPONENT DISCONNECTED");
        break;
    case FLEET_STATUS_YOUR_TURN:
        snprintf(buf, n, "YOUR TURN " DOT " SHOT %d",
                 fleet_match_shots_by(m, (enum fleet_mp_role)m->role) + 1);
        break;
    case FLEET_STATUS_WAITING:
        short_name(name, sizeof(name), who(peer));
        snprintf(buf, n, "WAITING FOR %s", name);
        break;
    case FLEET_STATUS_GAME_OVER:
        snprintf(buf, n, "GAME OVER");
        break;
    default:
        snprintf(buf, n, "MULTIPLAYER");
        break;
    }
    return 0;
}

int fleet_view_mp_chat_line(const struct fleet_chat_line *l, const char *peer, char *buf,
                            size_t n)
{
    char name[FLEET_MATCH_NAME_MAX + 4];
    const char *how = "";

    if (!l || !buf || n == 0) {
        return fail(buf, n);
    }
    if (l->mine) {
        snprintf(name, sizeof(name), "YOU");
        switch (l->state) {
        case FLEET_CHAT_QUEUED:
            how = " " DOT " WAITING";
            break;
        case FLEET_CHAT_SENDING:
            how = " " DOT " SENDING";
            break;
        case FLEET_CHAT_FAILED:
            how = " " DOT " NOT DELIVERED";
            break;
        default:
            break;
        }
    } else {
        short_name(name, sizeof(name), peer && peer[0] ? peer : "THEM");
    }
    snprintf(buf, n, "%s " DOT " %s%s", name, l->text, how);
    return 0;
}

int fleet_view_mp_chat_tile(const struct fleet_match *m, const char *peer, char *title,
                            size_t tn, char *preview, size_t pn)
{
    const struct fleet_chat_line *last;

    if (!m || !title || tn == 0 || !preview || pn == 0) {
        fail(preview, pn);
        return fail(title, tn);
    }
    if (m->chat.unread) {
        snprintf(title, tn, "CHAT " DOT " %u NEW", (unsigned)m->chat.unread);
    } else {
        snprintf(title, tn, "CHAT");
    }
    last = fleet_chat_last(&m->chat);
    if (last) {
        return fleet_view_mp_chat_line(last, peer, preview, pn);
    }
    snprintf(preview, pn, "Say something to %s.", who(peer));
    return 0;
}

int fleet_view_mp_note(const struct fleet_match *m, const char *peer, int64_t now_ms,
                       char *buf, size_t n)
{
    char cell[FLEET_CELL_NAME_MAX];

    if (!m || !buf || n == 0) {
        return fail(buf, n);
    }
    buf[0] = '\0';
    switch (fleet_match_link(m)) {
    case FLEET_LINK_LOST:
        snprintf(buf, n, "%s is out of reach. The match is paused, not lost.", who(peer));
        return 0;
    case FLEET_LINK_RESYNCING:
        snprintf(buf, n, "Checking the match with %s" ELLIPSIS, who(peer));
        return 0;
    case FLEET_LINK_THROTTLED:
        snprintf(buf, n, "Holding back to spare the airtime. It will go shortly.");
        return 0;
    default:
        break;
    }
    if (m->phase == FLEET_MP_DEPLOY || m->phase == FLEET_MP_COMMITTED) {
        if (!m->committed) {
            snprintf(buf, n, "Place your fleet.");
        } else {
            snprintf(buf, n, "Waiting for %s to deploy.", who(peer));
        }
        return 0;
    }
    if (m->phase != FLEET_MP_BATTLE) {
        return 0;
    }
    if (m->pending != FLEET_NO_CELL) {
        fleet_cell_name(m->pending / FLEET_GRID, m->pending % FLEET_GRID, cell, sizeof(cell));
        if (fleet_match_link(m) == FLEET_LINK_RETRYING) {
            snprintf(buf, n, "Link problem: no report on %s yet. Retrying (%d of %d).", cell,
                     m->attempts, FLEET_RETRY_MAX);
        } else {
            snprintf(buf, n, "Shot at %s sent. Waiting for the report.", cell);
        }
        return 0;
    }
    if (fleet_match_my_turn(m)) {
        return 0;
    }
    {
        int64_t quiet = now_ms - m->last_heard;

        if (quiet >= 120000) {
            snprintf(buf, n, "%s is aiming. No word for %d min.", who(peer), (int)(quiet / 60000));
        } else {
            snprintf(buf, n, "%s is aiming.", who(peer));
        }
    }
    return 0;
}

/* The most recent ply fired by one side, or 0. */
static int last_by(const struct fleet_match *m, int mine)
{
    int k;

    for (k = m->resolved; k >= 1; k--) {
        if ((fleet_match_shooter(k) == m->role) == (mine != 0)) {
            return k;
        }
    }
    return 0;
}

static void shot_text(const struct fleet_match *m, int k, char *buf, size_t n)
{
    uint8_t res = m->log_res[k];
    int cell = m->log_cell[k];

    fleet_view_shot(cell / FLEET_GRID, cell % FLEET_GRID,
                    (enum fleet_shot_result)fleet_res_outcome(res), fleet_res_ship(res), buf, n);
}

int fleet_view_mp_exchange(const struct fleet_match *m, const char *peer, char *buf, size_t n)
{
    char own[40] = "";
    char theirs[40] = "";
    char name[FLEET_MATCH_NAME_MAX];
    int a;
    int b;

    if (!m || !buf || n == 0) {
        return fail(buf, n);
    }
    a = last_by(m, 1);
    b = last_by(m, 0);
    if (a) {
        shot_text(m, a, own, sizeof(own));
    }
    if (b) {
        shot_text(m, b, theirs, sizeof(theirs));
    }
    upper_into(name, sizeof(name), peer && peer[0] ? peer : "ENEMY");
    if (a && b) {
        snprintf(buf, n, "YOU %s " DOT " %s %s", own, name, theirs);
    } else if (a) {
        snprintf(buf, n, "YOU %s", own);
    } else if (b) {
        snprintf(buf, n, "%s %s", name, theirs);
    } else {
        buf[0] = '\0';
    }
    return 0;
}

int fleet_view_mp_lobby(const struct fleet_match *m, const char *peer, char *buf, size_t n)
{
    if (!m || !buf || n == 0) {
        return fail(buf, n);
    }
    switch (m->phase) {
    case FLEET_MP_INVITING:
        if (m->attempts >= 2) {
            snprintf(buf, n, "Inviting %s" ELLIPSIS " no answer yet (try %d of %d).", who(peer),
                     m->attempts, FLEET_RETRY_MAX);
        } else {
            snprintf(buf, n, "Inviting %s" ELLIPSIS, who(peer));
        }
        return 0;
    case FLEET_MP_INVITED:
        snprintf(buf, n, "%s invites you to an engagement.", who(peer));
        return 0;
    case FLEET_MP_ACCEPTING:
        if (m->notice == FLEET_NOTICE_CROSSED) {
            snprintf(buf, n, "You invited each other. Joining %s's engagement" ELLIPSIS, who(peer));
        } else {
            snprintf(buf, n, "Joining %s's engagement" ELLIPSIS, who(peer));
        }
        return 0;
    default:
        break;
    }
    if (fleet_match_active(m)) {
        snprintf(buf, n, "An engagement with %s is under way.", who(peer));
        return 0;
    }
    switch (m->notice) {
    case FLEET_NOTICE_DECLINED:
        snprintf(buf, n, "The invitation was declined.");
        break;
    case FLEET_NOTICE_BUSY:
        snprintf(buf, n, "That player is already in an engagement.");
        break;
    case FLEET_NOTICE_INCOMPATIBLE:
        snprintf(buf, n, "That player's Fleet cannot play this version.");
        break;
    case FLEET_NOTICE_NO_ANSWER:
        snprintf(buf, n, "No answer. The other device may be out of range, or not in Fleet.");
        break;
    case FLEET_NOTICE_CANCELLED:
        snprintf(buf, n, "The invitation was withdrawn.");
        break;
    default:
        snprintf(buf, n, "Choose a player to invite. They need Fleet open on their device.");
        break;
    }
    return 0;
}

const char *fleet_view_mp_outcome(const struct fleet_match *m)
{
    if (!m || (m->phase != FLEET_MP_DONE && m->phase != FLEET_MP_REVEAL)) {
        return "";
    }
    /* A forfeit sank nothing: say who gave up, not that a fleet went down. */
    switch (m->outcome) {
    case FLEET_OUTCOME_WIN:
        return m->end_reason == FLEET_END_FORFEIT ? "Opponent forfeited" : "Enemy fleet destroyed";
    case FLEET_OUTCOME_LOSS:
        return m->end_reason == FLEET_END_FORFEIT ? "You forfeited" : "Fleet lost";
    default:
        return "No result";
    }
}

const char *fleet_view_mp_ended(const struct fleet_match *m)
{
    if (!m) {
        return "";
    }
    switch (m->end_reason) {
    case FLEET_END_NONE:
        return "ALL SHIPS SUNK";
    case FLEET_END_FORFEIT:
        return m->end_by_me ? "YOU FORFEITED" : "THEY FORFEITED";
    case FLEET_END_VIOLATION:
        return "VOID " DOT " BAD REPORTS";
    case FLEET_END_UNKNOWN:
    case FLEET_END_ABANDON:
        return "VOID " DOT " MATCH LOST";
    case FLEET_END_CANCELLED:
        return "WITHDRAWN";
    default:
        return "VOID " DOT " RECORDS DIFFER";
    }
}

const char *fleet_view_mp_verify(const struct fleet_match *m)
{
    if (!m) {
        return "";
    }
    switch (m->verify) {
    case FLEET_VERIFY_OK:
        return "Verified";
    case FLEET_VERIFY_MISMATCH:
        return "Reports did not match";
    case FLEET_VERIFY_NONE:
        return "Not verified";
    default:
        return m->phase == FLEET_MP_REVEAL ? "Checking" : "\xe2\x80\x94";
    }
}

int fleet_view_mp_accuracy(const struct fleet_match *m, int mine, char *buf, size_t n)
{
    struct fleet_stats st = { 0, 0 };
    int k;

    if (!m || !buf || n == 0) {
        return fail(buf, n);
    }
    for (k = 1; k <= m->resolved; k++) {
        if ((fleet_match_shooter(k) == m->role) == (mine != 0)) {
            st.shots++;
            st.hits += fleet_res_outcome(m->log_res[k]) >= 2;
        }
    }
    return fleet_view_accuracy(&st, buf, n);
}

int fleet_view_mp_peer_row(const struct fleet_link_peer *p, int64_t now_ms, char *buf, size_t n)
{
    char name[FLEET_LINK_NAME_MAX];
    char heard[32];
    char hops[16];

    if (!p || !buf || n == 0) {
        return fail(buf, n);
    }
    upper_into(name, sizeof(name), p->name[0] ? p->name : "UNNAMED");
    if (p->hops < 0) {
        snprintf(hops, sizeof(hops), "NO ROUTE YET");
    } else if (p->hops == 0) {
        snprintf(hops, sizeof(hops), "DIRECT");
    } else {
        snprintf(hops, sizeof(hops), "%d HOP%s", p->hops, p->hops == 1 ? "" : "S");
    }
    if (!p->heard_ms) {
        snprintf(heard, sizeof(heard), "NOT HEARD LATELY");
    } else if (now_ms - p->heard_ms < 60000) {
        snprintf(heard, sizeof(heard), "HEARD JUST NOW");
    } else {
        snprintf(heard, sizeof(heard), "HEARD %d MIN AGO", (int)((now_ms - p->heard_ms) / 60000));
    }
    snprintf(buf, n, "%s " DOT " %s " DOT " %s", name, hops, heard);
    return 0;
}
