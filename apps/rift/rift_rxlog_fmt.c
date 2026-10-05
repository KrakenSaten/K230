/*
 * RX LOG: every string a row and the detail print. See rift_rxlog.h.
 *
 * The rules are RIFT's own (rift_format.h): a value nobody reported is "--",
 * never 0 and never a guess, and a path is printed whole - a long one wraps
 * on the screen, it is never shortened here.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_rxlog.h"

#include "rift_format.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define NA "--"


const char *rift_rxlog_type_word(const struct rift_rx_entry *e)
{
    /* MeshCore's PAYLOAD_TYPE_* in order (vendor/RIFT/src/Packet.h). */
    static const char *const words[16] = { "REQ", "RESP", "MSG",   "ACK",   "ADV", "MSG",
                                           "DATA", "ANON", "PATH", "TRACE", "MULTI", "CTRL",
                                           "UNK",  "UNK",  "UNK",  "RAW" };

    if (!e || e->kind == RIFT_RX_MARK) {
        return NA;
    }
    if (!(e->flags & RIFT_RXF_HAVE_HEADER)) {
        return "UNK";
    }
    /* A control packet carries its own type in its first byte: node discovery
     * (upstream's CTL_TYPE_NODE_DISCOVER_REQ 0x80 and _RESP 0x90) is DISC. */
    if (e->type_code == 11 && (e->flags & RIFT_RXF_HAVE_CONTROL) &&
        ((e->control_flags & 0xF0) == 0x80 || (e->control_flags & 0xF0) == 0x90)) {
        return "DISC";
    }
    return words[e->type_code & 0x0F];
}

void rift_rxlog_state_word(const struct rift_rx_entry *e, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!e || e->kind == RIFT_RX_MARK) {
        snprintf(out, out_len, NA);
        return;
    }
    switch (e->state) {
    case RIFT_RXS_REJECTED:
        snprintf(out, out_len, "REJ");
        break;
    case RIFT_RXS_UNRESOLVED:
        snprintf(out, out_len, "LOST");
        break;
    case RIFT_RXS_ECHO:
        if (e->dup > 1) {
            snprintf(out, out_len, "ECHO #%u", (unsigned)e->dup);
        } else {
            snprintf(out, out_len, "ECHO");
        }
        break;
    case RIFT_RXS_DUP:
        /* The number is the log's count of receptions of this hash; "DUP"
         * alone is MeshCore's seen-table matching a packet the log counted
         * once (it had heard it before the log's own table began). */
        if (e->dup > 1) {
            snprintf(out, out_len, "DUP #%u", (unsigned)e->dup);
        } else {
            snprintf(out, out_len, "DUP");
        }
        break;
    case RIFT_RXS_RX:
    default:
        snprintf(out, out_len, "RX");
        break;
    }
}

const char *rift_rxlog_filter_word(int filter)
{
    static const char *const words[RIFT_RXF_COUNT] = { "ALL", "DUP", "MSG", "ADV", "CTRL" };

    return (filter >= 0 && filter < RIFT_RXF_COUNT) ? words[filter] : "ALL";
}

void rift_rxlog_fmt_time(const struct rift_rx_entry *e, char *out, size_t out_len)
{
    int64_t ms;
    int64_t s;

    if (!out || out_len == 0) {
        return;
    }
    if (!e) {
        snprintf(out, out_len, NA);
        return;
    }
    if (e->flags & RIFT_RXF_HAVE_WALL) {
        time_t t = (time_t)(e->wall_ms / 1000);
        struct tm tm;

        localtime_r(&t, &tm);
        snprintf(out, out_len, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                 (int)(((e->wall_ms % 1000) + 1000) % 1000));
        return;
    }
    /* No wall clock: the time since the board started, which the shared
     * monotonic clock is. The screen says which of the two it is showing. */
    ms = e->mono_ms < 0 ? 0 : e->mono_ms;
    s = ms / 1000;
    snprintf(out, out_len, "%02d:%02d:%02d.%03d", (int)((s / 3600) % 100), (int)((s / 60) % 60),
             (int)(s % 60), (int)(ms % 1000));
}

