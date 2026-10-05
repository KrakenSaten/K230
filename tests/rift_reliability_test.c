/*
 * RIFT's message reliability, replies and contacts, without a display or a
 * service: what a message says about its delivery and its attempts, what is
 * kept when meshcored restarts under an unacknowledged message, what a
 * RESEND takes, how a reply is written and read back, and how the stored
 * contacts are listed and searched.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_contacts.h"
#include "rift_format.h"
#include "rift_model.h"
#include "rift_reply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    if (strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

#define KEY_A "aa11111111111111111111111111111111111111111111111111111111111111"
#define KEY_B "bb22222222222222222222222222222222222222222222222222222222222222"
#define KEY_R "cc33333333333333333333333333333333333333333333333333333333333333"

static int apply_messages(struct rift_model *m, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_messages(m, o);

    cJSON_Delete(o);
    return rc;
}

static int apply_event(struct rift_model *m, const char *name, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_event(m, name, o);

    cJSON_Delete(o);
    return rc;
}

static void give_status(struct rift_model *m, int uptime_s, int64_t now_ms)
{
    char json[128];
    cJSON *o;

    snprintf(json, sizeof(json), "{\"state\":\"online\",\"uptime_s\":%d}", uptime_s);
    o = cJSON_Parse(json);
    rift_model_apply_status(m, o, now_ms);
    cJSON_Delete(o);
}

static const struct rift_message *by_text(const struct rift_model *m, const char *text)
{
    int i;

    for (i = 0; i < m->msg_count; i++) {
        if (strcmp(m->msg[i].text, text) == 0) {
            return &m->msg[i];
        }
    }
    return NULL;
}

/* ---- the words a message's delivery is said in ------------------------------ */

static void test_states(void)
{
    struct rift_message msg;
    char out[RIFT_MSG_CAPTION_MAX];

    memset(&msg, 0, sizeof(msg));
    msg.dir = RIFT_MSG_OUT;
    msg.ack_expected = 1;
    msg.state = RIFT_MSG_SENT_DIRECT;
    rift_fmt_msg_caption(&msg, out, sizeof(out));
    text_is("waiting for its ACK is SENT, never delivered", out, "SENT \xC2\xB7 DIRECT");
    msg.state = RIFT_MSG_ACKED;
    msg.have_mono = 1;
    msg.mono_ms = 1000;
    msg.have_ack_mono = 1;
    msg.ack_mono_ms = 4000;
    rift_fmt_msg_caption(&msg, out, sizeof(out));
    text_is("an ACK is DELIVERED, with how long it took", out, "DELIVERED \xC2\xB7 ACK 3 s");
    msg.state = RIFT_MSG_NO_ACK;
    rift_fmt_msg_caption(&msg, out, sizeof(out));
    text_is("no ACK by the deadline is NO ACK", out, "NO ACK");
    check("and is drawn as a warning", rift_msg_is_warn(&msg));
    msg.attempts = 2;
    msg.state = RIFT_MSG_SENT_DIRECT;
    rift_fmt_msg_caption(&msg, out, sizeof(out));
    text_is("a resend says which attempt it is waiting on", out,
            "SENT \xC2\xB7 TRY 2 \xC2\xB7 DIRECT");
    msg.state = RIFT_MSG_ACKED;
    rift_fmt_msg_caption(&msg, out, sizeof(out));
    text_is("and which one was delivered", out, "DELIVERED \xC2\xB7 TRY 2 \xC2\xB7 ACK 3 s");
    msg.attempts = 0;
    msg.state = RIFT_MSG_SENT_DIRECT;
    msg.orphan = 1;
    rift_fmt_msg_caption(&msg, out, sizeof(out));
    text_is("an orphan is NO ACK whatever its last word was", out,
            "NO ACK \xC2\xB7 SERVICE RESTARTED");
    check("and is a warning", rift_msg_is_warn(&msg));
    check("and may be sent again", rift_model_can_resend(&msg));

    memset(&msg, 0, sizeof(msg));
    msg.dir = RIFT_MSG_OUT;
    msg.is_channel = 1;
    msg.state = RIFT_MSG_SENT_FLOOD;
    check("a channel message is never resent: nothing says it failed",
          !rift_model_can_resend(&msg));
    msg.is_channel = 0;
    msg.ack_expected = 1;
    msg.state = RIFT_MSG_ACKED;
    check("nor a delivered one", !rift_model_can_resend(&msg));
    msg.state = RIFT_MSG_SENT_FLOOD;
    check("nor one still waiting", !rift_model_can_resend(&msg));
    msg.state = RIFT_MSG_FAILED;
    check("a failed one may be", rift_model_can_resend(&msg));
    msg.dir = RIFT_MSG_IN;
    check("an incoming one may not", !rift_model_can_resend(&msg));
}

