/*
 * PocketFleet's mesh link (apps/fleet/link/fleet_link_mesh.c) against a real
 * socket and a scripted meshcored (tests/fake_meshcored.c).
 *
 * What it holds the link to: it connects only when polled, so a Fleet that is
 * not in Multiplayer asks the service nothing; it says which of the four link
 * states it is in; it lists the players the service has heard and nobody
 * else; it takes every Fleet datagram exactly once, from the inbox and from
 * events, and starts counting again when the service restarts; it refuses
 * what is not a Fleet packet; and what it transmits is exactly what it was
 * handed - proved by what the service was asked to send, not by its source.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_link_mesh.h"

#include "fake_meshcored.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WAIT_MS 3000

#define SELF "5f000000000000000000000000000000000000000000000000000000000000ff"
#define ANNA "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"
#define BOB "b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2"
#define RELAY "c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3"

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failed += !ok;
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void key(const char *hex, uint8_t out[FLEET_KEY_BYTES])
{
    int i;

    for (i = 0; i < FLEET_KEY_BYTES; i++) {
        unsigned v;

        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* Poll as the app's timer does until `done` or the budget runs out. */
static int spin(struct fleet_link *l, int budget_ms, int (*done)(struct fleet_link *))
{
    int64_t start = now_ms();

    for (;;) {
        l->ops->poll(l->ctx, now_ms());
        if (done && done(l)) {
            return 1;
        }
        if (now_ms() - start >= budget_ms) {
            return 0;
        }
        usleep(2000);
    }
}

static int is_up(struct fleet_link *l)
{
    return l->ops->state(l->ctx) == FLEET_LINK_UP;
}

static int is_radio_off(struct fleet_link *l)
{
    return l->ops->state(l->ctx) == FLEET_LINK_RADIO_OFF;
}

static int has_retry_base(struct fleet_link *l)
{
    return l->ops->retry_base(l->ctx) != 0;
}

static int file_has(const char *path, const char *want)
{
    char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, want) != NULL;
}

static long file_size(const char *path)
{
    struct stat sb;

    return stat(path, &sb) == 0 ? (long)sb.st_size : 0;
}

/* Drain what the link has, into a list of first bytes; returns the count. */
static int drain(struct fleet_link *l, uint8_t *firsts, size_t *lens, uint8_t froms[][32], int max)
{
    uint8_t from[FLEET_KEY_BYTES];
    uint8_t buf[FLEET_PROTO_MAX];
    size_t n;
    int count = 0;

    while (l->ops->recv(l->ctx, from, buf, &n)) {
        if (count < max) {
            firsts[count] = buf[0];
            lens[count] = n;
            memcpy(froms[count], from, FLEET_KEY_BYTES);
        }
        count++;
    }
    return count;
}

static const char NODES[] =
    "[{\"public_key\":\"" BOB "\",\"name\":\"Bob\",\"type\":1,\"path_known\":true,"
    "\"hops\":0,\"last_heard_mono_ms\":-5000},"
    "{\"public_key\":\"" RELAY "\",\"name\":\"Hilltop\",\"type\":2,\"path_known\":false,"
    "\"last_heard_mono_ms\":-1000},"
    "{\"public_key\":\"" ANNA "\",\"name\":\"Anna\",\"type\":1,\"path_known\":true,"
    "\"hops\":2,\"last_heard_mono_ms\":-60000}]";

/* An inbox with: a Fleet packet (id 1); one on another port (2); one that sat
 * there for twenty minutes (3); one too long to be a Fleet packet (4); one
 * that is not hex (6); and a second Fleet packet (7). */
static const char INBOX[] =
    "[{\"id\":1,\"port\":1,\"from\":\"" ANNA "\",\"payload_hex\":\"4100000100\","
    "\"route\":\"flood\",\"mono_ms\":-2000},"
    "{\"id\":2,\"port\":2,\"from\":\"" ANNA "\",\"payload_hex\":\"99\","
    "\"route\":\"flood\",\"mono_ms\":-2000},"
    "{\"id\":3,\"port\":1,\"from\":\"" ANNA "\",\"payload_hex\":\"4200000100\","
    "\"route\":\"flood\",\"mono_ms\":-1200000},"
    /* 43 bytes: one more than the longest Fleet packet, a whole chat line. */
    "{\"id\":4,\"port\":1,\"from\":\"" ANNA "\",\"payload_hex\":"
    "\"43000001000000000000000000000000000000000000000000000000000000000000000000000000000000\","
    "\"route\":\"direct\",\"mono_ms\":-1000},"
    "{\"id\":6,\"port\":1,\"from\":\"" ANNA "\",\"payload_hex\":\"zz\","
    "\"route\":\"direct\",\"mono_ms\":-1000},"
    "{\"id\":7,\"port\":1,\"from\":\"" BOB "\",\"payload_hex\":\"4700000100\","
    "\"route\":\"direct\",\"mono_ms\":-500}]";