void rift_rxlog_fmt_channel(const struct rift_rx_entry *e, char *out, size_t out_len)
{
    if (e && (e->flags & RIFT_RXF_HAVE_CHANNEL)) {
        snprintf(out, out_len, "CH:%02X", e->channel_hash);
    } else {
        snprintf(out, out_len, "CH:" NA);
    }
}

void rift_rxlog_fmt_hash(const struct rift_rx_entry *e, char *out, size_t out_len)
{
    /* Two bytes of the eight: enough to tell one packet's copies from the
     * next packet's at a glance; the detail has all eight. */
    if (e && (e->flags & RIFT_RXF_HAVE_HASH)) {
        snprintf(out, out_len, "H:%02X%02X", e->hash[0], e->hash[1]);
    } else {
        snprintf(out, out_len, "H:" NA);
    }
}

void rift_rxlog_fmt_size(const struct rift_rx_entry *e, char *out, size_t out_len)
{
    if (e && e->kind == RIFT_RX_FRAME) {
        snprintf(out, out_len, "%uB", (unsigned)e->bytes);
    } else {
        snprintf(out, out_len, NA);
    }
}

/* A signed number with a real minus sign, whole when it is whole. */
static void signed_value(double v, int decimals, char *out, size_t out_len)
{
    double mag = fabs(v);
    const char *sign = v < 0 ? RIFT_MINUS : "";

    if (decimals == 0 || mag == floor(mag)) {
        snprintf(out, out_len, "%s%.0f", sign, mag);
    } else {
        snprintf(out, out_len, "%s%.*f", sign, decimals, mag);
    }
}

void rift_rxlog_fmt_rssi(const struct rift_rx_entry *e, char *out, size_t out_len)
{
    char v[16];

    if (e && (e->flags & RIFT_RXF_HAVE_RSSI)) {
        signed_value(e->rssi_dbm, 0, v, sizeof(v));
        snprintf(out, out_len, "RSSI:%s", v);
    } else {
        snprintf(out, out_len, "RSSI:" NA);
    }
}

void rift_rxlog_fmt_snr(const struct rift_rx_entry *e, char *out, size_t out_len)
{
    char v[16];

    if (e && (e->flags & RIFT_RXF_HAVE_SNR)) {
        signed_value(e->snr_db, 1, v, sizeof(v));
        snprintf(out, out_len, "SNR:%s", v);
    } else {
        snprintf(out, out_len, "SNR:" NA);
    }
}

int rift_rxlog_rssi_grade(const struct rift_rx_entry *e)
{
    if (!e || !(e->flags & RIFT_RXF_HAVE_RSSI)) {
        return -1;
    }
    return e->rssi_dbm >= -85.0f ? 2 : e->rssi_dbm >= -105.0f ? 1 : 0;
}

int rift_rxlog_snr_grade(const struct rift_rx_entry *e)
{
    if (!e || !(e->flags & RIFT_RXF_HAVE_SNR)) {
        return -1;
    }
    return e->snr_db >= 5.0f ? 2 : e->snr_db >= -5.0f ? 1 : 0;
}

/* Append to out at *at, never past its end. */
static void put(char *out, size_t out_len, size_t *at, const char *fmt, const char *s)
{
    int n;

    if (*at >= out_len) {
        return;
    }
    n = snprintf(out + *at, out_len - *at, fmt, s);
    if (n > 0) {
        *at += (size_t)n;
    }
    if (*at >= out_len) {
        *at = out_len - 1;
    }
}

