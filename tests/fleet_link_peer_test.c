/*
 * PocketFleet's LAN link (apps/fleet/link/fleet_link_peer.c) against a real
 * socket and a scripted peerd (tests/fake_peerd.c).
 *
 * What it holds the link to: it asks peerd nothing until it is polled, so a
 * Fleet that is not in Multiplayer is not on the network; it says which link
 * state it is in and why; it plays on the LAN profile; it lists the players
 * peerd holds; it takes every Fleet datagram exactly once, from the inbox and
 * from events, and starts counting again when peerd restarts; what it sends
 * is exactly what it was handed, on Fleet's port; it renews the visibility
 * lease while polled and ends it on close; it keeps the opponent of the match
 * in hand and releases anyone else, only once the session has said who; and
 * it can ask a device at an address who it is.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_link_peer.h"

#include "fake_peerd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WAIT_MS 4000

#define ANNA "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"
#define BOB "b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2"
#define CAROL "c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3"

static int failed;
static int checks;
/* The link's clock is whatever poll is given: a test moves it forward
 * rather than waiting out a ten-second lease. */
static int64_t skew;

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
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + skew;
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

static int count_lines(const char *path, const char *want)
{
    static char buf[65536];
    FILE *f = fopen(path, "r");
    const char *at;
    size_t n;
    int count = 0;

    if (!f) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    for (at = buf; (at = strstr(at, want)) != NULL; at += strlen(want)) {
        count++;
    }
    return count;
}

static long file_size(const char *path)
{
    struct stat sb;

    return stat(path, &sb) == 0 ? (long)sb.st_size : 0;
}

static char LOG[600];
static const char *want_line;
static int want_count;

static int logged(struct fleet_link *l)
{
    (void)l;
    return count_lines(LOG, want_line) >= want_count;
}

/* Poll until the service's log holds `line` at least `count` times. */
static int until_logged(struct fleet_link *l, const char *line, int count)
{
    want_line = line;
    want_count = count;
    return spin(l, WAIT_MS, logged);
}

static int is_up(struct fleet_link *l)
{
    return l->ops->state(l->ctx) == FLEET_LINK_UP;
}

static int is_off(struct fleet_link *l)
{
    return l->ops->state(l->ctx) == FLEET_LINK_RADIO_OFF;
}

static int is_gone(struct fleet_link *l)
{
    return l->ops->state(l->ctx) == FLEET_LINK_NO_SERVICE;
}

static int has_three(struct fleet_link *l)
{
    struct fleet_link_peer p[4];

    return l->ops->peers(l->ctx, p, 4) == 3;
}

static int has_base(struct fleet_link *l)
{
    return l->ops->retry_base(l->ctx) != 0;
}

static int kept_want;

static int kept_is(struct fleet_link *l)
{
    struct fleet_link_peer_info info;

    return fleet_link_peer_info(l, &info) == 0 && info.kept == kept_want;
}

static enum fleet_link_peer_probe_state probe_want;

static int probe_is(struct fleet_link *l)
{
    return fleet_link_peer_probe_state(l) == probe_want;
}

static int got;
static uint8_t firsts[16];
static uint8_t froms[16][FLEET_KEY_BYTES];
static int got_want;

static int received(struct fleet_link *l)
{
    uint8_t from[FLEET_KEY_BYTES];
    uint8_t buf[FLEET_PROTO_MAX];
    size_t n;

    while (l->ops->recv(l->ctx, from, buf, &n)) {
        if (got < 16) {
            firsts[got] = buf[0];
            memcpy(froms[got], from, FLEET_KEY_BYTES);
        }
        got++;
    }
    return got >= got_want;
}

/* Bob, heard five seconds ago; this device itself; Anna, a minute ago; and
 * Carol, a kept peer nobody has heard this run and with no address. */
