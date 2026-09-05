/*
 * PocketFleet RNG test: reproducibility, the zero-seed guard, the recorded
 * regression vector, and unbiased bounded draws.
 *
 * The vector is a regression guard produced by this implementation, not an
 * external reference: it exists so a future change to fleet_rng.c that would
 * silently invalidate every saved game fails here first.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_rng.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void check_vector(const char *name, uint32_t seed, const uint32_t *want, int n)
{
    struct fleet_rng rng;
    int i;
    int ok = 1;

    fleet_rng_seed(&rng, seed);
    for (i = 0; i < n; i++) {
        uint32_t got = fleet_rng_next(&rng);

        if (got != want[i]) {
            printf("     seed %u draw %d: want 0x%08X got 0x%08X\n", seed, i, want[i], got);
            ok = 0;
        }
    }
    check(name, ok);
}

int main(void)
{
    static const uint32_t vec_1[8] = {
        0x00042021u, 0x04080601u, 0x9DCCA8C5u, 0x1255994Fu,
        0x8EF917D1u, 0x2C6F5BD0u, 0x25B2331Au, 0x19F91CB2u
    };
    static const uint32_t vec_12345[8] = {
        0xC6E5747Au, 0x652A09AFu, 0xA7E08FA0u, 0x748E41EAu,
        0x2AD8A9D3u, 0xC3B81262u, 0xFF726198u, 0xDB663F38u
    };
    struct fleet_rng a;
    struct fleet_rng b;
    int i;

    check_vector("regression vector for seed 1", 1u, vec_1, 8);
    check_vector("regression vector for seed 12345", 12345u, vec_12345, 8);

    /* Same seed, same stream. */
    fleet_rng_seed(&a, 777u);
    fleet_rng_seed(&b, 777u);
    {
        int same = 1;

        for (i = 0; i < 1000; i++) {
            same &= fleet_rng_next(&a) == fleet_rng_next(&b);
        }
        check("same seed reproduces the stream", same);
    }

    /* Different seeds diverge. */
    fleet_rng_seed(&a, 1u);
    fleet_rng_seed(&b, 2u);
    {
        int different = 0;

        for (i = 0; i < 16; i++) {
            different |= fleet_rng_next(&a) != fleet_rng_next(&b);
        }
        check("different seeds diverge", different);
    }

    /* Seed 0 is the xorshift fixed point and must be substituted. */
    fleet_rng_seed(&a, 0u);
    check("zero seed substituted", a.state == FLEET_RNG_ZERO_SEED);
    {
        int nonzero = 1;

        for (i = 0; i < 100; i++) {
            nonzero &= fleet_rng_next(&a) != 0;
        }
        check("zero seed still produces values", nonzero);
    }
    /* A restored save carrying state 0 must not lock the generator. */
    a.state = 0;
    check("zero state recovers", fleet_rng_next(&a) != 0);

    /* Bounded draws stay in range. */
    fleet_rng_seed(&a, 4242u);
    {
        int in_range = 1;

        for (i = 0; i < 20000; i++) {
            in_range &= fleet_rng_below(&a, 10u) < 10u;
            in_range &= fleet_rng_below(&a, 2u) < 2u;
            in_range &= fleet_rng_below(&a, 200u) < 200u;
        }
        check("bounded draws stay below the limit", in_range);
    }
    check("limit 1 is always 0", fleet_rng_below(&a, 1u) == 0u);
    check("limit 0 is 0", fleet_rng_below(&a, 0u) == 0u);

    /* Rough uniformity: 100000 draws over 10 buckets, each within 10 %. */
    {
        int bucket[10];
        int even = 1;

        memset(bucket, 0, sizeof(bucket));
        fleet_rng_seed(&a, 99u);
        for (i = 0; i < 100000; i++) {
            bucket[fleet_rng_below(&a, 10u)]++;
        }
        for (i = 0; i < 10; i++) {
            if (bucket[i] < 9000 || bucket[i] > 11000) {
                printf("     bucket %d = %d\n", i, bucket[i]);
                even = 0;
            }
        }
        check("bounded draws are roughly uniform", even);
    }

    /* NULL is tolerated. */
    check("NULL rng is safe", fleet_rng_next(NULL) == 0u && fleet_rng_below(NULL, 5u) == 0u);
    fleet_rng_seed(NULL, 1u);

    printf("fleet_rng_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
