/*
 * The scripted peerd as a program, so a shell test can start one beside the
 * real shell (tests/fleet_shell_test.sh). The script comes from the
 * environment, because the caller is a shell script:
 *
 *   FAKE_PEERD_STATE        the state peer.status reports (default "ready")
 *   FAKE_PEERD_NODES        the peer.nodes array, as JSON
 *   FAKE_PEERD_NODES_AFTER  the same once a probe has been answered
 *   FAKE_PEERD_NO_NETWORK   non-empty: bound, with no interface to announce on
 *   FAKE_PEERD_PROBE        "<address>=<key>": a probe of that address is
 *                           answered by that key; any other gets no answer
 *   FAKE_PEERD_LOG          where to record every request
 *   FAKE_PEERD_LIFE_MS      how long to run (default 10000)
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake_peerd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    struct fake_peerd_script script;
    const char *probe = getenv("FAKE_PEERD_PROBE");
    const char *life = getenv("FAKE_PEERD_LIFE_MS");
    static char address[32];

    memset(&script, 0, sizeof(script));
    script.state = getenv("FAKE_PEERD_STATE");
    script.nodes_json = getenv("FAKE_PEERD_NODES");
    script.nodes_after_probe_json = getenv("FAKE_PEERD_NODES_AFTER");
    script.no_interfaces = getenv("FAKE_PEERD_NO_NETWORK") != NULL;
    script.log = getenv("FAKE_PEERD_LOG");
    script.life_ms = life ? atoi(life) : 10000;
    if (probe && strchr(probe, '=')) {
        size_t n = (size_t)(strchr(probe, '=') - probe);

        if (n < sizeof(address)) {
            memcpy(address, probe, n);
            address[n] = '\0';
            script.probe_address = address;
            script.probe_key = probe + n + 1;
        }
    }
    return fake_peerd_run(&script);
}