static const char NODES[] =
    "[{\"key\":\"" ANNA "\",\"fingerprint\":\"a1a1-a1a1\",\"name\":\"Anna\","
    "\"address\":\"192.168.1.40\",\"heard_ms_ago\":60000,\"kept\":false,\"proved\":true,"
    "\"route\":\"lan\"},"
    "{\"key\":\"" FAKE_PEERD_SELF "\",\"name\":\"K230-A\",\"address\":\"192.168.1.23\","
    "\"heard_ms_ago\":10},"
    "{\"key\":\"" CAROL "\",\"fingerprint\":\"c3c3-c3c3\",\"name\":\"Carol\",\"kept\":true,"
    "\"proved\":false,\"route\":\"lan\"},"
    "{\"key\":\"" BOB "\",\"fingerprint\":\"b2b2-b2b2\",\"name\":\"Bob\","
    "\"address\":\"192.168.1.41\",\"heard_ms_ago\":5000,\"kept\":false,\"proved\":true,"
    "\"route\":\"lan\"},"
    "{\"key\":\"nothex\",\"name\":\"Nobody\"}]";

/* A Fleet packet (id 1); one on another port (2); one that sat there for
 * twenty minutes (3); one too long to be a Fleet packet (4); one that is not
 * hex (6); and a second Fleet packet (7). */
static const char INBOX[] =
    "[{\"id\":1,\"port\":1,\"from\":\"" ANNA "\",\"payload_hex\":\"4100000100\","
    "\"route\":\"lan\",\"mono_ms\":-2000},"
    "{\"id\":2,\"port\":2,\"from\":\"" ANNA "\",\"payload_hex\":\"99\","
    "\"route\":\"lan\",\"mono_ms\":-2000},"
    "{\"id\":3,\"port\":1,\"from\":\"" ANNA "\",\"payload_hex\":\"4200000100\","
    "\"route\":\"lan\",\"mono_ms\":-1200000},"
    "{\"id\":4,\"port\":1,\"from\":\"" ANNA "\",\"payload_hex\":"
    "\"43000001000000000000000000000000000000000000000000000000000000000000000000000000000000\","
    "\"route\":\"lan\",\"mono_ms\":-1000},"
    "{\"id\":6,\"port\":1,\"from\":\"" ANNA "\",\"payload_hex\":\"zz\","
    "\"route\":\"lan\",\"mono_ms\":-1000},"
    "{\"id\":7,\"port\":1,\"from\":\"" BOB "\",\"payload_hex\":\"4700000100\","
    "\"route\":\"lan\",\"mono_ms\":-500}]";

/* id 7 again (taken from the inbox already), a new one (8), and one on
 * another port. peer.app's data is the datagram itself. */
static const char *const EVENTS[] = {
    "{\"id\":7,\"port\":1,\"from\":\"" BOB "\",\"payload_hex\":\"4700000100\","
    "\"route\":\"lan\",\"mono_ms\":-500}",
    "{\"id\":8,\"port\":1,\"from\":\"" BOB "\",\"payload_hex\":\"4800000100\","
    "\"route\":\"lan\",\"mono_ms\":-10}",
    "{\"id\":9,\"port\":3,\"from\":\"" BOB "\",\"payload_hex\":\"4900000100\","
    "\"route\":\"lan\",\"mono_ms\":-10}",
    NULL,
};

static const char INBOX_AFTER_RESTART[] =
    "[{\"id\":1,\"port\":1,\"from\":\"" BOB "\",\"payload_hex\":\"5100000100\","
    "\"route\":\"lan\",\"mono_ms\":-100}]";

