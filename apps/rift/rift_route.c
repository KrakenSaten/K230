/*
 * The explicit route of a direct message. See rift_route.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_route.h"

#include "rift_format.h"
#include "rift_json.h"
#include "rift_model.h"

#include <stdio.h>
#include <string.h>

/* The capability is the JSON value true, and nothing else is: a client must
 * require the flag to be explicitly true (docs/api/mesh.md, mesh.info). */
static int cap_true(const cJSON *caps, const char *name)
{
    return cJSON_IsObject(caps) && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(caps, name));
}

void rift_route_apply_info(struct rift_route *r, const cJSON *result)
{
    const cJSON *caps;

    if (!r || !cJSON_IsObject(result)) {
        return;
    }
    caps = cJSON_GetObjectItemCaseSensitive(result, "capabilities");
    r->caps_known = 1;
    r->can_preview = cap_true(caps, "manual_path_preview");
    r->can_explicit = cap_true(caps, "explicit_direct_send");
}

void rift_route_service_lost(struct rift_route *r)
{
    if (!r) {
        return;
    }
    r->caps_known = 0;
    r->can_preview = 0;
    r->can_explicit = 0;
    if (r->draft.check == RIFT_ROUTE_CHECKING) {
        r->draft.check = RIFT_ROUTE_UNANSWERED;
        snprintf(r->draft.why, sizeof(r->draft.why),
                 "meshcored went away before it answered; the route is checked again "
                 "when it is back");
    }
    r->draft.request = 0;
    r->send_check_request = 0;
}

int rift_route_supported(const struct rift_route *r)
{
    return r && r->caps_known && r->can_preview && r->can_explicit;
}

/* ---- the draft ------------------------------------------------------------------ */

static void edited(struct rift_route *r)
{
    struct rift_route_draft *d = &r->draft;

    d->gen++;
    d->check = RIFT_ROUTE_UNCHECKED;
    d->request = 0; /* an answer still out is about the route before this */
    d->why[0] = '\0';
    d->note[0] = '\0';
    memset(&d->enc, 0, sizeof(d->enc));
    if (d->marked >= d->count) {
        d->marked = -1;
    }
}

int rift_route_open(struct rift_route *r, const char *to)
{
    const struct rift_route_stage *s;
    unsigned gen;

    if (!r || !hex_only(to, 64)) {
        return -1;
    }
    gen = r->draft.gen;
    memset(&r->draft, 0, sizeof(r->draft));
    r->draft.gen = gen; /* never reused: a late answer cannot match a new draft */
    r->draft.active = 1;
    r->draft.marked = -1;
    snprintf(r->draft.to, sizeof(r->draft.to), "%s", to);
    s = rift_route_staged(r, to);
    if (s) {
        memcpy(r->draft.via, s->via, sizeof(r->draft.via));
        r->draft.count = s->count;
    }
    edited(r);
    return 0;
}

void rift_route_close(struct rift_route *r)
{
    if (r) {
        r->draft.active = 0;
        r->draft.request = 0;
        r->draft.marked = -1;
    }
}