/* ---- ACKs, attempts, resends and the service restarting ---------------------- */

static void test_resend_and_orphans(void)
{
    struct rift_model m;
    const struct rift_message *x;

    rift_model_init(&m);
    give_status(&m, 600, 600000);
    apply_messages(&m, "{\"messages\":["
                       "{\"id\":1,\"direction\":\"out\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_A
                       "\",\"text\":\"delivered one\",\"state\":\"acked\",\"ack_expected\":true,"
                       "\"attempts\":1,\"mono_ms\":1000},"
                       "{\"id\":2,\"direction\":\"out\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_A
                       "\",\"text\":\"lost one \xF0\x9F\x91\x8D\",\"state\":\"no_ack\",\"ack_expected\":true,"
                       "\"attempts\":1,\"mono_ms\":2000},"
                       "{\"id\":3,\"direction\":\"out\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_B
                       "\",\"text\":\"waiting one\",\"state\":\"sent_direct\",\"ack_expected\":true,"
                       "\"attempts\":1,\"mono_ms\":3000},"
                       "{\"id\":4,\"direction\":\"in\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_B
                       "\",\"text\":\"theirs\",\"state\":\"received\",\"mono_ms\":3500},"
                       "{\"id\":5,\"direction\":\"out\",\"kind\":\"channel\",\"channel\":0,"
                       "\"channel_hash\":\"11\",\"channel_name\":\"Public\",\"sender_name\":\"K230-A\","
                       "\"text\":\"K230-A: to all\",\"state\":\"sent_flood\",\"ack_expected\":false,"
                       "\"mono_ms\":4000}"
                       "],\"count\":5,\"total\":5,\"persistent\":false}");
    x = rift_model_message(&m, 2);
    check("attempts are read from the service", x && x->attempts == 1);

    /* ---- a RESEND of a message the service still holds ---- */
    check("a delivered message is not taken for a resend", rift_model_resend_begin(&m, 1, 10) != 0);
    check("nor one still waiting", rift_model_resend_begin(&m, 3, 10) != 0);
    check("nor a channel message", rift_model_resend_begin(&m, 5, 10) != 0);
    check("a no-ACK one is", rift_model_resend_begin(&m, 2, 10) == 0);
    check("as a resend of that id, not a new message",
          m.outbox.active && m.outbox.resend_id == 2 && m.outbox.replaces_id == 0);
    text_is("with its own text, emoji and all", m.outbox.text, "lost one \xF0\x9F\x91\x8D");
    text_is("to its own peer", m.outbox.conv_key, KEY_A);
    check("one at a time", rift_model_resend_begin(&m, 2, 11) != 0 && rift_model_sending(&m));
    rift_model_send_accepted(&m, 2, "direct");
    check("accepted, the submission is done", !rift_model_sending(&m));
    check("and no second message appeared", m.msg_count == 5);
    /* The service's own word moves it on: same id, attempt 2. */
    apply_event(&m, "mesh.message",
                "{\"message\":{\"id\":2,\"direction\":\"out\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_A
                "\",\"text\":\"lost one \xF0\x9F\x91\x8D\",\"state\":\"sent_direct\",\"ack_expected\":true,"
                "\"attempts\":2,\"mono_ms\":2000}}");
    x = rift_model_message(&m, 2);
    check("the resent message is the same row, waiting again, attempt 2",
          m.msg_count == 5 && x && x->state == RIFT_MSG_SENT_DIRECT && x->attempts == 2);
    check("and may not be resent while it waits", !rift_model_can_resend(x));
    /* A late ACK after it had gone NO ACK again is still delivery. */
    apply_event(&m, "mesh.message",
                "{\"message\":{\"id\":2,\"direction\":\"out\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_A
                "\",\"text\":\"lost one \xF0\x9F\x91\x8D\",\"state\":\"acked\",\"ack_expected\":true,"
                "\"attempts\":2,\"mono_ms\":2000,\"ack_mono_ms\":9000}}");
    x = rift_model_message(&m, 2);
    check("delivered when the service says so, and only then",
          x && x->state == RIFT_MSG_ACKED && x->have_ack_mono);

    /* ---- the service restarts under an unacknowledged message ---- */
    apply_event(&m, "mesh.message",
                "{\"message\":{\"id\":6,\"direction\":\"out\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_B
                "\",\"text\":\"never answered\",\"state\":\"no_ack\",\"ack_expected\":true,"
                "\"attempts\":1,\"mono_ms\":5000}}");
    rift_model_service_lost(&m, "meshcored closed the connection");
    rift_model_service_found(&m);
    give_status(&m, 2, 612000);
    check("the restart is noticed", m.svc_restarts == 1);
    apply_messages(&m, "{\"messages\":["
                       "{\"id\":1,\"direction\":\"in\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_B
                       "\",\"text\":\"new run\",\"state\":\"received\",\"mono_ms\":611000}"
                       "],\"count\":1,\"total\":1,\"persistent\":false}");
    check("the new run's message is held, and nothing of the old run's but the orphans",
          by_text(&m, "new run") && !by_text(&m, "theirs") && !by_text(&m, "delivered one") &&
              !by_text(&m, "to all"));
    x = by_text(&m, "waiting one");
    check("a message still waiting for its ACK is kept as an orphan",
          x && x->orphan && x->id < 0);
    x = by_text(&m, "never answered");
    check("and so is one that had gone no ACK", x && x->orphan && x->id < 0);
    check("a delivered one is not kept", !by_text(&m, "lost one \xF0\x9F\x91\x8D"));
    check("orphans keep the order they were written in, before the new run",
          m.msg_count == 3 && strcmp(m.msg[0].text, "waiting one") == 0 &&
              strcmp(m.msg[1].text, "never answered") == 0 && m.msg[2].id == 1);
    check("counted apart from what was forgotten", m.msgs_orphaned == 2 && m.msgs_forgotten == 4);
    {
        const struct rift_message *t[8];
        struct rift_conv conv[8];
        int n = rift_model_thread(&m, KEY_B, t, 8, NULL);
        int c = rift_model_conversations(&m, conv, 8);

        check("the orphans are in their conversation, with the new run's message",
              n == 3 && t[0]->orphan && t[1]->orphan && !t[2]->orphan);
        check("and its tally counts them as no ACK", c == 1 && conv[0].no_ack == 2);
    }
    /* ---- an orphan sent again is a new message, and replaces itself ---- */
    {
        int64_t orphan_id = by_text(&m, "never answered")->id;

        check("an orphan is taken for a resend", rift_model_resend_begin(&m, orphan_id, 20) == 0);
        check("as a new message replacing it, since the service no longer holds it",
              m.outbox.replaces_id == orphan_id && m.outbox.resend_id == 0);
        rift_model_send_failed(&m, "the radio is not available");
        check("refused, the orphan and its words stay", by_text(&m, "never answered") != NULL);
        check("and it can be tried again", rift_model_resend_begin(&m, orphan_id, 21) == 0);
        rift_model_send_accepted(&m, 2, "flood");
        check("accepted, the orphan's row goes", !by_text(&m, "never answered"));
        apply_event(&m, "mesh.message",
                    "{\"message\":{\"id\":2,\"direction\":\"out\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_B
                    "\",\"text\":\"never answered\",\"state\":\"sent_flood\",\"ack_expected\":true,"
                    "\"attempts\":1,\"mono_ms\":613000}}");
        x = by_text(&m, "never answered");
        check("and the new message is the one shown, once", x && !x->orphan && x->id == 2);
    }
    /* ---- a second restart keeps the orphan that is left, and adds no copy ---- */
    give_status(&m, 1, 640000);
    apply_messages(&m, "{\"messages\":[],\"count\":0,\"total\":0,\"persistent\":false}");
    check("the orphan from before is kept again, and the new unanswered one joins it",
          by_text(&m, "waiting one") && by_text(&m, "never answered") && m.msg_count == 2 &&
              m.msg[0].id < m.msg[1].id && m.msg[1].id < 0);
    /* ---- a bounded number ---- */
    {
        int i;
        char json[512];

        for (i = 0; i < RIFT_MAX_ORPHANS + 4; i++) {
            snprintf(json, sizeof(json),
                     "{\"message\":{\"id\":%d,\"direction\":\"out\",\"kind\":\"direct\","
                     "\"peer_public_key\":\"" KEY_A "\",\"text\":\"o%d\",\"state\":\"no_ack\","
                     "\"ack_expected\":true,\"mono_ms\":%d}}",
                     i + 1, i, 650000 + i);
            apply_event(&m, "mesh.message", json);
        }
        give_status(&m, 1, 700000);
        apply_messages(&m, "{\"messages\":[],\"count\":0,\"total\":0,\"persistent\":false}");
        check("at most RIFT_MAX_ORPHANS are kept, the newest",
              m.msg_count == RIFT_MAX_ORPHANS && by_text(&m, "o19") && !by_text(&m, "o0"));
    }
}

