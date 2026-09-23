/*
 * DOORS runtime art file rules. See art_format.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "art_format.h"

#include <stdio.h>
#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

int art_header_parse(const uint8_t *buf, size_t file_size, struct art_header *out, const char **why)
{
    const char *dummy;
    size_t plane;

    if (!why) {
        why = &dummy;
    }
    if (!buf || !out || file_size < ART_HEADER_SIZE) {
        *why = "shorter than an image header";
        return -1;
    }
    if (buf[0] != ART_MAGIC) {
        *why = "not an LVGL 9 image (magic)";
        return -1;
    }
    out->cf = buf[1];
    out->w = le16(buf + 4);
    out->h = le16(buf + 6);
    out->stride = le16(buf + 8);
    if (le16(buf + 2) != 0 || le16(buf + 10) != 0) {
        *why = "flags or reserved bits set";
        return -1;
    }
    if (out->w == 0 || out->h == 0 || out->w > ART_MAX_SIDE || out->h > ART_MAX_SIDE) {
        *why = "size out of range";
        return -1;
    }
    switch (out->cf) {
    case ART_CF_A8:
        if (out->stride != out->w) {
            *why = "A8 stride is not the width";
            return -1;
        }
        out->data_size = (size_t)out->w * out->h;
        break;
    case ART_CF_RGB565:
        if (out->stride != out->w * 2u) {
            *why = "RGB565 stride is not twice the width";
            return -1;
        }
        out->data_size = (size_t)out->stride * out->h;
        break;
    case ART_CF_RGB565A8:
        /* The colour plane's stride; the alpha plane follows it, one byte a pixel. */
        if (out->stride != out->w * 2u) {
            *why = "RGB565A8 stride is not twice the width";
            return -1;
        }
        plane = (size_t)out->w * out->h;
        out->data_size = plane * 3;
        break;
    default:
        *why = "colour format not used for DOORS art";
        return -1;
    }
    if (file_size != ART_HEADER_SIZE + out->data_size) {
        *why = file_size < ART_HEADER_SIZE + out->data_size ? "truncated" : "longer than its header says";
        return -1;
    }
    return 0;
}

int art_path(const char *dir, const char *name, char *out, size_t out_len)
{
    size_t n;
    int len;

    if (!dir || !name || !out || out_len == 0) {
        return -1;
    }
    n = strlen(name);
    if (n == 0 || n > 48 || strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789-") != n) {
        return -1;
    }
    len = snprintf(out, out_len, "%s/%s.bin", dir, name);
    return (len < 0 || (size_t)len >= out_len) ? -1 : 0;
}