int rift_route_index(const struct rift_route *r, const char *key)
{
    int i;

    for (i = 0; r && key && i < r->draft.count; i++) {
        if (strcmp(r->draft.via[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

static int say(char *why, size_t len, const char *text)
{
    if (why && len) {
        snprintf(why, len, "%s", text);
    }
    return -1;
}

int rift_route_eligible(const struct rift_route *r, const struct rift_model *m, const char *key,
                        char *why, size_t why_len)
{
    const struct rift_node *n;
    char label[RIFT_LABEL_MAX];
    char text[RIFT_ROUTE_WHY_MAX];

    if (!r || !m || !r->draft.active) {
        return say(why, why_len, "The route builder is not open.");
    }
    n = hex_only(key, 64) ? rift_model_find(m, key) : NULL;
    if (!n) {
        return say(why, why_len, "That node is not in the node list.");
    }
    rift_fmt_label(n, label, sizeof(label));
    if (strcmp(key, r->draft.to) == 0) {
        snprintf(text, sizeof(text), "%s is the recipient, not a relay.", label);
        return say(why, why_len, text);
    }
    if (m->have_identity && strcmp(key, m->self_key) == 0) {
        return say(why, why_len, "This device is the sender, not a relay.");
    }
    if (!rift_node_is_repeater(n)) {
        snprintf(text, sizeof(text), "%s is not a repeater; only repeaters relay.", label);
        return say(why, why_len, text);
    }
    if (rift_route_index(r, key) >= 0) {
        snprintf(text, sizeof(text), "%s is already in the route.", label);
        return say(why, why_len, text);
    }
    if (r->draft.count >= RIFT_ROUTE_MAX) {
        return say(why, why_len, "The route is full: MeshCore carries 63 relays at most.");
    }
    return 0;
}

int rift_route_add(struct rift_route *r, const struct rift_model *m, const char *key)
{
    char why[RIFT_ROUTE_WHY_MAX];

    if (rift_route_eligible(r, m, key, why, sizeof(why)) != 0) {
        if (r) {
            snprintf(r->draft.note, sizeof(r->draft.note), "%s", why);
        }
        return -1;
    }
    snprintf(r->draft.via[r->draft.count], sizeof(r->draft.via[0]), "%s", key);
    r->draft.count++;
    r->draft.marked = -1;
    edited(r);
    return 0;
}

int rift_route_undo(struct rift_route *r)
{
    if (!r || !r->draft.active || r->draft.count == 0) {
        return -1;
    }
    r->draft.count--;
    r->draft.via[r->draft.count][0] = '\0';
    r->draft.marked = -1;
    edited(r);
    return 0;
}

void rift_route_clear(struct rift_route *r)
{
    if (!r || !r->draft.active) {
        return;
    }
    memset(r->draft.via, 0, sizeof(r->draft.via));
    r->draft.count = 0;
    r->draft.marked = -1;
    edited(r);
}

int rift_route_cut(struct rift_route *r, int index)
{
    int i;

    if (!r || !r->draft.active || index < 0 || index >= r->draft.count) {
        return -1;
    }
    for (i = index; i < r->draft.count; i++) {
        r->draft.via[i][0] = '\0';
    }
    r->draft.count = index;
    r->draft.marked = -1;
    edited(r);
    return 0;
}

void rift_route_mark(struct rift_route *r, int index)
{
    if (r) {
        r->draft.marked = (index >= 0 && index < r->draft.count) ? index : -1;
    }
}

int rift_route_missing(const struct rift_route *r, const struct rift_model *m)
{
    int missing = 0;
    int i;

    if (!r || !m) {
        return 0;
    }
    if (!rift_model_find(m, r->draft.to)) {
        missing++;
    }
    for (i = 0; i < r->draft.count; i++) {
        if (!rift_model_find(m, r->draft.via[i])) {
            missing++;
        }
    }
    return missing;
}

int rift_route_wants_check(const struct rift_route *r)
{
    return r && r->draft.active && rift_route_supported(r) && r->draft.request == 0 &&
           (r->draft.check == RIFT_ROUTE_UNCHECKED || r->draft.check == RIFT_ROUTE_UNANSWERED ||
            r->draft.checked_gen != r->draft.gen);
}

void rift_route_check_sent(struct rift_route *r, int request)
{
    if (!r || request <= 0) {
        return;
    }
    r->draft.request = request;
    r->draft.check = RIFT_ROUTE_CHECKING;
    r->draft.checked_gen = r->draft.gen;
    r->draft.why[0] = '\0';
}

int rift_route_parse_preview(const cJSON *result, int relays, struct rift_route_encoding *enc)
{
    const char *route;
    const char *hex;
    double d;

    if (!cJSON_IsObject(result) || !enc) {
        return -1;
    }
    memset(enc, 0, sizeof(*enc));
    route = str_of(result, "route");
    /* The service's own word that this is a valid explicit route and that
     * nothing went on the air: a preview that said otherwise is not one this
     * app will stage. */
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "valid")) || !route ||
        strcmp(route, "explicit") != 0 ||
        !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "transmitted"))) {
        return -1;
    }
    if (!num_of(result, "relays", &d) || d != (double)relays) {
        return -1;
    }
    enc->relays = relays;
    enc->hash_bytes = (num_of(result, "hash_bytes", &d) && d >= 1 && d <= 3) ? (int)d : 0;
    enc->path_bytes = (num_of(result, "path_bytes", &d) && d >= 0 && d <= 64) ? (int)d : 0;
    hex = str_of(result, "path_hex");
    if (hex && strlen(hex) < sizeof(enc->path_hex) && hex_only(hex, strlen(hex))) {
        snprintf(enc->path_hex, sizeof(enc->path_hex), "%s", hex);
    }
    return 0;
}