/* ---- a reply, as text ---------------------------------------------------------- */

static void channel_msg(struct rift_message *msg, const char *sender, const char *text)
{
    memset(msg, 0, sizeof(*msg));
    msg->is_channel = 1;
    msg->dir = RIFT_MSG_IN;
    msg->have_sender_name = sender != NULL;
    snprintf(msg->sender_name, sizeof(msg->sender_name), "%s", sender ? sender : "");
    snprintf(msg->text, sizeof(msg->text), "%s: %s", sender ? sender : "x", text);
}

static void test_reply(void)
{
    struct rift_message msg;
    struct rift_reply r;
    char out[RIFT_REPLY_PREFIX_MAX];
    char whole[256];

    channel_msg(&msg, "Anna", "Are you coming up?");
    check("an incoming channel message with a sender can be replied to",
          rift_reply_possible(&msg));
    rift_reply_prefix(&msg, out, sizeof(out));
    text_is("the prefix names the sender and quotes the message", out,
            "@[Anna] \"Are you coming up?\" ");
    snprintf(whole, sizeof(whole), "%sYes, in 10 minutes \xF0\x9F\x91\x8D", out);
    check("what is sent reads back as a reply", rift_reply_parse(whole, &r));
    text_is("to Anna", r.name, "Anna");
    text_is("quoting her", r.quote, "Are you coming up?");
    text_is("answering with the emoji intact", r.rest, "Yes, in 10 minutes \xF0\x9F\x91\x8D");
    check("and the whole of it is plain text a channel takes",
          rift_utf8_valid(whole) && strlen(whole) <= 160 && !strchr(whole, '\n'));

    channel_msg(&msg, "Ola [RPT]", "a \"quoted\" word\nand a second line");
    rift_reply_prefix(&msg, out, sizeof(out));
    text_is("brackets leave the name and quotes become apostrophes, on one line", out,
            "@[Ola RPT] \"a 'quoted' word and a second\xE2\x80\xA6\" ");

#define FAMILY "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"
#define FLAG_NO "\xF0\x9F\x87\xB3\xF0\x9F\x87\xB4"
    channel_msg(&msg, "Kari", "ok " FAMILY FAMILY);
    rift_reply_prefix(&msg, out, sizeof(out));
    check("a long quotation is cut, never inside a character", rift_utf8_valid(out));
    text_is("nor inside an emoji sequence: the first family stays whole, the second goes", out,
            "@[Kari] \"ok " FAMILY "\xE2\x80\xA6\" ");
    /* One byte first, so a cut at the limit would fall between the two
     * halves of a flag. */
    channel_msg(&msg, "Kari", "x" FLAG_NO FLAG_NO FLAG_NO FLAG_NO FLAG_NO);
    rift_reply_prefix(&msg, out, sizeof(out));
    {
        /* Flags are pairs of regional indicators: the quotation holds an
         * even number of them. */
        const char *p = out;
        int ri = 0;

        while ((p = strstr(p, "\xF0\x9F\x87")) != NULL) {
            ri++;
            p += 4;
        }
        check("and no flag is split in half", ri == 6);
    }
    check("and the cut is said with an ellipsis", strstr(out, "\xE2\x80\xA6\"") != NULL);

    channel_msg(&msg, NULL, "no name");
    msg.have_sender_name = 0;
    check("a line with no claimed sender cannot be replied to", !rift_reply_possible(&msg) &&
                                                                    rift_reply_prefix(&msg, out, sizeof(out)) == 0);
    msg.is_channel = 0;
    msg.have_sender_name = 1;
    check("nor a direct message: its thread is the reply", !rift_reply_possible(&msg));

    check("a mention alone reads back, with no quotation",
          rift_reply_parse("@[Per] takk", &r) && strcmp(r.name, "Per") == 0 && r.quote[0] == '\0' &&
              strcmp(r.rest, "takk") == 0);
    check("ordinary text is not a reply", !rift_reply_parse("hello @[Per] there", &r) &&
                                              !rift_reply_parse("@[] x", &r) &&
                                              !rift_reply_parse("@[unclosed x", &r));
    check("an unclosed quotation is left in the answer",
          rift_reply_parse("@[Per] \"half quote", &r) && r.quote[0] == '\0' &&
              strcmp(r.rest, "\"half quote") == 0);
}