void rift_rxlog_fmt_path(const struct rift_rx_entry *e, char *out, size_t out_len)
{
    size_t at = 0;
    int direct;
    int i;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!e || e->kind == RIFT_RX_MARK) {
        return;
    }
    if (!(e->flags & RIFT_RXF_PARSED)) {
        snprintf(out, out_len, "PATH  " NA);
        return;
    }
    if (e->flags & RIFT_RXF_PATH_SNR) {
        /* A TRACE carries what each hop measured, a quarter dB a byte. */
        put(out, out_len, &at, "%s", "SNRS  ");
        if (e->path_hops == 0) {
            put(out, out_len, &at, "%s", "0 HOP");
        }
        for (i = 0; i < e->path_hops && i < e->path_bytes; i++) {
            char v[16];

            snprintf(v, sizeof(v), "%+.2f", (double)(int8_t)e->path[i] / 4.0);
            put(out, out_len, &at, i == 0 ? "%s" : " > %s", v);
        }
        return;
    }
    /* MeshCore's own meaning of the path: on a flood the relays it came
     * through, first one first; on a direct packet the hops it has still to
     * take, next one first. Neither is reordered or completed here. */
    direct = (e->flags & RIFT_RXF_HAVE_HEADER) &&
             (e->route == RIFT_RXRT_DIRECT || e->route == RIFT_RXRT_TRANSPORT_DIRECT);
    put(out, out_len, &at, "%s", direct ? "ROUTE " : "PATH  ");
    if (e->path_hops == 0) {
        put(out, out_len, &at, "%s", "0 HOP");
        return;
    }
    for (i = 0; i < e->path_hops; i++) {
        char hop[8];
        int b;

        hop[0] = '\0';
        for (b = 0; b < e->path_hash_size && i * e->path_hash_size + b < e->path_bytes; b++) {
            snprintf(hop + 2 * b, sizeof(hop) - (size_t)(2 * b), "%02X",
                     e->path[i * e->path_hash_size + b]);
        }
        put(out, out_len, &at, i == 0 ? "%s" : " > %s", hop);
    }
}

void rift_rxlog_fmt_who(const struct rift_rx_entry *e, char *out, size_t out_len)
{
    char shown[RIFT_RXLOG_NAME_MAX];
    char to[RIFT_RXLOG_NAME_MAX];

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!e || e->kind == RIFT_RX_MARK) {
        return;
    }
    rift_text_shown(e->sender, shown, sizeof(shown));
    switch (e->decode) {
    case RIFT_RXD_CHANNEL:
        /* The channel, and the name its sender claims: nothing signs a group
         * frame, so the name keeps its "?" here as it does in a thread. */
        rift_text_shown(e->where, to, sizeof(to));
        if (shown[0]) {
            snprintf(out, out_len, "#%s %s?:", to[0] ? to : NA, shown);
        } else {
            snprintf(out, out_len, "#%s UNNAMED:", to[0] ? to : NA);
        }
        break;
    case RIFT_RXD_DIRECT:
        /* A contact, by its key: who sent it is known, no "?". */
        rift_text_shown(e->where, to, sizeof(to));
        snprintf(out, out_len, "DM %s \xE2\x86\x92 %s:", shown, to[0] ? to : NA);
        break;
    case RIFT_RXD_ADVERT:
        snprintf(out, out_len, "ADV %s", shown);
        break;
    case RIFT_RXD_NONE:
    default:
        break;
    }
}

