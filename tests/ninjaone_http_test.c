/* The production curl path against a local synthetic TLS server. The CA and
 * base are injected by this test executable only, never a shipped option.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define NINJA_CURL
#define main ninja_helper_main
#include "../tools/ninjaone/pos_ninjaone.c"
#undef main
int main(int argc, char **argv)
{
    struct transport t = {0};
    char *body = NULL;
    size_t len = 0;
    int retry = 0, rc;
    if (argc != 4) return 2;
    snprintf(t.base, sizeof(t.base), "%s", argv[1]);
    snprintf(t.token, sizeof(t.token), "synthetic-secret-token");
    t.ca_file = strcmp(argv[2], "-") ? argv[2] : NULL;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    rc = live_get(&t, argv[3], &body, &len, &retry);
    printf("%d %d %zu\n", rc, retry, len);
    free(body); explicit_bzero(t.token, sizeof(t.token)); curl_global_cleanup();
    return 0;
}