int main(void)
{
    char runtime[] = "/tmp/fleet_link_peer_test.XXXXXX";
    uint8_t self[FLEET_KEY_BYTES];
    uint8_t want_self[FLEET_KEY_BYTES];
    uint8_t anna[FLEET_KEY_BYTES];
    uint8_t bob[FLEET_KEY_BYTES];
    uint8_t carol[FLEET_KEY_BYTES];
    uint8_t who[FLEET_KEY_BYTES];
    struct fleet_link_peer peers[8];
    struct fleet_link_peer_info info;
    struct fake_peerd_script script;
    struct fleet_link *l;
    pid_t pid;
    int n;

    if (!mkdtemp(runtime)) {
        printf("FAIL could not make a runtime directory\n");
        return 1;
    }
    setenv("POCKETOS_RUNTIME_DIR", runtime, 1);
    snprintf(LOG, sizeof(LOG), "%s/requests.txt", runtime);
    key(FAKE_PEERD_SELF, want_self);
    key(ANNA, anna);
    key(BOB, bob);
    key(CAROL, carol);

    /* ---- no service ------------------------------------------------------- */
    l = fleet_link_peer_open(NULL);
    check("a link opens", l != NULL);
    if (!l) {
        return 1;
    }
    check("before its first poll it is looking", l->ops->state(l->ctx) == FLEET_LINK_CONNECTING);
    check("it plays on the LAN profile",
          l->ops->profile && l->ops->profile(l->ctx) == &fleet_profile_lan);
    l->ops->poll(l->ctx, now_ms());
    check("with nothing listening it says there is no service",
          l->ops->state(l->ctx) == FLEET_LINK_NO_SERVICE);
    check("and why", fleet_link_peer_info(l, &info) == 0 && info.trouble == FLEET_PEER_NO_SERVICE &&
          info.kept == -1);
    check("a packet is refused, not queued",
          l->ops->send(l->ctx, anna, (const uint8_t *)"\x41\0\0\1\0", 5) == FLEET_LINK_FAILED);
    check("there is no key to play under", l->ops->self_key(l->ctx, self) != 0);
    check("nobody to invite", l->ops->peers(l->ctx, peers, 8) == 0);
    check("no address can be asked", fleet_link_peer_probe(l, "192.168.1.50") != 0 &&
          fleet_link_peer_probe_state(l) == FLEET_PEER_PROBE_REFUSED);
    l->ops->close(l->ctx);

    /* ---- nothing asked before the first poll ------------------------------- */
    memset(&script, 0, sizeof(script));
    script.nodes_json = NODES;
    script.inbox_json = INBOX;
    script.events = EVENTS;
    script.kept = CAROL;
    script.probe_address = "192.168.1.41";
    script.probe_key = BOB;
    script.log = LOG;
    script.life_ms = 120000;
    pid = fake_peerd_spawn(&script);
    check("a scripted peerd starts", pid > 0 && fake_peerd_wait_ready(&script, WAIT_MS));
    l = fleet_link_peer_open(NULL);
    usleep(150000);
    check("a link that is open and not polled asks peerd nothing", file_size(LOG) == 0);

    /* ---- up: identity, visibility, players --------------------------------- */
    check("polled, it comes up", spin(l, WAIT_MS, is_up));
    check("under peerd's key", l->ops->self_key(l->ctx, self) == 0 &&
          memcmp(self, want_self, FLEET_KEY_BYTES) == 0);
    check("it asked to be visible", until_logged(l, "peer.set_visible|{\"visible\":true}", 1));
    check("on a subscription", count_lines(LOG, "peer.subscribe|") == 1);
    check("the players peerd holds are listed", spin(l, WAIT_MS, has_three));
    n = l->ops->peers(l->ctx, peers, 8);
    check("most recently heard first, without this device or a broken entry",
          n == 3 && memcmp(peers[0].key, bob, FLEET_KEY_BYTES) == 0 &&
          memcmp(peers[1].key, anna, FLEET_KEY_BYTES) == 0 &&
          memcmp(peers[2].key, carol, FLEET_KEY_BYTES) == 0);
    check("one with an address is zero hops away", n == 3 && peers[0].hops == 0 &&
          peers[0].heard_ms > 0 && peers[0].heard_ms > peers[1].heard_ms);
    check("a kept peer not heard this run has no time and no route",
          n == 3 && peers[2].heard_ms == 0 && peers[2].hops == -1);
    check("which of them peerd has shown to be live is kept, not flattened",
          fleet_link_peer_proved(l, bob) == 1 && fleet_link_peer_proved(l, carol) == 0 &&
          fleet_link_peer_proved(l, want_self) == -1);
    check("names come from peerd", strcmp(l->ops->peer_name(l->ctx, bob), "Bob") == 0 &&
          l->ops->peer_name(l->ctx, want_self) == NULL);
    spin(l, 2500, NULL);
    check("it knows its address and that it is visible",
          fleet_link_peer_info(l, &info) == 0 && info.trouble == FLEET_PEER_OK &&
          strcmp(info.address, "192.168.1.23") == 0 && info.visible == 1);

    /* ---- every datagram once ------------------------------------------------ */
    got_want = 3;
    spin(l, WAIT_MS, received);
    spin(l, 300, received);
    check("three Fleet datagrams, each once: two from the inbox, one from an event",
          got == 3 && firsts[0] == 0x41 && firsts[1] == 0x47 && firsts[2] == 0x48);
    check("each from who sent it", got == 3 && memcmp(froms[0], anna, FLEET_KEY_BYTES) == 0 &&
          memcmp(froms[2], bob, FLEET_KEY_BYTES) == 0);

    /* ---- sending -------------------------------------------------------------- */
    check("a packet is handed over",
          l->ops->send(l->ctx, anna, (const uint8_t *)"\x41\0\0\1\0", 5) == FLEET_LINK_SENT);
    check("as exactly those bytes, to that key, on Fleet's port",
          until_logged(l, "peer.app_send|{\"to\":\"" ANNA "\",\"port\":1,"
                          "\"payload_hex\":\"4100000100\"}", 1));
    check("peerd's estimate becomes the retry base", spin(l, WAIT_MS, has_base) &&
          l->ops->retry_base(l->ctx) == 500);
    {
        uint8_t big[FLEET_PROTO_MAX + 1] = { 0 };

        check("nothing longer than a Fleet packet is sent",
              l->ops->send(l->ctx, anna, big, sizeof(big)) == FLEET_LINK_FAILED);
    }

    /* ---- the lease --------------------------------------------------------------- */
    n = count_lines(LOG, "peer.set_visible|{\"visible\":true}");
    spin(l, 300, NULL);
    check("the lease is not renewed on every poll",
          count_lines(LOG, "peer.set_visible|{\"visible\":true}") == n);
    skew += 10001;
    check("ten seconds on, it is renewed",
          until_logged(l, "peer.set_visible|{\"visible\":true}", n + 1));
    n = count_lines(LOG, "peer.set_visible|{\"visible\":true}");
    check("and at once when the player asks to be seen",
          l->ops->advertise(l->ctx) == 0 &&
          until_logged(l, "peer.set_visible|{\"visible\":true}", n + 1));

    /* ---- the opponent ------------------------------------------------------------ */
    check("nothing is kept or released before the session has said who the match is with",
          count_lines(LOG, "peer.kept|") == 0 && count_lines(LOG, "peer.keep|") == 0 &&
          count_lines(LOG, "peer.release|") == 0);
    l->ops->match_peer(l->ctx, bob);
    check("told the opponent, it has peerd keep them",
          until_logged(l, "peer.keep|{\"key\":\"" BOB "\"}", 1));
    check("and release the peer of a match that is no longer in hand",
          until_logged(l, "peer.release|{\"key\":\"" CAROL "\"}", 1));
    kept_want = 1;
    check("peerd then keeps exactly one", spin(l, WAIT_MS, kept_is));
    l->ops->match_peer(l->ctx, bob);
    spin(l, 300, NULL);
    check("told the same again, it asks nothing more",
          count_lines(LOG, "peer.keep|") == 1 && count_lines(LOG, "peer.release|") == 1);

    /* ---- an address ---------------------------------------------------------------- */
    check("what is not an address never becomes a request",
          fleet_link_peer_probe(l, "192.168.1") != 0 && fleet_link_peer_probe(l, "1.2.3.4.5") != 0 &&
          fleet_link_peer_probe(l, "256.1.1.1") != 0 && fleet_link_peer_probe(l, "bob.local") != 0 &&
          fleet_link_peer_probe(l, "") != 0 && count_lines(LOG, "peer.probe|") == 0);
    check("an address is asked", fleet_link_peer_probe(l, "192.168.1.41") == 0 &&
          fleet_link_peer_probe_state(l) == FLEET_PEER_PROBE_PENDING);
    probe_want = FLEET_PEER_PROBE_ANSWERED;
    check("the device there answers", spin(l, WAIT_MS, probe_is));
    check("and is who peerd says", fleet_link_peer_probe_key(l, who) == 0 &&
          memcmp(who, bob, FLEET_KEY_BYTES) == 0);
    check("as that address", count_lines(LOG, "peer.probe|{\"address\":\"192.168.1.41\"}") == 1);
    check("an address nobody is at is asked too", fleet_link_peer_probe(l, "192.168.1.99") == 0);
    probe_want = FLEET_PEER_PROBE_NO_ANSWER;
    check("and ends with no answer and no key", spin(l, WAIT_MS, probe_is) &&
          fleet_link_peer_probe_key(l, who) != 0);
    /* A probe replaced before its answer came: the first one's reply and any
     * list asked for it must not decide the second. */
    check("a probe is replaced by another before it is answered",
          fleet_link_peer_probe(l, "192.168.1.99") == 0 &&
          fleet_link_peer_probe(l, "192.168.1.41") == 0);
    probe_want = FLEET_PEER_PROBE_ANSWERED;
    check("and the outcome is the second one's", spin(l, WAIT_MS, probe_is) &&
          fleet_link_peer_probe_key(l, who) == 0 && memcmp(who, bob, FLEET_KEY_BYTES) == 0);
    spin(l, 700, NULL);
    check("and stays so", fleet_link_peer_probe_state(l) == FLEET_PEER_PROBE_ANSWERED);

    /* ---- peerd restarts ---------------------------------------------------------- */
    fake_peerd_stop(&script, pid);
    check("peerd gone, the link says so", spin(l, WAIT_MS, is_gone));
    script.run_id = "00000000000000b2";
    script.inbox_json = INBOX_AFTER_RESTART;
    script.events = NULL;
    script.kept = NULL;
    if (remove(LOG) != 0) {
        printf("note: no log to remove\n");
    }
    pid = fake_peerd_spawn(&script);
    check("peerd comes back", pid > 0 && fake_peerd_wait_ready(&script, WAIT_MS));
    skew += 6000;                    /* past the reconnect back-off */
    check("the link finds it again", spin(l, WAIT_MS, is_up));
    got = 0;
    got_want = 1;
    check("its ids start again: datagram 1 of the new run is taken",
          spin(l, WAIT_MS, received) && got == 1 && firsts[0] == 0x51);
    check("it asks to be visible again",
          until_logged(l, "peer.set_visible|{\"visible\":true}", 1));
    check("and asserts the opponent again to a peerd that has lost its list",
          until_logged(l, "peer.keep|{\"key\":\"" BOB "\"}", 1));

    /* ---- the match ends, Fleet leaves ---------------------------------------------- */
    l->ops->match_peer(l->ctx, NULL);
    check("the match over, the opponent is released",
          until_logged(l, "peer.release|{\"key\":\"" BOB "\"}", 1));
    kept_want = 0;
    check("and peerd keeps nobody", spin(l, WAIT_MS, kept_is));
    /* Closed with answers from peerd still unread, as an app that is shut
     * between two polls closes it: the request to hide must get through
     * all the same. */
    l->ops->match_peer(l->ctx, bob);
    l->ops->advertise(l->ctx);
    l->ops->poll(l->ctx, now_ms() + 2100);
    usleep(50000);
    l->ops->close(l->ctx);
    {
        int waited = 0;

        while (count_lines(LOG, "peer.set_visible|{\"visible\":false}") == 0 && waited < WAIT_MS) {
            usleep(5000);
            waited += 5;
        }
    }
    check("closing ends the visibility lease",
          count_lines(LOG, "peer.set_visible|{\"visible\":false}") == 1);
    fake_peerd_stop(&script, pid);

    /* ---- refusals --------------------------------------------------------------- */
    memset(&script, 0, sizeof(script));
    script.refuse_send = 1;
    script.refuse_keep = 1;
    script.probe_pending = 1;
    script.log = LOG;
    script.life_ms = 60000;
    remove(LOG);
    pid = fake_peerd_spawn(&script);
    if (pid > 0 && fake_peerd_wait_ready(&script, WAIT_MS)) {
        l = fleet_link_peer_open(NULL);
        spin(l, WAIT_MS, is_up);
        check("a send peerd refuses afterwards is still handed over - the match retries",
              l->ops->send(l->ctx, anna, (const uint8_t *)"\x41\0\0\1\0", 5) == FLEET_LINK_SENT &&
              until_logged(l, "peer.app_send|", 1) && is_up(l));
        l->ops->match_peer(l->ctx, bob);
        until_logged(l, "peer.keep|", 1);
        n = count_lines(LOG, "peer.keep|");
        spin(l, 400, NULL);
        check("a keep peerd cannot write is not hammered",
              count_lines(LOG, "peer.keep|") == n && n == 1);
        skew += 1700;                /* 2.1 s of link time gone; the next try is due at 4 s */
        spin(l, 400, NULL);
        check("nor after the usual two seconds: it waits longer each time",
              count_lines(LOG, "peer.keep|") == 1);
        skew += 2500;
        check("and is tried again later", until_logged(l, "peer.keep|", 2));
        check("a probe that is pending stays pending", fleet_link_peer_probe(l, "10.0.0.9") == 0 &&
              (spin(l, 800, NULL), fleet_link_peer_probe_state(l) == FLEET_PEER_PROBE_PENDING));
        fake_peerd_stop(&script, pid);
        pid = 0;
        check("and ends when peerd goes away", spin(l, WAIT_MS, is_gone) &&
              fleet_link_peer_probe_state(l) == FLEET_PEER_PROBE_NO_ANSWER);
        l->ops->close(l->ctx);
    } else {
        check("a refusing peerd starts", 0);
    }
    if (pid > 0) {
        fake_peerd_stop(&script, pid);
    }

    /* ---- events during the catch-up, and a reconnect to the same run -------------- */
    {
        static const char two[] =
            "[{\"id\":1,\"port\":1,\"from\":\"" ANNA "\",\"payload_hex\":\"4100000100\","
            "\"route\":\"lan\",\"mono_ms\":-2000},"
            "{\"id\":7,\"port\":1,\"from\":\"" BOB "\",\"payload_hex\":\"4700000100\","
            "\"route\":\"lan\",\"mono_ms\":-500}]";
        static const char three[] =
            "[{\"id\":1,\"port\":1,\"from\":\"" ANNA "\",\"payload_hex\":\"4100000100\","
            "\"route\":\"lan\",\"mono_ms\":-2000},"
            "{\"id\":7,\"port\":1,\"from\":\"" BOB "\",\"payload_hex\":\"4700000100\","
            "\"route\":\"lan\",\"mono_ms\":-500},"
            "{\"id\":9,\"port\":1,\"from\":\"" BOB "\",\"payload_hex\":\"4900000100\","
            "\"route\":\"lan\",\"mono_ms\":-100}]";
        /* Datagram 7 as an event, raised before the inbox has been answered.
         * Taken then, it would put the cursor past datagram 1. */
        static const char *const early[] = {
            "{\"id\":7,\"port\":1,\"from\":\"" BOB "\",\"payload_hex\":\"4700000100\","
            "\"route\":\"lan\",\"mono_ms\":-500}",
            NULL,
        };

        memset(&script, 0, sizeof(script));
        script.inbox_json = two;
        script.events = early;
        script.events_early = 1;
        script.log = LOG;
        script.life_ms = 60000;
        remove(LOG);
        pid = fake_peerd_spawn(&script);
        if (pid > 0 && fake_peerd_wait_ready(&script, WAIT_MS)) {
            l = fleet_link_peer_open(NULL);
            got = 0;
            got_want = 2;
            spin(l, WAIT_MS, received);
            spin(l, 300, received);
            check("an event that arrives during the catch-up neither hides an older datagram "
                  "nor is taken twice", got == 2 && firsts[0] == 0x41 && firsts[1] == 0x47);
            fake_peerd_stop(&script, pid);
            spin(l, WAIT_MS, is_gone);
            script.inbox_json = three;
            script.events = NULL;
            remove(LOG);
            pid = fake_peerd_spawn(&script);
            fake_peerd_wait_ready(&script, WAIT_MS);
            skew += 6000;
            got = 0;
            got_want = 1;
            spin(l, WAIT_MS, received);
            spin(l, 300, received);
            check("reconnected to the same run, only what is new is taken",
                  got == 1 && firsts[0] == 0x49);
            check("because the inbox was asked from where the link had got to",
                  count_lines(LOG, "peer.app_inbox|{\"port\":1,\"after_id\":7}") >= 1 &&
                  count_lines(LOG, "\"after_id\":0}") == 0);
            l->ops->close(l->ctx);
        } else {
            check("a peerd with early events starts", 0);
        }
        if (pid > 0) {
            fake_peerd_stop(&script, pid);
        }
    }

    /* ---- closed by a fault ------------------------------------------------------- */
    memset(&script, 0, sizeof(script));
    script.state = "identity_fault";
    script.log = LOG;
    script.life_ms = 60000;
    remove(LOG);
    pid = fake_peerd_spawn(&script);
    if (pid > 0 && fake_peerd_wait_ready(&script, WAIT_MS)) {
        l = fleet_link_peer_open(NULL);
        check("a peerd closed by a fault: the link is not usable", spin(l, WAIT_MS, is_off));
        check("and names the fault", fleet_link_peer_info(l, &info) == 0 &&
              info.trouble == FLEET_PEER_FAULT && strcmp(info.fault, "identity_fault") == 0);
        check("a packet is held, not lost",
              l->ops->send(l->ctx, anna, (const uint8_t *)"\x41\0\0\1\0", 5) == FLEET_LINK_BUSY);
        l->ops->match_peer(l->ctx, bob);
        skew += 30000;
        spin(l, 500, NULL);
        check("nothing but its status is asked of it while it is closed",
              count_lines(LOG, "peer.kept|") == 0 && count_lines(LOG, "peer.nodes|") <= 1 &&
              count_lines(LOG, "peer.set_visible|") <= 1 && count_lines(LOG, "peer.status|") >= 2);
        l->ops->close(l->ctx);
    } else {
        check("a faulted peerd starts", 0);
    }
    if (pid > 0) {
        fake_peerd_stop(&script, pid);
    }

    /* ---- no network -------------------------------------------------------------- */
    memset(&script, 0, sizeof(script));
    script.no_interfaces = 1;
    script.life_ms = 60000;
    pid = fake_peerd_spawn(&script);
    if (pid > 0 && fake_peerd_wait_ready(&script, WAIT_MS)) {
        l = fleet_link_peer_open(NULL);
        spin(l, WAIT_MS, is_up);
        skew += 2100;
        spin(l, 300, NULL);
        skew += 2100;
        spin(l, 300, NULL);
        check("a peerd with no interface to announce on: the link says so",
              fleet_link_peer_info(l, &info) == 0 && info.trouble == FLEET_PEER_NO_NETWORK);
        check("and stays up, for a device at a known address", is_up(l) &&
              l->ops->send(l->ctx, anna, (const uint8_t *)"\x41\0\0\1\0", 5) == FLEET_LINK_SENT);
        l->ops->close(l->ctx);
    } else {
        check("a peerd without a network starts", 0);
    }
    if (pid > 0) {
        fake_peerd_stop(&script, pid);
    }

    {
        char cmd[700];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", runtime);
        if (system(cmd) != 0) {
            fprintf(stderr, "note: could not remove %s\n", runtime);
        }
    }
    printf("fleet_link_peer_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
