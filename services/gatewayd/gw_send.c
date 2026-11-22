/*
 * gatewayd: sending. See gw_send.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_send.h"

#include "gw_util.h"

#include <ctype.h>
#include <math.h>
#include <string.h>
#include <strings.h>

/* ---- the request -------------------------------------------------------- */

static bool only(const cJSON *params, const char *const *names)
{
    const cJSON *el;
    size_t i;

    cJSON_ArrayForEach(el, params) {
        for (i = 0; names[i] != NULL; i++) {
            if (el->string != NULL && strcmp(el->string, names[i]) == 0) {
                break;
            }
        }
        if (names[i] == NULL) {
            return false;
        }
    }
    return true;
}

static bool whole(const cJSON *v, double lo, double hi)
{
    return cJSON_IsNumber(v) && v->valuedouble >= lo && v->valuedouble <= hi &&
           v->valuedouble == floor(v->valuedouble);
}

static bool key_ok(const cJSON *v)
{
    size_t i;

    if (!cJSON_IsString(v) || strlen(v->valuestring) != GW_FP_HEX) {
        return false;
    }
    for (i = 0; i < GW_FP_HEX; i++) {
        if (!isxdigit((unsigned char)v->valuestring[i])) {
            return false;
        }
    }
    return true;
}

static bool cmid_ok(const cJSON *v)
{
    size_t i, n;

    if (!cJSON_IsString(v)) {
        return false;
    }
    n = strlen(v->valuestring);
    if (n < GW_CMID_MIN || n > GW_CMID_MAX) {
        return false;
    }
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)v->valuestring[i];

        if (!isalnum(ch) && ch != '-' && ch != '_') {
            return false;
        }
    }
    return true;
}

/* A message is text: well-formed UTF-8, 1 to GW_SEND_TEXT_MAX bytes, and no
 * control character but newline and tab - not the C0 set, not DEL and not
 * the C1 set (U+0080 to U+009F), which a terminal or a panel would act on. */
static bool text_ok(const cJSON *v)
{
    const unsigned char *p;
    size_t i, n;

    if (!cJSON_IsString(v)) {
        return false;
    }
    p = (const unsigned char *)v->valuestring;
    n = strlen(v->valuestring);
    if (n < 1 || n > GW_SEND_TEXT_MAX || !gw_utf8_ok(p, n)) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if ((p[i] < 0x20 && p[i] != '\n' && p[i] != '\t') || p[i] == 0x7f ||
            (p[i] == 0xc2 && p[i + 1] >= 0x80 && p[i + 1] <= 0x9f)) {
            return false;
        }
    }
    return true;
}

static void mix(uint64_t *h, const void *p, size_t n)
{
    const unsigned char *b = p;
    size_t i;

    for (i = 0; i < n; i++) {
        *h = (*h ^ b[i]) * 1099511628211ull;
    }
    *h = (*h ^ 0xffu) * 1099511628211ull; /* a boundary: "ab","c" is not "a","bc" */
}

