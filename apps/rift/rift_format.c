/*
 * RIFT's words. See rift_format.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_format.h"

#include <stdio.h>
#include <string.h>

/* ---- UTF-8 ------------------------------------------------------------- */

static size_t seq_len(unsigned char c)
{
    if (c < 0x80u) {
        return 1;
    }
    if ((c & 0xE0u) == 0xC0u) {
        return 2;
    }
    if ((c & 0xF0u) == 0xE0u) {
        return 3;
    }
    if ((c & 0xF8u) == 0xF0u) {
        return 4;
    }
    /* Not a lead byte. meshcored does not emit these (docs/api/mesh.md,
     * "Remote text"), but a byte is consumed anyway so a caller cannot be
     * made to loop for ever by input from somewhere else. */
    return 1;
}

/* How many bytes of src fit in limit without splitting a character. */
static size_t fit(const char *src, size_t limit)
{
    size_t i = 0;

    while (src[i]) {
        size_t l = seq_len((unsigned char)src[i]);
        size_t j;

        /* A sequence cut short in the source is not copied half. */
        for (j = 1; j < l; j++) {
            if (!src[i + j]) {
                return i;
            }
        }
        if (i + l > limit) {
            return i;
        }
        i += l;
    }
    return i;
}

size_t rift_utf8_copy(char *out, size_t out_len, const char *src)
{
    size_t n;

    if (!out || out_len == 0) {
        return 0;
    }
    if (!src) {
        out[0] = '\0';
        return 0;
    }
    n = fit(src, out_len - 1);
    memcpy(out, src, n);
    out[n] = '\0';
    return n;
}

size_t rift_utf8_ellipsis(char *out, size_t out_len, const char *src)
{
    size_t whole;
    size_t n;
    const size_t mark = sizeof(RIFT_ELLIPSIS) - 1;

    if (!out || out_len == 0) {
        return 0;
    }
    if (!src) {
        out[0] = '\0';
        return 0;
    }
    whole = strlen(src);
    if (whole < out_len) {
        memcpy(out, src, whole + 1);
        return whole;
    }
    if (out_len <= mark + 1) {
        return rift_utf8_copy(out, out_len, src);
    }
    n = fit(src, out_len - 1 - mark);
    memcpy(out, src, n);
    memcpy(out + n, RIFT_ELLIPSIS, mark + 1);
    return n + mark;
}

/* ---- ages -------------------------------------------------------------- */

/* The interval as a number and a unit. Returns 0 when it is not knowable. */
static int age_parts(int64_t age_ms, int known, long *value, const char **unit,
                     const char **word)
{
    if (!known || age_ms < 0) {
        return 0;
    }
    if (age_ms < 60000) {
        *value = (long)(age_ms / 1000);
        *unit = "s";
        *word = "s";
        return 1;
    }
    if (age_ms < 3600000) {
        *value = (long)(age_ms / 60000);
        *unit = "m";
        *word = "min";
        return 1;
    }
    if (age_ms < 86400000) {
        *value = (long)(age_ms / 3600000);
        *unit = "h";
        *word = "h";
        return 1;
    }
    *value = (long)(age_ms / 86400000);
    *unit = "d";
    *word = "d";
    return 1;
}

void rift_fmt_age(int64_t age_ms, int known, char *out, size_t out_len)
{
    long value = 0;
    const char *unit = "";
    const char *word = "";

    if (!out || out_len == 0) {
        return;
    }
    if (!age_parts(age_ms, known, &value, &unit, &word)) {
        snprintf(out, out_len, "%s", RIFT_UNKNOWN);
        return;
    }
    /* Past a hundred days the number stops being information and starts
     * being width; the column is four characters wide. */
    if (value > 99) {
        snprintf(out, out_len, ">99%s", unit);
        return;
    }
    snprintf(out, out_len, "%ld%s", value, unit);
}

