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