int gw_send_parse(const cJSON *params, bool resend, struct gw_send_req *q)
{
    static const char *const send_names[] = {"to", "channel", "text", "client_msg_id", "via", NULL};
    static const char *const resend_names[] = {"message_id", "run_id", "client_msg_id", NULL};
    const cJSON *cmid, *to, *chan, *text, *via, *el, *mid, *run;
    uint64_t h = 1469598103934665603ull;
    size_t i;

    memset(q, 0, sizeof(*q));
    q->channel = -1;
    q->resend = resend;
    if (!cJSON_IsObject(params) || !only(params, resend ? resend_names : send_names)) {
        return -1;
    }
    cmid = cJSON_GetObjectItemCaseSensitive(params, "client_msg_id");
    if (!cmid_ok(cmid)) {
        return -1;
    }
    memcpy(q->cmid, cmid->valuestring, strlen(cmid->valuestring) + 1);
    mix(&h, resend ? "r" : "s", 1);
    if (resend) {
        mid = cJSON_GetObjectItemCaseSensitive(params, "message_id");
        run = cJSON_GetObjectItemCaseSensitive(params, "run_id");
        if (!whole(mid, 1, GW_MSG_ID_MAX) || !cJSON_IsString(run) || !gw_ident_ok(run->valuestring)) {
            return -1;
        }
        q->message_id = mid->valuedouble;
        memcpy(q->run_id, run->valuestring, strlen(run->valuestring) + 1);
        mix(&h, &q->message_id, sizeof(q->message_id));
        mix(&h, q->run_id, strlen(q->run_id));
        q->digest = h;
        return 0;
    }
    to = cJSON_GetObjectItemCaseSensitive(params, "to");
    chan = cJSON_GetObjectItemCaseSensitive(params, "channel");
    text = cJSON_GetObjectItemCaseSensitive(params, "text");
    via = cJSON_GetObjectItemCaseSensitive(params, "via");
    /* Exactly one destination: both is a mistake, not a precedence. */
    if ((to == NULL) == (chan == NULL) || !text_ok(text)) {
        return -1;
    }
    if (to != NULL) {
        if (!key_ok(to)) {
            return -1;
        }
        for (i = 0; i < GW_FP_HEX; i++) {
            q->to[i] = (char)tolower((unsigned char)to->valuestring[i]);
        }
    } else {
        if (!whole(chan, 0, GW_SEND_CHANNEL_MAX) || via != NULL) {
            return -1;
        }
        q->channel = (int)chan->valuedouble;
    }
    memcpy(q->text, text->valuestring, strlen(text->valuestring) + 1);
    mix(&h, q->to, strlen(q->to));
    mix(&h, &q->channel, sizeof(q->channel));
    mix(&h, q->text, strlen(q->text));
    if (via != NULL) {
        /* An array, even an empty one: null is not "no route chosen". */
        if (!cJSON_IsArray(via) || cJSON_GetArraySize(via) > GW_SEND_VIA_MAX) {
            return -1;
        }
        q->has_via = true;
        mix(&h, "v", 1);
        cJSON_ArrayForEach(el, via) {
            char low[GW_FP_HEX];

            if (!key_ok(el)) {
                return -1;
            }
            for (i = 0; i < GW_FP_HEX; i++) {
                low[i] = (char)tolower((unsigned char)el->valuestring[i]);
            }
            mix(&h, low, sizeof(low));
        }
    }
    q->digest = h;
    return 0;
}