void rift_rxlog_fmt_what(const struct rift_rx_entry *e, char *out, size_t out_len)
{
    int t;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!e) {
        return;
    }
    if (e->kind == RIFT_RX_MARK) {
        snprintf(out, out_len, "%s", e->text);
        return;
    }
    if (e->state == RIFT_RXS_REJECTED) {
        snprintf(out, out_len, "%s",
                 e->reject == RIFT_RXR_QUEUE_FULL  ? "[DROPPED" RIFT_SEP "RX QUEUE FULL]"
                 : e->reject == RIFT_RXR_NO_BUFFER ? "[DROPPED" RIFT_SEP "NO PACKET BUFFER]"
                                                   : "[REJECTED" RIFT_SEP "NOT A VALID FRAME]");
        return;
    }
    if (e->state == RIFT_RXS_UNRESOLVED) {
        snprintf(out, out_len, "[NOT RESOLVED BY THE SERVICE]");
        return;
    }
    if (e->decode == RIFT_RXD_DIRECT || e->decode == RIFT_RXD_CHANNEL) {
        snprintf(out, out_len, "\"%s\"", e->text);
        return;
    }
    if (e->decode == RIFT_RXD_ADVERT) {
        return;
    }
    t = (e->flags & RIFT_RXF_HAVE_HEADER) ? e->type_code : -1;
    switch (t) {
    case 5: /* group text */
    case 6: /* group data */
        if (!(e->flags & RIFT_RXF_CHANNEL_HELD)) {
            snprintf(out, out_len, "[ENCRYPTED" RIFT_SEP "NO KEY FOR THIS CHANNEL]");
        } else if (e->state == RIFT_RXS_DUP || e->state == RIFT_RXS_ECHO) {
            snprintf(out, out_len, "[REPEAT" RIFT_SEP "NOT DECODED AGAIN]");
        } else if (t == 6) {
            snprintf(out, out_len, "[GROUP DATA" RIFT_SEP "NOT TEXT]");
        } else {
            snprintf(out, out_len, "[UNREADABLE]");
        }
        return;
    case 0: /* request */
    case 1: /* response */
    case 2: /* direct text */
    case 7: /* anonymous request */
    case 8: /* returned path */
        if ((e->flags & RIFT_RXF_HAVE_DEST) && !(e->flags & RIFT_RXF_FOR_US)) {
            if (e->flags & RIFT_RXF_HAVE_SRC) {
                snprintf(out, out_len, "[ENCRYPTED" RIFT_SEP "%02X \xE2\x86\x92 %02X]", e->src_hash,
                         e->dest_hash);
            } else {
                snprintf(out, out_len, "[ENCRYPTED" RIFT_SEP "FOR %02X]", e->dest_hash);
            }
        } else if (!(e->flags & RIFT_RXF_HAVE_DEST)) {
            snprintf(out, out_len, "[ENCRYPTED]");
        } else if (e->state == RIFT_RXS_DUP || e->state == RIFT_RXS_ECHO) {
            snprintf(out, out_len, "[REPEAT" RIFT_SEP "NOT DECODED AGAIN]");
        } else if (t == 2) {
            snprintf(out, out_len, "[UNREADABLE]");
        } else {
            snprintf(out, out_len, "[FOR THIS NODE" RIFT_SEP "NO TEXT]");
        }
        return;
    case 4:
        snprintf(out, out_len, "[NAME NOT KNOWN]");
        return;
    case -1:
        snprintf(out, out_len, "[UNREADABLE]");
        return;
    default:
        /* An ACK, a TRACE, a control packet: there is no content to read. */
        return;
    }
}

static const char *route_word(const struct rift_rx_entry *e)
{
    static const char *const words[4] = { "TRANSPORT FLOOD", "FLOOD", "DIRECT", "TRANSPORT DIRECT" };

    return (e->flags & RIFT_RXF_HAVE_HEADER) ? words[e->route & 3] : NA;
}

