/*
 * Wi-Fi text rules for netd. See wifi_parse.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "wifi_parse.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- SSIDs ---------------------------------------------------------------- */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int wifi_ssid_decode_printf(const char *text, struct wifi_ssid *out)
{
    const char *p = text;

    memset(out, 0, sizeof(*out));
    if (!text) {
        return -1;
    }
    while (*p) {
        uint8_t byte;

        if (*p != '\\') {
            byte = (uint8_t)*p++;
        } else {
            p++;
            switch (*p) {
            case '"': byte = '"'; p++; break;
            case '\\': byte = '\\'; p++; break;
            case 'e': byte = 0x1b; p++; break;
            case 'n': byte = '\n'; p++; break;
            case 'r': byte = '\r'; p++; break;
            case 't': byte = '\t'; p++; break;
            case 'x': {
                int hi = hex_val(p[1]);
                int lo = hi < 0 ? -1 : hex_val(p[2]);

                if (hi < 0 || lo < 0) {
                    return -1;
                }
                byte = (uint8_t)(hi * 16 + lo);
                p += 3;
                break;
            }
            default:
                return -1;
            }
        }
        if (out->len >= WIFI_SSID_MAX) {
            return -1;
        }
        out->bytes[out->len++] = byte;
    }
    return 0;
}

int wifi_ssid_from_hex(const char *hex, struct wifi_ssid *out)
{
    size_t n = hex ? strlen(hex) : 0;
    size_t i;

    memset(out, 0, sizeof(*out));
    if (n == 0 || n % 2 != 0 || n > WIFI_SSID_MAX * 2) {
        return -1;
    }
    for (i = 0; i < n; i += 2) {
        int hi = hex_val(hex[i]);
        int lo = hex_val(hex[i + 1]);

        if (hi < 0 || lo < 0) {
            memset(out, 0, sizeof(*out));
            return -1;
        }
        out->bytes[out->len++] = (uint8_t)(hi * 16 + lo);
    }
    return 0;
}

int wifi_ssid_from_text(const char *text, struct wifi_ssid *out)
{
    size_t n = text ? strlen(text) : 0;

    memset(out, 0, sizeof(*out));
    if (n == 0 || n > WIFI_SSID_MAX) {
        return -1;
    }
    memcpy(out->bytes, text, n);
    out->len = (uint8_t)n;
    return 0;
}

void wifi_ssid_to_hex(const struct wifi_ssid *s, char *out, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    size_t o = 0;

    if (n == 0) {
        return;
    }
    for (i = 0; i < s->len && o + 2 < n; i++) {
        out[o++] = digits[s->bytes[i] >> 4];
        out[o++] = digits[s->bytes[i] & 0x0f];
    }
    out[o] = '\0';
}

/* Length of the valid UTF-8 sequence at p (max bytes available), or 0. Rejects
 * overlong forms, surrogates and code points above U+10FFFF. */
