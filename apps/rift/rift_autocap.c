/*
 * RIFT: whether the next letter starts a sentence (rift_autocap.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_autocap.h"

#include <stddef.h>

static int is_letter(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= 0xC0u && c <= 0x24Fu && c != 0xD7u && c != 0xF7u);
}

static int is_space(uint32_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0u;
}

static int is_end(uint32_t c)
{
    return c == '.' || c == '!' || c == '?' || c == 0x2026u;
}

/* What may stand between a sentence end and the space after it: closing
 * quotation marks and brackets ("Stop." then, (yes!) and). */
static int is_closer(uint32_t c)
{
    return c == '"' || c == '\'' || c == ')' || c == ']' || c == '}' || c == 0xBBu ||
           c == 0x2019u || c == 0x201Du;
}

/* The character ending just before *pos, which is moved to its start. A
 * stray continuation byte is a character of its own. */
static uint32_t prev_char(const unsigned char *s, size_t *pos)
{
    size_t end = *pos;
    size_t p = end - 1;
    uint32_t c;
    size_t len;
    size_t i;

    while (p > 0 && end - p < 4 && (s[p] & 0xC0u) == 0x80u) {
        p--;
    }
    c = s[p];
    len = c < 0x80u ? 1 : c >= 0xF0u ? 4 : c >= 0xE0u ? 3 : c >= 0xC0u ? 2 : 1;
    if (len != end - p) {
        /* Not one whole character: the last byte alone. */
        *pos = end - 1;
        return s[end - 1];
    }
    if (len > 1) {
        c &= 0x3Fu >> (len - 1);
    }
    for (i = 1; i < len; i++) {
        c = (c << 6) | (s[p + i] & 0x3Fu);
    }
    *pos = p;
    return c;
}

/* Bytes before the caret'th character, as LVGL counts them. */
static size_t caret_bytes(const unsigned char *s, uint32_t caret)
{
    size_t p = 0;

    while (s[p] && caret > 0) {
        p++;
        while ((s[p] & 0xC0u) == 0x80u) {
            p++;
        }
        caret--;
    }
    return p;
}

int rift_autocap_wanted(const char *text, uint32_t caret)
{
    const unsigned char *s = (const unsigned char *)(text ? text : "");
    size_t pos = caret_bytes(s, caret);
    int spaced = 0; /* white space seen, and since then only closers */

    /* Back from the caret to the letter before it, or to the start. */
    while (pos > 0) {
        uint32_t c = prev_char(s, &pos);

        if (is_letter(c)) {
            return 0;
        }
        if (is_space(c)) {
            spaced = 1;
            continue;
        }
        if (is_closer(c) && spaced) {
            continue;
        }
        if (is_end(c) && spaced) {
            /* The whole run: "?!" and "!!!" end, one dot ends, two or more
             * dots or an ellipsis character go on. */
            int dots = c == '.';
            int other = c == '!' || c == '?';
            int ellipsis = c == 0x2026u;

            while (pos > 0) {
                size_t back = pos;
                uint32_t d = prev_char(s, &back);

                if (!is_end(d)) {
                    break;
                }
                dots += d == '.';
                other += d == '!' || d == '?';
                ellipsis += d == 0x2026u;
                pos = back;
            }
            if (other > 0 || (dots == 1 && ellipsis == 0)) {
                return 1;
            }
        }
        /* An emoji, a digit, other punctuation: not a letter, so whatever
         * capital was due is still due, but no sentence end is spaced off by
         * what came after it. */
        spaced = 0;
    }
    return 1;
}
