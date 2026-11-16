/*
 * pocketpcap capture files: classic PCAP 2.4 (DESIGN_DECISIONS D3).
 *
 * The writer always writes the nanosecond variant (magic 0xa1b23c4d,
 * linktype 1, host byte order), the format the feasibility proof of concept
 * wrote on Unit A and both tcpdump 4.99.5 (unit) and 4.99.1 (host) read back.
 * It writes to a descriptor the caller opened, so mode, O_EXCL and the
 * `.part` rename stay the caller's (the SAVE path, a later milestone). No
 * libpcap: this module is host-testable on its own (tests/ppc_pcapfile_test.c).
 *
 * The reader takes microsecond and nanosecond files in either byte order,
 * Ethernet only, and refuses a record longer than PPC_PCAP_RECORD_MAX: it is
 * what `pos-capture decode` and the replay source read, and its input is a
 * file someone else may have made.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_PCAPFILE_H
#define PPC_PCAPFILE_H

#include "ppc_record.h"

#include <stdint.h>
#include <stdio.h>

#define PPC_PCAP_MAGIC_NS 0xa1b23c4du
#define PPC_PCAP_MAGIC_US 0xa1b2c3d4u
#define PPC_PCAP_LINKTYPE_ETHERNET 1u
#define PPC_PCAP_HEADER_LEN 24u
#define PPC_PCAP_RECORD_HEADER_LEN 16u
/* Longest record the reader accepts (libpcap's own limit for Ethernet). */
#define PPC_PCAP_RECORD_MAX 262144u

/* 0, or -1 with errno. Writes the whole header or nothing useful. */
int ppc_pcap_write_header(int fd, uint32_t snaplen);
/* One record: ts_ns is wall time in nanoseconds since 1970. 0 or -1. */
int ppc_pcap_write_record(int fd, uint64_t ts_ns, const uint8_t *data, uint32_t cap, uint32_t wire);

struct ppc_pcap_reader {
    FILE *f;
    int swapped;    /* the file's byte order is not ours */
    int nano;       /* nanosecond stamps */
    uint32_t snaplen;
    uint32_t linktype;
    uint64_t records;
    uint8_t *buf;   /* PPC_PCAP_RECORD_MAX bytes, allocated at open */
    char err[96];
};

/* 0, or -1 with r->err set (nothing to close). */
int ppc_pcap_open(struct ppc_pcap_reader *r, const char *path);
/* 1 with *out filled (data valid until the next call), 0 at a clean end of
 * the file, -1 for a damaged file (r->err says why). pkttype is UNKNOWN. */
int ppc_pcap_next(struct ppc_pcap_reader *r, struct ppc_frame *out);
void ppc_pcap_close(struct ppc_pcap_reader *r);

#endif
