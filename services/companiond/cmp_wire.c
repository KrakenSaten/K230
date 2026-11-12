/*
 * Companion protocol framing and small helpers. See cmp_wire.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmp_wire.h"

#include <string.h>

void cmp_decoder_init(struct cmp_decoder *d)
{
    memset(d, 0, sizeof(*d));
}

size_t cmp_decoder_feed(struct cmp_decoder *d, const uint8_t *data, size_t n,
                        size_t *frame_len)
{
    size_t used = 0;

    *frame_len = 0;
    while (used < n) {
        if (d->want == 0) {
            uint8_t type;
            uint16_t len;

            d->hdr[d->hdr_len++] = data[used++];
            if (d->hdr_len < CMP_HDR_LEN) {
                continue;
            }
            d->hdr_len = 0;
            type = d->hdr[0];
            len = (uint16_t)(d->hdr[1] | (d->hdr[2] << 8));
            /* Upstream holds a header only when both its type and its
             * length are non-zero (hasReceivedFrameHeader); anything else is
             * three bytes read and dropped, and the next three are a header. */
            if (type == 0 || len == 0) {
                d->empty++;
                continue;
            }
            d->want = len;
            d->len = 0;
            d->skipping = 0;
            if (type != CMP_TYPE_FROM_APP) {
                d->skipping = 1;
                d->skipped_type++;
            } else if (len > CMP_MAX_FRAME) {
                d->skipping = 1;
                d->skipped_size++;
            }
            continue;
        }
        {
            size_t take = n - used;

            if (take > d->want) {
                take = d->want;
            }
            if (!d->skipping) {
                memcpy(d->buf + d->len, data + used, take);
                d->len += take;
            }
            used += take;
            d->want -= take;
            if (d->want == 0 && !d->skipping) {
                d->frames++;
                *frame_len = d->len;
                return used;
            }
        }
    }
    return used;
}

size_t cmp_frame_encode(const uint8_t *payload, size_t len, uint8_t *out, size_t cap)
{
    if (len == 0 || len > CMP_MAX_FRAME || cap < len + CMP_HDR_LEN) {
        return 0;
    }
    out[0] = CMP_TYPE_TO_APP;
    cmp_put_u16(out + 1, (uint16_t)len);
    memcpy(out + CMP_HDR_LEN, payload, len);
    return len + CMP_HDR_LEN;
}

/* Length of the well-formed UTF-8 sequence starting at s (n bytes left), or
 * 0 when it is not one. Overlong forms, surrogates and code points above
 * U+10FFFF are not well formed. */
static size_t utf8_seq(const unsigned char *s, size_t n)
{
    unsigned char c = s[0];
    size_t need;
    uint32_t cp;
    size_t i;

    if (c < 0x80) {
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        need = 2;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 3;
        cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (n < need) {
        return 0;
    }
    for (i = 1; i < need; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if ((need == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
        (need == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
        return 0;
    }
    return need;
}

size_t cmp_utf8_prefix(const char *s, size_t n, size_t max)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t at = 0;

    while (at < n) {
        size_t k = utf8_seq(u + at, n - at);

        if (k == 0 || at + k > max) {
            break;
        }
        at += k;
    }
    return at;
}

int cmp_text_ok(const uint8_t *s, size_t n)
{
    size_t at = 0;

    while (at < n) {
        size_t k = utf8_seq(s + at, n - at);

        if (k == 0) {
            return 0;
        }
        if (k == 1 && ((s[at] < 0x20 && s[at] != '\n' && s[at] != '\t') || s[at] == 0x7F)) {
            return 0;
        }
        /* C1 controls (U+0080..U+009F), which meshcored refuses too. */
        if (k == 2 && s[at] == 0xC2 && s[at + 1] < 0xA0) {
            return 0;
        }
        at += k;
    }
    return 1;
}

void cmp_hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static int nibble(char c)
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

int cmp_unhex(const char *in, uint8_t *out, size_t n)
{
    size_t i;

    if (!in || strlen(in) != 2 * n) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        int hi = nibble(in[2 * i]);
        int lo = nibble(in[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}
