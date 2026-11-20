/*
 * peerd wire format (peer_wire.h). Pure; bounds checked against n only.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "peer_wire.h"

#include <string.h>

uint64_t pw_get64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;

    for (i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

void pw_put64(uint8_t *p, uint64_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

static int type_signed(uint8_t type)
{
    return type == PW_BEACON || type == PW_PROBE || type == PW_PROBE_REPLY ||
           type == PW_LOOKUP || type == PW_LOOKUP_REPLY;
}

int pw_name_ok(const uint8_t *s, size_t len)
{
    size_t i = 0;

    if (len > PW_NAME_MAX) {
        return 0;
    }
    while (i < len) {
        uint8_t c = s[i];
        uint32_t cp;
        size_t need;
        size_t k;

        if (c < 0x80) {
            if (c < 0x20 || c == 0x7F) {
                return 0;
            }
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            cp = c & 0x07;
        } else {
            return 0;       /* a continuation byte, an overlong lead, or beyond U+10FFFF */
        }
        if (len - i <= need) {
            return 0;
        }
        for (k = 1; k <= need; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0x80 && cp <= 0x9F)) {
            return 0;       /* overlong, out of range, a surrogate, or a C1 control */
        }
        i += need + 1;
    }
    return 1;
}

int pw_parse_header(const uint8_t *buf, size_t n, struct pw_header *h)
{
    if (!buf || !h || n < PW_HEADER) {
        return PW_E_SHORT;
    }
    if (buf[0] != PW_VERSION) {
        return PW_E_VERSION;
    }
    if (buf[1] == PW_DATA) {
        if (n < PW_DATA_MIN || n > PW_DATA_MAX) {
            return PW_E_LENGTH;
        }
    } else if (type_signed(buf[1])) {
        if (n != PW_SIGNED_SIZE) {
            return PW_E_LENGTH;
        }
    } else {
        return PW_E_TYPE;
    }
    h->type = buf[1];
    memcpy(h->sender, buf + 2, PW_KEY);
    memcpy(h->recipient, buf + 34, PW_PREFIX);
    h->epoch = pw_get64(buf + 42);
    h->seq = pw_get64(buf + 50);
    if (h->epoch == 0 || h->seq == 0) {
        return PW_E_FIELD;
    }
    return PW_OK;
}

void pw_build_header(const struct pw_header *h, uint8_t *buf)
{
    buf[0] = PW_VERSION;
    buf[1] = h->type;
    memcpy(buf + 2, h->sender, PW_KEY);
    memcpy(buf + 34, h->recipient, PW_PREFIX);
    pw_put64(buf + 42, h->epoch);
    pw_put64(buf + 50, h->seq);
}

static int all_zero(const uint8_t *p, size_t n)
{
    uint8_t acc = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        acc |= p[i];
    }
    return acc == 0;
}

int pw_parse_signed(const uint8_t *buf, size_t n, struct pw_signed *out)
{
    int rc;

    if (!out) {
        return PW_E_SHORT;
    }
    memset(out, 0, sizeof(*out));       /* nothing of the stack rides out in a field */
    rc = pw_parse_header(buf, n, &out->h);
    if (rc != PW_OK) {
        return rc;
    }
    if (!type_signed(out->h.type)) {
        return PW_E_TYPE;
    }
    out->addr = ((uint32_t)buf[58] << 24) | ((uint32_t)buf[59] << 16) | ((uint32_t)buf[60] << 8) | buf[61];
    out->port = (uint16_t)((buf[62] << 8) | buf[63]);
    out->flags = buf[64];
    out->name_len = buf[65];
    if (out->port == 0 || (out->flags & ~PW_FLAG_TABLE) != 0 || out->name_len > PW_NAME_MAX ||
        !pw_name_ok(buf + 66, out->name_len) ||
        !all_zero(buf + 66 + out->name_len, PW_NAME_MAX - out->name_len)) {
        return PW_E_FIELD;
    }
    memcpy(out->name, buf + 66, out->name_len);
    out->name[out->name_len] = '\0';
    out->table.port = buf[90];
    out->table.proto_min = buf[91];
    out->table.proto_max = buf[92];
    out->table.seats = buf[93];
    out->table.tag = ((uint32_t)buf[94] << 24) | ((uint32_t)buf[95] << 16) | ((uint32_t)buf[96] << 8) | buf[97];
    if (out->flags & PW_FLAG_TABLE) {
        if (out->table.port < PW_PORT_MIN || out->table.port > PW_PORT_MAX ||
            out->table.proto_min > out->table.proto_max || out->table.seats == 0) {
            return PW_E_FIELD;
        }
    } else if (!all_zero(buf + 90, 8)) {
        return PW_E_FIELD;
    }
    memcpy(out->challenge, buf + 98, PW_CHALLENGE);
    memcpy(out->target, buf + 114, PW_KEY);
    /* What a type does not use must be zero, so there is one encoding of
     * each frame and nothing unread rides along under the signature. */
    switch (out->h.type) {
    case PW_BEACON:
        if (!all_zero(out->h.recipient, PW_PREFIX) || !all_zero(out->challenge, PW_CHALLENGE) ||
            !all_zero(out->target, PW_KEY)) {
            return PW_E_FIELD;
        }
        break;
    case PW_PROBE:
        if (!all_zero(out->h.recipient, PW_PREFIX) || !all_zero(out->target, PW_KEY)) {
            return PW_E_FIELD;
        }
        break;
    case PW_LOOKUP:
        if (!all_zero(out->h.recipient, PW_PREFIX)) {
            return PW_E_FIELD;
        }
        break;
    default:    /* the two replies: addressed, and about nobody else */
        if (!all_zero(out->target, PW_KEY)) {
            return PW_E_FIELD;
        }
        break;
    }
    return PW_OK;
}

int pw_build_signed(const struct pw_signed *f, uint8_t *buf)
{
    struct pw_signed back;
    size_t len = f->name_len;

    if (len > PW_NAME_MAX) {
        return PW_E_FIELD;
    }
    memset(buf, 0, PW_SIGNED_SIZE);
    pw_build_header(&f->h, buf);
    buf[58] = (uint8_t)(f->addr >> 24);
    buf[59] = (uint8_t)(f->addr >> 16);
    buf[60] = (uint8_t)(f->addr >> 8);
    buf[61] = (uint8_t)f->addr;
    buf[62] = (uint8_t)(f->port >> 8);
    buf[63] = (uint8_t)f->port;
    buf[64] = f->flags;
    buf[65] = f->name_len;
    memcpy(buf + 66, f->name, len);
    if (f->flags & PW_FLAG_TABLE) {
        buf[90] = f->table.port;
        buf[91] = f->table.proto_min;
        buf[92] = f->table.proto_max;
        buf[93] = f->table.seats;
        buf[94] = (uint8_t)(f->table.tag >> 24);
        buf[95] = (uint8_t)(f->table.tag >> 16);
        buf[96] = (uint8_t)(f->table.tag >> 8);
        buf[97] = (uint8_t)f->table.tag;
    }
    memcpy(buf + 98, f->challenge, PW_CHALLENGE);
    memcpy(buf + 114, f->target, PW_KEY);
    /* Never send what this side would refuse to read. */
    return pw_parse_signed(buf, PW_SIGNED_SIZE, &back) == PW_OK ? PW_OK : PW_E_FIELD;
}
