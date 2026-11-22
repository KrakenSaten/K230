/*
 * gatewayd: small pure helpers (encodings, text checks, a constant-time
 * compare, a token bucket). No I/O, no OpenSSL.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_UTIL_H
#define GATEWAYD_GW_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* out holds 2 * n + 1 bytes. Lower case. */
void gw_hex(const uint8_t *in, size_t n, char *out);
/* Exactly 2 * n hex digits, either case, then NUL. 0, or -1. */
int gw_unhex(const char *s, uint8_t *out, size_t n);

/* base64url without padding. Returns the length written (NUL excluded), or
 * 0 when cap is too small. */
size_t gw_b64url(const uint8_t *in, size_t n, char *out, size_t cap);
/* Decodes exactly `want` bytes from an unpadded base64url string with no
 * other character and no excess bits set. 0, or -1. */
int gw_unb64url(const char *s, uint8_t *out, size_t want);

/* Well-formed UTF-8: no overlong form, no surrogate, nothing above U+10FFFF. */
bool gw_utf8_ok(const uint8_t *p, size_t n);
/* A client's name: 1 to GW_NAME_MAX bytes of well-formed UTF-8 with no
 * control character. */
bool gw_name_ok(const char *s);

/* 1 when equal. The time taken does not depend on where they differ. */
int gw_ct_eq(const void *a, const void *b, size_t n);

/* A token bucket in whole tokens. */
struct gw_bucket {
    int64_t last_ms;
    int64_t milli;      /* tokens * 1000 */
    int32_t burst;
    int32_t per_s;
};
void gw_bucket_init(struct gw_bucket *b, int burst, int per_s, int64_t now_ms);
/* Takes one token: true, or false when there is none. */
bool gw_bucket_take(struct gw_bucket *b, int64_t now_ms);

#endif
