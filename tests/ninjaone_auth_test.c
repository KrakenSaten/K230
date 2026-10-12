/* Synthetic-only OAuth injection and focused store checks.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define NINJA_CURL
#define main ninja_helper_main
#include "../tools/ninjaone/pos_ninjaone.c"
#undef main
#include <assert.h>
#include <sys/wait.h>
static void sample(struct ninja_credentials *c)
{
    memset(c, 0, sizeof(*c)); strcpy(c->base, "https://eu.ninjarmm.com");
    strcpy(c->client, "native-client"); strcpy(c->profile, "000000000000000000000001");
    strcpy(c->access, "synthetic-access-A"); strcpy(c->refresh, "synthetic-refresh-A");
    c->expires = time(NULL) - 1;
}
int main(int argc, char **argv)
{
    int dir = ninja_credentials_lock();
    assert(dir >= 0);
    struct ninja_credentials c = {0}, readback = {0};
    struct transport t = {0};
    struct ninja_snapshot s = {.magic = NINJA_MAGIC};
    if (argc == 2 && !strcmp(argv[1], "store")) {
        sample(&c); assert(ninja_credentials_valid(&c));
        assert(!ninja_credentials_write(dir, &c));
        assert(!ninja_credentials_read(dir, &readback)); assert(!memcmp(&c, &readback, sizeof(c)));
        int fd = openat(dir, "credentials.new", O_CREAT | O_EXCL | O_WRONLY, 0600);
        assert(fd >= 0); assert(write(fd, "interrupted", 11) == 11); close(fd);
        assert(ninja_credentials_pending(dir)); assert(ninja_credentials_write(dir, &c));
        assert(!ninja_credentials_read(dir, &readback)); assert(!memcmp(&c, &readback, sizeof(c)));
        assert(!ninja_credentials_forget(dir)); assert(ninja_credentials_read(dir, &readback));
        assert(!ninja_credentials_write(dir, &c)); assert(!ninja_credentials_begin(dir));
        assert(ninja_credentials_pending(dir)); assert(!ninja_credentials_forget(dir));
        /* A second process cannot overlap refresh/import/forget. */
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) { close(dir); int other = ninja_credentials_lock(); _exit(other >= 0 ? 1 : 0); }
        int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
        close(dir); explicit_bzero(&c, sizeof(c)); puts("store: atomic writes, interruption, forgetting and exclusive lock passed"); return 0;
    }
    assert(ninja_base_valid("https://jp.ninjarmm.com") && ninja_base_valid("https://fed.ninjarmm.com"));
    assert(!ninja_base_valid("https://app.ninjaone.com"));
    if (argc != 3 && argc != 4) return 2;
    assert(!ninja_credentials_read(dir, &c));
    snprintf(t.base, sizeof(t.base), "%s", argv[1]); t.ca_file = argv[2];
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (argc == 4) authorized_snapshot(&t, &c, dir, &s);
    else s.error = authorize(&t, &c, dir, &s.retry_s);
    s.expires = c.expires;
    strcpy(s.profile, c.profile);
    /* Only test outcome flags go through nonsecret model counters. */
    s.norgs = !strcmp(c.access, "synthetic-access-B");
    s.ndevices = !strcmp(c.refresh, "synthetic-refresh-B");
    s.nalerts = c.attempts;
    assert(fwrite(&s, sizeof(s), 1, stdout) == 1);
    close(dir); explicit_bzero(&c, sizeof(c)); explicit_bzero(t.token, sizeof(t.token));
    curl_global_cleanup(); return 0;
}