void rift_fmt_age_split(int64_t age_ms, int known, char *value, size_t value_len, char *unit,
                        size_t unit_len)
{
    long n = 0;
    const char *u = "";
    const char *word = "";

    if (!age_parts(age_ms, known, &n, &u, &word)) {
        if (value && value_len) {
            snprintf(value, value_len, "%s", RIFT_UNKNOWN);
        }
        if (unit && unit_len) {
            unit[0] = '\0';
        }
        return;
    }
    if (value && value_len) {
        if (n > 99) {
            snprintf(value, value_len, ">99");
        } else {
            snprintf(value, value_len, "%ld", n);
        }
    }
    if (unit && unit_len) {
        snprintf(unit, unit_len, "%s", word);
    }
}

/* ---- signal ------------------------------------------------------------ */

void rift_fmt_rssi(double dbm, int known, char *out, size_t out_len)
{
    long n;

    if (!out || out_len == 0) {
        return;
    }
    if (!known) {
        snprintf(out, out_len, "%s", RIFT_UNKNOWN);
        return;
    }
    n = (long)(dbm < 0 ? dbm - 0.5 : dbm + 0.5);
    if (n < 0) {
        snprintf(out, out_len, "%s%ld", RIFT_MINUS, -n);
    } else {
        snprintf(out, out_len, "%ld", n);
    }
}

void rift_fmt_snr(double db, int known, char *out, size_t out_len)
{
    int negative;
    double v = db;
    long tenths;

    if (!out || out_len == 0) {
        return;
    }
    if (!known) {
        snprintf(out, out_len, "%s", RIFT_UNKNOWN);
        return;
    }
    negative = v < 0;
    if (negative) {
        v = -v;
    }
    tenths = (long)(v * 10.0 + 0.5);
    snprintf(out, out_len, "%s%ld.%ld", negative && tenths != 0 ? RIFT_MINUS : "", tenths / 10,
             tenths % 10);
}

/* ---- a node's link ----------------------------------------------------- */

enum rift_link rift_link_of(const struct rift_node *n)
{
    if (!n || !n->path_known) {
        return RIFT_LINK_UNKNOWN;
    }
    return n->direct ? RIFT_LINK_DIRECT : RIFT_LINK_RELAYED;
}

int rift_node_is_stale(const struct rift_node *n, int64_t now_ms)
{
    int64_t age;

    if (!n || !n->have_heard) {
        return 0;
    }
    age = now_ms - n->heard_mono_ms;
    if (age < 0) {
        /* Heard in the future: one of the two clocks is wrong, and a wrong
         * clock is not evidence that a node is stale. */
        return 0;
    }
    return age > RIFT_STALE_MS;
}

void rift_fmt_hops(const struct rift_node *n, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!n || !n->path_known) {
        snprintf(out, out_len, "%s", RIFT_UNKNOWN);
        return;
    }
    if (n->direct) {
        snprintf(out, out_len, "DIR");
        return;
    }
    snprintf(out, out_len, "%d", n->hops);
}

void rift_fmt_state(const struct rift_node *n, char *out, size_t out_len)
{
    struct rift_path p;

    if (!out || out_len == 0) {
        return;
    }
    if (!n || !n->path_known) {
        snprintf(out, out_len, "NO PATH");
        return;
    }
    if (n->direct) {
        snprintf(out, out_len, "DIRECT");
        return;
    }
    if (rift_path_parse(n, &p) != 0) {
        /* A path this build could not take apart is a path whose hops are
         * all unnamed. Saying nothing about the uncertainty would be the
         * one answer the data does not support. */
        p.unknown = n->hops;
    }
    if (p.unknown > 0) {
        snprintf(out, out_len, "RELAYED" RIFT_SEP "%d HOP%s" RIFT_SEP "%d UNKNOWN HOP%s", n->hops,
                 n->hops == 1 ? "" : "S", p.unknown, p.unknown == 1 ? "" : "S");
        return;
    }
    snprintf(out, out_len, "RELAYED" RIFT_SEP "%d HOP%s", n->hops, n->hops == 1 ? "" : "S");
}

