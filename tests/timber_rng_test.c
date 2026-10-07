/*
 * PocketTimber RNG test: reproducibility, the zero-seed guard, the recorded
 * regression vectors, unbiased bounded draws and the inclusive range helper.
 *
 * The vectors are a regression guard produced by this implementation, not an
 * external reference: they exist so a future change to timber_rng.c that
 * would silently invalidate every recorded run fails here first. Both
 * vectors are identical to PocketRadar's and PocketFleet's by construction,
 * since all three files implement the same xorshift32; that agreement is
 * itself the check that the copy is faithful.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timber_rng.h"

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
    struct timber_rng rng;
    int i;
    int ok = 1;

    timber_rng_seed(&rng, seed);
    for (i = 0; i < n; i++) {
        uint32_t got = timber_rng_next(&rng);

        if (got != want[i]) {
            printf("     seed %u draw %d: want 0x%08X got 0x%08X\n", seed, i, want[i], got);
            ok = 0;
        }
    }
    check(name, ok);
}

static void test_vectors(void)
{
    static const uint32_t vec_1[8] = {
        0x00042021u, 0x04080601u, 0x9DCCA8C5u, 0x1255994Fu,
        0x8EF917D1u, 0x2C6F5BD0u, 0x25B2331Au, 0x19F91CB2u
    };
    static const uint32_t vec_4242[8] = {
        0x405862FBu, 0xCCE3BBF8u, 0xC80020F6u, 0x4FCE59D9u,
        0x1A524FC3u, 0x26EAC4D6u, 0xB077462Eu, 0x4EE7E497u
    };

    check_vector("regression vector for seed 1", 1u, vec_1, 8);
    check_vector("regression vector for seed 4242", 4242u, vec_4242, 8);
}

static void test_reproducibility(void)
{
    struct timber_rng a;
    struct timber_rng b;
    int same = 1;
    int different = 0;
    int i;

    timber_rng_seed(&a, 2026u);
    timber_rng_seed(&b, 2026u);
    for (i = 0; i < 1000; i++) {
        same &= timber_rng_next(&a) == timber_rng_next(&b);
    }
    check("the same seed reproduces the stream", same);

    timber_rng_seed(&a, 2026u);
    timber_rng_seed(&b, 2027u);
    for (i = 0; i < 16; i++) {
        different |= timber_rng_next(&a) != timber_rng_next(&b);
    }
    check("a different seed diverges", different);
}

static void test_zero_seed(void)
{
    struct timber_rng a;
    int nonzero = 1;
    int i;

    timber_rng_seed(&a, 0u);
    check("seed 0 becomes the substitute", a.state == TIMBER_RNG_ZERO_SEED);
    for (i = 0; i < 100; i++) {
        nonzero &= timber_rng_next(&a) != 0u;
    }
    check("a zero-seeded stream never yields 0", nonzero);

    a.state = 0u;
    check("a zeroed state heals itself", timber_rng_next(&a) != 0u);
}

static void test_below(void)
{
    struct timber_rng a;
    static const uint32_t limits[3] = { 10u, 2u, 200u };
    int in_range = 1;
    int even = 1;
    int bucket[10];
    int i;
    int l;

    timber_rng_seed(&a, 5150u);
    for (l = 0; l < 3; l++) {
        for (i = 0; i < 20000; i++) {
            in_range &= timber_rng_below(&a, limits[l]) < limits[l];
        }
    }
    check("bounded draws stay below the limit", in_range);

    check("limit 1 is always 0", timber_rng_below(&a, 1u) == 0u);
    check("limit 0 is 0", timber_rng_below(&a, 0u) == 0u);

    /* A degenerate choice must not move the stream, or adding one to the
     * engine would renumber every later draw. */
    {
        struct timber_rng before;

        timber_rng_seed(&a, 8080u);
        before = a;
        timber_rng_below(&a, 1u);
        timber_rng_below(&a, 0u);
        check("degenerate limits consume no draw", a.state == before.state);
    }

    /* Rough uniformity: 100000 draws over 10 buckets, each within 10 %. */
    memset(bucket, 0, sizeof(bucket));
    timber_rng_seed(&a, 99u);
    for (i = 0; i < 100000; i++) {
        bucket[timber_rng_below(&a, 10u)]++;
    }
    for (i = 0; i < 10; i++) {
        if (bucket[i] < 9000 || bucket[i] > 11000) {
            printf("     bucket %d = %d\n", i, bucket[i]);
            even = 0;
        }
    }
    check("bounded draws are roughly uniform", even);
}

static void test_range(void)
{
    struct timber_rng a;
    int inside = 1;
    int saw_lo = 0;
    int saw_hi = 0;
    int i;

    timber_rng_seed(&a, 4321u);
    for (i = 0; i < 20000; i++) {
        int32_t v = timber_rng_range(&a, -6, 6);

        inside &= v >= -6 && v <= 6;
        saw_lo |= v == -6;
        saw_hi |= v == 6;
    }
    check("a signed range stays inside its bounds", inside);
    check("a signed range reaches both ends", saw_lo && saw_hi);

    check("an empty range returns its low end", timber_rng_range(&a, 7, 3) == 7);
    check("a single-value range returns it", timber_rng_range(&a, 5, 5) == 5);

    {
        int32_t v = timber_rng_range(&a, -2147483647 - 1, 2147483647);

        check("the widest span stays representable", v >= -2147483647 - 1);
    }
}

static void test_null_is_safe(void)
{
    check("NULL rng is safe",
          timber_rng_next(NULL) == 0u && timber_rng_below(NULL, 5u) == 0u &&
          timber_rng_range(NULL, 3, 9) == 3);
    timber_rng_seed(NULL, 1u);
}

int main(void)
{
    test_vectors();
    test_reproducibility();
    test_zero_seed();
    test_below();
    test_range();
    test_null_is_safe();

    printf("timber_rng_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
