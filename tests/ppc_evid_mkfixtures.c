/*
 * Writes the evidence history's fixtures (tests/ppc_evid_fixtures.h) as
 * pcap files, for a replay through the real helper
 * (tests/pos_capture_evid_test.sh):
 *
 *   ppc_evid_mkfixtures <dir>     <dir>/evid-<name>.pcap, one per fixture
 *
 * A pcap file carries frames and their times, nothing else: the marks, the
 * HOLD and the frames reported lost of a fixture are not in it, and neither
 * is a frame's direction. The replay learns direction from the MAC of this
 * device (POCKETOS_CAPTURE_SELF_MAC, which the test sets to FX_SELF's), and
 * the test sets its mark when the replay has ended.
 *
 * Stamps start at 2026-01-01 00:00:00 UTC, the fixture's START, so every
 * run writes the same bytes.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketpcap/ppc_pcapfile.h"
#include "ppc_evid_fixtures.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define EPOCH_NS 1767225600000000000ull

int main(int argc, char **argv)
{
    static struct fx f;
    unsigned i;

    if (argc != 2) {
        fprintf(stderr, "usage: ppc_evid_mkfixtures <dir>\n");
        return 2;
    }
    for (i = 0; i < FX_COUNT; i++) {
        char path[512];
        uint32_t k;
        int fd;

        fx_build(i, &f);
        snprintf(path, sizeof(path), "%s/evid-%s.pcap", argv[1], f.name);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0 || ppc_pcap_write_header(fd, 65535) != 0) {
            fprintf(stderr, "cannot write %s\n", path);
            return 1;
        }
        for (k = 0; k < f.nsteps; k++) {
            const struct fx_step *s = &f.step[k];

            if (s->op == FX_FRAME &&
                ppc_pcap_write_record(fd, EPOCH_NS + (uint64_t)s->t_ms * 1000000u, s->frame, s->len, s->len) != 0) {
                fprintf(stderr, "cannot write %s\n", path);
                close(fd);
                return 1;
            }
        }
        close(fd);
        printf("%s\n", path);
    }
    return 0;
}
