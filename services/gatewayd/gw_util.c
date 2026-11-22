/*
 * gatewayd: small pure helpers. See gw_util.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_util.h"

#include "gw.h"

#include <string.h>

void gw_hex(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static int hexval(char c)
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

int gw_unhex(const char *s, uint8_t *out, size_t n)
{
    size_t i;

    if (s == NULL || strlen(s) != 2 * n) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        int hi = hexval(s[2 * i]);
        int lo = hexval(s[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t gw_b64url(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t need = (n * 4 + 2) / 3;
    size_t i, o = 0;

    if (cap < need + 1) {
        return 0;
    }
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];

        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;

        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);

        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
    }
    out[o] = '\0';
    return o;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return -1;
}

int gw_unb64url(const char *s, uint8_t *out, size_t want)
{
    size_t need = (want * 4 + 2) / 3;
    size_t i, o = 0;
    uint32_t acc = 0;
    int bits = 0;

    if (s == NULL || strlen(s) != need) {
        return -1;
    }
    for (i = 0; i < need; i++) {
        int v = b64val(s[i]);

        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= want) {
                return -1;
            }
            out[o++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1u;
        }
    }
    /* One encoding per value: the bits left over must be zero. */
    return (o == want && acc == 0) ? 0 : -1;
}

bool gw_utf8_ok(const uint8_t *p, size_t n)
{
    size_t i = 0;

    while (i < n) {
        uint8_t c = p[i];
        size_t more;
        uint32_t cp, min;
        size_t k;

        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xe0) == 0xc0) {
            more = 1; cp = c & 0x1f; min = 0x80;
        } else if ((c & 0xf0) == 0xe0) {
            more = 2; cp = c & 0x0f; min = 0x800;
        } else if ((c & 0xf8) == 0xf0) {
            more = 3; cp = c & 0x07; min = 0x10000;
        } else {
            return false;
        }
        if (n - i <= more) {
            return false;
        }
        for (k = 1; k <= more; k++) {
            if ((p[i + k] & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (p[i + k] & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            return false;
        }
        i += more + 1;
    }
    return true;
}

bool gw_name_ok(const char *s)
{
    size_t n, i;

    if (s == NULL) {
        return false;
    }
    n = strlen(s);
    if (n < 1 || n > GW_NAME_MAX || !gw_utf8_ok((const uint8_t *)s, n)) {
        return false;
    }
    for (i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];

        if (c < 0x20 || c == 0x7f) {
            return false;
        }
        /* C1 controls, U+0080 to U+009F: 0xC2 0x80..0x9F. */
        if (c == 0xc2 && i + 1 < n && (uint8_t)s[i + 1] < 0xa0) {
            return false;
        }
    }
    return true;
}

int gw_ct_eq(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a;
    const volatile uint8_t *y = b;
    uint8_t diff = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        diff |= (uint8_t)(x[i] ^ y[i]);
    }
    return diff == 0;
}

void gw_bucket_init(struct gw_bucket *b, int burst, int per_s, int64_t now_ms)
{
    b->burst = burst;
    b->per_s = per_s;
    b->milli = (int64_t)burst * 1000;
    b->last_ms = now_ms;
}

bool gw_bucket_take(struct gw_bucket *b, int64_t now_ms)
{
    int64_t dt = now_ms - b->last_ms;
    int64_t cap = (int64_t)b->burst * 1000;

    if (dt > 0) {
        /* dt ms at per_s tokens a second is dt * per_s thousandths. */
        if (dt > 3600000) {
            dt = 3600000;
        }
        b->milli += dt * b->per_s;
        if (b->milli > cap) {
            b->milli = cap;
        }
        b->last_ms = now_ms;
    }
    if (b->milli < 1000) {
        return false;
    }
    b->milli -= 1000;
    return true;
}