void rift_fmt_label(const struct rift_node *n, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!n) {
        out[0] = '\0';
        return;
    }
    if (n->have_name && n->name[0]) {
        rift_utf8_ellipsis(out, out_len, n->name);
        return;
    }
    snprintf(out, out_len, "%s", n->hash[0] ? n->hash : RIFT_UNKNOWN);
}

const char *rift_type_word(int type, int have_type)
{
    if (!have_type) {
        return NULL;
    }
    switch (type) {
    case 1:
        return "chat";
    case 2:
        return "repeater";
    case 3:
        return "room";
    case 4:
        return "sensor";
    default:
        /* A number this build has no word for is not given one. */
        return NULL;
    }
}

const char *rift_type_tag(int type, int have_type)
{
    if (!have_type) {
        return NULL;
    }
    switch (type) {
    case 2:
        return "RPT";
    case 3:
        return "ROOM";
    case 4:
        return "SENS";
    default:
        return NULL;
    }
}

static char upper_hex(char c)
{
    return (c >= 'a' && c <= 'f') ? (char)(c - 'a' + 'A') : c;
}

void rift_fmt_key_short(const char *key, char *out, size_t out_len)
{
    size_t len;
    size_t i;
    size_t w = 0;
    char buf[RIFT_KEY_SHORT_MAX];

    if (!out || out_len == 0) {
        return;
    }
    if (!key) {
        out[0] = '\0';
        return;
    }
    len = strlen(key);
    if (len < 20) {
        for (i = 0; i < len && w + 1 < sizeof(buf) && w + 1 < out_len; i++) {
            buf[w++] = upper_hex(key[i]);
        }
        buf[w] = '\0';
        snprintf(out, out_len, "%s", buf);
        return;
    }
    for (i = 0; i < 12; i++) {
        if (i > 0 && i % 4 == 0) {
            buf[w++] = ' ';
        }
        buf[w++] = upper_hex(key[i]);
    }
    buf[w] = '\0';
    snprintf(out, out_len, "%s " RIFT_ELLIPSIS " %c%c%c%c", buf, upper_hex(key[len - 4]),
             upper_hex(key[len - 3]), upper_hex(key[len - 2]), upper_hex(key[len - 1]));
}

/* ---- the path ---------------------------------------------------------- */

static int is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int rift_path_parse(const struct rift_node *n, struct rift_path *out)
{
    size_t hexlen;
    size_t bytes;
    int per;
    int i;

    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (!n) {
        return -1;
    }
    if (!n->path_known) {
        return 0;
    }
    out->known = 1;
    out->direct = n->direct ? 1 : 0;
    out->hops = n->hops;
    if (out->hops < 0) {
        out->hops = 0;
    }
    if (out->hops > RIFT_MAX_HOPS) {
        out->hops = RIFT_MAX_HOPS;
    }
    hexlen = strlen(n->path_hex);
    for (i = 0; (size_t)i < hexlen; i++) {
        if (!is_hex(n->path_hex[i])) {
            return -1;
        }
    }
    if (hexlen % 2 != 0) {
        return -1;
    }
    bytes = hexlen / 2;
    if (out->hops == 0) {
        /* Direct, or a known path with nothing between here and there. */
        return 0;
    }
    out->count = out->hops;
    /* MeshCore packs pathHashSize() bytes per hop, so a path divides
     * exactly by its hop count - unless it was clipped at MCD_MAX_PATH, or
     * the count and the bytes disagree. When it does not divide, the hops
     * are real but none of them can be named: they are reported unknown
     * rather than sliced into plausible pieces. */
    if (bytes == 0 || bytes % (size_t)out->hops != 0) {
        out->unknown = out->hops;
        return 0;
    }
    per = (int)(bytes / (size_t)out->hops);
    if (per > (RIFT_HOP_ID_MAX - 1) / 2) {
        out->unknown = out->hops;
        return 0;
    }
    out->bytes_per_hop = per;
    for (i = 0; i < out->hops; i++) {
        memcpy(out->hop[i].id, n->path_hex + (size_t)i * (size_t)per * 2, (size_t)per * 2);
        out->hop[i].id[per * 2] = '\0';
        out->hop[i].have_id = 1;
    }
    out->identified = out->hops;
    return 0;
}

