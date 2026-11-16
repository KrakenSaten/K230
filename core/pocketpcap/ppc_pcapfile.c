/*
 * pocketpcap capture files. See ppc_pcapfile.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_pcapfile.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;

    while (n > 0) {
        ssize_t w = write(fd, p, n);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            if (w == 0) {
                errno = EIO;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int ppc_pcap_write_header(int fd, uint32_t snaplen)
{
    struct {
        uint32_t magic;
        uint16_t major;
        uint16_t minor;
        int32_t thiszone;
        uint32_t sigfigs;
        uint32_t snaplen;
        uint32_t network;
    } h = { PPC_PCAP_MAGIC_NS, 2, 4, 0, 0, snaplen, PPC_PCAP_LINKTYPE_ETHERNET };

    _Static_assert(sizeof(h) == PPC_PCAP_HEADER_LEN, "pcap header is 24 bytes");
    return write_all(fd, &h, sizeof(h));
}

int ppc_pcap_write_record(int fd, uint64_t ts_ns, const uint8_t *data, uint32_t cap, uint32_t wire)
{
    struct {
        uint32_t sec;
        uint32_t nsec;
        uint32_t incl;
        uint32_t orig;
    } h = { (uint32_t)(ts_ns / 1000000000u), (uint32_t)(ts_ns % 1000000000u), cap, wire };

    if (cap > PPC_PCAP_RECORD_MAX || (cap && !data)) {
        errno = EINVAL;
        return -1;
    }
    if (write_all(fd, &h, sizeof(h)) != 0) {
        return -1;
    }
    return cap ? write_all(fd, data, cap) : 0;
}

/* ---- reading ------------------------------------------------------------------------ */

static uint32_t sw32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
}

static uint32_t host32(const struct ppc_pcap_reader *r, uint32_t v)
{
    return r->swapped ? sw32(v) : v;
}

int ppc_pcap_open(struct ppc_pcap_reader *r, const char *path)
{
    uint32_t h[6];

    memset(r, 0, sizeof(*r));
    r->f = fopen(path, "rb");
    if (!r->f) {
        snprintf(r->err, sizeof(r->err), "cannot open: %s", strerror(errno));
        return -1;
    }
    if (fread(h, 4, 6, r->f) != 6) {
        snprintf(r->err, sizeof(r->err), "not a capture file: shorter than its header");
        goto fail;
    }
    if (h[0] == PPC_PCAP_MAGIC_NS || h[0] == PPC_PCAP_MAGIC_US) {
        r->swapped = 0;
    } else if (sw32(h[0]) == PPC_PCAP_MAGIC_NS || sw32(h[0]) == PPC_PCAP_MAGIC_US) {
        r->swapped = 1;
    } else {
        snprintf(r->err, sizeof(r->err), "not a classic pcap file (magic 0x%08x)", (unsigned)h[0]);
        goto fail;
    }
    r->nano = host32(r, h[0]) == PPC_PCAP_MAGIC_NS;
    r->snaplen = host32(r, h[4]);
    r->linktype = host32(r, h[5]);
    if (r->linktype != PPC_PCAP_LINKTYPE_ETHERNET) {
        snprintf(r->err, sizeof(r->err), "link type %u is not Ethernet", (unsigned)r->linktype);
        goto fail;
    }
    r->buf = malloc(PPC_PCAP_RECORD_MAX);
    if (!r->buf) {
        snprintf(r->err, sizeof(r->err), "out of memory");
        goto fail;
    }
    return 0;
fail:
    fclose(r->f);
    r->f = NULL;
    return -1;
}

int ppc_pcap_next(struct ppc_pcap_reader *r, struct ppc_frame *out)
{
    uint32_t h[4];
    size_t got;
    uint32_t sec;
    uint32_t frac;
    uint32_t incl;
    uint32_t orig;

    if (!r->f) {
        return -1;
    }
    got = fread(h, 1, sizeof(h), r->f);
    if (got == 0 && feof(r->f)) {
        return 0;
    }
    if (got != sizeof(h)) {
        snprintf(r->err, sizeof(r->err), "record %llu: header cut short",
                 (unsigned long long)r->records + 1);
        return -1;
    }
    sec = host32(r, h[0]);
    frac = host32(r, h[1]);
    incl = host32(r, h[2]);
    orig = host32(r, h[3]);
    if (incl > PPC_PCAP_RECORD_MAX) {
        snprintf(r->err, sizeof(r->err), "record %llu: %u bytes is more than a frame can be",
                 (unsigned long long)r->records + 1, (unsigned)incl);
        return -1;
    }
    if (frac >= (r->nano ? 1000000000u : 1000000u)) {
        snprintf(r->err, sizeof(r->err), "record %llu: fraction of a second out of range",
                 (unsigned long long)r->records + 1);
        return -1;
    }
    if (incl && fread(r->buf, 1, incl, r->f) != incl) {
        snprintf(r->err, sizeof(r->err), "record %llu: data cut short",
                 (unsigned long long)r->records + 1);
        return -1;
    }
    r->records++;
    out->data = r->buf;
    out->cap_len = incl;
    out->wire_len = orig < incl ? incl : orig;
    out->ts_ns = (uint64_t)sec * 1000000000u + (r->nano ? frac : (uint64_t)frac * 1000u);
    out->pkttype = PPC_PKT_UNKNOWN;
    return 1;
}

void ppc_pcap_close(struct ppc_pcap_reader *r)
{
    if (r->f) {
        fclose(r->f);
    }
    free(r->buf);
    r->f = NULL;
    r->buf = NULL;
}
