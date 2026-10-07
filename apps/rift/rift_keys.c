/*
 * Channel keys: made or checked here, kept nowhere. See rift_keys.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_keys.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

/* ---- SHA-256 (FIPS 180-4) ----------------------------------------------- *
 *
 * Here and not linked from the protocol library: RIFT links no protocol
 * code (tests/rift_lint.sh), and a hashtag key is one hash of a short name. */

static const uint32_t k256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

static uint32_t ror(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, hh;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0];
    b = h[1];
    c = h[2];
    d = h[3];
    e = h[4];
    f = h[5];
    g = h[6];
    hh = h[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + s1 + ch + k256[i] + w[i];
        uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;

        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void rift_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    uint8_t tail[128];
    uint64_t bits = (uint64_t)len * 8u;
    size_t full = len / 64;
    size_t rest = len % 64;
    size_t tail_len;
    size_t i;

    for (i = 0; i < full; i++) {
        sha256_block(h, data + 64 * i);
    }
    memset(tail, 0, sizeof(tail));
    if (rest) {
        memcpy(tail, data + 64 * full, rest);
    }
    tail[rest] = 0x80;
    tail_len = rest + 1 + 8 <= 64 ? 64 : 128;
    for (i = 0; i < 8; i++) {
        tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
    }
    sha256_block(h, tail);
    if (tail_len == 128) {
        sha256_block(h, tail + 64);
    }
    for (i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

/* ---- base64 (RFC 4648) ----------------------------------------------------- */

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t rift_base64_encode(const uint8_t *in, size_t len, char *out, size_t out_len)
{
    size_t need = 4 * ((len + 2) / 3) + 1;
    size_t o = 0;
    size_t i;

    if (!out || out_len < need) {
        return 0;
    }
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];

        out[o++] = b64_alphabet[(v >> 18) & 63];
        out[o++] = b64_alphabet[(v >> 12) & 63];
        out[o++] = b64_alphabet[(v >> 6) & 63];
        out[o++] = b64_alphabet[v & 63];
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0);

        out[o++] = b64_alphabet[(v >> 18) & 63];
        out[o++] = b64_alphabet[(v >> 12) & 63];
        out[o++] = i + 1 < len ? b64_alphabet[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

static int b64_value(char c)
{
    const char *at = c ? strchr(b64_alphabet, c) : NULL;

    return at ? (int)(at - b64_alphabet) : -1;
}

/* Strict: every character from the alphabet, padding only at the end and
 * only as much as the length needs, and the bits padding drops all zero -
 * the same key can be written one way only. Returns the decoded length, or
 * -1. */
static int b64_decode(const char *in, size_t len, uint8_t *out, size_t out_len)
{
    size_t o = 0;
    size_t i;
    int pad = 0;

    if (len == 0 || len % 4 != 0) {
        return -1;
    }
    if (in[len - 1] == '=') {
        pad++;
        if (in[len - 2] == '=') {
            pad++;
        }
    }
    for (i = 0; i < len; i += 4) {
        int v[4];
        int j;

        for (j = 0; j < 4; j++) {
            char c = in[i + (size_t)j];

            if (c == '=' && i + 4 == len && j >= 4 - pad) {
                v[j] = 0;
            } else {
                v[j] = b64_value(c);
                if (v[j] < 0) {
                    return -1;
                }
            }
        }
        if (o + 3 - (i + 4 == len ? (size_t)pad : 0) > out_len) {
            return -1;
        }
        out[o++] = (uint8_t)(v[0] << 2 | v[1] >> 4);
        if (!(i + 4 == len && pad == 2)) {
            out[o++] = (uint8_t)((v[1] & 15) << 4 | v[2] >> 2);
        } else if (v[1] & 15) {
            return -1;
        }
        if (!(i + 4 == len && pad >= 1)) {
            out[o++] = (uint8_t)((v[2] & 3) << 6 | v[3]);
        } else if (pad == 1 && (v[2] & 3)) {
            return -1;
        }
    }
    return (int)o;
}

/* ---- names ----------------------------------------------------------------- */

static void say(char *why, size_t why_len, const char *what)
{
    if (why && why_len) {
        snprintf(why, why_len, "%s", what);
    }
}

int rift_channel_name_check(const char *name, char *why, size_t why_len)
{
    size_t i;
    int blank = 1;

    if (!name || !name[0]) {
        say(why, why_len, "A channel needs a name.");
        return -1;
    }
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20 || c == 0x7F) {
            say(why, why_len, "A channel name is one line, with no control characters.");
            return -1;
        }
        if (c != ' ') {
            blank = 0;
        }
    }
    if (blank) {
        say(why, why_len, "A channel needs a name.");
        return -1;
    }
    if (i > RIFT_CHANNEL_NAME_BYTES) {
        say(why, why_len, "That name is longer than the 31 bytes a channel name can be.");
        return -1;
    }
    return 0;
}

