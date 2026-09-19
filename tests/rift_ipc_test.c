/*
 * RIFT's meshcored client, against a real socket and a scripted service.
 *
 * What is proved here is the part that cannot be proved with a mock in the
 * same process: that the client connects, reads a snapshot, subscribes,
 * keeps events that arrive while a request is outstanding, survives the
 * service disappearing under it, reconnects with a backoff and re-reads the
 * snapshot, and refuses rubbish without dropping the connection over it.
 *
 * It also proves a negative that matters more than any of them: this client
 * calls nothing that transmits. Opening RIFT must not put a packet on the
 * air, and the service here records every method it is asked for.
 *
 * No LVGL. The fake service runs in a child process, so the socket, the
 * framing and the disconnection are all real.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_ipc.h"

#include "fake_meshcored.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void text_is(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want);
    }
}

#define KEY_A "a19ac21e7d0411223344556677889900aabbccddeeff001122334455667788b1"
#define KEY_B "b2cafe1e7d0411223344556677889900aabbccddeeff001122334455667788b2"

#define NODES_TWO                                                                        \
    "[{\"public_key\":\"" KEY_A "\",\"node_hash\":\"a1\",\"name\":\"OSLO-01\",\"type\":1," \
    "\"path_known\":true,\"hops\":0,\"direct\":true,\"last_heard_mono_ms\":1000,"          \
    "\"last_rssi_dbm\":-71.0,\"last_snr_db\":9.5},"                                        \
    "{\"public_key\":\"" KEY_B "\",\"node_hash\":\"b2\",\"name\":\"HYTTA\",\"type\":2,"     \
    "\"path_known\":true,\"hops\":3,\"direct\":false,\"path_hex\":\"a1c2d3\","              \
    "\"last_heard_mono_ms\":500}]"

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Poll the client the way the app's timer does, until `done` or the budget
 * runs out. Returns the milliseconds spent. */
static int spin(struct rift_ipc *c, int budget_ms, int (*done)(const struct rift_ipc *,
                                                               const struct rift_model *),
                const struct rift_model *m)
{
    int64_t start = now_ms();

    for (;;) {
        int64_t now = now_ms();

        rift_ipc_poll(c, now);
        if (done && done(c, m)) {
            return (int)(now_ms() - start);
        }
        if (now - start >= budget_ms) {
            return (int)(now - start);
        }
        usleep(2000);
    }
}

static int have_snapshot(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)c;
    return m->snapshot_valid && m->have_identity && m->have_info && m->have_status;
}

static int have_events(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)m;
    return c->events_in >= 3;
}

static int is_down(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)m;
    return c->fd < 0;
}

static int is_up_again(const struct rift_ipc *c, const struct rift_model *m)
{
    return c->connects >= 2 && m->snapshot_valid;
}

