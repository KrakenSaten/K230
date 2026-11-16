/*
 * PacketShark's .pcap writer and reader (core/pocketpcap/ppc_pcapfile.h).
 *
 * What is checked: a file written is read back byte for byte with its
 * stamps and lengths (cap < wire included); the header is the nanosecond
 * magic, version 2.4, Ethernet; a microsecond file in the other byte order
 * reads with the right stamps; a damaged file (short header, a record cut
 * short, a record longer than any frame, a fraction of a second out of
 * range, a link type that is not Ethernet, not a pcap at all) is refused
 * with a reason and never read past. tests/pos_capture_test.sh also has the
 * host's tcpdump read a written file when it is installed.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_pcapfile.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failed;
static int checks;
static char path[128];

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static int fresh(void)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
}

static void raw(const void *p, size_t n)
{
    int fd = fresh();

    if (fd >= 0) {
        if (write(fd, p, n) != (ssize_t)n) {
            printf("note short write\n");
        }
        close(fd);
    }
}

static uint32_t sw(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
}

static void test_round_trip(void)
{
    struct ppc_pcap_reader rd;
    struct ppc_frame f;
    uint8_t a[60];
    uint8_t b[1514];
    int fd = fresh();
    int ok;
    uint32_t hdr[6];
    FILE *fp;

    memset(a, 0xa5, sizeof(a));
    memset(b, 0x5a, sizeof(b));
    ok = fd >= 0 && ppc_pcap_write_header(fd, 65535) == 0 &&
         ppc_pcap_write_record(fd, 1735776000123456789ull, a, sizeof(a), sizeof(a)) == 0 &&
         ppc_pcap_write_record(fd, 1735776001000000001ull, b, 54, sizeof(b)) == 0 &&
         ppc_pcap_write_record(fd, 5ull, NULL, 0, 0) == 0;
    if (fd >= 0) {
        close(fd);
    }
    check("three records written", ok);
    fp = fopen(path, "rb");
    check("the header: ns magic, 2.4, snaplen, Ethernet",
          fp && fread(hdr, 4, 6, fp) == 6 && hdr[0] == PPC_PCAP_MAGIC_NS && (hdr[1] & 0xffffu) == 2 &&
              (hdr[1] >> 16) == 4 && hdr[4] == 65535 && hdr[5] == 1);
    if (fp) {
        fclose(fp);
    }
    check("it opens", ppc_pcap_open(&rd, path) == 0 && rd.nano && !rd.swapped);
    check("record 1: bytes, lengths, stamp", ppc_pcap_next(&rd, &f) == 1 && f.cap_len == 60 && f.wire_len == 60 &&
                                                 f.ts_ns == 1735776000123456789ull && memcmp(f.data, a, 60) == 0);
    check("record 2: cap below wire", ppc_pcap_next(&rd, &f) == 1 && f.cap_len == 54 && f.wire_len == 1514 &&
                                          f.ts_ns == 1735776001000000001ull && memcmp(f.data, b, 54) == 0);
    check("record 3: empty", ppc_pcap_next(&rd, &f) == 1 && f.cap_len == 0 && f.ts_ns == 5);
    check("then a clean end", ppc_pcap_next(&rd, &f) == 0 && rd.records == 3);
    ppc_pcap_close(&rd);
    check("a record longer than a frame is not written",
          (fd = fresh()) >= 0 && ppc_pcap_write_record(fd, 0, b, PPC_PCAP_RECORD_MAX + 1, 1) != 0);
    if (fd >= 0) {
        close(fd);
    }
}

static void test_swapped_us(void)
{
    struct ppc_pcap_reader rd;
    struct ppc_frame f;
    uint32_t file[6 + 4 + 2];

    file[0] = sw(PPC_PCAP_MAGIC_US);
    file[1] = sw(0x00040002u);
    file[2] = 0;
    file[3] = 0;
    file[4] = sw(65535);
    file[5] = sw(1);
    file[6] = sw(100);
    file[7] = sw(250000); /* 0.25 s */
    file[8] = sw(8);
    file[9] = sw(60);
    file[10] = 0x11223344u;
    file[11] = 0x55667788u;
    raw(file, sizeof(file));
    check("a byte-swapped microsecond file opens", ppc_pcap_open(&rd, path) == 0 && rd.swapped && !rd.nano);
    check("its stamp is scaled to nanoseconds", ppc_pcap_next(&rd, &f) == 1 && f.ts_ns == 100250000000ull &&
                                                    f.cap_len == 8 && f.wire_len == 60);
    ppc_pcap_close(&rd);
}

static void refused(const char *what, const void *p, size_t n, int at_open)
{
    struct ppc_pcap_reader rd;
    struct ppc_frame f;
    char name[160];
    int ok;

    raw(p, n);
    if (at_open) {
        ok = ppc_pcap_open(&rd, path) != 0 && rd.err[0];
    } else {
        ok = ppc_pcap_open(&rd, path) == 0 && ppc_pcap_next(&rd, &f) == -1 && rd.err[0];
        ppc_pcap_close(&rd);
    }
    snprintf(name, sizeof(name), "refused: %s (%s)", what, rd.err);
    check(name, ok);
}

static void test_damaged(void)
{
    uint32_t h[6] = { PPC_PCAP_MAGIC_NS, 0x00040002u, 0, 0, 65535, 1 };
    uint32_t big[10];
    uint32_t cut[10 + 2];
    uint32_t frac[10];

    refused("a header cut short", h, 10, 1);
    h[0] = 0x0a0d0d0au; /* pcapng */
    refused("not classic pcap", h, sizeof(h), 1);
    h[0] = PPC_PCAP_MAGIC_NS;
    h[5] = 105; /* 802.11 */
    refused("not Ethernet", h, sizeof(h), 1);
    h[5] = 1;
    memcpy(big, h, sizeof(h));
    big[6] = 1;
    big[7] = 0;
    big[8] = PPC_PCAP_RECORD_MAX + 1;
    big[9] = 60;
    refused("a record longer than any frame", big, sizeof(big), 0);
    memcpy(cut, h, sizeof(h));
    cut[6] = 1;
    cut[7] = 0;
    cut[8] = 100;
    cut[9] = 100;
    cut[10] = 0;
    cut[11] = 0;
    refused("a record cut short", cut, sizeof(cut), 0);
    refused("a record header cut short", cut, sizeof(h) + 6, 0);
    memcpy(frac, h, sizeof(h));
    frac[6] = 1;
    frac[7] = 1000000000u;
    frac[8] = 0;
    frac[9] = 0;
    refused("nanoseconds out of range", frac, sizeof(frac), 0);
    {
        struct ppc_pcap_reader rd;

        check("a missing file is refused", ppc_pcap_open(&rd, "/nonexistent/x.pcap") != 0 && rd.err[0]);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(path, sizeof(path), "/tmp/ppc_pcapfile_test-%ld.pcap", (long)getpid());
    test_round_trip();
    test_swapped_us();
    test_damaged();
    unlink(path);
    printf("ppc_pcapfile_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