int rift_hashtag_name(const char *typed, char *out, size_t out_len, char *why, size_t why_len)
{
    size_t start = 0;
    size_t len;
    char buf[64];

    if (!out || out_len < 2) {
        return -1;
    }
    out[0] = '\0';
    if (!typed) {
        typed = "";
    }
    while (typed[start] == ' ') {
        start++;
    }
    len = strlen(typed + start);
    while (len > 0 && typed[start + len - 1] == ' ') {
        len--;
    }
    if (len > 0 && typed[start] == '#') {
        start++;
        len--;
    }
    if (len == 0) {
        say(why, why_len, "A hashtag channel needs a name after the #.");
        return -1;
    }
    /* The '#' is part of what is hashed, so a name that only fits without it
     * would key a different channel from the one typed: refused rather than
     * cut (MyMesh::addGroupChannelHashtag refuses it the same way). */
    if (len + 1 > RIFT_CHANNEL_NAME_BYTES || len + 2 > sizeof(buf) || len + 2 > out_len) {
        say(why, why_len, "That name is too long: with its # a channel name is at most 31 bytes.");
        return -1;
    }
    buf[0] = '#';
    memcpy(buf + 1, typed + start, len);
    buf[len + 1] = '\0';
    if (rift_channel_name_check(buf, why, why_len) != 0) {
        return -1;
    }
    memcpy(out, buf, len + 2);
    return 0;
}

int rift_hashtag_key(const char *canonical, char *b64, size_t b64_len)
{
    uint8_t digest[32];

    if (!canonical || canonical[0] != '#' || !canonical[1]) {
        return -1;
    }
    rift_sha256((const uint8_t *)canonical, strlen(canonical), digest);
    /* The first 16 bytes: a 128-bit key, as upstream derives it. */
    return rift_base64_encode(digest, 16, b64, b64_len) ? 0 : -1;
}

int rift_random_key(char *b64, size_t b64_len)
{
    uint8_t key[16];
    size_t got = 0;
    int zero = 1;
    size_t i;

    while (got < sizeof(key)) {
        ssize_t n = getrandom(key + got, sizeof(key) - got, 0);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            memset(key, 0, sizeof(key));
            return -1;
        }
        got += (size_t)n;
    }
    for (i = 0; i < sizeof(key); i++) {
        zero &= key[i] == 0;
    }
    /* All zero is what an empty MeshCore slot holds; the service refuses it,
     * and a kernel that hands it out is not one to trust with a key. */
    if (zero || !rift_base64_encode(key, sizeof(key), b64, b64_len)) {
        memset(key, 0, sizeof(key));
        return -1;
    }
    memset(key, 0, sizeof(key));
    return 0;
}

int rift_key_check(const char *typed, char *out, size_t out_len, char *why, size_t why_len)
{
    uint8_t key[33];
    size_t start = 0;
    size_t len;
    int n;
    int zero = 1;
    int upper_zero = 1;
    int i;

    if (!typed || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    while (typed[start] == ' ') {
        start++;
    }
    len = strlen(typed + start);
    while (len > 0 && typed[start + len - 1] == ' ') {
        len--;
    }
    if (len == 0) {
        say(why, why_len, "Paste the channel's key.");
        return -1;
    }
    if (len >= out_len) {
        say(why, why_len, "That is longer than a channel key: base64 of 16 or 32 bytes.");
        return -1;
    }
    n = b64_decode(typed + start, len, key, sizeof(key));
    if (n != 16 && n != 32) {
        say(why, why_len,
            n < 0 ? "That is not a base64 key (A-Z a-z 0-9 + / and = padding)."
                  : "A channel key is 16 or 32 bytes: 24 or 44 base64 characters.");
        memset(key, 0, sizeof(key));
        return -1;
    }
    for (i = 0; i < n; i++) {
        zero &= key[i] == 0;
        if (i >= 16) {
            upper_zero &= key[i] == 0;
        }
    }
    memset(key, 0, sizeof(key));
    if (zero) {
        say(why, why_len, "An all-zero key is an empty slot, not a channel.");
        return -1;
    }
    if (n == 32 && upper_zero) {
        say(why, why_len,
            "That 32-byte key has an all-zero second half; MeshCore reads it as a different "
            "16-byte key. Use the 16-byte form.");
        return -1;
    }
    memcpy(out, typed + start, len);
    out[len] = '\0';
    return 0;
}