int main(void)
{
    char runtime[] = "/tmp/rift_ipc_test.XXXXXX";
    struct rift_model m;
    struct rift_ipc c;
    char methods[600];
    pid_t pid;

    if (!mkdtemp(runtime)) {
        printf("FAIL could not make a runtime directory\n");
        return 1;
    }
    setenv("POCKETOS_RUNTIME_DIR", runtime, 1);
    snprintf(methods, sizeof(methods), "%s/methods.txt", runtime);

    /* ---- no service at all ------------------------------------------------ */
    rift_model_init(&m);
    rift_ipc_init(&c, &m, RIFT_SERVICE);
    check("a client with nowhere to connect starts down", !rift_ipc_connected(&c));
    rift_ipc_poll(&c, now_ms());
    check("and one pass leaves it down", !rift_ipc_connected(&c));
    check("the service is reported absent, not unknown", m.state == RIFT_SVC_ABSENT);
    check("and the app is told why", c.last_error[0] != '\0');
    check("nothing was invented to show", m.node_count == 0 && !m.snapshot_valid);
    {
        /* A poll before the backoff has expired must not hammer the socket:
         * the first attempt failed, so the next one waits. */
        unsigned attempts = c.connects;
        int64_t t = now_ms();
        int i;

        for (i = 0; i < 50; i++) {
            rift_ipc_poll(&c, t);
        }
        check("polling harder does not retry harder", c.connects == attempts);
    }
    rift_ipc_close(&c);

    /* ---- a service that answers -------------------------------------------- */
    {
        static const char *const events[] = {
            "mesh.state|{\"state\":\"online\",\"reason\":\"receiving\",\"mono_ms\":1200}",
            "mesh.activity|{\"kind\":\"rx\",\"payload_type\":\"advert\",\"bytes\":48,"
            "\"mono_ms\":1300,\"rssi_dbm\":-88.0,\"snr_db\":6.0}",
            /* The same node again, with a longer path: an update, not a
             * second row. */
            "mesh.node|{\"reason\":\"path\",\"node\":{\"public_key\":\"" KEY_B "\","
            "\"node_hash\":\"b2\",\"name\":\"HYTTA\",\"type\":2,\"path_known\":true,"
            "\"hops\":5,\"direct\":false,\"path_hex\":\"a1c2d3e4f5\","
            "\"last_heard_mono_ms\":1400}}",
            /* Rubbish, in the two shapes a subscriber actually sees. */
            "mesh.node|{\"reason\":\"discovered\",\"node\":{\"name\":\"no key\"}}",
            "mesh.nonsense|{\"whatever\":1}",
            NULL
        };
        struct fake_meshcored_script script;

        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.reason = "receiving";
        script.nodes_json = NODES_TWO;
        script.events = events;
        script.junk_frame = "{\"hello\":\"this is not a message\"}";
        script.life_ms = 8000;
        script.method_log = methods;

        pid = fake_meshcored_spawn(&script);
        check("the fake service came up", fake_meshcored_wait_ready(3000));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, RIFT_SERVICE);
        spin(&c, 3000, have_snapshot, &m);
        check("the client connects", rift_ipc_connected(&c));
        check("and reads the whole opening set", have_snapshot(&c, &m));
        check("the node list is the service's", m.node_count == 2);
        text_is("with this device's name", m.self_name, "K230-A");
        text_is("and the service's protocol", m.protocol, "meshcore");
        check("nothing is stale while it is answering", !m.stale);

        spin(&c, 3000, have_events, &m);
        check("events arrive", c.events_in >= 3);
        check("a state event is applied", m.state == RIFT_SVC_ONLINE);
        check("an activity event is kept", m.activity_total >= 1);
        check("a node event updates the node it names",
              rift_model_find(&m, KEY_B) && rift_model_find(&m, KEY_B)->hops == 5);
        check("and does not add a row", m.node_count == 2);

        /* Let the two bad events and the junk frame arrive. */
        spin(&c, 1500, NULL, &m);
        check("a malformed event is refused", m.events_malformed >= 2);
        check("and refusing it did not drop the connection", rift_ipc_connected(&c));
        check("a frame that is neither event nor reply is counted", c.bad_frames >= 1);
        check("and that did not drop the connection either", rift_ipc_connected(&c));
        check("the connection has not been remade behind our back", c.connects == 1);

        /* One node, asked for by name. */
        check("a single node can be asked for", rift_ipc_request_node(&c, KEY_A) == 0);
        spin(&c, 800, NULL, &m);
        check("and the answer updates the row it belongs to", m.node_count == 2);

        /* A prefix that matches nothing is refused by the service, and the
         * refusal is reported rather than swallowed. */
        {
            unsigned errors = c.errors_in;

            rift_ipc_request_node(&c, "ffff");
            spin(&c, 800, NULL, &m);
            check("a refusal from the service is counted", c.errors_in == errors + 1);
            check("and said in words", strstr(c.last_error, "mesh.node") != NULL);
            check("without dropping the connection", rift_ipc_connected(&c));
        }

        check("every request outstanding has been answered",
              c.replies_in >= c.requests_out - 1);
        check("and nothing was refused for want of room", c.requests_refused == 0);
        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
    }

    /* ---- the service goes away under a live client -------------------------- */
    {
        struct fake_meshcored_script script;

        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.life_ms = 1200;
        script.method_log = methods;

        pid = fake_meshcored_spawn(&script);
        check("a short-lived service came up", fake_meshcored_wait_ready(3000));
        rift_model_init(&m);
        rift_ipc_init(&c, &m, RIFT_SERVICE);
        spin(&c, 3000, have_snapshot, &m);
        check("the client read its snapshot", m.node_count == 2);

        fake_meshcored_stop(pid);
        spin(&c, 3000, is_down, &m);
        check("the client notices the service has gone", !rift_ipc_connected(&c));
        check("and says so", m.state == RIFT_SVC_ABSENT);
        /* The nodes stay, marked as what they are. An empty list would say
         * something less true than a stale one. */
        check("what it knew is still shown", m.node_count == 2);
        check("marked cached", m.stale);
        check("and the snapshot is no longer valid", !m.snapshot_valid);
        check("the backoff has grown past its floor", c.backoff_ms > RIFT_BACKOFF_MIN_MS);

        /* ---- and comes back -------------------------------------------------- */
        {
            struct fake_meshcored_script again;

            memset(&again, 0, sizeof(again));
            again.state = "degraded";
            again.reason = "the radio is not receiving";
            /* One node fewer: the snapshot replaces, so the one the service
             * has forgotten must go. */
            again.nodes_json = "[{\"public_key\":\"" KEY_A "\",\"node_hash\":\"a1\","
                               "\"name\":\"OSLO-01\",\"path_known\":false,"
                               "\"last_heard_mono_ms\":2000}]";
            again.life_ms = 6000;
            again.method_log = methods;
            pid = fake_meshcored_spawn(&again);
            check("the service comes back", fake_meshcored_wait_ready(3000));
            spin(&c, 6000, is_up_again, &m);
            check("the client reconnects on its own", c.connects >= 2);
            check("re-reads the snapshot", m.snapshot_valid);
            check("which replaces what it had", m.node_count == 1);
            check("so a node the service has forgotten is gone", rift_model_find(&m, KEY_B) ==
                                                                     NULL);
            check("nothing is cached any more", !m.stale);
            check("and the new state is the one it is in now", m.state == RIFT_SVC_DEGRADED);
            check("the backoff went back to its floor", c.backoff_ms == RIFT_BACKOFF_MIN_MS);
            rift_ipc_close(&c);
            fake_meshcored_stop(pid);
        }
    }

    /* ---- a service that refuses ---------------------------------------------- */
    {
        struct fake_meshcored_script script;

        memset(&script, 0, sizeof(script));
        script.state = "error";
        script.reason = "radiod refused the profile";
        script.refuse_nodes = 1;
        script.life_ms = 3000;
        script.method_log = methods;
        pid = fake_meshcored_spawn(&script);
        check("a refusing service came up", fake_meshcored_wait_ready(3000));
        rift_model_init(&m);
        rift_ipc_init(&c, &m, RIFT_SERVICE);
        spin(&c, 2000, NULL, &m);
        check("the client stays connected to a service that refuses one method",
              rift_ipc_connected(&c));
        check("the refusal is counted", c.errors_in >= 1);
        check("no node list is claimed", !m.snapshot_valid && m.node_count == 0);
        check("but the state it is in is still read", m.state == RIFT_SVC_ERROR);
        text_is("with the reason", m.reason, "radiod refused the profile");
        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
    }

    /* ---- open, close, open again --------------------------------------------- */
    {
        struct fake_meshcored_script script;
        int round;

        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.life_ms = 8000;
        script.method_log = methods;
        pid = fake_meshcored_spawn(&script);
        check("a service for the restart rounds came up", fake_meshcored_wait_ready(3000));
        for (round = 0; round < 3; round++) {
            rift_model_init(&m);
            rift_ipc_init(&c, &m, RIFT_SERVICE);
            spin(&c, 2500, have_snapshot, &m);
            check("each time the app opens it reads the service fresh", m.node_count == 2);
            /* Closing gives the subscription back and closes the socket; a
             * second close must be safe, because a failed create reaches
             * the same path. */
            rift_ipc_close(&c);
            rift_ipc_close(&c);
            check("and closing leaves nothing open", c.fd < 0);
        }
        fake_meshcored_stop(pid);
    }

    /* ---- the negative that matters -------------------------------------------- */
    /* Not proved by reading the client's source, but by what the service
     * was asked for over the whole of this run. Opening RIFT must not put a
     * packet on the air. */
    {
        char line[128];
        int saw_send = 0;
        int saw_advert = 0;
        int saw_unexpected = 0;
        int saw_subscribe = 0;
        int saw_unsubscribe = 0;
        int lines = 0;
        FILE *f = fopen(methods, "r");

        check("the service recorded what it was asked for", f != NULL);
        while (f && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';
            lines++;
            if (strcmp(line, "mesh.send") == 0) {
                saw_send = 1;
            } else if (strcmp(line, "mesh.advert") == 0) {
                saw_advert = 1;
            } else if (strcmp(line, "mesh.subscribe") == 0) {
                saw_subscribe = 1;
            } else if (strcmp(line, "mesh.unsubscribe") == 0) {
                saw_unsubscribe = 1;
            } else if (strcmp(line, "mesh.info") != 0 && strcmp(line, "mesh.status") != 0 &&
                       strcmp(line, "mesh.identity") != 0 && strcmp(line, "mesh.nodes") != 0 &&
                       strcmp(line, "mesh.node") != 0) {
                saw_unexpected = 1;
                printf("     unexpected method: %s\n", line);
            }
        }
        if (f) {
            fclose(f);
        }
        check("it was asked for something", lines > 0);
        check("nothing in this app ever asked the service to send a message", !saw_send);
        check("or to advert", !saw_advert);
        check("only the seven methods phase 1 consumes were used", !saw_unexpected);
        check("the subscription was taken", saw_subscribe);
        check("and given back rather than merely dropped", saw_unsubscribe);
        unlink(methods);
    }

    rmdir(runtime);
    printf("rift_ipc_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
