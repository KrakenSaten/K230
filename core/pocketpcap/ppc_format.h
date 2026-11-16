/*
 * pocketpcap row text: a record and its stored bytes to the words a packet
 * row shows. Built on demand, only for the rows a window asks for, never in
 * the capture path (DESIGN_DECISIONS D6).
 *
 * Every string is plain printable ASCII: bytes taken from a packet (a DNS
 * name, an SSDP line) have everything below 0x20, 0x7f and everything above
 * replaced with '.', so no text derived from a packet can carry a tab, a
 * newline or broken UTF-8 into the line protocol or onto the panel.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_FORMAT_H
#define PPC_FORMAT_H

#include "ppc_record.h"

#include <stddef.h>
#include <stdint.h>

#define PPC_ADDR_TEXT 48
#define PPC_INFO_TEXT 160
#define PPC_NAME_TEXT 256

struct ppc_row_text {
    char dir[6];                 /* "in", "out", "bcast", "mcast" */
    char word[8];                /* ppc_proto_word() */
    char src[PPC_ADDR_TEXT];
    char dst[PPC_ADDR_TEXT];
    char info[PPC_INFO_TEXT];
};

/* bytes/n are the frame's stored bytes (n may be 0 when they left the
 * buffer: the row is then built from the record alone, without names). */
void ppc_format_row(const struct ppc_record *r, const uint8_t *bytes, uint32_t n,
                    struct ppc_row_text *out);

/* An IPv4 (len 4), IPv6 (16) or MAC (6) address. */
void ppc_format_addr(char *out, size_t outlen, const uint8_t *addr, unsigned len);

/* A DNS name at off in msg[0..len): labels joined by dots, compression
 * pointers followed (at most 16 hops, never forward of a loop), at most 127
 * labels, sanitized. *next, when not NULL, is the offset just past the name
 * where it began (not where a pointer led). Returns 0, or -1 for a name that
 * runs out of the bytes, loops or is too long (out then holds what was read). */
int ppc_dns_name(const uint8_t *msg, uint32_t len, uint32_t off, char *out, size_t outlen,
                 uint32_t *next);

/* "A", "AAAA", "PTR", ... or "TYPE<n>". */
const char *ppc_dns_type_name(unsigned type, char *buf, size_t buflen);

/* ppc_sanitize() is in ppc_proto.h: the shell needs it without the decoder. */

#endif