int rift_strip_build(const struct rift_path *p, struct rift_strip_cell *out, int max)
{
    int n = 0;
    int relays;
    int i;

    if (!p || !out || max < 2) {
        return 0;
    }
    out[n].kind = RIFT_CELL_SELF;
    out[n].more = 0;
    out[n].hop = -1;
    n++;
    if (!p->known) {
        out[n].kind = RIFT_CELL_TARGET;
        out[n].more = 0;
        out[n].hop = -1;
        return n + 1;
    }
    relays = p->hops;
    if (relays <= RIFT_STRIP_RELAYS_MAX) {
        for (i = 0; i < relays && n + 1 < max; i++) {
            out[n].kind = p->hop[i].have_id ? RIFT_CELL_RELAY : RIFT_CELL_UNKNOWN;
            out[n].more = 0;
            out[n].hop = i;
            n++;
        }
    } else if (n + 4 < max) {
        for (i = 0; i < 2; i++) {
            out[n].kind = p->hop[i].have_id ? RIFT_CELL_RELAY : RIFT_CELL_UNKNOWN;
            out[n].more = 0;
            out[n].hop = i;
            n++;
        }
        out[n].kind = RIFT_CELL_MORE;
        out[n].more = relays - 3;
        out[n].hop = -1;
        n++;
        out[n].kind = p->hop[relays - 1].have_id ? RIFT_CELL_RELAY : RIFT_CELL_UNKNOWN;
        out[n].more = 0;
        out[n].hop = relays - 1;
        n++;
    }
    if (n < max) {
        out[n].kind = RIFT_CELL_TARGET;
        out[n].more = 0;
        out[n].hop = -1;
        n++;
    }
    return n;
}

/* Append text, stopping at the end of the buffer. Returns the new length. */
static size_t append(char *out, size_t out_len, size_t at, const char *text)
{
    size_t room;

    if (at + 1 >= out_len) {
        return at;
    }
    room = out_len - at;
    snprintf(out + at, room, "%s", text);
    return at + strlen(out + at);
}

static const char *hop_label(const struct rift_path *p, int i, rift_resolve_fn resolve, void *user)
{
    const char *name;

    if (!p->hop[i].have_id) {
        return RIFT_UNKNOWN;
    }
    name = resolve ? resolve(p->hop[i].id, user) : NULL;
    return name ? name : p->hop[i].id;
}

void rift_path_chain(const char *self_label, const struct rift_path *p, const char *target_label,
                     rift_resolve_fn resolve, void *user, char *out, size_t out_len)
{
    size_t at = 0;
    int i;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!p) {
        return;
    }
    at = append(out, out_len, at, self_label ? self_label : RIFT_UNKNOWN);
    if (!p->known) {
        at = append(out, out_len, at, RIFT_ARROW);
        at = append(out, out_len, at, RIFT_UNKNOWN);
        at = append(out, out_len, at, RIFT_ARROW);
        append(out, out_len, at, target_label ? target_label : RIFT_UNKNOWN);
        return;
    }
    for (i = 0; i < p->hops && i < p->count; i++) {
        at = append(out, out_len, at, RIFT_ARROW);
        at = append(out, out_len, at, hop_label(p, i, resolve, user));
    }
    at = append(out, out_len, at, RIFT_ARROW);
    append(out, out_len, at, target_label ? target_label : RIFT_UNKNOWN);
}

int rift_path_ladder_row(const struct rift_path *p, int index, const char *self_label,
                         const char *target_label, rift_resolve_fn resolve, void *user,
                         struct rift_ladder_row *out)
{
    const char *name;

