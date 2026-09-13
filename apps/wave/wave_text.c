/*
 * Wave's text rule. See wave_text.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "wave_text.h"

/* Length of the UTF-8 sequence at p (of at most n bytes) and its code
 * point, or 0 when it is not a well-formed, shortest-form scalar value. */
static size_t utf8_next(const unsigned char *p, size_t n, unsigned long *cp)
{
    unsigned long c;
    size_t len;
    size_t i;

    if (n == 0) {
        return 0;
    }
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if (p[0] >= 0xC2 && p[0] <= 0xDF) {
        len = 2;
        c = p[0] & 0x1F;
    } else if (p[0] >= 0xE0 && p[0] <= 0xEF) {
        len = 3;
        c = p[0] & 0x0F;
    } else if (p[0] >= 0xF0 && p[0] <= 0xF4) {
        len = 4;
        c = p[0] & 0x07;
    } else {
        return 0;
    }
    if (n < len) {
        return 0;
    }
    for (i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return 0;
        }
        c = (c << 6) | (p[i] & 0x3F);
    }
    if ((len == 3 && c < 0x800) || (len == 4 && (c < 0x10000 || c > 0x10FFFF)) ||
        (c >= 0xD800 && c <= 0xDFFF)) {
        return 0;
    }
    *cp = c;
    return len;
}

static int is_control(unsigned long cp)
{
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F);
}

int wave_text_clean(const char *bytes, size_t len)
{
    const unsigned char *p = (const unsigned char *)bytes;
    size_t off = 0;

    while (off < len) {
        unsigned long cp = 0;
        size_t n = utf8_next(p + off, len - off, &cp);

        if (n == 0 || is_control(cp)) {
            return 0;
        }
        off += n;
    }
    return 1;
}
