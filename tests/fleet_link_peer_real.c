/*
 * PocketFleet's LAN link against a real peerd: one end of
 * tests/fleet_link_peer_real_test.sh, which runs two of these, each beside
 * its own peerd.
 *
 * It does what a session does with a link and nothing more: comes up, finds
 * the other device (by asking an address when given one, otherwise by being
 * asked), names it the opponent so that peerd keeps it, and exchanges Fleet-
 * sized datagrams with it until one has come back.
 *
 *   fleet_link_peer_real <tag byte, hex> [address to ask]
 *
 * Exit 0 when all of that happened inside the budget.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_link_peer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BUDGET_MS 25000

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    struct fleet_link *l = fleet_link_peer_open(NULL);
    struct fleet_link_peer_info info;
    struct fleet_link_peer peers[2];
    uint8_t other[FLEET_KEY_BYTES];
    uint8_t from[FLEET_KEY_BYTES];
    uint8_t buf[FLEET_PROTO_MAX];
    uint8_t out[FLEET_PROTO_MAX];
    const char *ask = argc > 2 ? argv[2] : NULL;
    int64_t start = now_ms();
    int64_t last_send = 0;
    int64_t last_ask = 0;
    int64_t heard_at = 0;
    int have_other = 0;
    int kept = 0;
    unsigned sent = 0;
    unsigned heard = 0;
    size_t n;

    if (argc < 2 || !l) {
        fprintf(stderr, "usage: fleet_link_peer_real <tag> [address]\n");
        return 2;
    }
    memset(out, (int)strtol(argv[1], NULL, 16), sizeof(out));
    while (now_ms() - start < BUDGET_MS) {
        int64_t now = now_ms();

        l->ops->poll(l->ctx, now);
        if (l->ops->state(l->ctx) != FLEET_LINK_UP) {
            usleep(5000);
            continue;
        }
        if (!have_other) {
            if (ask) {
                enum fleet_link_peer_probe_state ps = fleet_link_peer_probe_state(l);

                if (ps == FLEET_PEER_PROBE_ANSWERED && fleet_link_peer_probe_key(l, other) == 0) {
                    have_other = 1;
                } else if (ps != FLEET_PEER_PROBE_PENDING && now - last_ask >= 1000) {
                    last_ask = now;
                    (void)fleet_link_peer_probe(l, ask);
                }
            } else if (l->ops->peers(l->ctx, peers, 2) == 1) {
                memcpy(other, peers[0].key, FLEET_KEY_BYTES);
                have_other = 1;
            }
            if (have_other) {
                l->ops->match_peer(l->ctx, other);
            }
        }
        if (have_other && !kept && fleet_link_peer_info(l, &info) == 0 && info.kept == 1) {
            kept = 1;
        }
        if (have_other && now - last_send >= 300) {
            last_send = now;
            if (l->ops->send(l->ctx, other, out, sizeof(out)) == FLEET_LINK_SENT) {
                sent++;
            }
        }
        while (l->ops->recv(l->ctx, from, buf, &n)) {
            if (have_other && n == sizeof(buf) && memcmp(from, other, FLEET_KEY_BYTES) == 0 &&
                buf[0] != out[0] && buf[n - 1] == buf[0]) {
                heard++;
                if (!heard_at) {
                    heard_at = now;
                }
            }
        }
        /* A second more after the first one heard, so the other end hears
         * ours as well. */
        if (heard && kept && now - heard_at >= 1000) {
            break;
        }
        usleep(5000);
    }
    (void)fleet_link_peer_info(l, &info);
    printf("tag=%s up=%d trouble=%d other=%d kept=%d sent=%u heard=%u base=%u profile=%s\n",
           argv[1], l->ops->state(l->ctx) == FLEET_LINK_UP, (int)info.trouble, have_other, kept,
           sent, heard, (unsigned)l->ops->retry_base(l->ctx), l->ops->profile(l->ctx)->name);
    l->ops->close(l->ctx);
    return have_other && kept && heard && sent ? 0 : 1;
}