int rift_route_check_answered(struct rift_route *r, int request, const cJSON *result)
{
    struct rift_route_draft *d;

    if (!r || request <= 0 || request != r->draft.request || !r->draft.active) {
        return -1;
    }
    d = &r->draft;
    d->request = 0;
    if (d->checked_gen != d->gen) {
        return -1; /* edited while it was out: asked again for the new gen */
    }
    if (rift_route_parse_preview(result, d->count, &d->enc) != 0) {
        d->check = RIFT_ROUTE_REFUSED;
        snprintf(d->why, sizeof(d->why), "meshcored answered with something that is not a "
                                         "valid explicit route for these relays");
        return 0;
    }
    d->check = RIFT_ROUTE_VALID;
    d->why[0] = '\0';
    return 0;
}

int rift_route_check_refused(struct rift_route *r, int request, const char *why)
{
    if (!r || request <= 0 || request != r->draft.request || !r->draft.active) {
        return -1;
    }
    r->draft.request = 0;
    if (r->draft.checked_gen != r->draft.gen) {
        return -1;
    }
    r->draft.check = RIFT_ROUTE_REFUSED;
    snprintf(r->draft.why, sizeof(r->draft.why), "%s", why && why[0] ? why : "refused");
    return 0;
}

int rift_route_usable(const struct rift_route *r, const struct rift_model *m)
{
    return r && r->draft.active && rift_route_supported(r) &&
           r->draft.check == RIFT_ROUTE_VALID && r->draft.checked_gen == r->draft.gen &&
           rift_route_missing(r, m) == 0;
}

int rift_route_use(struct rift_route *r, const struct rift_model *m)
{
    struct rift_route_stage *s;

    if (!rift_route_usable(r, m)) {
        return -1;
    }
    s = &r->stage;
    memset(s, 0, sizeof(*s));
    s->active = 1;
    snprintf(s->conv, sizeof(s->conv), "%s", r->draft.to);
    memcpy(s->via, r->draft.via, sizeof(s->via));
    s->count = r->draft.count;
    s->enc = r->draft.enc;
    rift_route_close(r);
    return 0;
}

/* ---- the stage ------------------------------------------------------------------------ */

const struct rift_route_stage *rift_route_staged(const struct rift_route *r, const char *conv)
{
    if (!r || !conv || !r->stage.active || strcmp(r->stage.conv, conv) != 0) {
        return NULL;
    }
    return &r->stage;
}

void rift_route_auto(struct rift_route *r, const char *conv)
{
    if (rift_route_staged(r, conv)) {
        memset(&r->stage, 0, sizeof(r->stage));
    }
}

/* ---- words ------------------------------------------------------------------------------- */

static void name_of(const struct rift_model *m, const char *key, char *out, size_t len)
{
    const struct rift_node *n = rift_model_find(m, key);

    if (n) {
        rift_fmt_label(n, out, len);
    } else {
        /* Gone from the list: said by its key, never by a name it might
         * share with whatever took its place. */
        snprintf(out, len, "%.8s?", key ? key : "");
    }
}

static void append(char *out, size_t len, size_t *at, const char *s)
{
    int w;

    if (*at >= len) {
        return;
    }
    w = snprintf(out + *at, len - *at, "%s", s);
    if (w > 0) {
        *at += (size_t)w < len - *at ? (size_t)w : len - *at - 1;
    }
}

void rift_route_chain(const struct rift_model *m, const char *to, const char (*via)[RIFT_ROUTE_KEY],
                      int count, char *out, size_t out_len)
{
    char name[RIFT_LABEL_MAX];
    size_t at = 0;
    int i;

    if (!out || !out_len) {
        return;
    }
    out[0] = '\0';
    append(out, out_len, &at, "YOU");
    for (i = 0; via && i < count; i++) {
        append(out, out_len, &at, " \xE2\x80\xBA ");
        name_of(m, via[i], name, sizeof(name));
        append(out, out_len, &at, name);
    }
    append(out, out_len, &at, " \xE2\x80\xBA ");
    name_of(m, to, name, sizeof(name));
    append(out, out_len, &at, name);
}

const char *rift_route_relays_word(int count, char *buf, size_t len)
{
    if (count <= 0) {
        snprintf(buf, len, "NO RELAY");
    } else {
        snprintf(buf, len, "%d RELAY%s", count, count == 1 ? "" : "S");
    }
    return buf;
}
