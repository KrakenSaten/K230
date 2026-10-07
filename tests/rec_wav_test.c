/*
 * core/pocketwav: the canonical header byte for byte, what a probe reports
 * for complete, cut-off, over-long, placeholder, stereo, extensible and
 * foreign files, the repair of an interrupted file, the 32-bit size limit,
 * and that no header - however broken, including random bytes - makes the
 * reader crash or claim audio that is not there.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketwav/pocketwav.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failed;
static int checks;
static char dir[] = "/tmp/rec_wav_test.XXXXXX";

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

static int file_with(const char *name, const void *bytes, size_t n)
{
    char path[128];
    int fd;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0 && write(fd, bytes, n) != (ssize_t)n) {
        close(fd);
        return -1;
    }
    return fd;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void test_header(void)
{
    static const uint8_t want[44] = {
        'R', 'I', 'F', 'F', 0x24, 0x7d, 0x00, 0x00, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
        16, 0, 0, 0, 1, 0, 1, 0, 0x80, 0x3e, 0, 0, 0x00, 0x7d, 0, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 0x00, 0x7d, 0x00, 0x00,
    };
    uint8_t h[44];

    /* One second of 16 kHz mono: 32000 data bytes. */
    pocketwav_header(h, 16000, 1, 32000);
    check("the header is the canonical 44 bytes, byte for byte", memcmp(h, want, 44) == 0);
    pocketwav_header(h, 48000, 1, 0xFFFFFFFFu);
    check("a data size past the limit is written as the limit, never wrapped",
          h[40] == 0xd8 && h[41] == 0xff && h[42] == 0xff && h[43] == 0x7f &&
              h[4] == 0xfc && h[5] == 0xff && h[6] == 0xff && h[7] == 0x7f);
    check("the limit is whole 4-byte frames and keeps the RIFF size at most 2^31 - 1",
          POCKETWAV_MAX_DATA_BYTES % 4 == 0 && 36ull + POCKETWAV_MAX_DATA_BYTES <= 0x7FFFFFFFull);
    check("the limit is 6 h 12 min at 48 kHz mono and 18 h 38 min at 16 kHz",
          POCKETWAV_MAX_DATA_BYTES / 96000u == 22369u && POCKETWAV_MAX_DATA_BYTES / 32000u == 67108u);
}

