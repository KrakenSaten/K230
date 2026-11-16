/*
 * A fuzz target for PacketShark's packet path: decode, the stored-bytes
 * rule, the row formatter and the DNS name reader, on arbitrary bytes.
 *
 * Built two ways:
 *   - with clang and -fsanitize=fuzzer (PPC_LIBFUZZER defined): the
 *     libFuzzer entry point below, nothing else;
 *   - with any compiler (the build host has no clang): a small mutation
 *     driver of its own, seeded from every frame of the .pcap files in the
 *     directories given, run for -seconds S. Under packetshark-san-test it
 *     runs with ASan and UBSan.
 *
 *   ppc_fuzz [-seconds S] [-seed N] DIR...
 *
 * Each input is checked, not only survived: the payload offset and the
 * stored length never exceed the bytes, and every text is terminated,
 * printable and free of tabs and newlines.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketpcap/ppc_decode.h"
#include "pocketpcap/ppc_format.h"
#include "pocketpcap/ppc_pcapfile.h"
#include "pocketpcap/ppc_source.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int text_ok(const char *s, size_t cap)
{
    size_t i;

    for (i = 0; i < cap; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == 0) {
            return 1;
        }
        if (c < 0x20 || c >= 0x7f) {
            return 0;
        }
    }
    return 0;
}

static int one(const uint8_t *data, size_t size)
{
    struct ppc_frame f;
    struct ppc_record r;
    struct ppc_row_text t;
    char name[PPC_NAME_TEXT];
    uint32_t pay;
    uint32_t keep;
    uint32_t next = 0;

    f.data = data;
    f.cap_len = (uint32_t)size;
    f.wire_len = (uint32_t)size + (size & 1u ? 100u : 0u);
    f.ts_ns = 0;
    f.pkttype = size ? (uint8_t)(data[0] % 5u) : PPC_PKT_UNKNOWN;
    pay = ppc_decode(&f, &r);
    keep = ppc_store_len(&r, pay, f.cap_len, (size & 2u) ? PPC_KEEP_FULL : PPC_KEEP_HEADERS);
    if (pay > f.cap_len || keep > f.cap_len) {
        fprintf(stderr, "ppc_fuzz: out of bounds: payload %u keep %u of %u\n", pay, keep, f.cap_len);
        abort();
    }
    ppc_format_row(&r, data, keep, &t);
    if (!text_ok(t.info, sizeof(t.info)) || !text_ok(t.src, sizeof(t.src)) || !text_ok(t.dst, sizeof(t.dst)) ||
        !text_ok(t.word, sizeof(t.word)) || !text_ok(t.dir, sizeof(t.dir))) {
        fprintf(stderr, "ppc_fuzz: unprintable row text\n");
        abort();
    }
    /* The name reader on its own, from every offset a name could start at. */
    if (size > 12) {
        ppc_dns_name(data, (uint32_t)size, 12, name, sizeof(name), &next);
        if (!text_ok(name, sizeof(name)) || (next && next > size)) {
            fprintf(stderr, "ppc_fuzz: bad name\n");
            abort();
        }
    }
    return 0;
}

#ifdef PPC_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return one(data, size);
}
#else

#define CORPUS_MAX 4096
#define INPUT_MAX 2048

static uint8_t *corpus[CORPUS_MAX];
static uint32_t corpus_len[CORPUS_MAX];
static unsigned corpus_n;
static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

static void add_seed(const uint8_t *b, uint32_t n)
{
    if (corpus_n >= CORPUS_MAX) {
        return;
    }
    if (n > INPUT_MAX) {
        n = INPUT_MAX;
    }
    corpus[corpus_n] = malloc(n ? n : 1);
    if (!corpus[corpus_n]) {
        return;
    }
    memcpy(corpus[corpus_n], b, n);
    corpus_len[corpus_n++] = n;
}