static size_t utf8_seq(const uint8_t *p, size_t max)
{
    uint32_t cp;
    size_t len;
    size_t i;

    if (p[0] < 0x80) {
        return 1;
    }
    if (p[0] >= 0xc2 && p[0] <= 0xdf) {
        len = 2;
        cp = p[0] & 0x1f;
    } else if (p[0] >= 0xe0 && p[0] <= 0xef) {
        len = 3;
        cp = p[0] & 0x0f;
    } else if (p[0] >= 0xf0 && p[0] <= 0xf4) {
        len = 4;
        cp = p[0] & 0x07;
    } else {
        return 0;
    }
    if (len > max) {
        return 0;
    }
    for (i = 1; i < len; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    if ((len == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) ||
        (len == 4 && (cp < 0x10000 || cp > 0x10ffff))) {
        return 0;
    }
    /* C1 controls are control characters too. */
    if (cp >= 0x80 && cp <= 0x9f) {
        return 0;
    }
    return len;
}

void wifi_ssid_to_text(const struct wifi_ssid *s, char *out, size_t n)
{
    size_t i = 0;
    size_t o = 0;

    if (n == 0) {
        return;
    }
    while (i < s->len) {
        size_t len = utf8_seq(s->bytes + i, s->len - i);

        if (len == 1 && (s->bytes[i] < 0x20 || s->bytes[i] == 0x7f)) {
            len = 0;
        }
        if (len == 0) {
            if (o + 1 >= n) {
                break;
            }
            out[o++] = '?';
            i++;
            continue;
        }
        if (o + len >= n) {
            break;
        }
        memcpy(out + o, s->bytes + i, len);
        o += len;
        i += len;
    }
    out[o] = '\0';
}

int wifi_ssid_equal(const struct wifi_ssid *a, const struct wifi_ssid *b)
{
    return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

int wifi_ssid_is_hidden(const struct wifi_ssid *s)
{
    uint8_t i;

    for (i = 0; i < s->len; i++) {
        if (s->bytes[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* ---- security --------------------------------------------------------------- */

enum wifi_security wifi_security_from_flags(const char *flags)
{
    int psk = 0;
    int sae = 0;
    int eap = 0;
    int owe = 0;
    int rsn = 0;
    int wpa = 0;
    const char *p;

    if (!flags) {
        return WIFI_SEC_OPEN;
    }
    /* Each [..] group is one information element: "[WPA2-PSK+SAE-CCMP]",
     * "[WPA-EAP-TKIP]", "[RSN-SAE-CCMP]" (mesh naming), "[WEP]", "[OWE-TRANS]". */
    for (p = strchr(flags, '['); p; p = strchr(p + 1, '[')) {
        const char *end = strchr(p, ']');
        char group[96];
        size_t len;
        char *key;

        if (!end) {
            break;
        }
        len = (size_t)(end - p - 1);
        if (len >= sizeof(group)) {
            continue;
        }
        memcpy(group, p + 1, len);
        group[len] = '\0';
        if (strcmp(group, "WEP") == 0) {
            if (!rsn && !wpa) {
                return WIFI_SEC_WEP;
            }
            continue;
        }
        if (strncmp(group, "WPA2-", 5) == 0 || strncmp(group, "RSN-", 4) == 0) {
            rsn = 1;
        } else if (strncmp(group, "WPA-", 4) == 0) {
            wpa = 1;
        } else {
            continue;
        }
        /* The rest is "<key management>-<ciphers>", both '+'-joined and both
         * allowed to contain '-' ("PSK-SHA256", "CCMP-256"), so it is not split;
         * no cipher name contains any of these words. */
        key = strchr(group, '-') + 1;
        if (strstr(key, "EAP") || strstr(key, "OSEN")) {
            eap = 1;
        }
        if (strstr(key, "SAE")) {
            sae = 1;
        }
        if (strstr(key, "PSK")) {
            psk = 1;
        }
        if (strstr(key, "OWE")) {
            owe = 1;
        }
    }
    if (eap) {
        return WIFI_SEC_ENTERPRISE;
    }
    if (psk && sae) {
        return WIFI_SEC_WPA2_WPA3;
    }
    if (sae) {
        return WIFI_SEC_WPA3_SAE;
    }
    if (psk) {
        return rsn ? WIFI_SEC_WPA2_PSK : WIFI_SEC_WPA_PSK;
    }
    if (owe) {
        return WIFI_SEC_OWE;
    }
    if (rsn || wpa) {
        /* An RSN element with no key management we know: do not guess open. */
        return WIFI_SEC_ENTERPRISE;
    }
    return WIFI_SEC_OPEN;
}

static const char *const security_names[] = {
    [WIFI_SEC_OPEN] = "open",
    [WIFI_SEC_WEP] = "wep",
    [WIFI_SEC_WPA_PSK] = "wpa",
    [WIFI_SEC_WPA2_PSK] = "wpa2",
    [WIFI_SEC_WPA2_WPA3] = "wpa2/wpa3",
    [WIFI_SEC_WPA3_SAE] = "wpa3",
    [WIFI_SEC_OWE] = "owe",
    [WIFI_SEC_ENTERPRISE] = "enterprise",
};

const char *wifi_security_name(enum wifi_security s)
{
    if ((size_t)s >= sizeof(security_names) / sizeof(security_names[0])) {
        return "unknown";
    }
    return security_names[s];
}

int wifi_security_parse(const char *name, enum wifi_security *out)
{
    size_t i;

    for (i = 0; name && i < sizeof(security_names) / sizeof(security_names[0]); i++) {
        if (strcmp(name, security_names[i]) == 0) {
            *out = (enum wifi_security)i;
            return 0;
        }
    }
    return -1;
}

enum wifi_join wifi_join_method(enum wifi_security s, int sae_capable)
{
    switch (s) {
    case WIFI_SEC_OPEN:
        return WIFI_JOIN_OPEN;
    case WIFI_SEC_WPA_PSK:
    case WIFI_SEC_WPA2_PSK:
        return WIFI_JOIN_PSK;
    case WIFI_SEC_WPA2_WPA3:
        /* A transition network accepts WPA2 clients, so a driver without SAE
         * still joins it, just not with WPA3. */
        return sae_capable ? WIFI_JOIN_SAE : WIFI_JOIN_PSK;
    case WIFI_SEC_WPA3_SAE:
        return sae_capable ? WIFI_JOIN_SAE : WIFI_JOIN_UNSUPPORTED;
    case WIFI_SEC_WEP:
    case WIFI_SEC_OWE:
    case WIFI_SEC_ENTERPRISE:
    default:
        return WIFI_JOIN_UNSUPPORTED;
    }
}

int wifi_security_needs_passphrase(enum wifi_security s)
{
    return s == WIFI_SEC_WPA_PSK || s == WIFI_SEC_WPA2_PSK || s == WIFI_SEC_WPA2_WPA3 ||
           s == WIFI_SEC_WPA3_SAE;
}

/* ---- passphrases ------------------------------------------------------------ */

int wifi_passphrase_valid(const char *pass)
{
    size_t n;
    size_t i;

    if (!pass) {
        return -1;
    }
    /* Bounded scan: never walk a hostile string further than the limit. */
    for (n = 0; n <= WIFI_PASSPHRASE_MAX && pass[n]; n++) {
    }
    if (n < WIFI_PASSPHRASE_MIN || n > WIFI_PASSPHRASE_MAX) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if ((unsigned char)pass[i] < 0x20 || (unsigned char)pass[i] > 0x7e) {
            return -1;
        }
    }
    return 0;
}

/* ---- wpa_supplicant replies ----------------------------------------------- */

static int parse_int_field(const char *s, const char *end, int *out)
{
    char buf[16];
    size_t len = (size_t)(end - s);
    char *e;
    long v;

    if (len == 0 || len >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    errno = 0;
    v = strtol(buf, &e, 10);
    if (errno != 0 || *e != '\0' || v < -100000 || v > 100000) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int bssid_valid(const char *s, size_t len)
{
    size_t i;

    if (len != WIFI_BSSID_LEN) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if ((i % 3) == 2 ? s[i] != ':' : hex_val(s[i]) < 0) {
            return 0;
        }
    }
    return 1;
}

int wifi_parse_scan_results(const char *reply, struct wifi_bss *out, int max, int *hidden)
{
    const char *line = reply;
    int count = 0;
    int first = 1;

    if (hidden) {
        *hidden = 0;
    }
    if (!reply) {
        return 0;
    }
    while (*line) {
        const char *eol = strchr(line, '\n');
        const char *end = eol ? eol : line + strlen(line);
        const char *f[5];
        const char *p = line;
        int nf = 0;
        struct wifi_bss b;
        char flags[256];
        char ssid[256];
        size_t len;

        if (first) {
            /* the "bssid / frequency / signal level / flags / ssid" header */
            first = 0;
            if (strncmp(line, "bssid", 5) == 0) {
                line = eol ? eol + 1 : end;
                continue;
            }
        }
        f[nf++] = p;
        while (p < end && nf < 5) {
            if (*p == '\t') {
                f[nf++] = p + 1;
            }
            p++;
        }
        if (nf != 5) {
            goto next;
        }
        memset(&b, 0, sizeof(b));
        if (!bssid_valid(f[0], (size_t)(f[1] - f[0] - 1))) {
            goto next;
        }
        memcpy(b.bssid, f[0], WIFI_BSSID_LEN);
        if (parse_int_field(f[1], f[2] - 1, &b.freq_mhz) < 0 ||
            parse_int_field(f[2], f[3] - 1, &b.signal_dbm) < 0) {
            goto next;
        }
        len = (size_t)(f[4] - f[3] - 1);
        if (len >= sizeof(flags)) {
            goto next;
        }
        memcpy(flags, f[3], len);
        flags[len] = '\0';
        len = (size_t)(end - f[4]);
        if (len > 0 && f[4][len - 1] == '\r') {
            len--;
        }
        if (len >= sizeof(ssid)) {
            goto next;
        }
        memcpy(ssid, f[4], len);
        ssid[len] = '\0';
        /* A tab inside the SSID field would have been escaped by printf_encode,
         * so a sixth field means the line is not what it claims to be. */
        if (memchr(ssid, '\t', len)) {
            goto next;
        }
        if (wifi_ssid_decode_printf(ssid, &b.ssid) < 0) {
            goto next;
        }
        b.security = wifi_security_from_flags(flags);
        if (wifi_ssid_is_hidden(&b.ssid)) {
            if (hidden) {
                (*hidden)++;
            }
            goto next;
        }
        if (count < max) {
            out[count++] = b;
        }
next:
        line = eol ? eol + 1 : end;
    }
    return count;
}

static int network_order(const void *pa, const void *pb)
{
    const struct wifi_network *a = pa;
    const struct wifi_network *b = pb;
    int n;

    if (a->signal_dbm != b->signal_dbm) {
        return a->signal_dbm > b->signal_dbm ? -1 : 1;
    }
    n = memcmp(a->ssid.bytes, b->ssid.bytes, a->ssid.len < b->ssid.len ? a->ssid.len : b->ssid.len);
    if (n != 0) {
        return n;
    }
    return (int)a->ssid.len - (int)b->ssid.len;
}

int wifi_networks_from_bss(const struct wifi_bss *bss, int n, struct wifi_network *out, int max)
{
    int count = 0;
    int i;
    int j;

    for (i = 0; i < n; i++) {
        for (j = 0; j < count; j++) {
            if (wifi_ssid_equal(&out[j].ssid, &bss[i].ssid)) {
                break;
            }
        }
        if (j == count) {
            if (count >= max) {
                continue;
            }
            count++;
            out[j].ssid = bss[i].ssid;
            out[j].bss_count = 0;
            out[j].signal_dbm = bss[i].signal_dbm;
            out[j].freq_mhz = bss[i].freq_mhz;
            out[j].security = bss[i].security;
        } else if (bss[i].signal_dbm > out[j].signal_dbm) {
            out[j].signal_dbm = bss[i].signal_dbm;
            out[j].freq_mhz = bss[i].freq_mhz;
            out[j].security = bss[i].security;
        }
        out[j].bss_count++;
    }
    qsort(out, (size_t)count, sizeof(out[0]), network_order);
    return count;
}

int wifi_kv_get(const char *reply, const char *key, char *out, size_t n)
{
    size_t klen = key ? strlen(key) : 0;
    const char *line = reply;

    if (!reply || klen == 0 || n == 0) {
        return -1;
    }
    while (*line) {
        const char *eol = strchr(line, '\n');
        const char *end = eol ? eol : line + strlen(line);

        if ((size_t)(end - line) > klen && strncmp(line, key, klen) == 0 && line[klen] == '=') {
            const char *v = line + klen + 1;
            size_t len = (size_t)(end - v);

            if (len > 0 && v[len - 1] == '\r') {
                len--;
            }
            if (len >= n) {
                len = n - 1;
            }
            memcpy(out, v, len);
            out[len] = '\0';
            return 0;
        }
        line = eol ? eol + 1 : end;
    }
    return -1;
}

int wifi_kv_get_int(const char *reply, const char *key, int *out)
{
    char buf[24];

    if (wifi_kv_get(reply, key, buf, sizeof(buf)) < 0) {
        return -1;
    }
    return parse_int_field(buf, buf + strlen(buf), out);
}

int wifi_signal_bars(int dbm)
{
    if (dbm >= -55) {
        return 4;
    }
    if (dbm >= -66) {
        return 3;
    }
    if (dbm >= -77) {
        return 2;
    }
    if (dbm >= -88) {
        return 1;
    }
    return 0;
}

/* ---- events ------------------------------------------------------------------ */

/* The integer after "name=" anywhere in msg, or -1. */
static int field_int(const char *msg, const char *name)
{
    size_t nlen = strlen(name);
    const char *p = msg;

    while ((p = strstr(p, name)) != NULL) {
        if ((p == msg || p[-1] == ' ') && p[nlen] == '=') {
            char *end;
            long v = strtol(p + nlen + 1, &end, 10);

            if (end != p + nlen + 1 && (*end == '\0' || *end == ' ') && v >= 0 && v < 100000) {
                return (int)v;
            }
            return -1;
        }
        p += nlen;
    }
    return -1;
}

static int starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

void wifi_parse_event(const char *msg, struct wifi_event *ev)
{
    memset(ev, 0, sizeof(*ev));
    ev->type = WIFI_EV_OTHER;
    ev->network_id = -1;
    ev->reason = -1;
    if (!msg) {
        return;
    }
    if (msg[0] == '<') {
        const char *gt = strchr(msg, '>');

        if (gt && gt - msg <= 3) {
            msg = gt + 1;
        }
    }
    if (starts(msg, "CTRL-EVENT-CONNECTED ")) {
        ev->type = WIFI_EV_CONNECTED;
        ev->network_id = field_int(msg, "[id");
        if (ev->network_id < 0) {
            /* "... completed [id=0 id_str=]" */
            const char *p = strstr(msg, "[id=");

            ev->network_id = p ? atoi(p + 4) : -1;
        }
    } else if (starts(msg, "CTRL-EVENT-DISCONNECTED ")) {
        ev->type = WIFI_EV_DISCONNECTED;
        ev->reason = field_int(msg, "reason");
        ev->locally_generated = field_int(msg, "locally_generated") == 1;
    } else if (starts(msg, "CTRL-EVENT-SCAN-RESULTS")) {
        ev->type = WIFI_EV_SCAN_RESULTS;
    } else if (starts(msg, "CTRL-EVENT-SCAN-FAILED")) {
        ev->type = WIFI_EV_SCAN_FAILED;
    } else if (starts(msg, "CTRL-EVENT-NETWORK-NOT-FOUND")) {
        ev->type = WIFI_EV_NETWORK_NOT_FOUND;
    } else if (starts(msg, "CTRL-EVENT-SSID-TEMP-DISABLED ")) {
        /* reason= is the last field and ssid="..." comes before it, so the
         * last occurrence is the real one: an SSID cannot forge it. */
        const char *reason = NULL;
        const char *p = msg;

        while ((p = strstr(p, " reason=")) != NULL) {
            reason = p + 8;
            p += 8;
        }
        ev->network_id = field_int(msg, "id");
        ev->type = reason && strncmp(reason, "WRONG_KEY", 9) == 0 &&
                           (reason[9] == '\0' || reason[9] == ' ' || reason[9] == '\n')
                       ? WIFI_EV_WRONG_KEY
                       : WIFI_EV_CONN_FAILED;
    } else if (starts(msg, "WPA: 4-Way Handshake failed - pre-shared key may be incorrect")) {
        ev->type = WIFI_EV_WRONG_KEY;
    } else if (starts(msg, "CTRL-EVENT-ASSOC-REJECT ")) {
        ev->type = WIFI_EV_ASSOC_REJECT;
        ev->reason = field_int(msg, "status_code");
    } else if (starts(msg, "CTRL-EVENT-AUTH-REJECT ")) {
        ev->type = WIFI_EV_AUTH_REJECT;
        ev->reason = field_int(msg, "status_code");
    } else if (starts(msg, "CTRL-EVENT-TERMINATING")) {
        ev->type = WIFI_EV_TERMINATING;
    }
}