    if (!p || !out || index < 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->index = index;
    if (index == 0) {
        out->kind = RIFT_CELL_SELF;
        rift_utf8_ellipsis(out->label, sizeof(out->label), self_label ? self_label : RIFT_UNKNOWN);
        snprintf(out->note, sizeof(out->note), "this device");
        return 0;
    }
    if (!p->known) {
        if (index != 1) {
            return -1;
        }
        out->kind = RIFT_CELL_TARGET;
        rift_utf8_ellipsis(out->label, sizeof(out->label),
                           target_label ? target_label : RIFT_UNKNOWN);
        snprintf(out->note, sizeof(out->note), "no path observed");
        return 0;
    }
    if (index == p->hops + 1) {
        out->kind = RIFT_CELL_TARGET;
        rift_utf8_ellipsis(out->label, sizeof(out->label),
                           target_label ? target_label : RIFT_UNKNOWN);
        snprintf(out->note, sizeof(out->note), p->direct ? "target, heard direct" : "target");
        return 0;
    }
    if (index > p->hops || index > p->count) {
        return -1;
    }
    if (!p->hop[index - 1].have_id) {
        out->kind = RIFT_CELL_UNKNOWN;
        rift_utf8_copy(out->label, sizeof(out->label), RIFT_UNKNOWN);
        snprintf(out->note, sizeof(out->note), "not named in this path");
        return 0;
    }
    out->kind = RIFT_CELL_RELAY;
    name = resolve ? resolve(p->hop[index - 1].id, user) : NULL;
    if (name) {
        rift_utf8_ellipsis(out->label, sizeof(out->label), name);
        snprintf(out->note, sizeof(out->note), "known node");
    } else {
        rift_utf8_copy(out->label, sizeof(out->label), p->hop[index - 1].id);
        snprintf(out->note, sizeof(out->note), "hash only");
    }
    return 0;
}

/* ---- messages ----------------------------------------------------------- */

void rift_fmt_msg_state(const struct rift_message *msg, char *out, size_t out_len)
{
    const char *word;

    if (!out || out_len == 0) {
        return;
    }
    if (!msg) {
        rift_utf8_copy(out, out_len, RIFT_UNKNOWN);
        return;
    }
    switch (msg->state) {
    case RIFT_MSG_SENDING:
        word = "SENDING";
        break;
    case RIFT_MSG_RECEIVED:
        word = "RECEIVED";
        break;
    case RIFT_MSG_SENT_FLOOD:
    case RIFT_MSG_SENT_DIRECT:
        word = "SENT";
        break;
    case RIFT_MSG_ACKED:
        /* The API calls it "acked"; to a reader an ACK is what delivery
         * means, and the handoff's own caption is DELIVERED. */
        word = "DELIVERED";
        break;
    case RIFT_MSG_NO_ACK:
        word = "NO ACK";
        break;
    case RIFT_MSG_FAILED:
        word = "FAILED";
        break;
    case RIFT_MSG_STATE_UNKNOWN:
    default:
        /* A state this build does not know: the service's own word, so a
         * v0 API that grows one is readable here before this app is
         * rebuilt for it. */
        if (msg->state_word[0]) {
            rift_utf8_copy(out, out_len, msg->state_word);
            return;
        }
        word = RIFT_UNKNOWN;
        break;
    }
    rift_utf8_copy(out, out_len, word);
}

int rift_msg_is_warn(const struct rift_message *msg)
{
    if (!msg) {
        return 0;
    }
    return msg->state == RIFT_MSG_NO_ACK || msg->state == RIFT_MSG_FAILED ||
           msg->state == RIFT_MSG_STATE_UNKNOWN;
}

void rift_fmt_ack(const struct rift_message *msg, char *out, size_t out_len)
{
    int64_t took;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!msg || !msg->have_ack_mono || !msg->have_mono) {
        return;
    }
    took = msg->ack_mono_ms - msg->mono_ms;
    if (took < 0) {
        /* An acknowledgement before the message it answers can only be a
         * fault in the stamps, and is not printed as a negative duration. */
        return;
    }
    if (took < 60000) {
        snprintf(out, out_len, "%d s", (int)((took + 500) / 1000));
        return;
    }
    snprintf(out, out_len, "%dm %02ds", (int)(took / 60000), (int)((took % 60000) / 1000));
}

