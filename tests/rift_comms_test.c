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
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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

/* A channel's conversation key, built the way the model builds it. A few
 * rotating buffers, so one check can name several. */
static const char *chan_key(int slot, const char *hash, const char *name)
{
    static char buf[6][RIFT_KEY_HEX];
    static int at;
    char *out = buf[at++ % 6];

    rift_channel_key(slot, hash, name, out, RIFT_KEY_HEX);
    return out;
}

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

/* One mesh.status, read at a stated moment on the shared monotonic clock.
 * The uptime and that moment are how the model tells one run of meshcored
 * from the next, which is what the message cache is keyed to. */
static void give_status(struct rift_model *m, int uptime_s, int64_t now_ms)
{
    char json[96];
    cJSON *o;

    snprintf(json, sizeof(json), "{\"state\":\"online\",\"reason\":\"receiving\","
                                 "\"uptime_s\":%d}", uptime_s);
    o = cJSON_Parse(json);
    rift_model_apply_status(m, o, now_ms);
    cJSON_Delete(o);
}

/* Is there a message holding this text? Used to ask whether something the
 * service has forgotten is still on this side. */
static int holds_text(const struct rift_model *m, const char *want)
{
    int i;

    for (i = 0; i < m->msg_count; i++) {
        if (strcmp(m->msg[i].text, want) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The message as a reader sees it: the body once, and one caption line.
 *
 * A channel payload is "<sender>: <body>" (MeshCore writes the name in), and
 * the thread names the claimed sender in its caption. Printing the payload
 * whole said the name twice, and put this node's own name in front of every
 * line the reader wrote on a channel. */
static const struct rift_message *held(const struct rift_model *m, int64_t id)
{
    int i;

    for (i = 0; i < m->msg_count; i++) {
        if (m->msg[i].id == id) {
            return &m->msg[i];
        }
    }
    return NULL;
}

/* ---- a slot reused by a different channel ------------------------------
 *
 * Leaving a channel empties its slot and keeps its messages; the next channel
 * added takes the lowest empty slot (docs/api/mesh.md). Keyed by slot alone,
 * the old channel's history showed under the new channel's name and a reply
 * written in it went to the new channel's audience. */
static int thread_len(const struct rift_model *m, const char *key)
{
    const struct rift_message *t[RIFT_MAX_MESSAGES];

    return rift_model_thread(m, key, t, RIFT_MAX_MESSAGES, NULL);
}

static int apply_channels_json(struct rift_model *m, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_channels(m, o);

    cJSON_Delete(o);
    return rc;
}

static int name_is(const char *got, const char *want)
{
    return got && strcmp(got, want) == 0;
}

#define MSG_ON(id, dir, hash, name, text, state)                                                   \
    "{\"id\":" #id ",\"direction\":\"" dir "\",\"kind\":\"channel\",\"channel\":0,"                \
    "\"channel_name\":\"" name "\",\"channel_hash\":\"" hash "\",\"sender_name\":\"X\","           \
    "\"text\":\"" text "\",\"state\":\"" state "\",\"ack_expected\":false,\"mono_ms\":" #id "00}"
#define MSG_DIRECT(id)                                                                             \
    "{\"id\":" #id ",\"direction\":\"in\",\"kind\":\"direct\",\"peer_public_key\":\"" KEY_A "\","  \
    "\"peer_name\":\"HYTTA\",\"text\":\"direct\",\"state\":\"received\","                          \
    "\"ack_expected\":false,\"mono_ms\":" #id "00}"

/* A channel message in a slot of the caller's choosing. */
#define MSG_CH(id, slot, hash, name)                                                               \
    "{\"id\":" #id ",\"direction\":\"in\",\"kind\":\"channel\",\"channel\":" #slot ","             \
    "\"channel_name\":\"" name "\",\"channel_hash\":\"" hash "\",\"sender_name\":\"X\","           \
    "\"text\":\"X: hello\",\"state\":\"received\",\"ack_expected\":false,\"mono_ms\":" #id "00}"

/* COMMS puts MeshCore's Public channel first, known by the service's word
 * about its key - never by a name, never by the one-byte hash - and moves
 * nothing else. */
static void test_public_first(void)
{
    static struct rift_model s;
    static struct rift_conv conv[RIFT_MAX_CONVERSATIONS];
    static struct rift_conv was[RIFT_MAX_CONVERSATIONS];
    char bench[RIFT_KEY_HEX];
    char pub[RIFT_KEY_HEX];
    char fake[RIFT_KEY_HEX];
    int n;
    int i;
    int hits;

    rift_channel_key(0, "9a", "#doorsbench", bench, sizeof(bench));
    rift_channel_key(1, "11", "torget", pub, sizeof(pub));
    rift_channel_key(2, "11", "Public", fake, sizeof(fake));
    rift_model_init(&s);
    /* Slot 1 holds the well-known key under a local name. Slot 2 is called
     * Public and even shares the hash byte, and is not it. */
    apply_channels_json(&s,
                        "{\"channels\":["
                        "{\"channel\":0,\"name\":\"#doorsbench\",\"channel_hash\":\"9a\","
                        "\"key_bits\":128,\"text_limit\":150},"
                        "{\"channel\":1,\"name\":\"torget\",\"channel_hash\":\"11\","
                        "\"key_bits\":128,\"well_known\":\"public\",\"text_limit\":150},"
                        "{\"channel\":2,\"name\":\"Public\",\"channel_hash\":\"11\","
                        "\"key_bits\":128,\"text_limit\":150}],\"count\":3,\"max\":8}");
    check("public first: the service's mark is taken for the channel that has it",
          rift_model_channel(&s, 1) && rift_model_channel(&s, 1)->is_public);
    check("public first: a channel named Public with the same hash byte is not it",
          rift_model_channel(&s, 2) && !rift_model_channel(&s, 2)->is_public &&
              !rift_model_channel(&s, 0)->is_public);
    /* Public spoke first, so it is the oldest conversation and sorts last. */
    check("public first: messages on all three and a direct one",
          apply_message(&s, MSG_CH(1, 1, "11", "torget")) == 0 &&
              apply_message(&s, MSG_CH(2, 2, "11", "Public")) == 0 &&
              apply_message(&s, MSG_CH(3, 0, "9a", "#doorsbench")) == 0 &&
              apply_message(&s, MSG_DIRECT(4)) == 0);
    n = rift_model_conversations(&s, conv, RIFT_MAX_CONVERSATIONS);
    check("public first: four conversations, newest first as before",
          n == 4 && strcmp(conv[0].key, KEY_A) == 0 && strcmp(conv[1].key, bench) == 0 &&
              strcmp(conv[2].key, fake) == 0 && strcmp(conv[3].key, pub) == 0);
    memcpy(was, conv, sizeof(conv[0]) * (size_t)n);
    check("public first: the Public row is found where the order had left it",
          rift_conv_public_first(&s, conv, n) == 3);
    check("public first: and is the first row now", strcmp(conv[0].key, pub) == 0);
    check("public first: the others keep the order they had",
          strcmp(conv[1].key, was[0].key) == 0 && strcmp(conv[2].key, was[1].key) == 0 &&
              strcmp(conv[3].key, was[2].key) == 0);
    check("public first: the row itself is unchanged - its unread, its newest message",
          conv[0].unread == was[3].unread && conv[0].newest == was[3].newest &&
              conv[0].total == was[3].total && conv[0].is_channel);
    hits = 0;
    for (i = 0; i < n; i++) {
        if (strcmp(conv[i].key, pub) == 0) {
            hits++;
        }
    }
    check("public first: there is one Public row, not two", hits == 1);
    check("public first: asked again, it is already first and nothing moves",
          rift_conv_public_first(&s, conv, n) == 0 && strcmp(conv[0].key, pub) == 0 &&
              strcmp(conv[1].key, was[0].key) == 0);

    /* The same node without the service's mark: nothing is guessed. */
    apply_channels_json(&s,
                        "{\"channels\":["
                        "{\"channel\":0,\"name\":\"#doorsbench\",\"channel_hash\":\"9a\","
                        "\"key_bits\":128,\"text_limit\":150},"
                        "{\"channel\":1,\"name\":\"torget\",\"channel_hash\":\"11\","
                        "\"key_bits\":128,\"text_limit\":150},"
                        "{\"channel\":2,\"name\":\"Public\",\"channel_hash\":\"11\","
                        "\"key_bits\":128,\"well_known\":\"somethingelse\",\"text_limit\":150}],"
                        "\"count\":3,\"max\":8}");
    n = rift_model_conversations(&s, conv, RIFT_MAX_CONVERSATIONS);
    memcpy(was, conv, sizeof(conv[0]) * (size_t)n);
    check("public first: with no mark from the service no row is moved",
          rift_conv_public_first(&s, conv, n) == -1 &&
              memcmp(was, conv, sizeof(conv[0]) * (size_t)n) == 0);
    check("public first: nothing to do with no list", rift_conv_public_first(&s, NULL, 0) == -1 &&
                                                          rift_conv_public_first(&s, conv, 0) == -1);
}

static void test_slot_reuse(void)
{
    static struct rift_model s;
    static struct rift_model r;
    struct rift_conv conv[RIFT_MAX_CONVERSATIONS];
    char a[RIFT_KEY_HEX];
    char b[RIFT_KEY_HEX];
    char c[RIFT_KEY_HEX];
    int n;
    int i;
    int a_rows = 0;
    int b_rows = 0;

    rift_channel_key(0, "a1", "ALPHA", a, sizeof(a));
    rift_channel_key(0, "b2", "BRAVO", b, sizeof(b));
    rift_channel_key(0, "b2", "CHARLIE", c, sizeof(c));
    rift_model_init(&s);

    /* 1. A is joined in slot 0; a direct conversation runs beside it. */
    apply_channels_json(&s, "{\"channels\":[{\"channel\":0,\"name\":\"ALPHA\",\"channel_hash\":"
                            "\"a1\",\"key_bits\":256,\"text_limit\":147}],\"count\":1,\"max\":8}");
    check("slot reuse: A is joined in slot 0", rift_model_key_channel(&s, a) != NULL);

    /* 2. History on A, both ways, and a direct message. */
    check("slot reuse: A's history is taken",
          apply_message(&s, MSG_ON(1, "in", "a1", "ALPHA", "X: from A", "received")) == 0 &&
              apply_message(&s, MSG_ON(2, "out", "a1", "ALPHA", "X: to A", "sent_flood")) == 0 &&
              apply_message(&s, MSG_DIRECT(3)) == 0);
    check("slot reuse: A holds two messages", thread_len(&s, a) == 2);

    /* 3. A is left. */
    apply_event(&s, "mesh.channel", "{\"reason\":\"removed\",\"channel\":{\"channel\":0}}");
    check("slot reuse: A left, its key names no joined channel", rift_model_key_channel(&s, a) == NULL);
    check("slot reuse: and its history is kept", thread_len(&s, a) == 2);

    /* 4. B, a different channel, is added into the same slot. */
    apply_event(&s, "mesh.channel",
                "{\"reason\":\"added\",\"channel\":{\"channel\":0,\"name\":\"BRAVO\","
                "\"channel_hash\":\"b2\",\"key_bits\":256,\"text_limit\":140}}");
    check("slot reuse: B takes slot 0",
          rift_model_channel(&s, 0) && strcmp(rift_model_channel(&s, 0)->name, "BRAVO") == 0);
    check("slot reuse: B's conversation is not A's", strcmp(a, b) != 0);

    /* 5. B inherits nothing of A. */
    check("slot reuse: B's thread is empty", thread_len(&s, b) == 0);
    check("slot reuse: B has nothing unread of A's", rift_model_unread(&s, b) == 0);
    check("slot reuse: A's history stays A's", thread_len(&s, a) == 2);
    check("slot reuse: and is still named ALPHA, not BRAVO", name_is(rift_model_conv_name(&s, a), "ALPHA"));
    n = rift_model_conversations(&s, conv, RIFT_MAX_CONVERSATIONS);
    for (i = 0; i < n; i++) {
        a_rows += strcmp(conv[i].key, a) == 0 && conv[i].total == 2;
        b_rows += strcmp(conv[i].key, b) == 0;
    }
    check("slot reuse: the list holds A's conversation, whole", a_rows == 1);
    check("slot reuse: and no conversation of B's made of A's messages", b_rows == 0);

    /* 6. A reply in A's conversation cannot reach B's audience. */
    check("slot reuse: writing in A's conversation is refused",
          rift_model_send_begin(&s, a, "reply", 1000) == -1 && !rift_model_sending(&s));
    rift_model_send_clear(&s);
    check("slot reuse: writing to B is not", rift_model_send_begin(&s, b, "hello", 1000) == 0);
    rift_model_send_clear(&s);
    check("slot reuse: B's limit is B's", rift_model_text_limit(&s, b) == 140);
    check("slot reuse: A's is no longer known", rift_model_text_limit(&s, a) == 0);

    /* 7. B's own messages stay with B. */
    check("slot reuse: a message on B is taken",
          apply_message(&s, MSG_ON(4, "in", "b2", "BRAVO", "X: on B", "received")) == 0);
    check("slot reuse: it is B's", thread_len(&s, b) == 1);
    check("slot reuse: and not A's", thread_len(&s, a) == 2);
    check("slot reuse: B is named BRAVO", name_is(rift_model_conv_name(&s, b), "BRAVO"));
    check("slot reuse: B's unread is B's own", rift_model_unread(&s, b) == 1);

    /* 8. Direct messages are untouched. */
    check("slot reuse: the direct conversation is unchanged",
          thread_len(&s, KEY_A) == 1 && rift_model_unread(&s, KEY_A) == 1 &&
              name_is(rift_model_conv_name(&s, KEY_A), "HYTTA"));
    check("slot reuse: and still a destination",
          rift_model_send_begin(&s, KEY_A, "direct", 1000) == 0);
    rift_model_send_clear(&s);

    /* 9. The same hash under another name is another channel too. */
    apply_event(&s, "mesh.channel", "{\"reason\":\"removed\",\"channel\":{\"channel\":0}}");
    apply_event(&s, "mesh.channel",
                "{\"reason\":\"added\",\"channel\":{\"channel\":0,\"name\":\"CHARLIE\","
                "\"channel_hash\":\"b2\",\"key_bits\":256,\"text_limit\":140}}");
    check("slot reuse: C, same hash as B, is not B",
          rift_model_key_channel(&s, c) != NULL && rift_model_key_channel(&s, b) == NULL);
    check("slot reuse: C inherits neither", thread_len(&s, c) == 0);
    check("slot reuse: and B's conversation cannot reach C",
          rift_model_send_begin(&s, b, "reply", 1000) == -1);
    rift_model_send_clear(&s);

    /* 10. A run that never saw the swap - RIFT opened afterwards - sees the
     * channel list and meshcored's ring, and still files each message under
     * the channel it was on. */
    rift_model_init(&r);
    apply_channels_json(&r, "{\"channels\":[{\"channel\":0,\"name\":\"CHARLIE\",\"channel_hash\":"
                            "\"b2\",\"key_bits\":256,\"text_limit\":140}],\"count\":1,\"max\":8}");
    check("slot reuse: a fresh run takes the ring",
          apply_messages(&r, "{\"messages\":[" MSG_ON(1, "in", "a1", "ALPHA", "X: from A", "received")
                             "," MSG_ON(2, "out", "a1", "ALPHA", "X: to A", "sent_flood")
                             "," MSG_DIRECT(3)
                             "," MSG_ON(4, "in", "b2", "BRAVO", "X: on B", "received")
                             "],\"count\":4,\"total\":4,\"persistent\":false}") == 0);
    check("slot reuse: fresh run, A's history is A's", thread_len(&r, a) == 2);
    check("slot reuse: fresh run, B's is B's", thread_len(&r, b) == 1);
    check("slot reuse: fresh run, C has none of it", thread_len(&r, c) == 0);
    check("slot reuse: fresh run, the left channels keep their own names",
          name_is(rift_model_conv_name(&r, a), "ALPHA") && name_is(rift_model_conv_name(&r, b), "BRAVO"));
    check("slot reuse: fresh run, only C can be written to",
          rift_model_send_begin(&r, a, "x", 1000) == -1 && rift_model_send_begin(&r, b, "x", 1000) == -1 &&
              rift_model_send_begin(&r, c, "x", 1000) == 0);
    rift_model_send_clear(&r);
    check("slot reuse: fresh run, the direct conversation is there", thread_len(&r, KEY_A) == 1);

    /* 11. A channel message that cannot be identified is kept apart, not
     * guessed into whatever the slot holds now. */
    check("slot reuse: a message with no hash is taken",
          apply_message(&r, "{\"id\":5,\"direction\":\"in\",\"kind\":\"channel\",\"channel\":0,"
                            "\"channel_name\":\"CHARLIE\",\"text\":\"X: ?\",\"state\":\"received\","
                            "\"ack_expected\":false,\"mono_ms\":500}") == 0);
    check("slot reuse: in a conversation of its own", thread_len(&r, "#0:?") == 1);
    check("slot reuse: not in C's", thread_len(&r, c) == 0);
    check("slot reuse: and nothing can be written there",
          rift_model_send_begin(&r, "#0:?", "x", 1000) == -1);
    rift_model_send_clear(&r);
}

static void test_body_and_meta(void)
{
    static struct rift_model s;
    const struct rift_message *msg;
    char text[RIFT_MSG_META_MAX];

    rift_model_init(&s);
    apply_message(&s, "{\"id\":1,\"direction\":\"in\",\"kind\":\"channel\",\"channel\":0,"
                      "\"sender_name\":\"HYTTA\",\"text\":\"HYTTA: str\xC3\xB8m tilbake\","
                      "\"state\":\"received\",\"ack_expected\":false,\"mono_ms\":1000,"
                      "\"rssi_dbm\":-88.0}");
    apply_message(&s, "{\"id\":2,\"direction\":\"out\",\"kind\":\"channel\",\"channel\":0,"
                      "\"sender_name\":\"K230-A\",\"text\":\"K230-A: mottatt\","
                      "\"state\":\"sent_flood\",\"ack_expected\":false,\"mono_ms\":2000}");
    /* A payload whose claimed name does not match its prefix is shown as it
     * came: where a name ends is not guessed at. */
    apply_message(&s, "{\"id\":3,\"direction\":\"in\",\"kind\":\"channel\",\"channel\":0,"
                      "\"sender_name\":\"ANNA\",\"text\":\"ANNAB: hei\","
                      "\"state\":\"received\",\"ack_expected\":false,\"mono_ms\":3000}");
    apply_message(&s, "{\"id\":4,\"direction\":\"in\",\"kind\":\"channel\",\"channel\":0,"
                      "\"text\":\"no prefix at all\",\"state\":\"received\","
                      "\"ack_expected\":false,\"mono_ms\":4000}");
    apply_message(&s, "{\"id\":5,\"direction\":\"in\",\"peer_public_key\":"
                      "\"b2cafe1e7d0411223344556677889900aabbccddeeff001122334455667788b2\","
                      "\"peer_name\":\"HYTTA\",\"text\":\"HYTTA: this is a direct message\","
                      "\"state\":\"received\",\"mono_ms\":5000,\"snr_db\":6.5}");
    apply_message(&s, "{\"id\":6,\"direction\":\"out\",\"peer_public_key\":"
                      "\"b2cafe1e7d0411223344556677889900aabbccddeeff001122334455667788b2\","
                      "\"text\":\"ok\",\"state\":\"acked\",\"mono_ms\":6000,"
                      "\"ack_mono_ms\":7000}");

    msg = held(&s, 1);
    text_is("a channel body loses the prefix the sender's name was written into",
            rift_msg_body(msg), "str\xC3\xB8m tilbake");
    text_is("while the whole payload is still kept", msg ? msg->text : "",
            "HYTTA: str\xC3\xB8m tilbake");
    rift_fmt_msg_meta(msg, 61000, text, sizeof(text));
    text_is("its caption says how long ago, who it CLAIMS to be, and what was measured", text,
            "1m \xC2\xB7 HYTTA? \xC2\xB7 RECEIVED \xC2\xB7 \xE2\x88\x92" "88 dBm");
    rift_fmt_preview(msg, text, sizeof(text));
    text_is("and its preview marks the claim too", text, "HYTTA?: str\xC3\xB8m tilbake");

    msg = held(&s, 2);
    text_is("this node's own channel line is what the reader typed", rift_msg_body(msg),
            "mottatt");
    rift_fmt_msg_meta(msg, 12000, text, sizeof(text));
    text_is("with no name in front of it and no delivery claimed", text,
            "10s \xC2\xB7 SENT \xC2\xB7 FLOOD \xC2\xB7 NO ACK ON CHANNELS");
    rift_fmt_preview(msg, text, sizeof(text));
    text_is("and its preview is the reader's own words", text, "you: mottatt");

    text_is("a prefix that is not exactly the claimed name is left alone",
            rift_msg_body(held(&s, 3)), "ANNAB: hei");
    text_is("so is a channel line with no claimed name", rift_msg_body(held(&s, 4)),
            "no prefix at all");
    rift_fmt_msg_meta(held(&s, 4), 5000, text, sizeof(text));
    text_is("which is said to be unnamed rather than left blank", text,
            "1s \xC2\xB7 UNNAMED \xC2\xB7 RECEIVED");

    /* A direct message is never cut: a peer_name came with a public key, and
     * whatever its text begins with is what was said. */
    text_is("a direct message's text is never shortened", rift_msg_body(held(&s, 5)),
            "HYTTA: this is a direct message");
    rift_fmt_msg_meta(held(&s, 5), 5000, text, sizeof(text));
    text_is("and its caption names nobody: the thread already does", text,
            "0s \xC2\xB7 RECEIVED \xC2\xB7 SNR 6.5");
    rift_fmt_msg_meta(held(&s, 6), 66000, text, sizeof(text));
    text_is("an acknowledged one says how long the ACK took", text,
            "1m \xC2\xB7 DELIVERED \xC2\xB7 ACK 1 s");
    rift_fmt_msg_meta(NULL, 0, text, sizeof(text));
    text_is("and no message is no caption", text, "");
    text_is("nor any body", rift_msg_body(NULL), "");
}

/* ---- emoji and other multi-byte text: limits are bytes, as on the air ------ */
static void test_multibyte_limits(void)
{
    static struct rift_model s;
    char text[256];
    char why[128];
    int i;

    for (i = 0; i < 40; i++) {
        memcpy(text + 4 * i, "\xF0\x9F\x99\x82", 4);
    }
    text[160] = '\0';
    check("forty 4-byte emoji are 160 bytes, and a message takes them",
          rift_send_text_bytes(text) == 160 && rift_send_text_check(text, why, sizeof(why)) == 0);
    memcpy(text + 160, "\xF0\x9F\x99\x82", 5);
    check("forty-one are 164 bytes, and refused in bytes, not characters",
          rift_send_text_check(text, why, sizeof(why)) != 0 && strstr(why, "164 bytes") != NULL);
    check("text that is not well-formed is refused before it is asked",
          rift_send_text_check("hei \xF0\x9F\x99", why, sizeof(why)) != 0 &&
              strstr(why, "UTF-8") != NULL);
    check("Norwegian letters are two bytes each and fine",
          rift_send_text_check("bl\xC3\xA5" "b\xC3\xA6rsyltet\xC3\xB8y", why, sizeof(why)) == 0);
    check("a text smiley is plain ASCII", rift_send_text_check(":) <3 :D", why, sizeof(why)) == 0);

    /* A channel's limit is bytes too: 147 here, so 36 emoji (144 bytes) go and
     * 37 (148 bytes) do not, though both are far fewer characters. */
    rift_model_init(&s);
    {
        cJSON *o = cJSON_Parse("{\"channels\":[{\"channel\":0,\"name\":\"SITE\",\"channel_hash\":"
                               "\"8c\",\"key_bits\":128,\"text_limit\":147}],\"count\":1,\"max\":8}");

        rift_model_apply_channels(&s, o);
        cJSON_Delete(o);
    }
    s.state = RIFT_SVC_ONLINE;
    s.have_status = 1;
    s.radio_online = 1;
    {
        char key[RIFT_KEY_HEX];

        rift_channel_key(0, "8c", "SITE", key, sizeof(key));
        for (i = 0; i < 37; i++) {
            memcpy(text + 4 * i, "\xF0\x9F\x98\x82", 4);
        }
        text[144] = '\0';
        check("a channel takes what fits its byte limit",
              rift_model_send_begin(&s, key, text, 1000) == 0);
        rift_model_send_failed(&s, "x");
        rift_model_send_clear(&s);
        memcpy(text + 144, "\xF0\x9F\x98\x82", 5);
        check("and refuses a character more, which would be 148 bytes",
              rift_model_send_begin(&s, key, text, 1000) == -1);
    }
}

int main(void)
{
    struct rift_model m;
    struct rift_conv conv[RIFT_MAX_CONVERSATIONS];
    const struct rift_message *thread[RIFT_MAX_MESSAGES];
    char text[RIFT_MSG_CAPTION_MAX];
    int older = 0;
    int n;

    test_public_first();

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

    /* ---- the service restarts, and its ids start again ---------------------- */
    /* Found on unit A, 2026-09-21 (docs/hardware/RIFT_CHANNELS_GATE.md):
     * meshcored hands out ids from 1 on every run and keeps no messages
     * across one, so a cache that merged a snapshot by id let a new id 1
     * land on an old id 1 and orphaned every old id with no new counterpart.
     * The panel showed four messages while the service held three. */
    {
        struct rift_model s;
        int64_t t = 3600000;

        rift_model_init(&s);
        give_status(&s, 3600, t);

        /* One run of the service, with a history read at the start and one
         * message that arrived while RIFT was watching. */
        apply_messages(&s,
                       "{\"messages\":["
                       "{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
                       "\"peer_name\":\"HYTTA\",\"text\":\"old one\",\"state\":\"received\","
                       "\"mono_ms\":1000},"
                       "{\"id\":2,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A "\","
                       "\"text\":\"old two\",\"state\":\"acked\",\"mono_ms\":2000},"
                       "{\"id\":3,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
                       "\"peer_name\":\"OSLO-01\",\"text\":\"old three\",\"state\":\"received\","
                       "\"mono_ms\":3000}"
                       "],\"count\":3,\"total\":3,\"persistent\":false}");
        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":4,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B
                    "\",\"peer_name\":\"OSLO-01\",\"text\":\"old four\",\"state\":\"received\","
                    "\"mono_ms\":4000}}");
        check("four messages before the restart", s.msg_count == 4);
        check("the one that arrived while watching is unread",
              rift_model_unread(&s, KEY_B) == 1);
        rift_model_mark_read(&s, KEY_B);
        check("and reading it clears it", rift_model_unread_total(&s) == 0);

        /* A snapshot merging with an event already delivered in the same run
         * is the behaviour this fix must not break: id 4 is already held, so
         * the snapshot updates it rather than doubling it. */
        apply_messages(&s,
                       "{\"messages\":["
                       "{\"id\":4,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
                       "\"peer_name\":\"OSLO-01\",\"text\":\"old four\",\"state\":\"received\","
                       "\"mono_ms\":4000}"
                       "],\"count\":1,\"total\":4,\"persistent\":false}");
        check("a snapshot in the same run still merges", s.msg_count == 4);
        check("and forgets nothing", s.msgs_forgotten == 0 && s.svc_restarts == 0);

        /* meshcored goes, and another comes up in its place. A dropped socket
         * is all a client sees; the uptime is what says the process behind it
         * is a different one. */
        rift_model_service_lost(&s, "meshcored closed the connection");
        check("what was known is still shown while it is away", s.msg_count == 4);
        rift_model_service_found(&s);
        give_status(&s, 2, t + 9000);
        check("the restart is noticed", s.svc_restarts == 1);

        /* The new run has heard two messages of its own, under ids 1 and 2.
         * Merged, they would have landed on the old 1 and 2 - one of them
         * replacing a message in a different conversation - and the old 3
         * and 4 would have stayed on screen with nothing behind them. */
        apply_messages(&s,
                       "{\"messages\":["
                       "{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
                       "\"peer_name\":\"OSLO-01\",\"text\":\"new one\",\"state\":\"received\","
                       "\"mono_ms\":12000},"
                       "{\"id\":2,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
                       "\"peer_name\":\"OSLO-01\",\"text\":\"new two\",\"state\":\"received\","
                       "\"mono_ms\":13000}"
                       "],\"count\":2,\"total\":2,\"persistent\":false}");
        check("the cache holds exactly what the service holds", s.msg_count == 2);
        check("nothing from the run before it is left behind",
              !holds_text(&s, "old three") && !holds_text(&s, "old four"));
        check("and the new ids are the new messages, not overwritten old ones",
              holds_text(&s, "new one") && holds_text(&s, "new two"));
        check("what was dropped is counted rather than merely gone", s.msgs_forgotten == 4);
        check("the history is valid again once it has been re-read", s.messages_valid);
        /* The read mark was an id in the old run's space. Kept, it would have
         * marked both of the new messages as already read - the same fault in
         * the direction that hides a message. */
        check("the messages of the new run are unread", rift_model_unread(&s, KEY_B) == 2);
        check("and the conversation the new run has never heard of is gone",
              rift_model_thread(&s, KEY_A, thread, RIFT_MAX_MESSAGES, NULL) == 0);
        n = rift_model_conversations(&s, conv, RIFT_MAX_CONVERSATIONS);
        check("one conversation, not three", n == 1);

        /* The run after a restart behaves like any other: an event merges, a
         * snapshot merges with it, and nothing is forgotten again until the
         * service restarts again. */
        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":3,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B
                    "\",\"text\":\"new three\",\"state\":\"received\",\"mono_ms\":14000}}");
        give_status(&s, 12, t + 19000);
        apply_messages(&s,
                       "{\"messages\":["
                       "{\"id\":3,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
                       "\"text\":\"new three\",\"state\":\"received\",\"mono_ms\":14000}"
                       "],\"count\":1,\"total\":3,\"persistent\":false}");
        check("the same run keeps merging", s.msg_count == 3);
        check("and nothing more was forgotten", s.msgs_forgotten == 4 && s.svc_restarts == 1);
    }

    /* ---- a message of the new run arrives before its history does ----------- */
    /* On a reconnect the client subscribes before it asks for the history, so
     * a mesh.message event reaches the cache first whenever one is raised in
     * between. The event path has to empty the cache as well, or the first
     * message of the new run is filed beside the old run's ids and the
     * snapshot then merges into the mixture. */
    {
        struct rift_model s;

        rift_model_init(&s);
        give_status(&s, 1200, 1200000);
        apply_messages(&s,
                       "{\"messages\":["
                       "{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
                       "\"text\":\"old one\",\"state\":\"received\",\"mono_ms\":1000},"
                       "{\"id\":2,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
                       "\"text\":\"old two\",\"state\":\"received\",\"mono_ms\":2000}"
                       "],\"count\":2,\"total\":2,\"persistent\":false}");
        check("two messages, read, because the first snapshot is history",
              s.msg_count == 2 && rift_model_unread_total(&s) == 0);

        rift_model_service_lost(&s, "meshcored closed the connection");
        rift_model_service_found(&s);
        give_status(&s, 3, 1206000);
        check("the restart is noticed", s.svc_restarts == 1);

        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B
                    "\",\"text\":\"new one\",\"state\":\"received\",\"mono_ms\":9000}}");
        check("the event empties the old run before it is filed", s.msg_count == 1);
        check("and what is held is the new message, not an old one rewritten",
              holds_text(&s, "new one") && !holds_text(&s, "old one"));
        check("with the history not read yet, which is not the same as empty",
              !s.messages_valid);
        check("and the old run's read marks gone with it, so it is unread",
              rift_model_unread(&s, KEY_B) == 1);

        apply_messages(&s,
                       "{\"messages\":["
                       "{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
                       "\"text\":\"new one\",\"state\":\"received\",\"mono_ms\":9000},"
                       "{\"id\":2,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
                       "\"text\":\"new two\",\"state\":\"received\",\"mono_ms\":10000}"
                       "],\"count\":2,\"total\":2,\"persistent\":false}");
        check("the history that follows merges with it rather than doubling it",
              s.msg_count == 2);
        check("nothing was forgotten a second time", s.msgs_forgotten == 2);
        check("and the history is valid again", s.messages_valid);
    }

    /* ---- a restart with nothing to show for it ------------------------------ */
    /* The snapshot that follows a restart is often empty: the service has
     * heard nothing yet. An empty one must empty the cache too, or the whole
     * of the previous run stays on the panel with nothing behind it. */
    {
        struct rift_model s;

        rift_model_init(&s);
        give_status(&s, 900, 900000);
        apply_messages(&s,
                       "{\"messages\":["
                       "{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
                       "\"text\":\"before\",\"state\":\"received\",\"mono_ms\":1000}"
                       "],\"count\":1,\"total\":1,\"persistent\":false}");
        check("one message before the restart", s.msg_count == 1);
        rift_model_service_lost(&s, "meshcored closed the connection");
        rift_model_service_found(&s);
        give_status(&s, 1, 903000);
        apply_messages(&s, "{\"messages\":[],\"count\":0,\"total\":0,\"persistent\":false}");
        check("an empty snapshot after a restart empties the cache", s.msg_count == 0);
        check("and says the history was read rather than missing", s.messages_valid);
        n = rift_model_conversations(&s, conv, RIFT_MAX_CONVERSATIONS);
        check("so no conversation is left", n == 0);
    }

    /* ---- emptied is not the same as empty ----------------------------------- */
    /* A cache emptied because the run changed has not been read from the new
     * one yet, and that is the difference COMMS draws between "Waiting for
     * meshcored." and "No messages yet." Nothing drops the connection here,
     * because this is the cache's own rule and not the socket's: the model
     * is asked for a status and then for a message, which is all it takes. */
    {
        struct rift_model s;

        rift_model_init(&s);
        give_status(&s, 600, 600000);
        apply_messages(&s,
                       "{\"messages\":["
                       "{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
                       "\"text\":\"read from that run\",\"state\":\"received\","
                       "\"mono_ms\":1000}"
                       "],\"count\":1,\"total\":1,\"persistent\":false}");
        check("a history has been read", s.messages_valid && s.msg_count == 1);
        give_status(&s, 1, 602000);
        check("the run changed", s.svc_restarts == 1);
        apply_event(&s, "mesh.message",
                    "{\"message\":{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A
                    "\",\"text\":\"heard since\",\"state\":\"received\",\"mono_ms\":3000}}");
        check("emptying the cache un-reads the history", !s.messages_valid);
        check("though what has arrived since is held",
              s.msg_count == 1 && holds_text(&s, "heard since"));
        apply_messages(&s,
                       "{\"messages\":["
                       "{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
                       "\"text\":\"heard since\",\"state\":\"received\",\"mono_ms\":3000}"
                       "],\"count\":1,\"total\":1,\"persistent\":false}");
        check("and reading the new run's history makes it valid again", s.messages_valid);
    }

    /* ---- a reconnect that is not a restart ---------------------------------- */
    /* A socket can go without the process behind it going. This cache of 96
     * can hold more of one kind than either of the service's two rings of
     * 64, so emptying on every reconnect would throw away messages the
     * service can no longer supply. Only a changed run empties it. */
    {
        struct rift_model s;

        rift_model_init(&s);
        give_status(&s, 4000, 4000000);
        apply_messages(&s,
                       "{\"messages\":["
                       "{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
                       "\"text\":\"kept\",\"state\":\"received\",\"mono_ms\":1000}"
                       "],\"count\":1,\"total\":1,\"persistent\":false}");
        rift_model_service_lost(&s, "meshcored stopped reading");
        rift_model_service_found(&s);
        /* The same process, five seconds older. */
        give_status(&s, 4005, 4005000);
        check("a reconnect to the same run is not a restart", s.svc_restarts == 0);
        apply_messages(&s,
                       "{\"messages\":["
                       "{\"id\":2,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
                       "\"text\":\"and this\",\"state\":\"received\",\"mono_ms\":5000}"
                       "],\"count\":1,\"total\":2,\"persistent\":false}");
        check("so a message the service can no longer supply is kept",
              s.msg_count == 2 && holds_text(&s, "kept"));
        check("and nothing was forgotten", s.msgs_forgotten == 0);
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
        char site[RIFT_KEY_HEX];

        rift_model_init(&s);
        rift_channel_key(0, "8c", "SITE", site, sizeof(site));

        /* The key, and its inverse. These two are what let a channel be an
         * ordinary conversation everywhere else. */
        {
            const char *k = chan_key(0, "8c", "SITE");

            check("a channel's key is its slot, its hash and its name",
                  strncmp(k, "#0:8c:", 6) == 0 && strlen(k) == 14);
            check("and it reads back as its slot", rift_key_is_channel(k) == 0);
            text_is("the hash is written one way whatever case it came in",
                    chan_key(0, "8C", "SITE"), k);
            /* The three parts each tell channels apart. */
            check("another slot is another channel", strcmp(chan_key(3, "8c", "SITE"), k) != 0);
            check("so is another hash in the same slot", strcmp(chan_key(0, "8d", "SITE"), k) != 0);
            check("and another name in the same slot with the same hash",
                  strcmp(chan_key(0, "8c", "SITE2"), k) != 0);
            /* A channel that cannot be identified is kept apart, not guessed. */
            text_is("no hash: a key of its own", chan_key(3, NULL, "SITE"), "#3:?");
            text_is("no name: the same", chan_key(3, "8c", NULL), "#3:?");
            text_is("an empty name: the same", chan_key(3, "8c", ""), "#3:?");
            text_is("a hash that is not one byte of hex: the same", chan_key(3, "zz", "SITE"), "#3:?");
            text_is("a slot outside the table has no key", chan_key(RIFT_MAX_CHANNELS, "8c", "S"), "");
            check("an unidentified channel's key reads back as its slot",
                  rift_key_is_channel("#3:?") == 3);
            check("slot zero is a slot, not an absence", rift_key_is_channel("#0:?") == 0);
            check("the bare slot of older builds is not a channel key",
                  rift_key_is_channel("#3") == -1 && rift_key_is_channel("#0") == -1);
            check("a public key is not a channel", rift_key_is_channel(KEY_A) == -1);
            check("nor is a bare hash", rift_key_is_channel("#") == -1);
            check("nor is a slot that is not a number", rift_key_is_channel("#x:?") == -1);
            check("nor one past the table", rift_key_is_channel("#9:?") == -1);
            check("nor a fingerprint one digit short", rift_key_is_channel("#0:8c:0000000") == -1);
            check("nor one with a digit too many", rift_key_is_channel("#0:8c:000000000") == -1);
            check("nor an upper-case one", rift_key_is_channel("#0:8C:00000000") == -1);
            check("nor anything after the '?'", rift_key_is_channel("#0:?x") == -1);
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
              rift_model_text_limit(&s, site) == 147);
        check("the list's channel is the one its key names",
              rift_model_key_channel(&s, site) == rift_model_channel(&s, 0));
        check("and a key for another channel in that slot is not",
              rift_model_key_channel(&s, chan_key(0, "8d", "SITE")) == NULL);
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
        text_is("its conversation key is that channel's", s.msg[0].conv_key, site);
        text_is("and the hash that was on the air is kept", s.msg[0].channel_hash, "8c");
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
        text_is("keyed by its channel", conv[0].key, site);
        check("and marked as a channel", conv[0].is_channel && conv[0].channel_slot == 0);
        text_is("named by this node's own name for it", conv[0].name, "SITE");
        check("with one message in it", conv[0].total == 1);
        /* A channel message that arrived after this app opened is unread the
         * same way a direct one is. */
        check("and one unread", conv[0].unread == 1);
        check("which the total agrees with", rift_model_unread_total(&s) == 1);

        /* The thread. */
        {
            int nn = rift_model_thread(&s, site, thread, RIFT_MAX_MESSAGES, &older);

            check("the thread holds it", nn == 1 && older == 0);
            check("and it is the message that was sent", nn == 1 && thread[0]->id == 10);
            /* The public key of a real peer is not this channel. */
            nn = rift_model_thread(&s, KEY_A, thread, RIFT_MAX_MESSAGES, &older);
            check("a peer's thread does not pick up channel messages", nn == 0);
        }

        /* Reading it clears the badge, and only for that conversation. */
        check("marking it read clears one", rift_model_mark_read(&s, site) == 1);
        check("and it is read now", rift_model_unread(&s, site) == 0);
        check("a second message is unread again",
              apply_message(&s, "{\"id\":14,\"direction\":\"in\",\"kind\":\"channel\","
                                "\"channel\":0,\"channel_name\":\"SITE\","
                                "\"channel_hash\":\"8c\",\"text\":\"K230-A: hi\","
                                "\"sender_name\":\"K230-A\",\"state\":\"received\","
                                "\"ack_expected\":false,\"mono_ms\":200}") == 0 &&
                  rift_model_unread(&s, site) == 1);

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
              rift_model_mark_read(&s, site) == 1 && rift_model_unread(&s, KEY_A) == 1);

        /* The tally: nothing on a channel can be delivered or time out. */
        check("an outgoing channel message is taken",
              apply_message(&s, "{\"id\":16,\"direction\":\"out\",\"kind\":\"channel\","
                                "\"channel\":0,\"channel_name\":\"SITE\","
                                "\"channel_hash\":\"8c\",\"text\":\"K230-A: sending\","
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
        check("a joined channel is a destination this app will send to",
              rift_model_send_begin(&s, site, "hello", 1000) == 0);
        rift_model_send_clear(&s);
        /* mesh.send addresses a slot. A key that does not name the channel
         * now in its slot must never reach it: that is how a reply to one
         * audience would go to another. */
        check("a slot with no channel in it is not a destination",
              rift_model_send_begin(&s, chan_key(1, "8c", "SITE"), "hello", 1000) == -1);
        rift_model_send_clear(&s);
        check("nor is a different channel's key for an occupied slot",
              rift_model_send_begin(&s, chan_key(0, "77", "OTHER"), "hello", 1000) == -1);
        rift_model_send_clear(&s);
        check("nor a channel that cannot be identified",
              rift_model_send_begin(&s, "#0:?", "hello", 1000) == -1);
        rift_model_send_clear(&s);
        check("nor the bare slot older builds used",
              rift_model_send_begin(&s, "#0", "hello", 1000) == -1);
        rift_model_send_clear(&s);
        check("but a key that is neither is refused",
              rift_model_send_begin(&s, "not-a-key", "hello", 1000) == -1);
        rift_model_send_clear(&s);
        {
            char too_long[200];

            memset(too_long, 'x', sizeof(too_long));
            too_long[147] = '\0';
            check("a body of exactly the channel's limit is taken",
                  rift_model_send_begin(&s, site, too_long, 1000) == 0);
            rift_model_send_clear(&s);
            too_long[147] = 'x';
            too_long[148] = '\0';
            /* Refused here rather than written and refused by the service:
             * the limit is shorter on a channel because this node's name
             * travels inside the payload. */
            check("and one byte more is refused before anything is written",
                  rift_model_send_begin(&s, site, too_long, 1000) == -1);
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
        nm = rift_model_conv_name(&s, chan_key(4, "11", "RENAMED"));
        check("under its new name", nm && strcmp(nm, "RENAMED") == 0);
        /* The name is part of the identity, so what the slot held under the
         * old name is not this channel any more - kept apart, not merged. */
        check("and the old name's key no longer names a joined channel",
              rift_model_key_channel(&s, chan_key(4, "11", "NEW")) == NULL);
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
              rift_model_thread(&s, site, thread, RIFT_MAX_MESSAGES, &older) > 0);
        check("and a name is still found for them",
              rift_model_conv_name(&s, site) != NULL);
        {
            /* Even after the channel itself is gone: the name falls back to
             * what the messages carried. */
            apply_event(&s, "mesh.channel",
                        "{\"reason\":\"removed\",\"channel\":{\"channel\":0}}");
            nm = rift_model_conv_name(&s, site);
            check("a left channel is still named by its messages",
                  nm && strcmp(nm, "SITE") == 0);
            check("but it is no longer in the list", rift_model_channel(&s, 0) == NULL);
            check("and nothing can be written to it any more",
                  rift_model_send_begin(&s, site, "hello", 1000) == -1);
            rift_model_send_clear(&s);
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
                  rift_model_apply_status(&s, o, rift_mono_ms()) == 0);
            cJSON_Delete(o);
        }
        check("the channel fault is reported", s.have_channel_fault);
        check("and is not the node table's", !s.have_state_fault);
    }

    test_body_and_meta();
    test_slot_reuse();

    test_multibyte_limits();
    printf("rift_comms_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
