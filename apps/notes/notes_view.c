/*
 * PocketNotes text rules. See notes_view.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "notes_view.h"

#include <string.h>

static int is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

/* Length of the UTF-8 character starting here, or 0 when it is not one. */
static size_t utf8_len(const unsigned char *p)
{
    if (p[0] < 0x80u) {
        return 1;
    }
    if ((p[0] & 0xE0u) == 0xC0u) {
        return (p[1] & 0xC0u) == 0x80u ? 2 : 0;
    }
    if ((p[0] & 0xF0u) == 0xE0u) {
        return ((p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) ? 3 : 0;
    }
    if ((p[0] & 0xF8u) == 0xF0u) {
        return ((p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u &&
                (p[3] & 0xC0u) == 0x80u)
                   ? 4
                   : 0;
    }
    return 0;
}

int notes_text_is_utf8(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    if (!text) {
        return 0;
    }
    while (*p) {
        size_t n = utf8_len(p);
        size_t i;

        if (n == 0) {
            return 0;
        }
        /* utf8_len only looked as far as it needed; make sure the character
         * does not run past the end of the string. */
        for (i = 1; i < n; i++) {
            if (p[i] == '\0') {
                return 0;
            }
        }
        p += n;
    }
    return 1;
}

int notes_text_is_blank(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    if (!text) {
        return 1;
    }
    for (; *p; p++) {
        if (!is_space(*p)) {
            return 0;
        }
    }
    return 1;
}

size_t notes_text_chars(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    size_t n = 0;

    if (!text) {
        return 0;
    }
    for (; *p; p++) {
        /* Every character has exactly one byte that is not a continuation
         * byte, so counting those counts characters. */
        if ((*p & 0xC0u) != 0x80u) {
            n++;
        }
    }
    return n;
}

void notes_title_unreadable(char *out, size_t out_len)
{
    if (out && out_len > 0) {
        strncpy(out, "Unreadable note", out_len - 1);
        out[out_len - 1] = '\0';
    }
}

void notes_title_from_text(const char *text, char *out, size_t out_len)
{
    const char *line;
    const char *end;
    size_t n;
    size_t copied = 0;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!text) {
        text = "";
    }

    /* The first line with something on it. A note that opens with blank
     * lines is titled by the first line that says anything. */
    line = text;
    for (;;) {
        const char *nl = strchr(line, '\n');
        const char *stop = nl ? nl : line + strlen(line);
        const char *s = line;

        while (s < stop && is_space((unsigned char)*s)) {
            s++;
        }
        if (s < stop) {
            line = s;
            end = stop;
            break;
        }
        if (!nl) {
            strncpy(out, NOTES_UNTITLED, out_len - 1);
            out[out_len - 1] = '\0';
            return;
        }
        line = nl + 1;
    }

    /* Drop trailing whitespace, then copy whole characters only. */
    while (end > line && is_space((unsigned char)end[-1])) {
        end--;
    }
    while (line < end) {
        n = utf8_len((const unsigned char *)line);
        if (n == 0 || line + n > end) {
            break;
        }
        if (copied + n >= out_len) {
            break;
        }
        memcpy(out + copied, line, n);
        copied += n;
        line += n;
    }
    out[copied] = '\0';
    if (copied == 0) {
        strncpy(out, NOTES_UNTITLED, out_len - 1);
        out[out_len - 1] = '\0';
    }
}