void rift_rxlog_fmt_detail(const struct rift_rx_entry *e, char *out, size_t out_len)
{
    char time_s[24];
    char state[24];
    char rssi[24];
    char snr[24];
    char path[RIFT_RXLOG_LINE_MAX];
    char path_line[RIFT_RXLOG_LINE_MAX + 8];
    char who[RIFT_RXLOG_NAME_MAX * 2 + 16];
    char what[RIFT_RXLOG_TEXT_MAX + 64];
    char hash[RIFT_RXLOG_HASH_LEN * 2 + 1];
    char ch[24] = NA;
    char dest[24] = NA;
    char src[24] = NA;
    char code[8] = NA;
    const char *meshcore;
    int i;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!e) {
        return;
    }
    rift_rxlog_fmt_time(e, time_s, sizeof(time_s));
    if (e->kind == RIFT_RX_MARK) {
        snprintf(out, out_len, "TIME     %s%s\nNOTE     %s", time_s,
                 (e->flags & RIFT_RXF_HAVE_WALL) ? "" : " (since boot)", e->text);
        return;
    }
    rift_rxlog_state_word(e, state, sizeof(state));
    rift_rxlog_fmt_rssi(e, rssi, sizeof(rssi));
    rift_rxlog_fmt_snr(e, snr, sizeof(snr));
    rift_rxlog_fmt_path(e, path, sizeof(path));
    /* The row's "PATH  6E > 67" with its word in the detail's column. */
    {
        char word[8];

        memcpy(word, path, 6);
        word[6] = '\0';
        word[strcspn(word, " ")] = '\0';
        snprintf(path_line, sizeof(path_line), "%-9s%s", word, strlen(path) > 6 ? path + 6 : "");
    }
    rift_rxlog_fmt_who(e, who, sizeof(who));
    rift_rxlog_fmt_what(e, what, sizeof(what));
    hash[0] = '\0';
    if (e->flags & RIFT_RXF_HAVE_HASH) {
        for (i = 0; i < RIFT_RXLOG_HASH_LEN; i++) {
            snprintf(hash + 2 * i, sizeof(hash) - (size_t)(2 * i), "%02X", e->hash[i]);
        }
    }
    if (e->flags & RIFT_RXF_HAVE_CHANNEL) {
        snprintf(ch, sizeof(ch), "%02X%s", e->channel_hash,
                 (e->flags & RIFT_RXF_CHANNEL_HELD) ? " (held)" : " (not held)");
    }
    if (e->flags & RIFT_RXF_HAVE_DEST) {
        snprintf(dest, sizeof(dest), "%02X%s", e->dest_hash,
                 (e->flags & RIFT_RXF_FOR_US) ? " (this node)" : "");
    }
    if (e->flags & RIFT_RXF_HAVE_SRC) {
        snprintf(src, sizeof(src), "%02X", e->src_hash);
    }
    if (e->flags & RIFT_RXF_HAVE_HEADER) {
        snprintf(code, sizeof(code), "%u", (unsigned)e->type_code);
    }
    /* What MeshCore itself made of it, beside the log's own count. */
    meshcore = e->state == RIFT_RXS_REJECTED        ? "NEVER SAW IT"
               : e->state == RIFT_RXS_UNRESOLVED    ? "NOT REPORTED"
               : (e->flags & RIFT_RXF_MC_DUPLICATE) ? "DUPLICATE"
                                                    : "NEW";
    snprintf(out, out_len,
             "TIME     %s%s\n"
             "STATE    %s" RIFT_SEP "MESHCORE %s%s%s\n"
             "TYPE     %s" RIFT_SEP "CODE %s" RIFT_SEP "%s\n"
             "SIZE     %u B\n"
             "SIGNAL   %s" RIFT_SEP "%s\n"
             "HASH     %s\n"
             "CHANNEL  %s\n"
             "TO       %s\n"
             "FROM     %s\n"
             "%s\n"
             "SEQ      %llu\n"
             "%s%s%s%s",
             time_s, (e->flags & RIFT_RXF_HAVE_WALL) ? "" : " (since boot)", state, meshcore,
             (e->flags & RIFT_RXF_RELAYED) ? RIFT_SEP "RELAYED" : "",
             (e->flags & RIFT_RXF_OWN) ? RIFT_SEP "OWN PACKET" : "", rift_rxlog_type_word(e), code,
             route_word(e), (unsigned)e->bytes, rssi, snr,
             hash[0] ? hash : NA, ch, dest, src, path_line, (unsigned long long)e->seq,
             who[0] ? who : "", who[0] ? " " : "", what[0] ? what : "",
             (e->flags & RIFT_RXF_TEXT_BORROWED) ? "\n(text from an earlier copy of this packet)" : "");
}