/* Events: id 7 again (already taken from the inbox), a new one (8), one on
 * another port, and Anna forgotten. */
static const char *const EVENTS[] = {
    "mesh.app|{\"datagram\":{\"id\":7,\"port\":1,\"from\":\"" BOB "\","
    "\"payload_hex\":\"4700000100\",\"route\":\"direct\",\"mono_ms\":-500}}",
    "mesh.app|{\"datagram\":{\"id\":8,\"port\":1,\"from\":\"" BOB "\","
    "\"payload_hex\":\"4800000100\",\"route\":\"direct\",\"mono_ms\":-10}}",
    "mesh.app|{\"datagram\":{\"id\":9,\"port\":3,\"from\":\"" BOB "\","
    "\"payload_hex\":\"4900000100\",\"route\":\"direct\",\"mono_ms\":-10}}",
    "mesh.node|{\"reason\":\"removed\",\"node\":{\"public_key\":\"" ANNA "\",\"name\":\"Anna\","
    "\"type\":1}}",
    NULL,
};

static int events_done(struct fleet_link *l)
{
    struct fleet_link_peer p[4];

    return l->ops->peers(l->ctx, p, 4) == 1;
}

static int has_peers(struct fleet_link *l)
{
    struct fleet_link_peer p[1];

    return l->ops->peers(l->ctx, p, 1) == 1;
}

/* A mesh.nodes array of nodes first..last (either way round): node i's key is
 * byte i throughout, it was heard i seconds ago, and it is a companion - one
 * that could play - from node `players_from` on; before that, alternately a
 * repeater and a room server. */
static void crowd_json(char *out, size_t size, int first, int last, int players_from)
{
    int step = first <= last ? 1 : -1;
    size_t used = 0;
    int i;

    used += (size_t)snprintf(out + used, size - used, "[");
    for (i = first;; i += step) {
        char hex[2 * FLEET_KEY_BYTES + 1];
        int b;

        for (b = 0; b < FLEET_KEY_BYTES; b++) {
            snprintf(hex + 2 * b, 3, "%02x", i);
        }
        used += (size_t)snprintf(out + used, size - used,
                                 "%s{\"public_key\":\"%s\",\"name\":\"N%d\",\"type\":%d,"
                                 "\"path_known\":true,\"hops\":1,\"last_heard_mono_ms\":-%d}",
                                 i == first ? "" : ",", hex, i,
                                 i >= players_from ? 1 : 2 + (i & 1), i * 1000);
        if (i == last) {
            break;
        }
    }
    snprintf(out + used, size - used, "]");
}

/* Who the link lists from a service holding `nodes`, into peers. */
static int crowd_peers(const char *nodes, struct fleet_link_peer *peers, int max)
{
    struct fake_meshcored_script script;
    struct fleet_link *l;
    pid_t pid;
    int n = -1;

    memset(&script, 0, sizeof(script));
    script.nodes_json = nodes;
    script.life_ms = 20000;
    pid = fake_meshcored_spawn(&script);
    if (pid > 0 && fake_meshcored_wait_ready(WAIT_MS)) {
        l = fleet_link_mesh_open(NULL);
        spin(l, WAIT_MS, is_up);
        spin(l, WAIT_MS, has_peers);
        n = l->ops->peers(l->ctx, peers, max);
        l->ops->close(l->ctx);
    }
    if (pid > 0) {
        fake_meshcored_stop(pid);
    }
    return n;
}

/* Every peer i in order is node first + i, for count of them. */
static int peers_are(const struct fleet_link_peer *peers, int count, int first)
{
    int i;
    int b;

    for (i = 0; i < count; i++) {
        for (b = 0; b < FLEET_KEY_BYTES; b++) {
            if (peers[i].key[b] != (uint8_t)(first + i)) {
                return 0;
            }
        }
    }
    return 1;
}

