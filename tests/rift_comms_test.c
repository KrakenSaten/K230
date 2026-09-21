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
        check("no name is invented", rift_model_conv_name(&s, KEY_B) == NULL);
        n = rift_model_conversations(&s, conv, RIFT_MAX_CONVERSATIONS);
        check("and the conversation says it has none", n == 1 && !conv[0].have_name);
    }

    /* ---- channels ---------------------------------------------------------
     *
     * A channel is a conversation like any other here - one list, one
     * thread, one read mark - and the thing these cases pin down is where it
     * is NOT like any other: nothing acknowledges it, nobody is named by a
     * key, and a slot is not a public key.
     */
    {
        struct rift_model s;
        const char *nm;

        rift_model_init(&s);

        /* The key, and its inverse. These two are what let a channel be an
         * ordinary conversation everywhere else. */
        {
            char key[RIFT_KEY_HEX];

            rift_channel_key(0, key, sizeof(key));
            text_is("a channel's conversation key is its slot", key, "#0");
            rift_channel_key(3, key, sizeof(key));
            text_is("and so is any other slot's", key, "#3");
            rift_channel_key(RIFT_MAX_CHANNELS, key, sizeof(key));
            text_is("a slot outside the table has no key", key, "");
            check("a channel key reads back as its slot", rift_key_is_channel("#3") == 3);
            check("slot zero is a slot, not an absence", rift_key_is_channel("#0") == 0);
            check("a public key is not a channel", rift_key_is_channel(KEY_A) == -1);
            check("nor is a bare hash", rift_key_is_channel("#") == -1);
            check("nor is a slot that is not a number", rift_key_is_channel("#x") == -1);
            check("nor one past the table", rift_key_is_channel("#9") == -1);
            check("and neither is nothing at all", rift_key_is_channel("") == -1);
        }

        /* The list, from mesh.channels. */
        {
            cJSON *o = cJSON_Parse("{\"channels\":[{\"channel\":0,\"name\":\"SITE\","
                                   "\"channel_hash\":\"8c\",\"key_bits\":256,"
                                   "\"text_limit\":147,\"ack_expected\":false},"
                                   "{\"channel\":2,\"name\":\"OPS\",\"channel_hash\":\"4d\","
                                   "\"key_bits\":128,\"text_limit\":147}],"
                                   "\"count\":2,\"max\":8,\"persistent\":true}");

            check("the channel list is taken", rift_model_apply_channels(&s, o) == 0);
            cJSON_Delete(o);
        }
        check("both channels are held", s.channel_count == 2);
        check("and the list is valid", s.channels_valid);
        check("the first is in the slot it said", s.channels[0].slot == 0);
        text_is("with its name", s.channels[0].name, "SITE");
        text_is("and the hash that goes on the air", s.channels[0].hash, "8c");
        check("and its key length", s.channels[0].have_key_bits && s.channels[0].key_bits == 256);
        /* The slot is the identity, not the position: the second channel is
         * in slot 2 because slot 1 is empty. */
        check("the second keeps its own slot", s.channels[1].slot == 2);
        check("a channel can be found by slot", rift_model_channel(&s, 2) != NULL);
        check("and an empty slot cannot", rift_model_channel(&s, 1) == NULL);
        check("nor one outside the table", rift_model_channel(&s, 99) == NULL);
        /* No key is reported by the API and none is held here. */
        check("the text limit is the service's, not 160",
              rift_model_text_limit(&s, "#0") == 147);
        check("and a direct conversation keeps the API's own limit",
              rift_model_text_limit(&s, KEY_A) == RIFT_SEND_TEXT_MAX);

        /* A message on the channel. */
        check("a channel message is taken",
              apply_message(&s, "{\"id\":10,\"direction\":\"in\",\"kind\":\"channel\","
                                "\"channel\":0,\"channel_name\":\"SITE\","
                                "\"channel_hash\":\"8c\",\"sender_name\":\"HYTTA\","
                                "\"text\":\"HYTTA: all clear\",\"state\":\"received\","
                                "\"ack_expected\":false,\"mono_ms\":100}") == 0);
        check("it is marked as a channel message", s.msg[0].is_channel);
        check("in the slot it named", s.msg[0].channel_slot == 0);
        text_is("its conversation key is that slot's", s.msg[0].conv_key, "#0");
        text_is("it carries no peer key at all", s.msg[0].peer_key, "");
        check("and no peer name", !s.msg[0].have_peer_name);
        text_is("the sender's claimed name is kept as a claim", s.msg[0].sender_name, "HYTTA");
        /* The whole payload, prefix and all: the service does not cut it and
         * neither does this. */
        text_is("and the text is the whole payload", s.msg[0].text, "HYTTA: all clear");
        check("nothing will acknowledge it", !s.msg[0].ack_expected);

        /* A channel message with a slot this build cannot hold is refused
         * whole, rather than shown in a conversation nothing can open. */
        check("a channel message in an impossible slot is refused",
              apply_message(&s, "{\"id\":11,\"direction\":\"in\",\"kind\":\"channel\","
                                "\"channel\":99,\"text\":\"x\",\"state\":\"received\"}") == -1);
        check("and so is one with no slot at all",
              apply_message(&s, "{\"id\":12,\"direction\":\"in\",\"kind\":\"channel\","
                                "\"text\":\"x\",\"state\":\"received\"}") == -1);
        check("a direct message still needs a peer key",
              apply_message(&s, "{\"id\":13,\"direction\":\"in\",\"text\":\"x\","
                                "\"state\":\"received\"}") == -1);
        check("only the one message was held", s.msg_count == 1);

        /* The conversation. */
        n = rift_model_conversations(&s, conv, RIFT_MAX_CONVERSATIONS);
        check("the channel is one conversation", n == 1);
        text_is("keyed by its slot", conv[0].key, "#0");
        check("and marked as a channel", conv[0].is_channel && conv[0].channel_slot == 0);
        text_is("named by this node's own name for it", conv[0].name, "SITE");
        check("with one message in it", conv[0].total == 1);
        /* A channel message that arrived after this app opened is unread the
         * same way a direct one is. */
        check("and one unread", conv[0].unread == 1);
        check("which the total agrees with", rift_model_unread_total(&s) == 1);

        /* The thread. */
        {
            int nn = rift_model_thread(&s, "#0", thread, RIFT_MAX_MESSAGES, &older);

            check("the thread holds it", nn == 1 && older == 0);
            check("and it is the message that was sent", nn == 1 && thread[0]->id == 10);
            /* The public key of a real peer is not this channel. */
            nn = rift_model_thread(&s, KEY_A, thread, RIFT_MAX_MESSAGES, &older);
            check("a peer's thread does not pick up channel messages", nn == 0);
        }

        /* Reading it clears the badge, and only for that conversation. */
        check("marking it read clears one", rift_model_mark_read(&s, "#0") == 1);
        check("and it is read now", rift_model_unread(&s, "#0") == 0);
        check("a second message is unread again",
              apply_message(&s, "{\"id\":14,\"direction\":\"in\",\"kind\":\"channel\","
                                "\"channel\":0,\"text\":\"K230-A: hi\","
                                "\"sender_name\":\"K230-A\",\"state\":\"received\","
                                "\"ack_expected\":false,\"mono_ms\":200}") == 0 &&
                  rift_model_unread(&s, "#0") == 1);

        /* Two conversations at once: a channel and a peer, kept apart. */
        check("a direct message arrives too",
              apply_message(&s, "{\"id\":15,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A
                                "\",\"peer_name\":\"HYTTA\",\"text\":\"direct\","
                                "\"state\":\"received\",\"ack_expected\":false,"
                                "\"mono_ms\":300}") == 0);
        n = rift_model_conversations(&s, conv, RIFT_MAX_CONVERSATIONS);
        check("there are two conversations now", n == 2);
        check("the newest first, which is the direct one",
              n == 2 && strcmp(conv[0].key, KEY_A) == 0 && !conv[0].is_channel);
        check("and the channel second", n == 2 && conv[1].is_channel);
        check("reading the channel leaves the direct one unread",
              rift_model_mark_read(&s, "#0") == 1 && rift_model_unread(&s, KEY_A) == 1);

        /* The tally: nothing on a channel can be delivered or time out. */
        check("an outgoing channel message is taken",
              apply_message(&s, "{\"id\":16,\"direction\":\"out\",\"kind\":\"channel\","
                                "\"channel\":0,\"text\":\"K230-A: sending\","
                                "\"sender_name\":\"K230-A\",\"state\":\"sent_flood\","
                                "\"ack_expected\":false,\"mono_ms\":400}") == 0);
        n = rift_model_conversations(&s, conv, RIFT_MAX_CONVERSATIONS);
        {
            int at = -1;
            int i;

            for (i = 0; i < n; i++) {
                if (conv[i].is_channel) {
                    at = i;
                }
            }
            check("the channel counts it as sent", at >= 0 && conv[at].outgoing == 1);
            /* Not in acked and not in no_ack: neither is a number this
             * protocol can produce, and putting it in no_ack would report a
             * failure nothing promised to avoid. */
            check("but not as delivered", at >= 0 && conv[at].acked == 0);
            check("and not as having timed out", at >= 0 && conv[at].no_ack == 0);
            check("it is counted where it belongs",
                  at >= 0 && conv[at].unacknowledgeable == 1);
        }
        /* And the caption says so rather than leaving a permanent "SENT". */
        {
            const struct rift_message *out = NULL;
            int i;

            for (i = 0; i < s.msg_count; i++) {
                if (s.msg[i].id == 16) {
                    out = &s.msg[i];
                }
            }
            check("the outgoing channel message is held", out != NULL);
            if (out) {
                rift_fmt_msg_caption(out, text, sizeof(text));
                check("and its caption says nothing acknowledges it",
                      strstr(text, "NO ACK ON CHANNELS") != NULL);
                /* DELIVERED is the word this app prints for an acknowledgement
                 * (rift_fmt_msg_state), and it must never appear on a
                 * message the protocol cannot acknowledge. */
                check("without claiming it was delivered",
                      strstr(text, "DELIVERED") == NULL);
                check("and it reads as sent and no more", strncmp(text, "SENT", 4) == 0);
            }
        }

        /* Sending. */
        check("a channel is a destination this app will send to",
              rift_model_send_begin(&s, "#0", "hello", 1000) == 0);
        rift_model_send_clear(&s);
        check("a slot with no channel in it is still a destination it can address",
              rift_model_send_begin(&s, "#1", "hello", 1000) == 0);
        rift_model_send_clear(&s);
        check("but a key that is neither is refused",
              rift_model_send_begin(&s, "not-a-key", "hello", 1000) == -1);
        rift_model_send_clear(&s);
        {
            char too_long[200];

            memset(too_long, 'x', sizeof(too_long));
            too_long[147] = '\0';
            check("a body of exactly the channel's limit is taken",
                  rift_model_send_begin(&s, "#0", too_long, 1000) == 0);
            rift_model_send_clear(&s);
            too_long[147] = 'x';
            too_long[148] = '\0';
            /* Refused here rather than written and refused by the service:
             * the limit is shorter on a channel because this node's name
             * travels inside the payload. */
            check("and one byte more is refused before anything is written",
                  rift_model_send_begin(&s, "#0", too_long, 1000) == -1);
            rift_model_send_clear(&s);
        }

        /* Events: a channel added and one removed. */
        check("an added channel arrives as an event",
              apply_event(&s, "mesh.channel",
                          "{\"reason\":\"added\",\"channel\":{\"channel\":4,"
                          "\"name\":\"NEW\",\"channel_hash\":\"11\",\"key_bits\":256,"
                          "\"text_limit\":147}}") == 0);
        check("and is in the list", rift_model_channel(&s, 4) != NULL);
        check("a second copy of it updates rather than doubling",
              apply_event(&s, "mesh.channel",
                          "{\"reason\":\"added\",\"channel\":{\"channel\":4,"
                          "\"name\":\"RENAMED\",\"channel_hash\":\"11\"}}") == 0 &&
                  s.channel_count == 3);
        nm = rift_model_conv_name(&s, "#4");
        check("under its new name", nm && strcmp(nm, "RENAMED") == 0);
        check("a removal takes it out",
              apply_event(&s, "mesh.channel",
                          "{\"reason\":\"removed\",\"channel\":{\"channel\":4}}") == 0 &&
                  rift_model_channel(&s, 4) == NULL);
        check("and leaves the others where they were",
              s.channel_count == 2 && rift_model_channel(&s, 0) != NULL &&
                  rift_model_channel(&s, 2) != NULL);
        check("removing one that is not held changes nothing",
              apply_event(&s, "mesh.channel",
                          "{\"reason\":\"removed\",\"channel\":{\"channel\":5}}") == 0 &&
                  s.channel_count == 2);
        check("a malformed channel event is refused and counted",
              apply_event(&s, "mesh.channel", "{\"reason\":\"added\"}") == -1);
        check("and so is one with an impossible slot",
              apply_event(&s, "mesh.channel",
                          "{\"reason\":\"added\",\"channel\":{\"channel\":99}}") == -1);

        /* Leaving a channel does not erase what was said on it. */
        check("the messages on a channel outlive leaving it",
              rift_model_thread(&s, "#0", thread, RIFT_MAX_MESSAGES, &older) > 0);
        check("and a name is still found for them",
              rift_model_conv_name(&s, "#0") != NULL);
        {
            /* Even after the channel itself is gone: the name falls back to
             * what the messages carried. */
            apply_event(&s, "mesh.channel",
                        "{\"reason\":\"removed\",\"channel\":{\"channel\":0}}");
            nm = rift_model_conv_name(&s, "#0");
            check("a left channel is still named by its messages",
                  nm && strcmp(nm, "SITE") == 0);
            check("but it is no longer in the list", rift_model_channel(&s, 0) == NULL);
        }

        /* Reconnect: the channel list is re-read, and stops being valid
         * while the service is away. */
        rift_model_service_lost(&s, "gone");
        check("the channels stop being current when the service goes",
              !s.channels_valid);
        check("but they are still shown rather than blanked", s.channel_count == 1);
        rift_model_service_found(&s);
        check("and are still not current until they are re-read", !s.channels_valid);
        {
            cJSON *o = cJSON_Parse("{\"channels\":[{\"channel\":7,\"name\":\"AFTER\","
                                   "\"channel_hash\":\"ff\",\"key_bits\":256,"
                                   "\"text_limit\":140}],\"count\":1,\"max\":8}");

            check("a fresh snapshot replaces the list",
                  rift_model_apply_channels(&s, o) == 0 && s.channel_count == 1 &&
                      s.channels[0].slot == 7);
            cJSON_Delete(o);
        }
        check("and it is current again", s.channels_valid);
        /* A channel the service no longer holds is gone from the list, so
         * nothing offers a reader somewhere to write that nothing carries. */
        check("the channel that was not in it is gone", rift_model_channel(&s, 2) == NULL);

        /* A channel fault, which is not a node fault. */
        {
            cJSON *o = cJSON_Parse("{\"state\":\"online\",\"reason\":\"\","
                                   "\"channel_fault\":\"channels.v1 is version 9 "
                                   "(kept as channels.v1.corrupt.0)\"}");

            check("a status with a channel fault is taken",
                  rift_model_apply_status(&s, o) == 0);
            cJSON_Delete(o);
        }
        check("the channel fault is reported", s.have_channel_fault);
        check("and is not the node table's", !s.have_state_fault);
    }

    printf("rift_comms_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
