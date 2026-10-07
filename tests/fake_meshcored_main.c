/*
 * The scripted meshcored as a program, so a shell test can start one beside
 * the real shell. The script comes from the environment, because the caller
 * is a shell script:
 *
 *   FAKE_MESHCORED_STATE    the service state mesh.status reports
 *   FAKE_MESHCORED_REASON   its reason, in plain words
 *   FAKE_MESHCORED_NODES    the mesh.nodes array, as JSON
 *   FAKE_MESHCORED_EVENTS   a file of "<event>|<json>" lines, one per event
 *   FAKE_MESHCORED_METHODS  where to record every method it was asked for
 *   FAKE_MESHCORED_LIFE_MS  how long to run (default 10000)
 *   FAKE_MESHCORED_REFUSE   non-empty: answer mesh.nodes with an error
 *   FAKE_MESHCORED_EVENTS_AFTER_SNAPSHOT  non-empty: hold the events until
 *                           the snapshots are answered (events_after_snapshot)
 *
 * A negative last_heard_mono_ms or mono_ms means "this long ago", so a
 * fixture's ages do not depend on how long the host has been up.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake_meshcored.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 64

int main(void)
{
    struct fake_meshcored_script script;
    static char *events[MAX_EVENTS + 1];
    const char *path = getenv("FAKE_MESHCORED_EVENTS");
    const char *life = getenv("FAKE_MESHCORED_LIFE_MS");
    int n = 0;

    memset(&script, 0, sizeof(script));
    script.state = getenv("FAKE_MESHCORED_STATE");
    script.reason = getenv("FAKE_MESHCORED_REASON");
    script.nodes_json = getenv("FAKE_MESHCORED_NODES");
    script.method_log = getenv("FAKE_MESHCORED_METHODS");
    script.refuse_nodes = getenv("FAKE_MESHCORED_REFUSE") != NULL;
    /* The channels it starts with, where it logs a request that manages the
     * node, and whether its name is pinned (tests/rift_app_test.c drives
     * RIFT's management panels against this). */
    script.channels_json = getenv("FAKE_MESHCORED_CHANNELS");
    script.manage_log = getenv("FAKE_MESHCORED_MANAGE");
    script.name_pinned = getenv("FAKE_MESHCORED_NAME_PINNED") != NULL;
    script.events_after_snapshot = getenv("FAKE_MESHCORED_EVENTS_AFTER_SNAPSHOT") != NULL;
    script.repeater_json = getenv("FAKE_MESHCORED_REPEATER");
    script.remote_log = getenv("FAKE_MESHCORED_REMOTE_LOG");
    script.cli_timeout_first = getenv("FAKE_MESHCORED_CLI_TIMEOUT_FIRST") != NULL;
    script.life_ms = life ? atoi(life) : 10000;
    if (path) {
        FILE *f = fopen(path, "r");
        char line[1024];

        while (f && n < MAX_EVENTS && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0' || line[0] == '#') {
                continue;
            }
            events[n] = strdup(line);
            if (!events[n]) {
                break;
            }
            n++;
        }
        if (f) {
            fclose(f);
        }
    }
    events[n] = NULL;
    script.events = (const char *const *)events;
    return fake_meshcored_run(&script);
}