cJSON *gw_send_upstream(const struct gw_send_req *q, const cJSON *params)
{
    cJSON *o = cJSON_CreateObject(), *arr;
    const cJSON *el;
    bool ok;

    if (o == NULL) {
        return NULL;
    }
    if (q->resend) {
        ok = cJSON_AddNumberToObject(o, "resend", q->message_id) != NULL;
    } else {
        ok = (q->channel >= 0 ? cJSON_AddNumberToObject(o, "channel", q->channel) != NULL
                              : cJSON_AddStringToObject(o, "to", q->to) != NULL) &&
             cJSON_AddStringToObject(o, "text", q->text) != NULL;
        if (ok && q->has_via) {
            arr = cJSON_AddArrayToObject(o, "via");
            ok = arr != NULL;
            cJSON_ArrayForEach(el, cJSON_GetObjectItemCaseSensitive(params, "via")) {
                cJSON *k = ok && cJSON_IsString(el) ? cJSON_CreateString(el->valuestring) : NULL;

                if (k == NULL || !cJSON_AddItemToArray(arr, k)) {
                    cJSON_Delete(k);
                    ok = false;
                    break;
                }
            }
        }
    }
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

/* ---- the records -------------------------------------------------------- */

static void rec_clear(struct gw_send_rec *r)
{
    cJSON_Delete(r->result);
    cJSON_Delete(r->up);
    memset(r, 0, sizeof(*r));
}

static void client_clear(struct gw_send_client *c)
{
    int i;

    for (i = 0; i < GW_SEND_RECORDS; i++) {
        rec_clear(&c->r[i]);
    }
    memset(c, 0, sizeof(*c));
}

void gw_send_init(struct gw_send *gs)
{
    memset(gs, 0, sizeof(*gs));
}

void gw_send_free(struct gw_send *gs)
{
    int i;

    for (i = 0; i < GW_MAX_CLIENTS; i++) {
        client_clear(&gs->c[i]);
    }
}

struct gw_send_client *gw_send_client(struct gw_send *gs, const uint8_t fp[GW_FP_LEN],
                                      bool (*paired)(const uint8_t fp[GW_FP_LEN]))
{
    struct gw_send_client *spare = NULL;
    int i, pass;

    for (pass = 0; pass < 2 && spare == NULL; pass++) {
        for (i = 0; i < GW_MAX_CLIENTS; i++) {
            struct gw_send_client *c = &gs->c[i];

            if (c->used && memcmp(c->fp, fp, GW_FP_LEN) == 0) {
                return c;
            }
            if (c->used && pass == 1 && paired != NULL && !paired(c->fp)) {
                client_clear(c); /* a client that was removed: its records go when room is needed */
            }
            if (!c->used && spare == NULL) {
                spare = c;
            }
        }
    }
    if (spare != NULL) {
        spare->used = true;
        memcpy(spare->fp, fp, GW_FP_LEN);
        spare->next_order = 1;
    }
    return spare;
}

struct gw_send_rec *gw_send_by_tag(struct gw_send *gs, uint64_t tag, struct gw_send_client **cl)
{
    int i, k;

    for (i = 0; i < GW_MAX_CLIENTS; i++) {
        for (k = 0; gs->c[i].used && k < GW_SEND_RECORDS; k++) {
            struct gw_send_rec *r = &gs->c[i].r[k];

            if ((r->state == GW_SS_PENDING || r->state == GW_SS_RESOLVING) && r->tag == tag) {
                if (cl != NULL) {
                    *cl = &gs->c[i];
                }
                return r;
            }
        }
    }
    return NULL;
}

int gw_send_inflight(const struct gw_send_client *c)
{
    int k, n = 0;

    for (k = 0; k < GW_SEND_RECORDS; k++) {
        n += c->r[k].state == GW_SS_PENDING || c->r[k].state == GW_SS_RESOLVING;
    }
    return n;
}

/* The sends of the last minute; true when one more fits. Counts nothing. */
static bool rate_room(struct gw_send_client *c, int64_t now_ms)
{
    int i, kept = 0;

    for (i = 0; i < c->win_n; i++) {
        if (now_ms - c->win[i] < GW_SEND_WINDOW_MS) {
            c->win[kept++] = c->win[i];
        }
    }
    c->win_n = kept;
    return kept < GW_SEND_PER_MIN;
}

static struct gw_send_rec *rec_find(struct gw_send_client *c, const char *cmid)
{
    int k;

    for (k = 0; k < GW_SEND_RECORDS; k++) {
        if (c->r[k].state != GW_SS_FREE && strcmp(c->r[k].cmid, cmid) == 0) {
            return &c->r[k];
        }
    }
    return NULL;
}

/* A free record, or the oldest that waits for nothing. A record whose
 * outcome is settled (DONE) goes before one whose outcome is not: forgetting
 * an UNCERTAIN or UNKNOWN id would let a repeat of it through as new, so
 * those are given up only when every record is one. */
static struct gw_send_rec *rec_room(struct gw_send_client *c)
{
    struct gw_send_rec *settled = NULL, *open = NULL;
    int k;

    for (k = 0; k < GW_SEND_RECORDS; k++) {
        struct gw_send_rec *r = &c->r[k];

        if (r->state == GW_SS_FREE) {
            return r;
        }
        if (r->state == GW_SS_DONE) {
            if (settled == NULL || r->order < settled->order) {
                settled = r;
            }
        } else if (r->state != GW_SS_PENDING && r->state != GW_SS_RESOLVING &&
                   (open == NULL || r->order < open->order)) {
            open = r;
        }
    }
    return settled != NULL ? settled : open;
}

enum gw_send_action gw_send_begin(struct gw_send_client *c, const struct gw_send_req *q, const char *run_id,
                                  double id_before, int64_t now_ms, struct gw_send_rec **rec)
{
    struct gw_send_rec *r = rec_find(c, q->cmid);

    *rec = r;
    if (r != NULL) {
        if (r->digest != q->digest) {
            return GW_SA_MISMATCH;
        }
        switch (r->state) {
        case GW_SS_DONE:
            return GW_SA_REPLY;
        case GW_SS_UNKNOWN:
            return GW_SA_UNKNOWN;
        case GW_SS_PENDING:
        case GW_SS_RESOLVING:
            return GW_SA_IN_PROGRESS;
        default:
            break;
        }
        /* UNCERTAIN: find out before anything is sent. */
        if (run_id == NULL) {
            return GW_SA_UNAVAILABLE;
        }
        if (strcmp(r->run_id, run_id) != 0) {
            /* meshcored started again: what it took before is gone with
             * its list, and may or may not have gone on the air. */
            r->state = GW_SS_UNKNOWN;
            return GW_SA_UNKNOWN;
        }
        if (gw_send_inflight(c) >= GW_SEND_INFLIGHT) {
            return GW_SA_BUSY;
        }
        if (!rate_room(c, now_ms)) {
            return GW_SA_RATE;
        }
        c->win[c->win_n++] = now_ms;
        r->state = GW_SS_RESOLVING;
        return GW_SA_LOOKUP;
    }
    if (run_id == NULL) {
        return GW_SA_UNAVAILABLE;
    }
    if (q->resend && strcmp(q->run_id, run_id) != 0) {
        return GW_SA_STALE_RUN;
    }
    if (gw_send_inflight(c) >= GW_SEND_INFLIGHT) {
        return GW_SA_BUSY;
    }
    if (!rate_room(c, now_ms)) {
        return GW_SA_RATE;
    }
    r = rec_room(c);
    if (r == NULL || strlen(run_id) > GW_RUN_ID_MAX) {
        return GW_SA_BUSY;
    }
    c->win[c->win_n++] = now_ms;
    rec_clear(r);
    r->state = GW_SS_PENDING;
    memcpy(r->cmid, q->cmid, sizeof(r->cmid));
    r->digest = q->digest;
    r->resend = q->resend;
    memcpy(r->to, q->to, sizeof(r->to));
    r->channel = q->channel;
    memcpy(r->text, q->text, sizeof(r->text));
    r->message_id = q->message_id;
    r->id_before = id_before;
    r->attempts_before = -1;
    memcpy(r->run_id, run_id, strlen(run_id) + 1);
    r->order = c->next_order++;
    *rec = r;
    return GW_SA_FORWARD;
}

/* ---- meshcored's answers ------------------------------------------------ */

static bool str_is(const cJSON *v, const char *s)
{
    return cJSON_IsString(v) && strcmp(v->valuestring, s) == 0;
}

static void done(struct gw_send_rec *rec, cJSON *result, double message_id)
{
    cJSON_Delete(rec->up);
    rec->up = NULL;
    cJSON_Delete(rec->result);
    rec->result = result;
    rec->message_id = message_id;
    rec->state = GW_SS_DONE;
}

enum gw_send_outcome gw_send_answered(struct gw_send_rec *rec, const cJSON *result, int code)
{
    const cJSON *id, *v;
    cJSON *o;

    if (result == NULL) {
        if (code >= 1 && code <= 6) {
            /* Its own refusal: it read the request and sent nothing. */
            rec_clear(rec);
            return GW_SO_REFUSED;
        }
        rec->state = GW_SS_UNCERTAIN;
        return GW_SO_UNCERTAIN;
    }
    id = cJSON_GetObjectItemCaseSensitive(result, "message_id");
    o = cJSON_CreateObject();
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "accepted")) || !whole(id, 1, GW_MSG_ID_MAX) ||
        (rec->resend && id->valuedouble != rec->message_id) || o == NULL) {
        /* It answered, and not with a refusal: something may have been
         * sent, and nothing here says what. Never sent again. */
        cJSON_Delete(o);
        rec->state = GW_SS_UNKNOWN;
        return GW_SO_UNKNOWN;
    }
    cJSON_AddBoolToObject(o, "accepted", 1);
    cJSON_AddNumberToObject(o, "message_id", id->valuedouble);
    cJSON_AddStringToObject(o, "run_id", rec->run_id);
    v = cJSON_GetObjectItemCaseSensitive(result, "route");
    if (str_is(v, "flood") || str_is(v, "direct") || str_is(v, "explicit")) {
        cJSON_AddStringToObject(o, "route", v->valuestring);
    }
    v = cJSON_GetObjectItemCaseSensitive(result, "ack_expected");
    if (cJSON_IsBool(v)) {
        cJSON_AddBoolToObject(o, "ack_expected", cJSON_IsTrue(v));
    }
    v = cJSON_GetObjectItemCaseSensitive(result, "ack_timeout_ms");
    if (whole(v, 0, 86400000.0)) {
        cJSON_AddNumberToObject(o, "ack_timeout_ms", v->valuedouble);
    }
    v = cJSON_GetObjectItemCaseSensitive(result, "channel");
    if (whole(v, 0, GW_SEND_CHANNEL_MAX)) {
        cJSON_AddNumberToObject(o, "channel", v->valuedouble);
    }
    if (rec->resend) {
        cJSON_AddBoolToObject(o, "resent", 1);
    }
    if (!cJSON_HasObjectItem(o, "message_id") || !cJSON_HasObjectItem(o, "run_id")) {
        cJSON_Delete(o); /* no memory: accepted, and nothing to answer with */
        rec->state = GW_SS_UNKNOWN;
        return GW_SO_UNKNOWN;
    }
    done(rec, o, id->valuedouble);
    return GW_SO_DONE;
}

