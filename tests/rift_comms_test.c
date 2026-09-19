/*
 * COMMS without a screen: the conversations, the unread count, the send
 * state machine, and every string a message prints.
 *
 * The failures worth testing here are the ones that would make a chat lie.
 * A message shown as delivered before anything acknowledged it. The same
 * message twice because the service said its state changed. An unread badge
 * that counts what was already read, or that never clears. A conversation
 * whose preview is the wrong message because the list was sorted on a field
 * the service did not fill. A body cut in the middle of a UTF-8 character
 * because a remote node chose a longer name than the field that holds it.
 *
 * No LVGL, no socket, no service.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_format.h"
#include "rift_model.h"

#include <stdio.h>
#include <string.h>

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

static int apply_message(struct rift_model *m, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_message(m, o);

    cJSON_Delete(o);
    return rc;
}

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

int main(void)
{
    struct rift_model m;
    struct rift_conv conv[RIFT_MAX_CONVERSATIONS];
    const struct rift_message *thread[RIFT_MAX_MESSAGES];
    char text[RIFT_MSG_CAPTION_MAX];
    int older = 0;
    int n;

    /* ---- a snapshot is history, not news -------------------------------- */
    rift_model_init(&m);
    check("no messages before the service has answered", m.msg_count == 0);
    check("and the thread is not valid yet", !m.messages_valid);

    check("a snapshot is taken",
          apply_messages(&m,
                         "{\"messages\":["
                         "{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
                         "\"peer_name\":\"HYTTA\",\"text\":\"first\",\"state\":\"received\","
                         "\"mono_ms\":1000},"
                         "{\"id\":2,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A "\","
                         "\"text\":\"second\",\"state\":\"acked\",\"mono_ms\":2000,"
                         "\"ack_mono_ms\":43000}"
                         "],\"count\":2,\"total\":2,\"persistent\":false}") == 0);
    check("both messages are held", m.msg_count == 2);
    check("the thread is valid now", m.messages_valid);
    check("and the service says its store does not survive a restart", !m.messages_persistent);
    /* The first snapshot of a session is what happened before this app was
     * watching. Counting it as unread would put a badge on every
     * conversation every time RIFT opens. */
    check("nothing from the first snapshot is unread", rift_model_unread_total(&m) == 0);

    /* ---- one conversation, with its preview and its tally ---------------- */
    n = rift_model_conversations(&m, conv, RIFT_MAX_CONVERSATIONS);
    check("one peer, one conversation", n == 1);
    text_is("named by what the service called it", conv[0].name, "HYTTA");
    check("holding both messages", conv[0].total == 2);
    check("one of them ours", conv[0].outgoing == 1);
    check("and that one acknowledged", conv[0].acked == 1);
    check("the preview is the newest message", conv[0].newest && conv[0].newest->id == 2);
    rift_fmt_preview(conv[0].newest, text, sizeof(text));
    text_is("and says who said it", text, "you: second");

    /* ---- an event for a message already held updates it ------------------ */
    check("the same message again, with a new state",
          apply_event(&m, "mesh.message",
                      "{\"message\":{\"id\":2,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A
                      "\",\"text\":\"second\",\"state\":\"no_ack\",\"mono_ms\":2000}}") == 0);
    check("does not add a second copy", m.msg_count == 2);
    check("and is counted as the duplicate it is", m.msgs_duplicate == 1);
    n = rift_model_conversations(&m, conv, RIFT_MAX_CONVERSATIONS);
    check("the tally follows the state", conv[0].acked == 0 && conv[0].no_ack == 1);

    /* ---- a message that arrives while watching IS unread ----------------- */
    check("a new message arrives",
          apply_event(&m, "mesh.message",
                      "{\"message\":{\"id\":3,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A
                      "\",\"peer_name\":\"HYTTA\",\"text\":\"third\",\"state\":\"received\","
                      "\"mono_ms\":3000}}") == 0);
    check("and is unread", rift_model_unread(&m, KEY_A) == 1);
    check("which the total agrees with", rift_model_unread_total(&m) == 1);
    check("reading the conversation clears it", rift_model_mark_read(&m, KEY_A) == 1);
    check("and it stays clear", rift_model_unread(&m, KEY_A) == 0);
    check("marking a conversation twice clears nothing the second time",
          rift_model_mark_read(&m, KEY_A) == 0);
    /* The mark is an id, not a count: a message that arrives after the
     * screen was drawn has not been drawn. */
    check("a message arriving after the mark is still unread",
          apply_event(&m, "mesh.message",
                      "{\"message\":{\"id\":4,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A
                      "\",\"text\":\"fourth\",\"state\":\"received\",\"mono_ms\":4000}}") == 0 &&
              rift_model_unread(&m, KEY_A) == 1);
    /* Our own messages are never unread: we wrote them. */
    check("an outgoing message is never unread",
          apply_event(&m, "mesh.message",
                      "{\"message\":{\"id\":5,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A
                      "\",\"text\":\"mine\",\"state\":\"sent_flood\",\"mono_ms\":5000}}") == 0 &&
              rift_model_unread(&m, KEY_A) == 1);

    /* ---- a second conversation sorts by its newest ----------------------- */
    check("a message from someone else",
          apply_event(&m, "mesh.message",
                      "{\"message\":{\"id\":6,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B
                      "\",\"peer_name\":\"OSLO-01\",\"text\":\"hei\",\"state\":\"received\","
                      "\"mono_ms\":9000}}") == 0);
    n = rift_model_conversations(&m, conv, RIFT_MAX_CONVERSATIONS);
    check("two conversations now", n == 2);
    text_is("the one with the newest message is first", conv[0].name, "OSLO-01");
    check("and only it is unread", conv[0].unread == 1);
    check("the other keeps its own count", conv[1].unread == 1);

    /* ---- a thread is one peer's messages, oldest first -------------------- */
    n = rift_model_thread(&m, KEY_A, thread, RIFT_MAX_MESSAGES, &older);
    check("the thread holds only that peer's", n == 5);
    check("with nothing left off the front", older == 0);
    check("oldest first", thread[0]->id == 1 && thread[4]->id == 5);
    n = rift_model_thread(&m, KEY_A, thread, 2, &older);
    check("a window shows the newest that fit", n == 2 && thread[0]->id == 4 &&
                                                    thread[1]->id == 5);
    check("and says how many are earlier", older == 3);

    /* ---- what this model will not take ------------------------------------ */
    {
        unsigned before = m.events_malformed;

        check("a message with no id is refused",
              apply_message(&m, "{\"direction\":\"in\",\"peer_public_key\":\"" KEY_A
                                "\",\"text\":\"x\",\"state\":\"received\"}") == -1);
        check("a message with no peer is refused",
              apply_message(&m, "{\"id\":90,\"direction\":\"in\",\"text\":\"x\"}") == -1);
        check("a peer key that is not 64 hex is refused",
              apply_message(&m, "{\"id\":91,\"direction\":\"in\",\"peer_public_key\":\"abcd\","
                                "\"text\":\"x\"}") == -1);
        check("a direction that is neither in nor out is refused",
              apply_message(&m, "{\"id\":92,\"direction\":\"sideways\",\"peer_public_key\":\""
                                KEY_A "\",\"text\":\"x\"}") == -1);
        check("a message with no text is refused",
              apply_message(&m, "{\"id\":93,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A
                                "\"}") == -1);
        check("and none of them was half-applied", m.msg_count == 6);
        check("a malformed event is counted",
              apply_event(&m, "mesh.message", "{\"message\":{\"id\":94}}") == -1 &&
                  m.events_malformed == before + 1);
    }

    /* ---- the states, and the words for them -------------------------------- */
    {
        struct rift_model s;
        const struct rift_message *one[4];

        rift_model_init(&s);
        apply_messages(&s, "{\"messages\":[],\"count\":0,\"total\":0,\"persistent\":false}");
        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":1,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A
                    "\",\"text\":\"a\",\"state\":\"sent_flood\",\"mono_ms\":1000}}");
        rift_model_thread(&s, KEY_A, one, 4, NULL);
        rift_fmt_msg_caption(one[0], text, sizeof(text));
        text_is("submitted is SENT and the route, never delivered", text,
                "SENT \xC2\xB7 FLOOD");
        check("and is not a warning", !rift_msg_is_warn(one[0]));

        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":1,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A
                    "\",\"text\":\"a\",\"state\":\"acked\",\"mono_ms\":1000,"
                    "\"ack_mono_ms\":42000}}");
        rift_model_thread(&s, KEY_A, one, 4, NULL);
        rift_fmt_msg_caption(one[0], text, sizeof(text));
        text_is("an ACK makes it delivered, with how long it took", text,
                "DELIVERED \xC2\xB7 ACK 41 s");

        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":1,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A
                    "\",\"text\":\"a\",\"state\":\"no_ack\",\"mono_ms\":1000}}");
        rift_model_thread(&s, KEY_A, one, 4, NULL);
        rift_fmt_msg_caption(one[0], text, sizeof(text));
        text_is("a timeout says so", text, "NO ACK");
        check("and is a warning", rift_msg_is_warn(one[0]));

        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":1,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A
                    "\",\"text\":\"a\",\"state\":\"failed\",\"mono_ms\":1000}}");
        rift_model_thread(&s, KEY_A, one, 4, NULL);
        rift_fmt_msg_caption(one[0], text, sizeof(text));
        text_is("a failure says so", text, "FAILED");

        /* A v0 API may grow a state. The word is shown rather than mapped
         * onto the nearest one this build happens to have. */
        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":1,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A
                    "\",\"text\":\"a\",\"state\":\"quantum\",\"mono_ms\":1000}}");
        rift_model_thread(&s, KEY_A, one, 4, NULL);
        rift_fmt_msg_caption(one[0], text, sizeof(text));
        text_is("a state this build does not know is shown as the service's word", text,
                "quantum");
        check("and is treated as a warning, not as success", rift_msg_is_warn(one[0]));
    }

    /* ---- telemetry that was never measured --------------------------------- */
    {
        struct rift_model s;
        const struct rift_message *one[2];

        rift_model_init(&s);
        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A
                    "\",\"text\":\"quiet\",\"state\":\"received\",\"mono_ms\":1000}}");
        rift_model_thread(&s, KEY_A, one, 2, NULL);
        check("no RSSI was reported", !one[0]->have_rssi);
        check("and none was invented", !one[0]->have_snr);
        rift_fmt_msg_caption(one[0], text, sizeof(text));
        text_is("so the caption is the state alone, not a zero", text, "RECEIVED");

        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":2,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A
                    "\",\"text\":\"heard\",\"state\":\"received\",\"mono_ms\":2000,"
                    "\"rssi_dbm\":-88,\"snr_db\":7.25}}");
        rift_model_thread(&s, KEY_A, one, 2, NULL);
        rift_fmt_msg_caption(one[1], text, sizeof(text));
        text_is("and a measured one is printed with a real minus", text,
                "RECEIVED \xC2\xB7 \xE2\x88\x92" "88 dBm \xC2\xB7 SNR 7.3");

        /* An ACK before the message it answers can only be a fault in the
         * stamps, and is not printed as a negative duration. */
        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":3,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A
                    "\",\"text\":\"x\",\"state\":\"acked\",\"mono_ms\":5000,"
                    "\"ack_mono_ms\":1000}}");
        {
            const struct rift_message *three[3];

            rift_model_thread(&s, KEY_A, three, 3, NULL);
            rift_fmt_ack(three[2], text, sizeof(text));
            text_is("an ACK stamped before its message prints no duration", text, "");
        }
    }

    /* ---- the cache is bounded, and drops the oldest ------------------------- */
    {
        struct rift_model s;
        char json[256];
        int i;

        rift_model_init(&s);
        for (i = 1; i <= RIFT_MAX_MESSAGES + 10; i++) {
            snprintf(json, sizeof(json),
                     "{\"id\":%d,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A
                     "\",\"text\":\"m\",\"state\":\"received\",\"mono_ms\":%d}",
                     i, i * 10);
            apply_message(&s, json);
        }
        check("the cache does not grow past its bound", s.msg_count == RIFT_MAX_MESSAGES);
        check("and says how many it dropped", s.msgs_dropped == 10);
        n = rift_model_thread(&s, KEY_A, thread, RIFT_MAX_MESSAGES, &older);
        check("what is left is the newest", thread[n - 1]->id == RIFT_MAX_MESSAGES + 10);
        check("and the oldest held is not message 1", thread[0]->id == 11);
        /* A late message older than everything held is refused rather than
         * evicting a newer one to make room for it. */
        check("a message older than the whole window is refused",
              apply_message(&s, "{\"id\":2,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A
                                "\",\"text\":\"late\",\"state\":\"received\"}") == -1);
        check("and the window did not move", s.msg_count == RIFT_MAX_MESSAGES);
    }

    /* ---- sending ------------------------------------------------------------ */
    {
        struct rift_model s;

        rift_model_init(&s);
        check("nothing is in flight to begin with", !rift_model_sending(&s));
        check("a submission is taken", rift_model_send_begin(&s, KEY_A, "hei", 1000) == 0);
        check("and is in flight", rift_model_sending(&s));
        check("a second one is refused while the first is out",
              rift_model_send_begin(&s, KEY_A, "again", 1100) == -1);
        /* Accepting ends the submission and creates nothing: the message
         * arrives as mesh.message, keyed by the id, so one message is one
         * row however the reply and the event race. */
        rift_model_send_accepted(&s, 77, "flood");
        check("accepting ends the submission", !rift_model_sending(&s));
        check("and no message was invented on this side", s.msg_count == 0);
        check("the id is remembered", s.outbox.message_id == 77);

        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":77,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A
                    "\",\"text\":\"hei\",\"state\":\"sent_flood\",\"mono_ms\":1200}}");
        check("the service's copy is the only one", s.msg_count == 1);

        rift_model_send_clear(&s);
        check("a refusal is remembered as a failure", (rift_model_send_begin(&s, KEY_A, "x", 1) == 0));
        rift_model_send_failed(&s, "the radio is not available");
        check("which ends the submission", !rift_model_sending(&s));
        check("and says why", s.outbox.failed &&
                                  strcmp(s.outbox.error, "the radio is not available") == 0);
        rift_model_send_clear(&s);
        check("and clearing forgets it", !s.outbox.failed && !s.outbox.error[0]);

        /* A socket that goes away under a submission is the one case where
         * nothing on this side knows what happened, and it must say that
         * rather than pick an answer. */
        rift_model_send_begin(&s, KEY_A, "into the void", 2000);
        rift_model_service_lost(&s, "meshcored closed the connection");
        check("a lost connection ends the submission", !rift_model_sending(&s));
        check("as a failure", s.outbox.failed);
        check("that admits it does not know",
              strstr(s.outbox.error, "may or may not") != NULL);
        check("and the messages are kept, marked stale", s.stale && s.messages_valid == 0);

        check("a send to something that is not a key is refused",
              rift_model_send_begin(&s, "nothex", "x", 1) == -1);
        check("and a send with no text is refused", rift_model_send_begin(&s, KEY_A, "", 1) == -1);
    }

    /* ---- what mesh.send will take ------------------------------------------- */
    {
        char why[RIFT_TEXT_MAX];
        char long_text[RIFT_SEND_TEXT_MAX + 8];

        check("ordinary text is sendable", rift_send_text_check("hei pa deg", why, sizeof(why)) == 0);
        check("empty text is not", rift_send_text_check("", why, sizeof(why)) == -1);
        check("and says so", strstr(why, "Nothing to send") != NULL);
        check("newline and tab are allowed, as the API allows them",
              rift_send_text_check("two\nlines\there", why, sizeof(why)) == 0);
        check("an escape sequence is refused",
              rift_send_text_check("clear \x1b[2J now", why, sizeof(why)) == -1);
        check("and says why", strstr(why, "control character") != NULL);
        memset(long_text, 'x', sizeof(long_text) - 1);
        long_text[sizeof(long_text) - 1] = '\0';
        check("a body over 160 bytes is refused",
              rift_send_text_check(long_text, why, sizeof(why)) == -1);
        check("and counts the bytes, which is what the limit counts",
              strstr(why, "167 bytes") != NULL);
        /* The limit is bytes, not characters: five two-byte characters are
         * ten of the 160. */
        check("multi-byte characters count as their bytes",
              rift_send_text_bytes("\xC3\xA6\xC3\xB8\xC3\xA5") == 6);
    }

    /* ---- remote text this app did not choose the length of ------------------ */
    {
        struct rift_model s;
        const struct rift_message *one[1];
        /* A name longer than the field that holds it, ending in a two-byte
         * character right at the boundary. */
        const char *json =
            "{\"message\":{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
            "\"peer_name\":\"\xC3\xA6\xC3\xB8\xC3\xA5\xC3\xA6\xC3\xB8\xC3\xA5\xC3\xA6\xC3\xB8"
            "\xC3\xA5\xC3\xA6\xC3\xB8\xC3\xA5\xC3\xA6\xC3\xB8\xC3\xA5\xC3\xA6\xC3\xB8\xC3\xA5"
            "\xC3\xA6\xC3\xB8\xC3\xA5\xC3\xA6\xC3\xB8\xC3\xA5\xC3\xA6\xC3\xB8\xC3\xA5\","
            "\"text\":\"line one\\nline two\",\"state\":\"received\",\"mono_ms\":1}}";
        size_t i;
        int truncated_mid_char = 0;

        rift_model_init(&s);
        apply_event(&s, "mesh.message", json);
        rift_model_thread(&s, KEY_A, one, 1, NULL);
        check("a long remote name is kept, bounded", one[0]->have_peer_name);
        /* Every byte of the kept name must still be part of a whole UTF-8
         * character: a name cut mid-sequence is what makes a whole IPC frame
         * unreadable further down. */
        for (i = 0; one[0]->peer_name[i]; i++) {
            unsigned char c = (unsigned char)one[0]->peer_name[i];

            if ((c & 0xC0) == 0xC0 && !one[0]->peer_name[i + 1]) {
                truncated_mid_char = 1;
            }
        }
        check("and is not cut in the middle of a character", !truncated_mid_char);
        text_is("the body is kept exactly as it was said", one[0]->text, "line one\nline two");
        rift_fmt_preview(one[0], text, sizeof(text));
        check("but a preview is one line", strchr(text, '\n') == NULL);
        text_is("with the newline folded to a space", text, "line one line two");
    }

    /* ---- a peer with no name anywhere --------------------------------------- */
    {
        struct rift_model s;

        rift_model_init(&s);
        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B
                    "\",\"text\":\"x\",\"state\":\"received\",\"mono_ms\":1}}");
        check("no name is invented", rift_model_peer_name(&s, KEY_B) == NULL);
        n = rift_model_conversations(&s, conv, RIFT_MAX_CONVERSATIONS);
        check("and the conversation says it has none", n == 1 && !conv[0].have_name);
    }

    printf("rift_comms_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
