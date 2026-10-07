/*
 * radiod's stored radio choice and the backend it belongs to
 * (services/radiod/rf_state.h): what is written, what is read back, and
 * which stored choices apply to which backend - in particular that an "on"
 * made on the mock, or an untagged "on" from before 0.3.5, never switches
 * the SX1262 on without the antenna question.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "../services/radiod/rf_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks;
static int failed;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
    }
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

int main(void)
{
    char dir[] = "/tmp/rf-state-XXXXXX";
    char path[256];
    char be[RF_STATE_BACKEND_MAX];
    char err[128];
    char buf[256];
    FILE *f;
    size_t n;

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    check("the path is <state>/radiod/radio.conf",
          rf_state_path(dir, path, sizeof(path)) == 0 && strstr(path, "/radiod/radio.conf") != NULL);
    check("no file: absent, no backend",
          rf_state_load(path, be, sizeof(be)) == RF_STATE_ABSENT && be[0] == '\0');

    check("storing on, on sx1262", rf_state_store(path, true, "sx1262", err, sizeof(err)) == 0);
    f = fopen(path, "r");
    n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    buf[n] = '\0';
    if (f) {
        fclose(f);
    }
    check("the file says enabled=1 and backend=sx1262",
          strstr(buf, "\nenabled=1\n") != NULL && strstr(buf, "\nbackend=sx1262\n") != NULL);
    check("read back: on, made on sx1262",
          rf_state_load(path, be, sizeof(be)) == RF_STATE_ON && strcmp(be, "sx1262") == 0);
    check("a store without a backend name is refused",
          rf_state_store(path, true, "", err, sizeof(err)) < 0 && rf_state_load(path, be, sizeof(be)) == RF_STATE_ON);

    /* Which choices apply. */
    check("a choice applies on its own backend", rf_state_applies(RF_STATE_ON, "sx1262", "sx1262"));
    check("the mock's on does NOT apply to sx1262", !rf_state_applies(RF_STATE_ON, "mock", "sx1262"));
    check("nor the mock's off (the default, off, is the same)", !rf_state_applies(RF_STATE_OFF, "mock", "sx1262"));
    check("sx1262's on does not apply to the mock", !rf_state_applies(RF_STATE_ON, "sx1262", "mock"));
    check("an untagged on (before 0.3.5) does not switch sx1262 on", !rf_state_applies(RF_STATE_ON, "", "sx1262"));
    check("an untagged off is kept", rf_state_applies(RF_STATE_OFF, "", "sx1262"));
    check("an untagged on still applies to the mock", rf_state_applies(RF_STATE_ON, "", "mock"));
    check("absent and invalid never apply",
          !rf_state_applies(RF_STATE_ABSENT, "", "sx1262") && !rf_state_applies(RF_STATE_INVALID, "sx1262", "sx1262"));

    /* A fresh card: the mock toggled off and on, then the unit put on the
     * real radio. */
    check("mock on stored", rf_state_store(path, true, "mock", err, sizeof(err)) == 0);
    {
        enum rf_state_load s = rf_state_load(path, be, sizeof(be));

        check("after a mock toggle the SX1262 still starts with its default (off)",
              s == RF_STATE_ON && strcmp(be, "mock") == 0 && !rf_state_applies(s, be, "sx1262"));
    }
    put(path, "enabled=1\n");
    {
        enum rf_state_load s = rf_state_load(path, be, sizeof(be));

        check("a file from before the tag reads as untagged on", s == RF_STATE_ON && be[0] == '\0');
    }
    put(path, "backend=sx1262\nenabled=0\nbackend=mock\n");
    check("the last backend line wins, as the last enabled line does",
          rf_state_load(path, be, sizeof(be)) == RF_STATE_OFF && strcmp(be, "mock") == 0);

    unlink(path);
    snprintf(buf, sizeof(buf), "%s/radiod", dir);
    rmdir(buf);
    rmdir(dir);
    printf("rf_state_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
