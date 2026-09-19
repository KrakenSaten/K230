/*
 * meshcored: hex and validation. See mcd_util.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "mcd_util.h"

#include <string.h>

static int hex_digit(char c)
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

int mcd_hex_decode(const char *hex, uint8_t *dst, size_t dst_size)
{
    size_t len;
    size_t i;

    if (!hex || !dst) {
        return -1;
    }
    len = strlen(hex);
    if (len % 2 != 0) {
        return -1;
    }
    if (len / 2 > dst_size) {
        return -1;
    }
    for (i = 0; i < len; i += 2) {
        int hi = hex_digit(hex[i]);
        int lo = hex_digit(hex[i + 1]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        dst[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(len / 2);
}

char *mcd_hex_encode(const uint8_t *src, size_t len, char *dst, size_t dst_size)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    if (!dst || dst_size < len * 2 + 1) {
        return NULL;
    }
    for (i = 0; i < len; i++) {
        dst[i * 2] = digits[src[i] >> 4];
        dst[i * 2 + 1] = digits[src[i] & 0x0f];
    }
    dst[len * 2] = '\0';
    return dst;
}

int mcd_key_prefix_parse(const char *hex, uint8_t *dst, size_t dst_size)
{
    size_t len;

    if (!hex) {
        return -1;
    }
    len = strlen(hex);
    if (len < 2 || len > 64) {
        return -1;
    }
    return mcd_hex_decode(hex, dst, dst_size);
}

/* ---- remote text on the way out ---------------------------------------- */

/* How many bytes the sequence starting with this lead byte claims, or 0 when
 * it cannot start one. */
static int utf8_len(unsigned char c)
{
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 0;  /* a continuation byte, or 0xF8..0xFF, which start nothing */
}

/* Decode one well-formed sequence at p, rejecting the encodings that are
 * valid-looking and wrong: an overlong form (the shortest encoding is the
 * only legal one), a surrogate half (never legal in UTF-8), and anything
 * above U+10FFFF. Returns the byte count, or 0. */
static int utf8_decode(const unsigned char *p, size_t avail, uint32_t *out)
{
    int n = utf8_len(p[0]);
    uint32_t cp;
    int i;

    if (n == 0 || (size_t)n > avail) {
        return 0;
    }
    if (n == 1) {
        *out = p[0];
        return 1;
    }
    cp = (uint32_t)(p[0] & (0xFF >> (n + 1)));
    for (i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3F);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000)) {
        return 0;  /* overlong */
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        return 0;  /* a surrogate half */
    }
    if (cp > 0x10FFFF) {
        return 0;
    }
    *out = cp;
    return n;
}

static bool codepoint_is_safe(uint32_t cp)
{
    if (cp == '\n' || cp == '\t') {
        return true;
    }
    if (cp < 0x20 || cp == 0x7F) {
        return false;           /* C0 controls and DEL */
    }
    if (cp >= 0x80 && cp <= 0x9F) {
        return false;           /* C1 controls, which some terminals act on */
    }
    return true;
}

char *mcd_text_sanitize(const char *src, char *dst, size_t dst_size)
{
    static const char replacement[3] = { (char)0xEF, (char)0xBF, (char)0xBD };  /* U+FFFD */
    size_t in = 0;
    size_t out = 0;
    size_t len;

    if (!dst || dst_size == 0) {
        return dst;
    }
    if (!src) {
        dst[0] = '\0';
        return dst;
    }
    len = strlen(src);
    while (in < len) {
        const unsigned char *p = (const unsigned char *)src + in;
        uint32_t cp = 0;
        int n = utf8_decode(p, len - in, &cp);
        const char *emit;
        size_t emit_len;

        if (n > 0 && codepoint_is_safe(cp)) {
            emit = src + in;
            emit_len = (size_t)n;
            in += (size_t)n;
        } else {
            emit = replacement;
            emit_len = sizeof(replacement);
            /* How far to step depends on which of the two this was, and the
             * distinction matters.
             *
             * A sequence that decoded correctly and merely carries something
             * unwanted - a C1 control, say - is consumed whole: its length
             * was stated by bytes that were themselves well formed, so
             * trusting it is safe, and stepping one byte instead would leave
             * its continuation bytes behind to be reported as a second fault.
             *
             * A sequence that did NOT decode is stepped one byte only. Its
             * length was never established, and letting a bad lead byte say
             * how much of what follows to swallow is how a single stray byte
             * erases the rest of a name. */
            in += (n > 0) ? (size_t)n : 1;
        }
        if (out + emit_len + 1 > dst_size) {
            break;  /* truncate on a character boundary */
        }
        memcpy(dst + out, emit, emit_len);
        out += emit_len;
    }
    dst[out] = '\0';
    return dst;
}

bool mcd_text_acceptable(const char *text, size_t max_len)
{
    size_t i;

    if (!text) {
        return false;
    }
    for (i = 0; text[i] != '\0'; i++) {
        unsigned char c = (unsigned char)text[i];

        if (i >= max_len) {
            return false;
        }
        /* Control characters are refused apart from newline and tab. A
         * message is text; an escape sequence in it would be interpreted by
         * whatever terminal or panel showed it, not by this daemon. */
        if (c < 0x20 && c != '\n' && c != '\t') {
            return false;
        }
        if (c == 0x7f) {
            return false;
        }
    }
    return i > 0;
}