/* ---- contacts --------------------------------------------------------------------- */

static void add_node(struct rift_model *m, const char *key, const char *name, int type,
                     int heard)
{
    char json[512];
    cJSON *o;

    snprintf(json, sizeof(json),
             "{\"node\":{\"public_key\":\"%s\",\"node_hash\":\"%.2s\",\"name\":\"%s\",\"type\":%d,"
             "\"path_known\":false%s},\"reason\":\"discovered\"}",
             key, key, name, type, heard ? ",\"last_heard_mono_ms\":1000" : "");
    o = cJSON_Parse(json);
    rift_model_apply_event(m, "mesh.node", o);
    cJSON_Delete(o);
}

static void test_contacts(void)
{
    static struct rift_model m;
    const struct rift_node *out[RIFT_MAX_NODES];
    int n;

    rift_model_init(&m);
    add_node(&m, KEY_B, "oslo-01", 1, 1);
    add_node(&m, KEY_A, "Anna", 1, 1);
    add_node(&m, KEY_R, "HYTTA RPT", 2, 1);
    n = rift_contacts_list(&m, RIFT_CONTACTS_ALL, "", out, RIFT_MAX_NODES);
    check("ALL is every stored contact", n == 3);
    check("A to Z, case not counted", n == 3 && strcmp(out[0]->name, "Anna") == 0 &&
                                          strcmp(out[1]->name, "HYTTA RPT") == 0 &&
                                          strcmp(out[2]->name, "oslo-01") == 0);
    n = rift_contacts_list(&m, RIFT_CONTACTS_ALL, "OSLO", out, RIFT_MAX_NODES);
    check("search by name", n == 1 && strcmp(out[0]->key, KEY_B) == 0);
    n = rift_contacts_list(&m, RIFT_CONTACTS_ALL, "cc33", out, RIFT_MAX_NODES);
    check("search by public key prefix", n == 1 && strcmp(out[0]->key, KEY_R) == 0);
    n = rift_contacts_list(&m, RIFT_CONTACTS_ALL, "aa", out, RIFT_MAX_NODES);
    check("the node hash is a key prefix", n == 1 && strcmp(out[0]->key, KEY_A) == 0);
    n = rift_contacts_list(&m, RIFT_CONTACTS_ALL, "  ", out, RIFT_MAX_NODES);
    check("an empty search is everything", n == 3);
    n = rift_contacts_list(&m, RIFT_CONTACTS_ALL, "zzz", out, RIFT_MAX_NODES);
    check("a search nothing answers is nothing", n == 0);
    n = rift_contacts_list(&m, RIFT_CONTACTS_ALL, "hytta", out, RIFT_MAX_NODES);
    check("a repeater is a contact, and stays a repeater",
          n == 1 && rift_node_is_repeater(out[0]) && !rift_node_can_message(out[0]) &&
              rift_node_no_message_why(out[0]) != NULL);

    n = rift_contacts_list(&m, RIFT_CONTACTS_RECENT, "", out, RIFT_MAX_NODES);
    check("RECENT is empty with no conversation", n == 0);
    apply_messages(&m, "{\"messages\":["
                       "{\"id\":1,\"direction\":\"in\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_A
                       "\",\"text\":\"first\",\"state\":\"received\",\"mono_ms\":1000},"
                       "{\"id\":2,\"direction\":\"out\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_B
                       "\",\"text\":\"second\",\"state\":\"acked\",\"mono_ms\":2000},"
                       "{\"id\":3,\"direction\":\"in\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_A
                       "\",\"text\":\"third\",\"state\":\"received\",\"mono_ms\":3000},"
                       "{\"id\":4,\"direction\":\"in\",\"kind\":\"direct\",\"peer_public_key\":\"dd"
                       "44444444444444444444444444444444444444444444444444444444444444"
                       "\",\"text\":\"a stranger\",\"state\":\"received\",\"mono_ms\":4000}"
                       "],\"count\":4,\"total\":4,\"persistent\":false}");
    n = rift_contacts_list(&m, RIFT_CONTACTS_RECENT, "", out, RIFT_MAX_NODES);
    check("RECENT is the contacts spoken with, the latest first, each once",
          n == 2 && strcmp(out[0]->key, KEY_A) == 0 && strcmp(out[1]->key, KEY_B) == 0);
    n = rift_contacts_list(&m, RIFT_CONTACTS_RECENT, "oslo", out, RIFT_MAX_NODES);
    check("and is searched the same way", n == 1 && strcmp(out[0]->key, KEY_B) == 0);
    check("nothing about listing changed the table", m.node_count == 3);

    /* ---- at capacity: the whole table, and how long a search takes ---- */
    {
        char key[65];
        char name[32];
        struct timespec t0;
        struct timespec t1;
        double ms;
        int i;

        rift_model_init(&m);
        for (i = 0; i < RIFT_MAX_NODES; i++) {
            snprintf(key, sizeof(key), "%08x%056d", (unsigned)(i * 2654435761u), 0);
            snprintf(name, sizeof(name), "node-%04d", (i * 7919) % RIFT_MAX_NODES);
            add_node(&m, key, name, (i % 10) == 0 ? 2 : 1, 1);
        }
        check("the cache holds the service's 1000", m.node_count == RIFT_MAX_NODES);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        n = rift_contacts_list(&m, RIFT_CONTACTS_ALL, "", out, RIFT_MAX_NODES);
        n = rift_contacts_list(&m, RIFT_CONTACTS_ALL, "node-09", out, RIFT_MAX_NODES);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        printf("     1000 contacts: sorted list and a search together took %.2f ms\n", ms);
        check("a search near capacity finds every match", n == 100);
        n = rift_contacts_list(&m, RIFT_CONTACTS_ALL, "", out, RIFT_MAX_NODES);
        check("ALL lists all 1000, in order",
              n == RIFT_MAX_NODES && strcmp(out[0]->name, "node-0000") == 0 &&
                  strcmp(out[RIFT_MAX_NODES - 1]->name, "node-0999") == 0);
        n = rift_contacts_list(&m, RIFT_CONTACTS_ALL, "", out, 10);
        check("and never writes past what it was given", n == 10);
    }
}

int main(void)
{
    test_states();
    test_resend_and_orphans();
    test_reply();
    test_contacts();
    printf("rift_reliability_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
