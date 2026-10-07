/*
 * RIFT: emoji sequences folded into one code point each, for display
 * (rift_emoji.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_emoji.h"

#include <string.h>

/* Code points one piece of text is folded in: a message is at most 160 bytes
 * (RIFT_MSG_TEXT_MAX), a preview a little more; anything past this is not
 * shown, as out would not hold it either. */
#define FOLD_MAX 512

/* One code point; its length, or 0 when s starts no well-formed sequence. */
static size_t decode(const unsigned char *s, uint32_t *cp)
{
    uint32_t c = s[0];
    size_t len;
    uint32_t min;
    size_t i;

    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        c &= 0x1F;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        c &= 0x0F;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        c &= 0x07;
        min = 0x10000;
    } else {
        return 0;
    }
    for (i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        c = (c << 6) | (s[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFFu || (c >= 0xD800 && c <= 0xDFFF)) {
        return 0;
    }
    *cp = c;
    return len;
}

static size_t encode(uint32_t c, char *o)
{
    if (c < 0x80) {
        o[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        o[0] = (char)(0xC0 | (c >> 6));
        o[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12));
        o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (c >> 18));
    o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* Sequence i against the n code points at cp: <0, 0, >0 as strcmp. */
static int compare(unsigned i, const uint32_t *cp, size_t n)
{
    const uint32_t *s = &rift_emoji_seq_cps[rift_emoji_seqs[i].at];
    size_t len = rift_emoji_seqs[i].len;
    size_t k;

    for (k = 0; k < len && k < n; k++) {
        if (s[k] != cp[k]) {
            return s[k] < cp[k] ? -1 : 1;
        }
    }
    return len < n ? -1 : len > n ? 1 : 0;
}

/* The sequence that is exactly these n code points, or -1. */
static int lookup(const uint32_t *cp, size_t n)
{
    unsigned lo = 0;
    unsigned hi = rift_emoji_seq_count;

    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = compare(mid, cp, n);

        if (c == 0) {
            return (int)mid;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return -1;
}

size_t rift_emoji_fold(const char *in, char *out, size_t out_len)
{
    uint32_t cps[FOLD_MAX];
    const unsigned char *p = (const unsigned char *)in;
    size_t n = 0;
    size_t i = 0;
    size_t o = 0;

    if (!out || out_len == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!in) {
        return 0;
    }
    /* Normalised: what only selects a presentation, or a skin tone, goes. */
    while (*p && n < FOLD_MAX) {
        uint32_t cp = 0;
        size_t l = decode(p, &cp);

        if (l == 0) {
            l = 1;
            cp = 0xFFFDu;
        } else if (cp >= RIFT_EMOJI_PUA) {
            cp = 0xFFFDu;
        }
        p += l;
        if (cp == 0xFE0Eu || cp == 0xFE0Fu || (cp >= 0x1F3FBu && cp <= 0x1F3FFu)) {
            continue;
        }
        cps[n++] = cp;
    }
    while (i < n) {
        char buf[4];
        size_t want = n - i < RIFT_EMOJI_SEQ_MAX ? n - i : RIFT_EMOJI_SEQ_MAX;
        uint32_t cp = cps[i];
        size_t step = 1;
        size_t len;

        for (; want >= 2; want--) {
            int s = lookup(&cps[i], want);

            if (s >= 0) {
                cp = RIFT_EMOJI_PUA + (uint32_t)s;
                step = want;
                break;
            }
        }
        i += step;
        /* Left over from a sequence with no image: the joiner, the keycap
         * mark and tag characters shape nothing on their own. */
        if (step == 1 && (cp == 0x200Du || cp == 0x20E3u || (cp >= 0xE0020u && cp <= 0xE007Fu))) {
            continue;
        }
        len = encode(cp, buf);
        if (o + len + 1 > out_len) {
            break;
        }
        memcpy(out + o, buf, len);
        o += len;
    }
    out[o] = '\0';
    return o;
}