static void load_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;

    while (d && (e = readdir(d)) != NULL) {
        char path[512];
        struct ppc_pcap_reader rd;
        struct ppc_frame f;
        size_t l = strlen(e->d_name);

        if (l < 6 || strcmp(e->d_name + l - 5, ".pcap") != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (ppc_pcap_open(&rd, path) != 0) {
            continue;
        }
        while (ppc_pcap_next(&rd, &f) == 1) {
            add_seed(f.data, f.cap_len);
        }
        ppc_pcap_close(&rd);
    }
    if (d) {
        closedir(d);
    }
}

static uint32_t mutate(uint8_t *b, uint32_t n)
{
    unsigned k;
    unsigned edits = 1 + rnd() % 8u;

    for (k = 0; k < edits; k++) {
        switch (rnd() % 7u) {
        case 0: /* flip a bit */
            if (n) {
                b[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8u));
            }
            break;
        case 1: /* a random byte */
            if (n) {
                b[rnd() % n] = (uint8_t)rnd();
            }
            break;
        case 2: /* an interesting byte: lengths, pointers, ends */
            if (n) {
                static const uint8_t v[] = { 0, 1, 0x3f, 0x40, 0x7f, 0x80, 0xc0, 0xff, 5, 15, 53 };

                b[rnd() % n] = v[rnd() % sizeof(v)];
            }
            break;
        case 3: /* cut */
            if (n) {
                n = rnd() % (n + 1u);
            }
            break;
        case 4: /* grow with random bytes */
            while (n < INPUT_MAX && (rnd() & 3u)) {
                b[n++] = (uint8_t)rnd();
            }
            break;
        case 5: /* splice another seed's tail */
            if (corpus_n) {
                unsigned o = rnd() % corpus_n;
                uint32_t at = n ? rnd() % n : 0;
                uint32_t from = corpus_len[o] ? rnd() % corpus_len[o] : 0;
                uint32_t len = corpus_len[o] - from;

                if (at + len > INPUT_MAX) {
                    len = INPUT_MAX - at;
                }
                memcpy(b + at, corpus[o] + from, len);
                n = at + len;
            }
            break;
        default: /* a 16-bit field set to a boundary */
            if (n > 1) {
                static const uint16_t v[] = { 0, 1, 20, 40, 0x0800, 0x86dd, 0x8100, 0xffff, 0xc00c };
                uint32_t at = rnd() % (n - 1u);
                uint16_t x = v[rnd() % (sizeof(v) / sizeof(v[0]))];

                b[at] = (uint8_t)(x >> 8);
                b[at + 1] = (uint8_t)x;
            }
            break;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    double seconds = 10.0;
    uint64_t runs = 0;
    struct timespec t0;
    struct timespec t;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-seconds") == 0 && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (strcmp(argv[i], "-seed") == 0 && i + 1 < argc) {
            rng = strtoull(argv[++i], NULL, 10) | 1u;
        } else {
            load_dir(argv[i]);
        }
    }
    /* The synthetic generator's frames too, and the empty input. */
    for (i = 0; i < 64; i++) {
        uint8_t b[1600];

        add_seed(b, ppc_synth_frame((uint64_t)i, 11, b));
    }
    add_seed((const uint8_t *)"", 0);
    for (i = 0; i < (int)corpus_n; i++) {
        one(corpus[i], corpus_len[i]);
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        static uint8_t buf[INPUT_MAX];
        unsigned s = rnd() % corpus_n;
        uint32_t n = corpus_len[s];
        double el;

        memcpy(buf, corpus[s], n);
        n = mutate(buf, n);
        /* Exactly n bytes on the heap: a read past the input is a heap
         * overflow ASan reports, not a read of the static buffer's tail. */
        {
            uint8_t *h = malloc(n ? n : 1);

            memcpy(h, buf, n);
            one(h, n);
            free(h);
        }
        runs++;
        if ((runs & 1023u) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t);
            el = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9;
            if (el >= seconds) {
                break;
            }
        }
    }
    printf("ppc_fuzz: %llu inputs in %.0f s from %u seeds, no finding\n", (unsigned long long)runs, seconds, corpus_n);
    for (i = 0; i < (int)corpus_n; i++) {
        free(corpus[i]);
    }
    return 0;
}
#endif