static void test_probe(void)
{
    uint8_t buf[44 + 2000];
    struct pocketwav_info i;
    int fd;

    memset(buf, 0x11, sizeof(buf));
    pocketwav_header(buf, 16000, 1, 2000);
    fd = file_with("ok.wav", buf, sizeof(buf));
    check("a complete file probes", pocketwav_probe_fd(fd, &i) == POCKETWAV_OK);
    check("with its format", i.rate == 16000 && i.channels == 1 && i.block == 2 && i.data_offset == 44);
    check("and its length", i.frames == 1000 && pocketwav_frames_ms(i.frames, i.rate) == 62 &&
                                !i.truncated && !i.trailing);
    close(fd);

    fd = file_with("cut.wav", buf, 44 + 999);
    check("a file cut short probes", pocketwav_probe_fd(fd, &i) == POCKETWAV_OK);
    check("as what is present, in whole frames, and says so",
          i.frames == 499 && i.data_present == 998 && i.truncated && !i.trailing);
    close(fd);

    pocketwav_header(buf, 16000, 1, 0);
    fd = file_with("zero.wav", buf, sizeof(buf));
    check("a header that says no data yet, with data behind it, plays nothing it did not promise",
          pocketwav_probe_fd(fd, &i) == POCKETWAV_OK && i.frames == 0 && i.trailing);
    check("and the repair makes it tell the truth",
          pocketwav_fix_fd(fd, &i) == POCKETWAV_OK && i.frames == 1000 && !i.trailing && !i.truncated);
    close(fd);

    pocketwav_header(buf, 48000, 1, 0);
    put32(buf + 40, 0xFFFFFFFFu);
    put32(buf + 4, 0xFFFFFFFFu);
    fd = file_with("placeholder.wav", buf, 44 + 1001);
    check("a streaming placeholder size reads as the whole frames present",
          pocketwav_probe_fd(fd, &i) == POCKETWAV_OK && i.frames == 500 && i.truncated);
    check("the repair writes the real size and cuts the half frame",
          pocketwav_fix_fd(fd, &i) == POCKETWAV_OK && i.frames == 500 && i.file_bytes == 44 + 1000 &&
              i.data_declared == 1000 && !i.truncated);
    close(fd);

    /* Stereo, extensible, a LIST chunk before data. */
    {
        uint8_t x[200];
        size_t n = 0;

        memset(x, 0, sizeof(x));
        memcpy(x, "RIFF\0\0\0\0WAVE", 12);
        n = 12;
        memcpy(x + n, "fmt ", 4);
        put32(x + n + 4, 40);
        x[n + 8] = 0xFE;
        x[n + 9] = 0xFF;          /* WAVE_FORMAT_EXTENSIBLE */
        x[n + 10] = 2;            /* stereo */
        put32(x + n + 12, 48000);
        put32(x + n + 16, 192000);
        x[n + 20] = 4;            /* block */
        x[n + 22] = 16;           /* bits */
        x[n + 32] = 1;            /* sub-format: PCM */
        n += 8 + 40;
        memcpy(x + n, "LIST", 4);
        put32(x + n + 4, 5);      /* odd: a pad byte follows */
        n += 8 + 6;
        memcpy(x + n, "data", 4);
        put32(x + n + 4, 16);
        n += 8 + 16;
        fd = file_with("ext.wav", x, n);
        check("an extensible stereo file with a padded chunk before data probes",
              pocketwav_probe_fd(fd, &i) == POCKETWAV_OK && i.channels == 2 && i.block == 4 &&
                  i.frames == 4 && i.data_offset == n - 16);
        close(fd);
    }

    /* Refusals. */
    pocketwav_header(buf, 16000, 1, 100);
    buf[34] = 24; /* 24-bit */
    fd = file_with("24.wav", buf, 144);
    check("24-bit is unsupported, not invalid", pocketwav_probe_fd(fd, &i) == POCKETWAV_E_UNSUPPORTED);
    close(fd);
    pocketwav_header(buf, 16000, 1, 100);
    buf[20] = 3; /* float */
    fd = file_with("float.wav", buf, 144);
    check("float is unsupported", pocketwav_probe_fd(fd, &i) == POCKETWAV_E_UNSUPPORTED);
    close(fd);
    pocketwav_header(buf, 16000, 3, 100);
    fd = file_with("3ch.wav", buf, 144);
    check("three channels are unsupported", pocketwav_probe_fd(fd, &i) == POCKETWAV_E_UNSUPPORTED);
    close(fd);
    pocketwav_header(buf, 4000, 1, 100);
    fd = file_with("slow.wav", buf, 144);
    check("a rate below 8 kHz is invalid", pocketwav_probe_fd(fd, &i) == POCKETWAV_E_INVALID);
    close(fd);
    fd = file_with("empty.wav", "", 0);
    check("an empty file is invalid", pocketwav_probe_fd(fd, &i) == POCKETWAV_E_INVALID);
    check("and cannot be 'repaired' into a recording", pocketwav_fix_fd(fd, &i) == POCKETWAV_E_INVALID);
    close(fd);
    pocketwav_header(buf, 16000, 1, 100);
    put32(buf + 16, 0x7FFFFFF0u); /* fmt chunk claims to be huge */
    fd = file_with("hugefmt.wav", buf, 144);
    check("a chunk that claims more than the file is invalid", pocketwav_probe_fd(fd, &i) == POCKETWAV_E_INVALID);
    close(fd);
    check("a closed descriptor is an I/O error", pocketwav_probe_fd(-1, &i) == POCKETWAV_E_IO);
}

/* Random headers: whatever the reader says, it never claims more frames than
 * the file can hold. Run under the sanitizers by make recorder-san-test. */
static void test_random(void)
{
    uint8_t buf[300];
    struct pocketwav_info i;
    unsigned seed = 12345;
    int k;
    int bad = 0;

    for (k = 0; k < 2000; k++) {
        size_t n = 12 + (size_t)(seed % 280);
        size_t j;
        int fd;

        for (j = 0; j < n; j++) {
            seed = seed * 1103515245u + 12345u;
            buf[j] = (uint8_t)(seed >> 16);
        }
        if (k % 2 == 0) {
            pocketwav_header(buf, 8000 + (seed % 40000), 1 + (seed % 2), (uint32_t)(seed % 400));
        }
        fd = file_with("random.wav", buf, n);
        if (pocketwav_probe_fd(fd, &i) == POCKETWAV_OK &&
            (i.data_offset + i.data_present > n || i.frames * i.block != i.data_present)) {
            bad++;
        }
        close(fd);
    }
    check("2000 random headers: never more audio claimed than the file holds", bad == 0);
}

int main(void)
{
    char cmd[80];

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    test_header();
    test_probe();
    test_random();
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        printf("note could not remove %s\n", dir);
    }
    printf("rec_wav_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
