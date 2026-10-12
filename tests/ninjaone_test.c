/* API contracts, pagination, secure provisioning and helper lifecycle.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "ninjaone/ninja_api.h"
#include "ninja_session.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static int decode(struct ninja_snapshot *s, char kind, const char *json, int after)
{
    int next; unsigned rows;
    return ninja_decode(s, kind, json, strlen(json), after, &next, &rows);
}
static void api_test(void)
{
    struct ninja_snapshot s = {.magic = NINJA_MAGIC};
    assert(!decode(&s, 'o', "[{\"id\":1,\"name\":\"First\"},{\"id\":2}]", 0));
    assert(s.norgs == 2 && !strcmp(s.orgs[1].name, "Unnamed organization"));
    assert(!decode(&s, 'o', "[{\"id\":3}]", 2));
    assert(decode(&s, 'o', "[{\"id\":3}]", 3) == NINJA_MALFORMED);
    assert(decode(&s, 'o', "[{\"id\":1.5}]", 0) == NINJA_MALFORMED);
    assert(decode(&s, 'o', "[{\"id\":2147483648}]", 0) == NINJA_MALFORMED);
    assert(decode(&s, 'o', "[]garbage", 0) == NINJA_MALFORMED);
    assert(decode(&s, 'o', "{}", 0) == NINJA_MALFORMED);
    assert(!decode(&s, 'd', "[{\"id\":11,\"offline\":true,\"lastContact\":1000},{\"id\":12,\"offline\":\"false\",\"lastContact\":null}]", 0));
    assert(s.devices[0].offline == 1 && s.devices[0].last_contact == 1000);
    assert(s.devices[1].offline == -1 && s.devices[1].last_contact == 0);
    assert(!decode(&s, 'a', "[{\"deviceId\":11,\"message\":\"a\\nb\"},{\"deviceId\":11},{\"deviceId\":12}]", 0));
    assert(!strcmp(s.alerts[0].message, "a b"));
    assert(ninja_affected(&s, 0) == 2 && ninja_snapshot_valid(&s));
    s.nalerts = NINJA_ALERTS + 1; assert(!ninja_snapshot_valid(&s)); s.nalerts = NINJA_ALERTS;
    memset(s.alerts, 0, sizeof(s.alerts));
    for (unsigned i = 0; i < NINJA_ALERTS; i++) s.alerts[i].device = 11;
    assert(!decode(&s, 'a', "[{\"deviceId\":12}]", 0) && s.partial && s.nalerts == NINJA_ALERTS);
    assert(!ninja_base_valid("https://eu.ninjarmm.com.evil.example"));
    struct ninja_snapshot text = {.magic = NINJA_MAGIC};
    assert(!decode(&text, 'o', "[{\"id\":1,\"name\":\"bad\xff\"}]", 0));
    assert(!strcmp(text.orgs[0].name, "bad?"));
    assert(!ninja_base_valid("http://eu.ninjarmm.com"));
    assert(!ninja_base_valid("https://user:secret@eu.ninjarmm.com"));
    assert(ninja_base_valid("https://eu.ninjarmm.com"));
}
static int64_t now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void settle(struct ninja_session *session, struct ninja_snapshot *s)
{
    int rc = 0;
    int64_t deadline = now() + 3000;
    while (!rc && now() < deadline) {
        rc = ninja_session_poll(session, now(), s);
        usleep(1000);
    }
    assert(rc == 1);
}
static void session_test(void)
{
    struct ninja_session session;
    struct ninja_snapshot *s = calloc(1, sizeof(*s));
    ninja_session_init(&session);
    const char *cases[] = {"demo", "empty", "missing", "expired", "denied", "rate", "network", "malformed"};
    int errors[] = {0,0,0,NINJA_AUTH,NINJA_DENIED,NINJA_RATE,NINJA_NETWORK,NINJA_MALFORMED};
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        assert(!ninja_session_start(&session, now(), cases[i]));
        settle(&session, s); assert(s->error == errors[i] && s->demo);
        if (!i) assert(s->norgs == 3 && s->ndevices == 3 && s->nalerts == 2 && s->updated);
        if (i == 2) assert(s->devices[0].offline == -1 && s->devices[0].org == 0);
        if (i == 5) assert(s->retry_s == 180);
        if (errors[i]) assert(!s->nalerts && !s->updated);
    }
    assert(!ninja_session_start(&session, now(), "large"));
    settle(&session, s);
    assert(!s->error && s->norgs == 250 && s->ndevices == 500 && s->nalerts == 500 && s->partial);
    assert(ninja_affected(s, 0) == 500);
    for (int i = 0; i < 20; i++) {
        assert(!ninja_session_start(&session, now(), "hang"));
        pid_t cancelled = session.pid;
        assert(!ninja_session_start(&session, now(), "empty"));
        assert(kill(cancelled, 0) == -1);
        settle(&session, s); assert(!s->nalerts); /* No late demo can replace it. */
    }
    assert(!ninja_session_start(&session, now(), "hang"));
    pid_t hung = session.pid;
    assert(ninja_session_poll(&session, now() + 120001, s) == -1 && kill(hung, 0) == -1);
    assert(waitpid(-1, NULL, WNOHANG) == -1); /* No child/zombie from the 20 cycles. */
    free(s);
}
static void token_test(void)
{
    char dir[] = "/tmp/ninja-token-XXXXXX", path[256], buf[4096];
    assert(mkdtemp(dir)); setenv("POCKETOS_RUNTIME_DIR", dir, 1);
    assert(!ninja_token_write("synthetic-token.123"));
    assert(!ninja_token_read(buf, sizeof(buf)) && !strcmp(buf, "synthetic-token.123"));
    snprintf(path, sizeof(path), "%s/ninjaone/access-token", dir);
    assert(!chmod(path, 0644)); assert(ninja_token_read(buf, sizeof(buf)) == -1 && !buf[0]);
    assert(!chmod(path, 0600));
    assert(ninja_token_write("invalid\nheader") == -1);
    assert(!unlink(path)); assert(!symlink("/etc/passwd", path));
    assert(ninja_token_read(buf, sizeof(buf)) == -1);
    unlink(path); snprintf(path, sizeof(path), "%s/ninjaone", dir); rmdir(path); rmdir(dir);
}
int main(void)
{
    api_test(); token_test(); session_test();
    puts("ninjaone_test: decoding, pagination, missing fields, caps, token permissions, errors, cancellation, watchdog and 20 clean cycles passed");
    return 0;
}
