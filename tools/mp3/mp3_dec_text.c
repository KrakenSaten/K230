/*
 * Tag text for the screen: valid UTF-8, one line, bounded. See
 * mp3_decoder.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mp3_decoder.h"

#include <string.h>

/* The length of the valid UTF-8 sequence at p (1..4), or 0. Overlong forms,
 * surrogates and code points above U+10FFFF are not valid. */
static size_t utf8_len(const unsigned char *p)
{
    unsigned c = p[0];
    size_t n;
    size_t i;
    unsigned cp;

    if (c < 0x80) {
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    for (i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
        return 0;
    }
    return n;
}

void mp3_dec_clean_text(char *dst, size_t dstlen, const char *src)
{
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    size_t w = 0;

    if (!dst || dstlen == 0) {
        return;
    }
    while (*p == ' ') {
        p++;
    }
    while (*p) {
        size_t n = utf8_len(p);

        if (n == 0) {
            p++; /* an invalid byte is dropped */
            continue;
        }
        if (n == 1 && (*p < 0x20 || *p == 0x7F)) {
            /* A tab or a line break inside a tag reads as a space. */
            if ((*p == '\t' || *p == '\n' || *p == '\r') && w > 0 && dst[w - 1] != ' ' && w + 1 < dstlen) {
                dst[w++] = ' ';
            }
            p++;
            continue;
        }
        if (n == 1 && *p == ' ' && w == 0) {
            p++; /* what was dropped may have stood before a space */
            continue;
        }
        if (w + n >= dstlen) {
            break; /* never a cut character */
        }
        memcpy(dst + w, p, n);
        w += n;
        p += n;
    }
    while (w > 0 && dst[w - 1] == ' ') {
        w--;
    }
    dst[w] = '\0';
}
