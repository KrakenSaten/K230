/*
 * SHA-256 against the FIPS 180-4 example vectors (NIST CSRC "SHA256.pdf"
 * and the long-message vector from FIPS 180-2 Appendix B.3), and the same
 * data streamed in every split, so a block-boundary slip cannot hide.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_sha256.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void hex(const uint8_t *d, char *out)
{
    int i;

    for (i = 0; i < FLEET_SHA256_BYTES; i++) {
        sprintf(out + 2 * i, "%02x", d[i]);
    }
}

static int matches(const void *data, size_t n, const char *want)
{
    uint8_t d[FLEET_SHA256_BYTES];
    char h[2 * FLEET_SHA256_BYTES + 1];

    fleet_sha256(data, n, d);
    hex(d, h);
    return strcmp(h, want) == 0;
}

int main(void)
{
    static const char abc_want[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    static const char two_block[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const char two_want[] =
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
    static const char empty_want[] =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    static const char million_a_want[] =
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
    static uint8_t million[1000000];
    size_t len = strlen(two_block);
    size_t split;
    int all_splits = 1;

    check("\"abc\"", matches("abc", 3, abc_want));
    check("the two-block message", matches(two_block, len, two_want));
    check("the empty message", matches("", 0, empty_want));
    memset(million, 'a', sizeof(million));
    check("one million 'a'", matches(million, sizeof(million), million_a_want));

    for (split = 0; split <= len; split++) {
        struct fleet_sha256 s;
        uint8_t d[FLEET_SHA256_BYTES];
        char h[2 * FLEET_SHA256_BYTES + 1];

        fleet_sha256_init(&s);
        fleet_sha256_update(&s, two_block, split);
        fleet_sha256_update(&s, two_block + split, len - split);
        fleet_sha256_final(&s, d);
        hex(d, h);
        all_splits &= strcmp(h, two_want) == 0;
    }
    check("streamed in every two-part split", all_splits);

    /* 55, 56 and 64 bytes are where the padding changes shape. */
    {
        static const char want55[] =
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318";
        static const char want56[] =
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a";
        static const char want64[] =
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb";

        check("55 bytes (padding fits one block)", matches(million, 55, want55));
        check("56 bytes (padding spills a block)", matches(million, 56, want56));
        check("64 bytes (exactly one block)", matches(million, 64, want64));
    }

    printf("fleet_sha256_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
