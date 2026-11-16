/*
 * pocketpcap dissector: one stored packet to the inspector's layers, fields
 * and hex text (docs/apps/PACKETSHARK.md §7, INSPECT; milestone M6).
 *
 * Runs in the capture helper only, on demand, for the one packet the
 * inspector shows: never in the capture path and never in the shell, which
 * receives the text these items carry (ADR-016 rule 18). The bytes are the
 * stored bytes of a frame and are hostile:
 *
 *   - every read is bounds-checked against the stored length n; a field
 *     whose bytes were not stored is not emitted, and what the HEADERS rule
 *     left out is said ("1448 B · not stored"), never guessed;
 *   - no allocation, no recursion, every loop has a fixed bound (two VLAN
 *     tags, eight IPv6 extension headers, forty TCP option bytes, DHCP
 *     options inside the bytes, one DNS question and one answer);
 *   - every text is printable ASCII with no tab or newline (ppc_sanitize),
 *     so it can go into the line protocol as it is.
 *
 * Each field carries the byte range it was read from (for the hex view's
 * highlight) and, where one exists, the display filter it stands for
 * ("host:192.0.2.10", "port:443", "class:0"), which the shell sends back
 * unchanged when FILTER BY is pressed.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_DISSECT_H
#define PPC_DISSECT_H

#include "ppc_proto.h"
#include "ppc_record.h"

#include <stddef.h>
#include <stdint.h>

#define PPC_DS_NAME 24
#define PPC_DS_TEXT 112
#define PPC_DS_FILTER 56
#define PPC_DS_LAYERS 8
#define PPC_DS_ITEMS 64
/* The hex view shows this many stored bytes at most, eight to a line. */
#define PPC_HEX_MAX 512u

enum ppc_ds_kind { PPC_DS_LAYER = 0, PPC_DS_FIELD };

struct ppc_ds_item {
    enum ppc_ds_kind kind;
    int layer;                   /* the layer's index, 0-based, for both kinds */
    int cls;                     /* a layer's class colour (enum ppc_class), -1 for none */
    char name[PPC_DS_NAME];      /* "TCP", "SOURCE PORT" */
    char text[PPC_DS_TEXT];      /* a layer's summary, a field's value */
    int32_t off;                 /* first byte in the frame, -1 when none */
    uint32_t len;
    char filter[PPC_DS_FILTER];  /* "" when the field filters nothing */
};

typedef void (*ppc_ds_cb)(void *ctx, const struct ppc_ds_item *it);

/* Walk the stored bytes b[0..n) of the packet r describes and hand every
 * layer and field to cb, outer layers first, each layer before its fields.
 * n may be 0 (the bytes left the buffer): the frame layer is then built from
 * the record alone. At most PPC_DS_LAYERS layers and PPC_DS_ITEMS items. */
void ppc_dissect(const struct ppc_record *r, const uint8_t *b, uint32_t n, ppc_ds_cb cb, void *ctx);

/* The two ends for the inspector's slabs: the address, a line under it
 * ("PORT 443 · HTTPS", "MAC 02:00:5e:10:00:01"), and the filter on its host. */
struct ppc_endpoint {
    char addr[48];
    char sub[48];
    char filter[PPC_DS_FILTER];
};

void ppc_endpoints(const struct ppc_record *r, struct ppc_endpoint *src, struct ppc_endpoint *dst);

/* A well-known service name for a port ("https"), or NULL. */
const char *ppc_port_name(unsigned port);

/* One line of the hex view: the bytes at off (at most PPC_HEX_PER_LINE of
 * them, fewer at the end) as "02 00 5e 10  20 14 02 00" in hex and their
 * printable ASCII ('.' for the rest) in ascii. */
void ppc_hex_line(const uint8_t *b, uint32_t n, uint32_t off, char *hex, size_t hexlen, char *ascii,
                  size_t asciilen);

#endif