int main(void)
{
    char runtime[] = "/tmp/fleet_link_test.XXXXXX";
    char methods[600];
    char apps[600];
    char adverts[600];
    uint8_t self[FLEET_KEY_BYTES];
    uint8_t anna[FLEET_KEY_BYTES];
    uint8_t bob[FLEET_KEY_BYTES];
    uint8_t firsts[16];
    size_t lens[16];
    uint8_t froms[16][32];
    struct fleet_link_peer peers[8];
    struct fleet_link *l;
    pid_t pid;
    int n;

    if (!mkdtemp(runtime)) {
        printf("FAIL could not make a runtime directory\n");
        return 1;
    }
    setenv("POCKETOS_RUNTIME_DIR", runtime, 1);
    snprintf(methods, sizeof(methods), "%s/methods.txt", runtime);
    snprintf(apps, sizeof(apps), "%s/apps.txt", runtime);
    snprintf(adverts, sizeof(adverts), "%s/adverts.txt", runtime);
    key(SELF, self);
    key(ANNA, anna);
    key(BOB, bob);

    /* ---- no service ------------------------------------------------------- */
    l = fleet_link_mesh_open(NULL);
    check("a link opens", l != NULL);
    if (!l) {
        return 1;
    }
    check("before its first poll it is looking", l->ops->state(l->ctx) == FLEET_LINK_CONNECTING);
    l->ops->poll(l->ctx, now_ms());
    check("with nothing listening it says there is no service",
          l->ops->state(l->ctx) == FLEET_LINK_NO_SERVICE);
    check("a packet is refused, not queued",
          l->ops->send(l->ctx, anna, (const uint8_t *)"\x41\0\0\1\0", 5) == FLEET_LINK_FAILED);
    check("and there is no key to play under", l->ops->self_key(l->ctx, self) != 0);
    check("nor anyone to invite", l->ops->peers(l->ctx, peers, 8) == 0);
    check("nor an advert", l->ops->advertise(l->ctx) != 0);
    l->ops->close(l->ctx);

    /* ---- a service, the snapshot, the inbox and the events ------------------ */
    {
        struct fake_meshcored_script script;

        memset(&script, 0, sizeof(script));
        script.nodes_json = NODES;
        script.app_inbox_json = INBOX;
        script.events = EVENTS;
        script.events_after_inbox = 1;
        script.method_log = methods;
        script.app_log = apps;
        script.advert_log = adverts;
        script.app_est_timeout_ms = 12345;
        script.run_id = "run-one";
        script.life_ms = 20000;
        unlink(methods);
        pid = fake_meshcored_spawn(&script);
        check("the service came up", pid > 0 && fake_meshcored_wait_ready(WAIT_MS));

        l = fleet_link_mesh_open(NULL);
        usleep(150000);
        check("a link that is never polled asks the service nothing", file_size(methods) == 0);
        check("polled, it comes up", spin(l, WAIT_MS, is_up));
        {
            uint8_t k[FLEET_KEY_BYTES];

            check("it plays under the node's own key",
                  l->ops->self_key(l->ctx, k) == 0 && memcmp(k, self, FLEET_KEY_BYTES) == 0);
        }
        check("the events arrive", spin(l, WAIT_MS, events_done));
        spin(l, 100, NULL);

        n = drain(l, firsts, lens, froms, 16);
        check("exactly the Fleet packets arrive, each once: the inbox's two, and the event's one",
              n == 3);
        check("oldest first", n == 3 && firsts[0] == 0x41 && firsts[1] == 0x47 && firsts[2] == 0x48);
        check("whole", n == 3 && lens[0] == 5 && lens[1] == 5 && lens[2] == 5);
        check("from whom they came",
              n == 3 && memcmp(froms[0], anna, 32) == 0 && memcmp(froms[1], bob, 32) == 0);
        check("nothing is left", drain(l, firsts, lens, froms, 16) == 0);

        n = l->ops->peers(l->ctx, peers, 8);
        check("the players heard: a repeater is not one, a forgotten node is gone",
              n == 1 && memcmp(peers[0].key, bob, 32) == 0);
        check("with name and route", n == 1 && strcmp(peers[0].name, "Bob") == 0 &&
                                         peers[0].hops == 0 && peers[0].heard_ms > 0);
        check("a name for a key", l->ops->peer_name(l->ctx, bob) &&
                                      strcmp(l->ops->peer_name(l->ctx, bob), "Bob") == 0);
        check("and none for a stranger", l->ops->peer_name(l->ctx, self) == NULL);

        /* ---- sending ---- */
        check("a packet is handed to the service",
              l->ops->send(l->ctx, bob, (const uint8_t *)"\x47\0\0\x02\x05\x01", 6) ==
                  FLEET_LINK_SENT);
        check("and the service's estimate becomes the retry base", spin(l, WAIT_MS, has_retry_base) &&
                                                                   l->ops->retry_base(l->ctx) == 12345);
        check("as exactly those bytes, to that key, on Fleet's port",
              file_has(apps, BOB "|1|470000020501"));
        check("nothing longer than a Fleet packet is handed on",
              l->ops->send(l->ctx, bob, (const uint8_t *)"0123456789012345678901234567890123456789abc",
                           FLEET_PROTO_MAX + 1) == FLEET_LINK_FAILED);
        check("but a whole chat line, the longest Fleet packet, is",
              l->ops->send(l->ctx, bob, (const uint8_t *)"0123456789012345678901234567890123456789ab",
                           FLEET_PROTO_MAX) == FLEET_LINK_SENT);
        spin(l, 150, NULL);
        check("an advert is zero-hop", l->ops->advertise(l->ctx) == 0);
        spin(l, 150, NULL);
        check("and the service was asked for exactly that", file_has(adverts, "zero_hop") &&
                                                              !file_has(adverts, "flood"));
        check("nothing else was asked to transmit", !file_has(methods, "mesh.send\n"));
        l->ops->close(l->ctx);
        fake_meshcored_stop(pid);
    }

    /* ---- the service restarts: same run, then a new one ----------------------- */
    {
        struct fake_meshcored_script script;
        static const char ONE[] =
            "[{\"id\":1,\"port\":1,\"from\":\"" ANNA "\",\"payload_hex\":\"5100000100\","
            "\"route\":\"flood\",\"mono_ms\":-100}]";

        memset(&script, 0, sizeof(script));
        script.nodes_json = NODES;
        script.app_inbox_json = ONE;
        script.run_id = "run-a";
        script.life_ms = 20000;
        pid = fake_meshcored_spawn(&script);
        check("a service came up", pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
        l = fleet_link_mesh_open(NULL);
        spin(l, WAIT_MS, is_up);
        spin(l, 200, NULL);
        check("its datagram is taken", drain(l, firsts, lens, froms, 16) == 1 && firsts[0] == 0x51);
        fake_meshcored_stop(pid);
        spin(l, 200, NULL);
        check("the link notices the service went", l->ops->state(l->ctx) != FLEET_LINK_UP);

        pid = fake_meshcored_spawn(&script);
        check("the same run answers again", pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
        check("the link reconnects", spin(l, 8000, is_up));
        spin(l, 300, NULL);
        check("and does not take id 1 of the same run twice", drain(l, firsts, lens, froms, 16) == 0);
        fake_meshcored_stop(pid);

        script.run_id = "run-b";
        pid = fake_meshcored_spawn(&script);
        check("a new run of the service comes up", pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
        check("the link reconnects", spin(l, 8000, is_up));
        spin(l, 300, NULL);
        check("and in a new run id 1 is a new datagram", drain(l, firsts, lens, froms, 16) == 1);
        l->ops->close(l->ctx);
        fake_meshcored_stop(pid);
    }

    /* ---- the service is there, the radio is not -------------------------------- */
    {
        struct fake_meshcored_script script;

        memset(&script, 0, sizeof(script));
        script.nodes_json = NODES;
        script.radio_off = 1;
        script.app_log = apps;
        script.life_ms = 20000;
        unlink(apps);
        pid = fake_meshcored_spawn(&script);
        check("a service without a radio came up", pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
        l = fleet_link_mesh_open(NULL);
        check("the link says the radio is off", spin(l, WAIT_MS, is_radio_off));
        check("a packet is held back, not lost: busy",
              l->ops->send(l->ctx, bob, (const uint8_t *)"\x41\0\0\1\0", 5) == FLEET_LINK_BUSY);
        check("no advert either", l->ops->advertise(l->ctx) != 0);
        spin(l, 100, NULL);
        check("and nothing was asked to go out", file_size(apps) == 0);
        l->ops->close(l->ctx);
        fake_meshcored_stop(pid);
    }

    /* ---- more nodes than the link holds ------------------------------------------ */
    /* The link holds 64 players. It used to hold the first 64 nodes the service
     * listed, repeaters and room servers included, and filter after: a player
     * listed 65th never reached the lobby. */
    {
        static char nodes[16384];
        struct fleet_link_peer many[80];

        crowd_json(nodes, sizeof(nodes), 1, 65, 65);
        n = crowd_peers(nodes, many, 80);
        check("64 repeaters and room servers do not crowd out the player listed 65th",
              n == 1 && peers_are(many, 1, 65) && strcmp(many[0].name, "N65") == 0);

        crowd_json(nodes, sizeof(nodes), 1, 70, 1);
        n = crowd_peers(nodes, many, 80);
        check("of 70 players, the 64 most recently heard are listed, most recent first",
              n == 64 && peers_are(many, 64, 1));
        crowd_json(nodes, sizeof(nodes), 70, 1, 1);
        n = crowd_peers(nodes, many, 80);
        check("the same 64 in the same order, whichever way round the service lists them",
              n == 64 && peers_are(many, 64, 1));
        n = crowd_peers(nodes, many, 5);
        check("and a lobby asking for 5 gets the 5 most recent", n == 5 && peers_are(many, 5, 1));
    }

    {
        char cmd[700];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", runtime);
        if (system(cmd) != 0) {
            fprintf(stderr, "note: could not remove %s\n", runtime);
        }
    }
    printf("fleet_link_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
