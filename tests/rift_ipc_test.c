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
                       strcmp(line, "mesh.node") != 0 && strcmp(line, "mesh.messages") != 0 &&
                       strcmp(line, "mesh.channels") != 0) {
                saw_unexpected = 1;
                printf("     unexpected method: %s\n", line);
            }
        }
        if (f) {
            fclose(f);
        }
        check("it was asked for something", lines > 0);
        /* Everything above this point is what the app does on its own:
         * connect, read a snapshot, subscribe, lose the service, reconnect.
         * None of it may transmit. Phase 2 can send, but only when a reader
         * asks it to, which is the section after this one. */
        check("nothing the app does on its own asks the service to send", !saw_send);
        check("or to advert", !saw_advert);
        check("only the methods this phase consumes were used", !saw_unexpected);
        check("the subscription was taken", saw_subscribe);
        check("and given back rather than merely dropped", saw_unsubscribe);
        unlink(methods);
    }


    /* ---- sending, against a real socket ---------------------------------- */
    /* Everything above proved the app does not transmit on its own. This
     * proves that when it does, the message goes out once, its outcome comes
     * from the service, and a refusal reaches the reader. */
    {
        char sends[600];
        struct fake_meshcored_script script;
        pid_t pid;

        snprintf(sends, sizeof(sends), "%s/sends", runtime);
        unlink(sends);
        unlink(methods);
        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.messages_json =
            "[{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
            "\"peer_name\":\"HYTTA\",\"text\":\"er du der?\",\"state\":\"received\","
            "\"mono_ms\":-4000}]";
        script.send_log = sends;
        script.method_log = methods;
        script.life_ms = 6000;
        pid = fake_meshcored_spawn(&script);
        check("a service that takes messages is running", pid > 0);
        check("and its socket is there", fake_meshcored_wait_ready(2000));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored");
        spin(&c, 2000, have_snapshot, &m);
        check("the history was read with the snapshot", m.messages_valid && m.msg_count == 1);
        check("and the service said it does not keep it", !m.messages_persistent);
        check("nothing from that first snapshot is unread", rift_model_unread_total(&m) == 0);

        check("a message is sent", rift_ipc_send_message(&c, KEY_B, "kommer nå") == 0);
        /* The reply and the event may arrive in either order; either way the
         * message must exist exactly once, under the service's id. */
        spin(&c, 2000, NULL, &m);
        check("the submission is finished", !rift_model_sending(&m));
        check("the service gave it an id", m.outbox.message_id > 0);
        check("and exactly one message was added", m.msg_count == 2);
        {
            const struct rift_message *thread[8];
            int n = rift_model_thread(&m, KEY_B, thread, 8, NULL);

            check("the thread holds both", n == 2);
            check("ours is outgoing", thread[1]->dir == RIFT_MSG_OUT);
            check("with the service's state, not ours", thread[1]->state == RIFT_MSG_SENT_FLOOD);
            check("and it is not shown as delivered", thread[1]->state != RIFT_MSG_ACKED);
        }

        /* What went on the air is what the reader typed, once. */
        {
            FILE *f = fopen(sends, "r");
            char line[256];
            int lines = 0;

            while (f && fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = '\0';
                lines++;
                text_is("the service was asked to send exactly what was typed", line,
                        KEY_B "|kommer nå");
            }
            if (f) {
                fclose(f);
            }
            check("once, not twice", lines == 1);
        }

        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
        unlink(sends);
        unlink(methods);
    }

    /* ---- channels, against a real socket ---------------------------------- */
    /* The whole path: the channel list read on connect, a channel message
     * arriving as an event, a message sent to a channel rather than a node,
     * and the list re-read after the service has gone and come back. */
    {
        char sends[600];
        struct fake_meshcored_script script;
        pid_t pid;
        static const char *const chan_events[] = {
            "mesh.message|{\"message\":{\"id\":5,\"direction\":\"in\",\"kind\":\"channel\","
            "\"channel\":0,\"channel_name\":\"SITE\",\"channel_hash\":\"8c\","
            "\"sender_name\":\"HYTTA\",\"text\":\"HYTTA: all clear\","
            "\"state\":\"received\",\"ack_expected\":false,\"mono_ms\":-1000}}",
            "mesh.channel|{\"reason\":\"added\",\"channel\":{\"channel\":3,"
            "\"name\":\"LATE\",\"channel_hash\":\"2a\",\"key_bits\":256,"
            "\"text_limit\":147}}",
            NULL,
        };

        snprintf(sends, sizeof(sends), "%s/sends", runtime);
        unlink(sends);
        unlink(methods);
        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.channels_json =
            "[{\"channel\":0,\"name\":\"SITE\",\"channel_hash\":\"8c\",\"key_bits\":256,"
            "\"text_limit\":147,\"ack_expected\":false}]";
        script.events = chan_events;
        script.send_log = sends;
        script.method_log = methods;
        script.life_ms = 6000;
        pid = fake_meshcored_spawn(&script);
        check("a service with a channel is running", pid > 0);
        check("and its socket is there", fake_meshcored_wait_ready(2000));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored");
        spin(&c, 2000, have_snapshot, &m);
        check("the channel list was read on connect", m.channels_valid);
        check("and holds the one the service has", m.channel_count >= 1);
        check("in the slot it named", rift_model_channel(&m, 0) != NULL);
        check("with the limit the service gave, not the API's 160",
              rift_model_text_limit(&m, "#0") == 147);

        /* The event that arrives on the channel. */
        spin(&c, 2000, NULL, &m);
        {
            const struct rift_message *thread[8];
            int n = rift_model_thread(&m, "#0", thread, 8, NULL);

            check("a channel message arrived as an event", n == 1);
            check("as a channel message", n == 1 && thread[0]->is_channel);
            check("with the sender's claimed name",
                  n == 1 && thread[0]->have_sender_name &&
                      strcmp(thread[0]->sender_name, "HYTTA") == 0);
            check("and nothing that could acknowledge it",
                  n == 1 && !thread[0]->ack_expected);
            /* Deliberately no unread assertion here. This event races the
             * first mesh.messages reply, and whatever is in the cache when
             * that lands is seeded as read - nothing has been drawn yet, so
             * "unread since you last looked" is not a question that has an
             * answer (rift_model.h, messages_seeded). Whether this one is
             * unread therefore depends on which frame arrived first, which
             * is not a property of channels. tests/rift_comms_test.c settles
             * the unread behaviour deterministically instead. */
            {
                struct rift_conv conv[RIFT_MAX_CONVERSATIONS];

                check("and it is in the conversation list",
                      rift_model_conversations(&m, conv, RIFT_MAX_CONVERSATIONS) >= 1);
            }
        }
        /* A channel added while the client was connected turns up without a
         * snapshot being asked for. */
        check("a channel added later arrives as an event",
              rift_model_channel(&m, 3) != NULL);

        /* Sending to a channel. */
        check("a message is sent to the channel",
              rift_ipc_send_message(&c, "#0", "pa vei") == 0);
        spin(&c, 2000, NULL, &m);
        check("the submission finished", !rift_model_sending(&m));
        check("the service gave it an id", m.outbox.message_id > 0);
        /* What went on the air, and to where: a channel slot, not a node. */
        {
            FILE *f = fopen(sends, "r");
            char line[256];
            int lines = 0;

            while (f && fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = '\0';
                lines++;
                text_is("the service was asked to send to the channel", line, "#0|pa vei");
            }
            if (f) {
                fclose(f);
            }
            check("once, not twice", lines == 1);
        }
        /* A body too long for the channel is refused before anything is
         * written: the limit is the service's, and it is shorter than a
         * direct message's because this node's name travels inside. */
        {
            char too_long[200];
            FILE *f;
            char line[256];
            int lines = 0;

            memset(too_long, 'x', sizeof(too_long));
            too_long[148] = '\0';
            check("a body over the channel's limit is refused",
                  rift_ipc_send_message(&c, "#0", too_long) == -1);
            check("and the reader is told why", m.outbox.failed && m.outbox.error[0]);
            f = fopen(sends, "r");
            while (f && fgets(line, sizeof(line), f)) {
                lines++;
            }
            if (f) {
                fclose(f);
            }
            check("nothing more was written to the service", lines == 1);
        }

        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
        unlink(sends);
        unlink(methods);
    }

    /* ---- a channel list that changes across a reconnect -------------------- */
    {
        struct fake_meshcored_script script;
        pid_t pid;

        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.channels_json =
            "[{\"channel\":0,\"name\":\"SITE\",\"channel_hash\":\"8c\",\"key_bits\":256,"
            "\"text_limit\":147},"
            "{\"channel\":1,\"name\":\"OPS\",\"channel_hash\":\"4d\",\"key_bits\":128,"
            "\"text_limit\":147}]";
        script.serve_clients = 1;
        script.life_ms = 4000;
        pid = fake_meshcored_spawn(&script);
        check("a service with two channels came up", fake_meshcored_wait_ready(3000));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored");
        spin(&c, 2000, have_snapshot, &m);
        check("both channels were read", m.channel_count == 2);

        fake_meshcored_stop(pid);
        spin(&c, 2000, NULL, &m);
        check("the service went away", m.state == RIFT_SVC_ABSENT);
        /* The channels stay on screen and stop being current. Blanking them
         * would say something less true than a stale list does. */
        check("the channels are still shown", m.channel_count == 2);
        check("but are no longer current", !m.channels_valid);
        {
            struct fake_meshcored_script again;
            pid_t pid2;

            memset(&again, 0, sizeof(again));
            again.state = "online";
            again.nodes_json = NODES_TWO;
            /* One of them has been left while nobody was watching. */
            again.channels_json =
                "[{\"channel\":1,\"name\":\"OPS\",\"channel_hash\":\"4d\",\"key_bits\":128,"
                "\"text_limit\":147}]";
            again.life_ms = 4000;
            pid2 = fake_meshcored_spawn(&again);
            check("the service comes back", fake_meshcored_wait_ready(3000));
            spin(&c, 8000, have_snapshot, &m);
            check("the channel list was re-read", m.channels_valid);
            check("and is now what the service holds", m.channel_count == 1);
            /* The one that was left is gone rather than lingering as
             * somewhere to write that nothing would carry. */
            check("the channel that was left is not offered",
                  rift_model_channel(&m, 0) == NULL);
            check("and the one still held is", rift_model_channel(&m, 1) != NULL);
            rift_ipc_close(&c);
            fake_meshcored_stop(pid2);
        }
    }

    /* ---- the first message of a conversation ------------------------------ */
    /* NODES -> MESSAGE opens a conversation that holds nothing. Sending from
     * there is the case where there is no history to hide a mistake in: the
     * service must be asked exactly once, and exactly one message must
     * exist afterwards - the service's, not one this app made up. */
    {
        char sends[600];
        struct fake_meshcored_script script;
        pid_t pid;

        snprintf(sends, sizeof(sends), "%s/sends-new", runtime);
        unlink(sends);
        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.send_log = sends;
        script.life_ms = 6000;
        pid = fake_meshcored_spawn(&script);
        check("a service with no message history is running",
              pid > 0 && fake_meshcored_wait_ready(2000));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored");
        spin(&c, 2000, have_snapshot, &m);
        check("and it holds none", m.messages_valid && m.msg_count == 0);
        check("so the peer has no conversation yet",
              rift_model_thread(&m, KEY_A, NULL, 0, NULL) == 0);

        check("the first message is written", rift_ipc_send_message(&c, KEY_A, "first") == 0);
        spin(&c, 2000, NULL, &m);
        check("exactly one message exists afterwards", m.msg_count == 1);
        check("and it is the service's, under the service's id", m.msg[0].id > 0);
        check("nothing was invented before the service answered", m.msgs_duplicate == 0);
        {
            FILE *f = fopen(sends, "r");
            char line[256];
            int lines = 0;

            while (f && fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = '\0';
                lines++;
                text_is("the service was asked to send just that", line, KEY_A "|first");
            }
            if (f) {
                fclose(f);
            }
            check("once", lines == 1);
        }
        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
        unlink(sends);
    }

    /* ---- a service that refuses the send --------------------------------- */
    {
        struct fake_meshcored_script script;
        pid_t pid;

        memset(&script, 0, sizeof(script));
        script.state = "degraded";
        script.nodes_json = NODES_TWO;
        script.refuse_send = 1;
        script.life_ms = 5000;
        pid = fake_meshcored_spawn(&script);
        check("a service that will not send is running", pid > 0);
        check("and is answering", fake_meshcored_wait_ready(2000));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored");
        spin(&c, 2000, have_snapshot, &m);
        check("the send is written", rift_ipc_send_message(&c, KEY_B, "hallo") == 0);
        spin(&c, 2000, NULL, &m);
        check("and comes back refused", m.outbox.failed);
        check("with the service's own words, not ours",
              strstr(m.outbox.error, "the radio is not available") != NULL);
        check("nothing was added to the thread", m.msg_count == 0);
        check("and nothing is left in flight", !rift_model_sending(&m));

        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
    }

    /* ---- a service that accepts and then says nothing --------------------- */
    /* An id handed out and never spoken of again. The app must not decide
     * for itself what became of it. */
    {
        struct fake_meshcored_script script;
        pid_t pid;

        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.send_is_silent = 1;
        script.life_ms = 5000;
        pid = fake_meshcored_spawn(&script);
        check("a silent service is running", pid > 0 && fake_meshcored_wait_ready(2000));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored");
        spin(&c, 2000, have_snapshot, &m);
        rift_ipc_send_message(&c, KEY_B, "into the quiet");
        spin(&c, 1500, NULL, &m);
        check("the submission ended, because the reply came", !rift_model_sending(&m));
        check("it was not a failure", !m.outbox.failed);
        check("but no message exists, because none was ever reported", m.msg_count == 0);

        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
    }

    /* ---- sending with nobody there ---------------------------------------- */
    {
        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored-that-is-not-there");
        check("a send with no connection is refused",
              rift_ipc_send_message(&c, KEY_B, "hello?") == -1);
        check("and says so rather than failing silently", m.outbox.failed);
        check("nothing was queued", !rift_model_sending(&m));
        rift_ipc_close(&c);
    }
    rmdir(runtime);
    printf("rift_ipc_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