/* Is this id the message of another record of this run? Two sends of the
 * same words to the same place are two messages: the second must not be
 * taken for the first. */
static bool claimed(const struct gw_send *gs, const struct gw_send_rec *me, double id)
{
    int i, k;

    for (i = 0; i < GW_MAX_CLIENTS; i++) {
        for (k = 0; gs->c[i].used && k < GW_SEND_RECORDS; k++) {
            const struct gw_send_rec *r = &gs->c[i].r[k];

            if (r != me && r->state == GW_SS_DONE && !r->resend && r->message_id == id &&
                strcmp(r->run_id, me->run_id) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool text_matches(const struct gw_send_rec *rec, const cJSON *msg)
{
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(msg, "text");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(msg, "sender_name");
    size_t k;

    if (!cJSON_IsString(text)) {
        return false;
    }
    if (strcmp(text->valuestring, rec->text) == 0) {
        return true;
    }
    /* A channel message is reported as "<sender_name>: <text>": exactly
     * that, with the name meshcored reports beside it, and not any text
     * that merely ends the same way. */
    if (rec->channel < 0 || !cJSON_IsString(name)) {
        return false;
    }
    k = strlen(name->valuestring);
    return strncmp(text->valuestring, name->valuestring, k) == 0 && text->valuestring[k] == ':' &&
           text->valuestring[k + 1] == ' ' && strcmp(text->valuestring + k + 2, rec->text) == 0;
}

/* The answer a client gets for a send whose own answer never came, from the
 * message meshcored holds. */
static cJSON *result_of_message(const struct gw_send_rec *rec, const cJSON *msg, double id)
{
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(msg, "state");
    const cJSON *route = cJSON_GetObjectItemCaseSensitive(msg, "route");
    const cJSON *ack = cJSON_GetObjectItemCaseSensitive(msg, "ack_expected");
    cJSON *o = cJSON_CreateObject();

    if (o == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(o, "accepted", 1);
    cJSON_AddNumberToObject(o, "message_id", id);
    cJSON_AddStringToObject(o, "run_id", rec->run_id);
    if (str_is(route, "explicit")) {
        cJSON_AddStringToObject(o, "route", "explicit");
    } else if (str_is(state, "sent_flood") || str_is(state, "sent_direct")) {
        cJSON_AddStringToObject(o, "route", state->valuestring + 5);
    }
    if (cJSON_IsBool(ack)) {
        cJSON_AddBoolToObject(o, "ack_expected", cJSON_IsTrue(ack));
    }
    if (rec->channel >= 0) {
        cJSON_AddNumberToObject(o, "channel", rec->channel);
    }
    if (rec->resend) {
        cJSON_AddBoolToObject(o, "resent", 1);
    }
    return o;
}

enum gw_send_look gw_send_looked(struct gw_send *gs, struct gw_send_rec *rec, const cJSON *messages)
{
    const cJSON *el, *best = NULL;
    bool older = false, unclear = false;
    double best_id = 0;
    int newer = 0;
    cJSON *o;

    if (!cJSON_IsArray(messages)) {
        rec->state = GW_SS_UNKNOWN;
        return GW_SL_UNKNOWN;
    }
    cJSON_ArrayForEach(el, messages) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(el, "id");
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(el, "kind");
        bool channel = str_is(kind, "channel"), here;
        const cJSON *v;

        if (!whole(id, 1, GW_MSG_ID_MAX) || (!channel && !str_is(kind, "direct"))) {
            unclear = true; /* not a list this can be read from */
            continue;
        }
        if (rec->resend) {
            if (id->valuedouble == rec->message_id &&
                str_is(cJSON_GetObjectItemCaseSensitive(el, "direction"), "out")) {
                best = el;
                best_id = id->valuedouble;
            }
            continue;
        }
        /* meshcored keeps direct and channel messages apart, each kind in a
         * ring of its own: only messages of the record's kind say how far
         * back that ring reaches. */
        if (channel != (rec->channel >= 0)) {
            continue;
        }
        if (id->valuedouble <= rec->id_before) {
            older = true;
            continue;
        }
        newer++;
        if (!str_is(cJSON_GetObjectItemCaseSensitive(el, "direction"), "out") || claimed(gs, rec, id->valuedouble)) {
            continue;
        }
        /* An outgoing message that came after. To another place: not it.
         * To the same place with the same text: it. Anything else - other
         * words (meshcored reports text as it sanitised it), a place that
         * cannot be read - may be it, and then nothing is proven. */
        if (channel) {
            v = cJSON_GetObjectItemCaseSensitive(el, "channel");
            if (whole(v, 0, GW_SEND_CHANNEL_MAX) && v->valuedouble != (double)rec->channel) {
                continue;
            }
            here = whole(v, 0, GW_SEND_CHANNEL_MAX);
        } else {
            v = cJSON_GetObjectItemCaseSensitive(el, "peer_public_key");
            if (key_ok(v) && strcasecmp(v->valuestring, rec->to) != 0) {
                continue;
            }
            here = key_ok(v);
        }
        if (!here || !text_matches(rec, el)) {
            unclear = true;
        } else if (best == NULL || id->valuedouble < best_id) {
            best = el;
            best_id = id->valuedouble;
        }
    }
    if (rec->resend) {
        const cJSON *att = best != NULL ? cJSON_GetObjectItemCaseSensitive(best, "attempts") : NULL;

        /* One more attempt than before the resend was asked for: it was
         * taken. The same number: it was not. Anything else says nothing. */
        if (!cJSON_IsNumber(att) || rec->attempts_before < 0 || att->valuedouble < rec->attempts_before) {
            rec->state = GW_SS_UNKNOWN;
            return GW_SL_UNKNOWN;
        }
        if (att->valuedouble == rec->attempts_before) {
            return GW_SL_ABSENT;
        }
    } else if (best == NULL) {
        /* Not in the list. That proves it was not taken only when the ring
         * of its kind still reaches back to before it was asked for: it
         * holds a message from before (a ring loses its oldest first), or
         * so few from after that it cannot have gone round - and holds
         * something at all if anything had ever been seen. */
        if (!unclear && (older || newer < GW_SEND_RING_MIN) &&
            (rec->id_before == 0 || cJSON_GetArraySize(messages) > 0)) {
            return GW_SL_ABSENT;
        }
        rec->state = GW_SS_UNKNOWN;
        return GW_SL_UNKNOWN;
    }
    o = result_of_message(rec, best, best_id);
    if (o != NULL && (!cJSON_HasObjectItem(o, "message_id") || !cJSON_HasObjectItem(o, "run_id"))) {
        cJSON_Delete(o);
        o = NULL;
    }
    if (o == NULL) {
        rec->state = GW_SS_UNKNOWN;
        return GW_SL_UNKNOWN;
    }
    done(rec, o, best_id);
    return GW_SL_FOUND;
}

void gw_send_unresolved(struct gw_send_rec *rec)
{
    cJSON_Delete(rec->up);
    rec->up = NULL;
    rec->state = GW_SS_UNCERTAIN;
}

void gw_send_release(struct gw_send_rec *rec)
{
    rec_clear(rec);
}

cJSON *gw_send_result(const struct gw_send_rec *rec, bool duplicate)
{
    cJSON *o = rec->result != NULL ? cJSON_Duplicate(rec->result, 1) : NULL;

    if (o != NULL && cJSON_AddBoolToObject(o, "duplicate", duplicate) == NULL) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}