void rift_fmt_msg_caption(const struct rift_message *msg, char *out, size_t out_len)
{
    char state[RIFT_MSG_STATE_MAX];
    char ack[RIFT_ACK_MAX];
    char rssi[RIFT_SIGNAL_MAX];
    char snr[RIFT_SIGNAL_MAX];
    size_t at;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!msg) {
        return;
    }
    rift_fmt_msg_state(msg, state, sizeof(state));
    at = (size_t)snprintf(out, out_len, "%s", state);
    if (at >= out_len) {
        return;
    }
    if (msg->state == RIFT_MSG_ACKED) {
        rift_fmt_ack(msg, ack, sizeof(ack));
        if (ack[0]) {
            snprintf(out + at, out_len - at, RIFT_SEP "ACK %s", ack);
        }
        return;
    }
    if (msg->state == RIFT_MSG_SENT_FLOOD || msg->state == RIFT_MSG_SENT_DIRECT) {
        snprintf(out + at, out_len - at, RIFT_SEP "%s",
                 msg->state == RIFT_MSG_SENT_FLOOD ? "FLOOD" : "DIRECT");
        return;
    }
    /* What was measured for the frame this message arrived in, when the
     * service reported it. An outgoing message has nothing measured for it
     * here: what came back was an ACK, and its signal belongs to the ACK. */
    if (msg->dir != RIFT_MSG_IN) {
        return;
    }
    if (msg->have_rssi) {
        /* Labelled, unlike the NODES column of the same value: a caption has
         * no header above it to say what the number is. */
        rift_fmt_rssi(msg->rssi_dbm, 1, rssi, sizeof(rssi));
        at += (size_t)snprintf(out + at, out_len - at, RIFT_SEP "%s dBm", rssi);
        if (at >= out_len) {
            return;
        }
    }
    if (msg->have_snr) {
        rift_fmt_snr(msg->snr_db, 1, snr, sizeof(snr));
        snprintf(out + at, out_len - at, RIFT_SEP "SNR %s", snr);
    }
}

void rift_fmt_preview(const struct rift_message *msg, char *out, size_t out_len)
{
    char folded[RIFT_MSG_TEXT_MAX];
    size_t i;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!msg) {
        return;
    }
    /* Newline and tab are the two control characters the API lets through
     * (docs/api/mesh.md, "Remote text"). A row is one line, so they become
     * spaces here rather than being left to the label to interpret. */
    for (i = 0; i + 1 < sizeof(folded) && msg->text[i]; i++) {
        char c = msg->text[i];

        folded[i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    folded[i] = '\0';
    if (msg->dir == RIFT_MSG_OUT) {
        snprintf(out, out_len, "you: %s", folded);
    } else {
        rift_utf8_copy(out, out_len, folded);
    }
}

size_t rift_send_text_bytes(const char *text)
{
    return text ? strlen(text) : 0;
}

int rift_send_text_check(const char *text, char *why, size_t why_len)
{
    size_t len = rift_send_text_bytes(text);
    size_t i;

    if (why && why_len) {
        why[0] = '\0';
    }
    if (len == 0) {
        if (why) {
            rift_utf8_copy(why, why_len, "Nothing to send.");
        }
        return -1;
    }
    if (len > RIFT_SEND_TEXT_MAX) {
        if (why) {
            snprintf(why, why_len, "%u bytes; a message takes %d.", (unsigned)len,
                     RIFT_SEND_TEXT_MAX);
        }
        return -1;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];

        if (c == '\n' || c == '\t') {
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            /* meshcored refuses these rather than sanitising them on the
             * way out: a caller sending a control character has made a
             * mistake and is told so (docs/api/mesh.md). Same rule here, so
             * the reader is told before the request goes. */
            if (why) {
                rift_utf8_copy(why, why_len, "That text has a control character in it.");
            }
            return -1;
        }
    }
    return 0;
}
