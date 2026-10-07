/*
 * A reply as message text. See rift_reply.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_reply.h"

#include "rift_format.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int rift_reply_possible(const struct rift_message *msg)
{
    return msg && msg->is_channel && msg->dir == RIFT_MSG_IN && msg->have_sender_name &&
           msg->sender_name[0];
}

/* The code point at s (well-formed UTF-8: remote text has been made so by
 * the service, and the composer's by the field), and its length in *len. */
static uint32_t cp_at(const char *s, size_t *len)
{
    const unsigned char *u = (const unsigned char *)s;

    if (u[0] < 0x80) {
        *len = 1;
        return u[0];
    }
    if ((u[0] & 0xE0) == 0xC0 && u[1]) {
        *len = 2;
        return ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    }
    if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
        *len = 3;
        return ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    }
    if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
        *len = 4;
        return ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) |
               ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
    }
    *len = 1;
    return 0xFFFD;
}

/* A code point that only shapes the one before it, or joins it to the next:
 * a cut must not fall in front of one, or the emoji it belongs to is drawn
 * as its parts. */
static int joins_previous(uint32_t cp)
{
    return cp == 0x200D || cp == 0xFE0E || cp == 0xFE0F || cp == 0x20E3 ||
           (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F);
}

static int regional(uint32_t cp)
{
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

/* How many bytes of s, at most max, end on a whole character, a whole emoji
 * sequence and - when one is past halfway - a whole word. *cut says whether
 * anything was left over. */
static size_t quote_length(const char *s, size_t max, int *cut)
{
    size_t at = 0;
    size_t safe = 0;   /* the last boundary no sequence runs across */
    size_t space = 0;  /* the last boundary before a space */
    int ri = 0;        /* regional indicators since the last other code point */
    uint32_t prev = 0;

    *cut = 0;
    while (s[at]) {
        size_t len;
        uint32_t cp = cp_at(s + at, &len);

        if (at > 0 && !joins_previous(cp) && prev != 0x200D && !(regional(cp) && (ri % 2) == 1)) {
            safe = at;
            if (cp == ' ') {
                space = at;
            }
        }
        if (at + len > max) {
            *cut = 1;
            break;
        }
        ri = regional(cp) ? ri + 1 : 0;
        prev = cp;
        at += len;
    }
    if (!*cut) {
        return at;
    }
    return (space > max / 2) ? space : safe;
}

size_t rift_reply_prefix(const struct rift_message *msg, char *out, size_t out_len)
{
    char name[RIFT_REPLY_NAME_MAX];
    char body[RIFT_MSG_TEXT_MAX];
    char quote[RIFT_REPLY_QUOTE_MAX + 4];
    const char *src;
    size_t n = 0;
    size_t q;
    size_t i;
    int cut;
    int w;

    if (!out || out_len == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!rift_reply_possible(msg)) {
        return 0;
    }
    /* The name without the two characters that would end the mention, then
     * cut to a name's length on a character boundary. */
    {
        char raw[RIFT_NAME_MAX];

        for (src = msg->sender_name; *src && n + 1 < sizeof(raw); src++) {
            if (*src != '[' && *src != ']') {
                raw[n++] = *src;
            }
        }
        raw[n] = '\0';
        rift_utf8_copy(name, sizeof(name), raw);
    }
    if (!name[0]) {
        return 0;
    }
    /* The words answered, on one line and without a '"' that would end the
     * quotation early. Leading spaces carry nothing. */
    src = rift_msg_body(msg);
    while (*src == ' ') {
        src++;
    }
    for (i = 0; src[i] && i + 1 < sizeof(body); i++) {
        char c = src[i];

        body[i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : (c == '"') ? '\'' : c;
    }
    body[i] = '\0';
    q = quote_length(body, RIFT_REPLY_QUOTE_MAX, &cut);
    while (q > 0 && body[q - 1] == ' ') {
        q--;
    }
    memcpy(quote, body, q);
    quote[q] = '\0';
    if (cut) {
        snprintf(quote + q, sizeof(quote) - q, "\xE2\x80\xA6");
    }
    w = snprintf(out, out_len, "@[%s] \"%s\" ", name, quote);
    if (w < 0 || (size_t)w >= out_len) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)w;
}

int rift_reply_parse(const char *body, struct rift_reply *out)
{
    const char *end;
    size_t n;

    if (!body || !out || body[0] != '@' || body[1] != '[') {
        return 0;
    }
    end = strchr(body + 2, ']');
    if (!end || end[1] != ' ') {
        return 0;
    }
    n = (size_t)(end - (body + 2));
    if (n == 0 || n >= sizeof(out->name)) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->name, body + 2, n);
    out->name[n] = '\0';
    out->rest = end + 2;
    /* A quotation, when one follows and is closed on this line. */
    /* A quotation, when one follows: the next '"' closes it (a prefix this
     * app writes holds none inside), and a space or the end comes after. */
    if (out->rest[0] == '"') {
        const char *close = strchr(out->rest + 1, '"');

        if (close && (close[1] == ' ' || close[1] == '\0') &&
            !memchr(out->rest, '\n', (size_t)(close - out->rest)) &&
            (size_t)(close - out->rest - 1) < sizeof(out->quote)) {
            n = (size_t)(close - out->rest - 1);
            memcpy(out->quote, out->rest + 1, n);
            out->quote[n] = '\0';
            out->rest = close[1] ? close + 2 : close + 1;
        }
    }
    return 1;
}
