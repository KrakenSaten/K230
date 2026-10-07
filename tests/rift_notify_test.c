/*
 * RIFT's DM sound and activity measure, without a screen.
 *
 * The failures worth testing here are the ones a reader would notice from
 * across the room: a sound for a message that is not new - the history
 * loaded on opening, the same event twice, a state change, a snapshot after
 * a reconnect, a sender's retry filed under a fresh id, the reader's own
 * message, a channel - and twenty sounds for a burst of twenty. And, for
 * the activity measure, a word that claims more than was observed: "NOW"
 * for a node never heard, or anything at all derived from a signal.
 *
 * Also the preferences file the DM sound's setting lives in, and the seam
 * the sound goes through. No LVGL, no socket, no service.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_format.h"
#include "rift_model.h"
#include "rift_notify.h"
#include "rift_sound.h"
#include "rift_store.h"

#include "pocketwav/pocketwav.h"

#include <fcntl.h>
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

#define KEY_A "a19ac21e7d0411223344556677889900aabbccddeeff001122334455667788b1"
#define KEY_B "b2cafe1e7d0411223344556677889900aabbccddeeff001122334455667788b2"

static int event(struct rift_model *m, const char *name, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_event(m, name, o);

    cJSON_Delete(o);
    return rc;
}

static int snapshot(struct rift_model *m, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_messages(m, o);

    cJSON_Delete(o);
    return rc;
}

/* One mesh.message event: a direct message with a sender's timestamp. */
static int dm(struct rift_model *m, int id, const char *dir, const char *key, long stamp,
              const char *text, const char *state)
{
    char json[768];

    snprintf(json, sizeof(json),
             "{\"message\":{\"id\":%d,\"direction\":\"%s\",\"peer_public_key\":\"%s\","
             "\"peer_name\":\"P\",\"text\":\"%s\",\"state\":\"%s\",\"timestamp\":%ld,"
             "\"mono_ms\":%d}}",
             id, dir, key, text, state, stamp, 1000 * id);
    return event(m, "mesh.message", json);
}

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

/* ---- which direct messages are arrivals --------------------------------- */

static void test_arrivals(void)
{
    static struct rift_model m;
    unsigned before;

    rift_model_init(&m);
    /* The history the app finds when it opens: however recent, it is not an
     * arrival, and nothing in it may sound. */
    check("the history on opening is taken",
          snapshot(&m, "{\"messages\":["
                       "{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
                       "\"text\":\"old\",\"state\":\"received\",\"timestamp\":100},"
                       "{\"id\":2,\"direction\":\"out\",\"peer_public_key\":\"" KEY_A "\","
                       "\"text\":\"reply\",\"state\":\"acked\"},"
                       "{\"id\":3,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
                       "\"text\":\"older\",\"state\":\"received\",\"timestamp\":90}],"
                       "\"persistent\":false}") == 0);
    check("and none of it is an arrival", m.dm_arrivals == 0);
    check("but it sets the highest id this run has shown", m.dm_high_id == 3);

    /* A new direct message, live. */
    check("a live direct message is taken", dm(&m, 4, "in", KEY_A, 200, "hei", "received") == 0);
    check("and is an arrival", m.dm_arrivals == 1);
    check("from the peer it came from", strcmp(m.dm_last_key, KEY_A) == 0);

    /* mesh.message is raised again for the same message - an id already
     * held is an update, never a second arrival. */
    before = m.dm_arrivals;
    dm(&m, 4, "in", KEY_A, 200, "hei", "received");
    check("the same event twice is one arrival", m.dm_arrivals == before && m.dm_repeats >= 1);
    check("and still one message", m.msg_count == 4);

    /* This device's own message, sent and then acknowledged. */
    dm(&m, 5, "out", KEY_A, 210, "svar", "sent_direct");
    dm(&m, 5, "out", KEY_A, 210, "svar", "acked");
    check("an outgoing message is never an arrival", m.dm_arrivals == before);

    /* A channel message is not a direct message. */
    event(&m, "mesh.message",
          "{\"message\":{\"id\":6,\"direction\":\"in\",\"kind\":\"channel\",\"channel\":0,"
          "\"channel_name\":\"SITE\",\"channel_hash\":\"8c\",\"sender_name\":\"X\","
          "\"text\":\"X: all\",\"state\":\"received\",\"timestamp\":220}}");
    check("a channel message is not a DM arrival", m.dm_arrivals == before);

    /* A reconnect's snapshot carries what arrived while the socket was down,
     * and the ones already seen: none of it sounds. */
    check("a reconnect snapshot is taken",
          snapshot(&m, "{\"messages\":["
                       "{\"id\":4,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
                       "\"text\":\"hei\",\"state\":\"received\",\"timestamp\":200},"
                       "{\"id\":7,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
                       "\"text\":\"while away\",\"state\":\"received\",\"timestamp\":230}],"
                       "\"persistent\":false}") == 0);
    check("a snapshot after a reconnect sounds for nothing", m.dm_arrivals == before);
    check("it is unread, all the same",
          rift_model_unread(&m, KEY_B) == 1);
    /* And the same message coming round as an event afterwards is history
     * too: its id is at or below the highest shown. */
    dm(&m, 7, "in", KEY_B, 230, "while away", "received");
    check("an event for a message a snapshot already showed is not an arrival",
          m.dm_arrivals == before);

    /* An id below the highest this run has shown, new to the window - an
     * evicted message the service raised again - is history as well. */
    {
        static struct rift_model n;

        rift_model_init(&n);
        dm(&n, 10, "in", KEY_A, 300, "ten", "received");
        check("a first live message is an arrival", n.dm_arrivals == 1);
        dm(&n, 8, "in", KEY_A, 280, "eight", "received");
        check("a live id below the highest seen is not", n.dm_arrivals == 1 &&
                                                           n.dm_repeats == 1);
    }

    /* A retry the service filed as a new message: new id, same sender, same
     * timestamp, same text. */
    before = m.dm_arrivals;
    dm(&m, 20, "in", KEY_A, 400, "er du der?", "received");
    check("a new message is an arrival", m.dm_arrivals == before + 1);
    dm(&m, 21, "in", KEY_A, 400, "er du der?", "received");
    check("the sender's retry under a fresh id is not a second one",
          m.dm_arrivals == before + 1);
    check("though it is kept, as the service reported it", m.msg_count >= 2);
    dm(&m, 22, "in", KEY_A, 401, "er du der?", "received");
    check("the same words sent again later are a new message", m.dm_arrivals == before + 2);
    dm(&m, 23, "in", KEY_B, 400, "er du der?", "received");
    check("and the same words from somebody else are too", m.dm_arrivals == before + 3);

    /* No sender timestamp: two messages saying the same thing cannot be
     * told from a retry, and are not suppressed on a guess. */
    before = m.dm_arrivals;
    event(&m, "mesh.message", "{\"message\":{\"id\":30,\"direction\":\"in\",\"peer_public_key\":\""
          KEY_A "\",\"text\":\"ok\",\"state\":\"received\"}}");
    event(&m, "mesh.message", "{\"message\":{\"id\":31,\"direction\":\"in\",\"peer_public_key\":\""
          KEY_A "\",\"text\":\"ok\",\"state\":\"received\"}}");
    check("without a timestamp each new id is an arrival", m.dm_arrivals == before + 2);

    /* A malformed message is refused whole and is not an arrival. */
    before = m.dm_arrivals;
    event(&m, "mesh.message", "{\"message\":{\"direction\":\"in\",\"peer_public_key\":\"" KEY_A
                              "\",\"text\":\"no id\"}}");
    check("a message the model refuses is not an arrival", m.dm_arrivals == before);

    /* A new run of the service starts its ids again from 1. Its first
     * message is an arrival even though 1 is below the old run's highest. */
    {
        static struct rift_model r;

        rift_model_init(&r);
        give_status(&r, 100, 200000);
        dm(&r, 40, "in", KEY_A, 500, "before", "received");
        check("the old run's message arrived", r.dm_arrivals == 1);
        give_status(&r, 3, 300000);
        dm(&r, 1, "in", KEY_A, 600, "after", "received");
        check("the new run's id 1 is an arrival", r.dm_arrivals == 2);
        check("the old run's ids went with the window", r.msg_count == 1 && r.dm_high_id == 1);
        /* The old run's message, retried by its sender into the new run. */
        dm(&r, 2, "in", KEY_A, 500, "before", "received");
        check("a retry that crosses the restart is still recognised", r.dm_arrivals == 2);
    }
}

/* ---- the sound policy ---------------------------------------------------- */

static void test_policy(void)
{
    static struct rift_model m;
    struct rift_notify n;
    int64_t t = 1000000;
    int i;
    int sounds;

    rift_model_init(&m);
    dm(&m, 1, "in", KEY_A, 10, "before the listener", "received");
    rift_notify_init(&n, &m, 1);
    check("arrivals before the listener started are not news to it",
          rift_notify_poll(&n, &m, t, 1) == 0 && n.played == 0);

    dm(&m, 2, "in", KEY_A, 11, "one", "received");
    check("an arrival with the sound on sounds", rift_notify_poll(&n, &m, t, 1) == 1);
    check("once", rift_notify_poll(&n, &m, t + 10, 1) == 0 && n.played == 1);

    /* A burst: twenty messages over two seconds, polled as the app would. */
    sounds = 0;
    for (i = 0; i < 20; i++) {
        dm(&m, 100 + i, "in", KEY_B, 1000 + i, "burst", "received");
        sounds += rift_notify_poll(&n, &m, t + RIFT_NOTIFY_GAP_MS + 100 * i, 1);
    }
    check("a burst of twenty is one sound", sounds == 1);
    check("the rest are counted, not queued", n.coalesced == 19);
    check("nothing plays later for them",
          rift_notify_poll(&n, &m, t + 10 * RIFT_NOTIFY_GAP_MS, 1) == 0);

    /* Twenty arriving between two polls is one sound too. */
    for (i = 0; i < 20; i++) {
        dm(&m, 200 + i, "in", KEY_B, 2000 + i, "between polls", "received");
    }
    check("twenty in one poll is one sound",
          rift_notify_poll(&n, &m, t + 20 * RIFT_NOTIFY_GAP_MS, 1) == 1);

    /* The gap is measured from the last sound, so a steady trickle is heard
     * now and then. */
    {
        int64_t at = t + 40 * RIFT_NOTIFY_GAP_MS;

        dm(&m, 300, "in", KEY_A, 3000, "a", "received");
        check("a message sounds", rift_notify_poll(&n, &m, at, 1) == 1);
        dm(&m, 301, "in", KEY_A, 3001, "b", "received");
        check("the next inside the gap does not",
              rift_notify_poll(&n, &m, at + RIFT_NOTIFY_GAP_MS - 1, 1) == 0);
        dm(&m, 302, "in", KEY_A, 3002, "c", "received");
        check("one after the gap does",
              rift_notify_poll(&n, &m, at + RIFT_NOTIFY_GAP_MS + 1, 1) == 1);
    }

    /* The setting. */
    rift_notify_set_enabled(&n, 0);
    dm(&m, 400, "in", KEY_A, 4000, "off", "received");
    check("with the setting off nothing sounds",
          rift_notify_poll(&n, &m, t + 100 * RIFT_NOTIFY_GAP_MS, 1) == 0 && n.off == 1);
    rift_notify_set_enabled(&n, 1);
    check("and turning it on again does not play what arrived while it was off",
          rift_notify_poll(&n, &m, t + 101 * RIFT_NOTIFY_GAP_MS, 1) == 0);

    /* No sound to be had - no platform sound, or muted. */
    dm(&m, 401, "in", KEY_A, 4001, "muted", "received");
    check("with no sound to play nothing is asked for",
          rift_notify_poll(&n, &m, t + 102 * RIFT_NOTIFY_GAP_MS, 0) == 0 && n.silent == 1);

    /* The reader's own message, and a duplicate, reach the policy as no
     * arrival at all. */
    dm(&m, 402, "out", KEY_A, 4002, "mine", "sent_direct");
    dm(&m, 401, "in", KEY_A, 4001, "muted", "received");
    check("an outgoing message or a repeat never reaches the sound",
          rift_notify_poll(&n, &m, t + 200 * RIFT_NOTIFY_GAP_MS, 1) == 0);

    /* A clock that went backwards does not silence the next one for good. */
    dm(&m, 500, "in", KEY_A, 5000, "late", "received");
    check("a clock that stepped back still lets a sound through",
          rift_notify_poll(&n, &m, 5, 1) == 1);
}

/* ---- the sound seam ------------------------------------------------------ */

static int fake_plays;
static int fake_volume;
static int fake_kind = -1;
static int fake_stops;

static int fake_available(void)
{
    return 1;
}

static int fake_play(enum rift_sound_kind kind, int volume_percent)
{
    fake_plays++;
    fake_kind = kind;
    fake_volume = volume_percent;
    return 0;
}

static void fake_stop(void)
{
    fake_stops++;
}

static const struct rift_sound_backend fake = {
    .name = "fake",
    .available = fake_available,
    .play = fake_play,
    .stop = fake_stop,
    .why = "a test",
};

static void test_sound(void)
{
    check("the built-in backend is Doors's own audio path, pos-record",
          strcmp(rift_sound_backend_name(), "pos-record") == 0);
    rift_sound_set_backend(NULL);
    check("the silent backend has no sound", !rift_sound_available());
    check("and says why", strstr(rift_sound_why(), "No system notification sound") != NULL);
    check("asking it to play does nothing", rift_sound_play(RIFT_SOUND_DM, 80) == -1);
    rift_sound_stop();
    rift_sound_set_backend(&fake);
    check("a registered backend is used", strcmp(rift_sound_backend_name(), "fake") == 0 &&
                                              rift_sound_available());
    check("it plays the kind asked, at the volume given",
          rift_sound_play(RIFT_SOUND_CHANNEL, 70) == 0 && fake_plays == 1 && fake_volume == 70 &&
              fake_kind == RIFT_SOUND_CHANNEL);
    check("never at zero - muted is no sound",
          rift_sound_play(RIFT_SOUND_DM, 0) == -1 && fake_plays == 1);
    check("never above the system level",
          rift_sound_play(RIFT_SOUND_DM, 150) == 0 && fake_volume == 100 && fake_kind == RIFT_SOUND_DM);
    check("never a kind there is no sound for", rift_sound_play((enum rift_sound_kind)7, 50) == -1);
    rift_sound_stop();
    check("and is stopped when asked", fake_stops == 1);
    rift_sound_set_backend(NULL);
    check("NULL puts the silent one in", !rift_sound_available());
    rift_sound_set_backend(&rift_sound_pos_record);
}

/* ---- the two sounds, and the helper that plays them ----------------------- */

static int16_t tone[48000];

static void test_tones(void)
{
    size_t dm = rift_sound_tone_render(RIFT_SOUND_DM, tone, 48000);
    size_t ch;
    int peak_dm = 0;
    int peak_ch = 0;
    int crossings_lead = 0;
    size_t i;

    for (i = 0; i < dm; i++) {
        peak_dm = abs(tone[i]) > peak_dm ? abs(tone[i]) : peak_dm;
    }
    for (i = 0; i < 48000 * 30 / 1000; i++) {
        crossings_lead += tone[i] != 0;
    }
    check("the DM sound is short: under a quarter of a second", dm > 0 && dm < 48000 / 4);
    check("it starts with silence, for the amplifier", crossings_lead == 0);
    check("subtle: well under full scale", peak_dm > 3000 && peak_dm < 12000);
    check("and ends at rest, no click", abs(tone[dm - 1]) < 200);
    ch = rift_sound_tone_render(RIFT_SOUND_CHANNEL, tone, 48000);
    for (i = 0; i < ch; i++) {
        peak_ch = abs(tone[i]) > peak_ch ? abs(tone[i]) : peak_ch;
    }
    check("the channel sound is short too, and a different length",
          ch > 0 && ch < 48000 / 4 && ch != dm);
    check("and softer", peak_ch > 2000 && peak_ch < peak_dm);
    check("a buffer too small is refused", rift_sound_tone_render(RIFT_SOUND_DM, tone, 100) == 0);
    check("the length is asked for without a buffer",
          rift_sound_tone_render(RIFT_SOUND_DM, NULL, 0) == dm);
}

static int file_has(const char *path, const char *text)
{
    char buf[1024] = "";
    FILE *f = fopen(path, "r");
    size_t n = 0;

    if (f) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return strstr(buf, text) != NULL;
}

static void test_helper(void)
{
    char run[] = "/tmp/rift-sound-XXXXXX";
    char fake_helper[600];
    char log[600];
    struct pocketwav_info info;
    const char *dm;
    const char *ch;
    int fd;
    int waited;
    FILE *f;

    if (!mkdtemp(run)) {
        check("a temporary directory", 0);
        return;
    }
    setenv("POCKETOS_RUNTIME_DIR", run, 1);
    dm = rift_sound_wav_path(RIFT_SOUND_DM);
    check("the DM sound's file is written in RIFT's runtime directory",
          dm && strncmp(dm, run, strlen(run)) == 0 && strstr(dm, "/rift/dm.wav") != NULL);
    fd = dm ? open(dm, O_RDONLY) : -1;
    check("as a WAV pos-record plays: 48 kHz mono 16-bit",
          fd >= 0 && pocketwav_probe_fd(fd, &info) == POCKETWAV_OK && info.rate == 48000 &&
              info.channels == 1 && info.frames == rift_sound_tone_render(RIFT_SOUND_DM, NULL, 0));
    if (fd >= 0) {
        close(fd);
    }
    ch = rift_sound_wav_path(RIFT_SOUND_CHANNEL);
    check("and the channel sound's beside it", ch && strstr(ch, "/rift/channel.wav") != NULL);

    /* The helper: a stand-in for pos-record that writes down how it was
     * started and stays a moment, as a short sound does. */
    snprintf(fake_helper, sizeof(fake_helper), "%s/pos-record", run);
    snprintf(log, sizeof(log), "%s/helper.log", run);
    f = fopen(fake_helper, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\necho \"$@\" >> %s\nsleep 0.3\n", log);
        fclose(f);
        chmod(fake_helper, 0755);
    }
    setenv("POCKETOS_RECORD_HELPER", "/nonexistent/pos-record", 1);
    check("with no helper installed the backend says it cannot play",
          !rift_sound_available() && strstr(rift_sound_why(), "pos-record") != NULL);
    setenv("POCKETOS_RECORD_HELPER", fake_helper, 1);
    check("with the helper there, it can", rift_sound_available());
    check("a DM sound starts the helper and returns at once",
          rift_sound_play(RIFT_SOUND_DM, 40) == 0);
    check("a second while the first still sounds is refused, never stacked",
          rift_sound_play(RIFT_SOUND_CHANNEL, 40) == -1);
    for (waited = 0; waited < 3000 && !file_has(log, "dm.wav"); waited += 20) {
        usleep(20000);
    }
    check("pos-record play, at the system volume, on the DM file",
          file_has(log, "play --volume-percent 40 ") && file_has(log, "/rift/dm.wav"));
    usleep(400000);
    check("once it has finished the next is played",
          rift_sound_play(RIFT_SOUND_CHANNEL, 55) == 0);
    for (waited = 0; waited < 3000 && !file_has(log, "channel.wav"); waited += 20) {
        usleep(20000);
    }
    check("the channel sound, on its own file",
          file_has(log, "play --volume-percent 55 ") && file_has(log, "/rift/channel.wav"));
    rift_sound_stop();
    check("stop ends it, and another can start straight after",
          rift_sound_play(RIFT_SOUND_DM, 40) == 0);
    rift_sound_stop();
    unsetenv("POCKETOS_RECORD_HELPER");
    unsetenv("POCKETOS_RUNTIME_DIR");
}

/* ---- channel messages ---------------------------------------------------- */

static int chan(struct rift_model *m, int id, const char *dir, int slot, const char *name,
                const char *sender, long stamp, const char *text)
{
    char json[768];

    snprintf(json, sizeof(json),
             "{\"message\":{\"id\":%d,\"direction\":\"%s\",\"kind\":\"channel\",\"channel\":%d,"
             "\"channel_name\":\"%s\",\"channel_hash\":\"8c\",\"sender_name\":\"%s\","
             "\"text\":\"%s: %s\",\"state\":\"%s\",\"timestamp\":%ld}}",
             id, dir, slot, name, sender, sender, text,
             strcmp(dir, "in") == 0 ? "received" : "sent_flood", stamp);
    return event(m, "mesh.message", json);
}

static const char *muted_key;

static int muted_fake(const char *conv, void *user)
{
    (void)user;
    return muted_key && strcmp(conv, muted_key) == 0;
}

static void test_channels(void)
{
    static struct rift_model m;
    struct rift_notify n;
    char site[RIFT_KEY_HEX];
    char ops[RIFT_KEY_HEX];
    int64_t t = 5000000;

    rift_model_init(&m);
    check("the channel history on opening is taken",
          snapshot(&m, "{\"messages\":["
                       "{\"id\":1,\"direction\":\"in\",\"kind\":\"channel\",\"channel\":0,"
                       "\"channel_name\":\"SITE\",\"channel_hash\":\"8c\",\"sender_name\":\"X\","
                       "\"text\":\"X: old\",\"state\":\"received\",\"timestamp\":10}],"
                       "\"persistent\":false}") == 0);
    check("and none of it is an arrival", m.ch_arrivals == 0 && m.ch_high_id == 1);
    chan(&m, 2, "in", 0, "SITE", "Kari", 20, "hei alle");
    check("a live channel message is a channel arrival, not a DM",
          m.ch_arrivals == 1 && m.dm_arrivals == 0);
    snprintf(site, sizeof(site), "%s", rift_model_ch_arrival_conv(&m, 1));
    check("with the channel it came in on", site[0] == '#' && strstr(site, ":8c:") != NULL);
    chan(&m, 2, "in", 0, "SITE", "Kari", 20, "hei alle");
    check("the same event again is not a second one", m.ch_arrivals == 1 && m.ch_repeats >= 1);
    chan(&m, 3, "out", 0, "SITE", "K230-A", 21, "mine");
    check("this device's own channel message is never an arrival", m.ch_arrivals == 1);
    chan(&m, 4, "in", 0, "SITE", "Kari", 20, "hei alle");
    check("a repeater's second copy under a fresh id is not one either", m.ch_arrivals == 1);
    chan(&m, 5, "in", 2, "OPS", "Per", 30, "status");
    snprintf(ops, sizeof(ops), "%s", rift_model_ch_arrival_conv(&m, 2));
    check("another channel's message is, with its own key",
          m.ch_arrivals == 2 && strcmp(ops, site) != 0);

    /* The policy: two sounds, the channel one with its setting and mutes. */
    rift_model_init(&m);
    rift_notify_init(&n, &m, 1);
    check("channel sounds are on by default", n.ch_enabled == 1);
    rift_notify_set_channel(&n, 1, muted_fake, NULL);
    chan(&m, 10, "in", 0, "SITE", "Kari", 100, "a");
    check("a channel message plays the channel sound",
          rift_notify_poll(&n, &m, t, 1) == RIFT_NOTIFY_CHANNEL && n.ch_played == 1);
    dm(&m, 11, "in", KEY_A, 101, "dm", "received");
    chan(&m, 12, "in", 0, "SITE", "Kari", 102, "b");
    check("a DM and a channel message together are one sound, the DM's",
          rift_notify_poll(&n, &m, t + RIFT_NOTIFY_GAP_MS, 1) == RIFT_NOTIFY_DM && n.played == 1);
    chan(&m, 13, "in", 0, "SITE", "Kari", 103, "c");
    check("and nothing more inside the gap",
          rift_notify_poll(&n, &m, t + RIFT_NOTIFY_GAP_MS + 100, 1) == RIFT_NOTIFY_NONE);
    muted_key = site;
    {
        static struct rift_model q;
        struct rift_notify k;

        rift_model_init(&q);
        rift_notify_init(&k, &q, 1);
        rift_notify_set_channel(&k, 1, muted_fake, NULL);
        chan(&q, 1, "in", 0, "SITE", "Kari", 1, "muted one");
        check("a muted channel makes no sound",
              rift_notify_poll(&k, &q, t, 1) == RIFT_NOTIFY_NONE && k.ch_muted == 1);
        check("but its message is still received and unread",
              q.msg_count == 1 && rift_model_unread(&q, site) == 1);
        chan(&q, 2, "in", 2, "OPS", "Per", 2, "unmuted one");
        check("an unmuted channel still does",
              rift_notify_poll(&k, &q, t + 1, 1) == RIFT_NOTIFY_CHANNEL);
        chan(&q, 3, "in", 0, "SITE", "Kari", 3, "muted again");
        dm(&q, 4, "in", KEY_B, 4, "a dm", "received");
        check("a muted channel does not keep a DM quiet",
              rift_notify_poll(&k, &q, t + 2 * RIFT_NOTIFY_GAP_MS, 1) == RIFT_NOTIFY_DM);
        rift_notify_set_channel(&k, 0, muted_fake, NULL);
        chan(&q, 5, "in", 2, "OPS", "Per", 5, "off");
        check("channel sounds off: no channel sound, any channel",
              rift_notify_poll(&k, &q, t + 4 * RIFT_NOTIFY_GAP_MS, 1) == RIFT_NOTIFY_NONE &&
                  k.ch_off == 1);
        rift_notify_set_channel(&k, 1, muted_fake, NULL);
        check("and turning them on again plays nothing that arrived while off",
              rift_notify_poll(&k, &q, t + 5 * RIFT_NOTIFY_GAP_MS, 1) == RIFT_NOTIFY_NONE);
        dm(&q, 6, "in", KEY_B, 6, "dm while channels off", "received");
        rift_notify_set_channel(&k, 0, muted_fake, NULL);
        check("the channel setting does not silence a DM",
              rift_notify_poll(&k, &q, t + 6 * RIFT_NOTIFY_GAP_MS, 1) == RIFT_NOTIFY_DM);
        {
            int i;
            int sounds = 0;

            rift_notify_set_channel(&k, 1, muted_fake, NULL);
            for (i = 0; i < 20; i++) {
                chan(&q, 100 + i, "in", 2, "OPS", "Per", 100 + i, "burst");
                sounds += rift_notify_poll(&k, &q, t + 8 * RIFT_NOTIFY_GAP_MS + 100 * i, 1) != 0;
            }
            check("a burst of twenty channel messages is one sound", sounds == 1);
        }
    }
    muted_key = NULL;
}

/* ---- the preferences file ------------------------------------------------ */

static void test_store(void)
{
    struct rift_prefs p;
    char text[RIFT_STORE_FILE_MAX];
    char dir[] = "/tmp/rift-store-XXXXXX";
    char path[512];
    FILE *f;

    rift_prefs_defaults(&p);
    check("the DM sound is on by default", p.dm_sound == 1);
    check("and so is the channel sound, with no channel muted",
          p.ch_sound == 1 && p.mute_count == 0);
    check("channel sounds off is read as off",
          rift_prefs_parse(&p, "channel_sound=0\n") == 0 && p.ch_sound == 0);
    check("a channel sound value this build could not have written is refused",
          rift_prefs_parse(&p, "channel_sound=x\n") == 2 && p.ch_sound == 0);
    p.ch_sound = 1;
    check("a muted channel is read by its conversation key",
          rift_prefs_parse(&p, "channel_mute=#0:8c:1a2b3c4d\nchannel_mute=#2:4d:00ff00ff\n") == 0 &&
              p.mute_count == 2 && rift_prefs_channel_muted(&p, "#0:8c:1a2b3c4d") &&
              rift_prefs_channel_muted(&p, "#2:4d:00ff00ff"));
    check("the same one twice is one", rift_prefs_parse(&p, "channel_mute=#0:8c:1a2b3c4d\n") == 0 &&
                                         p.mute_count == 2);
    check("a key that is not a channel's is refused",
          rift_prefs_parse(&p, "channel_mute=" KEY_A "\n") == 4 && p.mute_count == 2);
    check("another channel in the same slot is not muted by it",
          !rift_prefs_channel_muted(&p, "#0:8c:99999999"));
    check("unmuting takes it off the list",
          rift_prefs_set_channel_muted(&p, "#2:4d:00ff00ff", 0) == 0 && p.mute_count == 1 &&
              !rift_prefs_channel_muted(&p, "#2:4d:00ff00ff"));
    {
        struct rift_prefs full;
        char key[32];
        int i;
        int ok = 1;

        rift_prefs_defaults(&full);
        for (i = 0; i < RIFT_PREF_MUTE_MAX; i++) {
            snprintf(key, sizeof(key), "#%d:%02x:0000%04x", i % 8, i, i);
            ok = ok && rift_prefs_set_channel_muted(&full, key, 1) == 0;
        }
        check("the list holds its bound", ok && full.mute_count == RIFT_PREF_MUTE_MAX);
        check("and refuses past it rather than dropping one",
              rift_prefs_set_channel_muted(&full, "#7:ff:ffffffff", 1) == -1);
        {
            char all[RIFT_STORE_FILE_MAX];
            struct rift_prefs back;

            rift_prefs_defaults(&back);
            check("a full list is written and read back whole",
                  rift_prefs_format(&full, all, sizeof(all)) > 0 &&
                      rift_prefs_parse(&back, all) == 0 && back.mute_count == RIFT_PREF_MUTE_MAX);
        }
        {
            char all[RIFT_STORE_FILE_MAX];
            struct rift_prefs back;

            /* The picker's recent emoji (rift_emoji_pick.h): the longest
             * value the field holds, beside a full mute list, still fits
             * the file. */
            memset(full.emoji_recent, 'x', sizeof(full.emoji_recent) - 1);
            full.emoji_recent[sizeof(full.emoji_recent) - 1] = '\0';
            rift_prefs_defaults(&back);
            check("the recent emoji are written and read back with a full mute list",
                  rift_prefs_format(&full, all, sizeof(all)) > 0 &&
                      rift_prefs_parse(&back, all) == 0 &&
                      strcmp(back.emoji_recent, full.emoji_recent) == 0 &&
                      back.mute_count == RIFT_PREF_MUTE_MAX);
        }
    }
    rift_prefs_defaults(&p);
    check("no recent emoji by default, and none written", p.emoji_recent[0] == '\0' &&
              rift_prefs_format(&p, text, sizeof(text)) > 0 &&
              strstr(text, RIFT_PREF_EMOJI_RECENT) == NULL);
    check("the recent emoji are read as written",
          rift_prefs_parse(&p, "emoji_recent=\xF0\x9F\x91\x8D \xE2\x9D\xA4\xEF\xB8\x8F\n") == 0 &&
              strcmp(p.emoji_recent, "\xF0\x9F\x91\x8D \xE2\x9D\xA4\xEF\xB8\x8F") == 0);
    check("a recent list with a control byte in it is refused",
          rift_prefs_parse(&p, "emoji_recent=a\tb\n") == 8 &&
              strcmp(p.emoji_recent, "\xF0\x9F\x91\x8D \xE2\x9D\xA4\xEF\xB8\x8F") == 0);
    check("and so is one longer than the field",
          rift_prefs_parse(&p, "emoji_recent=0123456789012345678901234567890123456789012345678\n") ==
                  8 &&
              strcmp(p.emoji_recent, "\xF0\x9F\x91\x8D \xE2\x9D\xA4\xEF\xB8\x8F") == 0);
    rift_prefs_defaults(&p);
    check("a file saying off is read as off",
          rift_prefs_parse(&p, "# c\ndm_sound=0\n") == 0 && p.dm_sound == 0);
    check("spaces around it are allowed", rift_prefs_parse(&p, " dm_sound = 1 \r\n") == 0 &&
                                              p.dm_sound == 1);
    check("a value this build could not have written is refused",
          rift_prefs_parse(&p, "dm_sound=yes\n") == 1 && p.dm_sound == 1);
    check("and an unknown key is ignored", rift_prefs_parse(&p, "volume=11\n") == 0 &&
                                               p.dm_sound == 1);
    p.dm_sound = 0;
    check("the text written is the text read",
          rift_prefs_format(&p, text, sizeof(text)) > 0 && strstr(text, "dm_sound=0\n") != NULL);
    check("a buffer too small is refused", rift_prefs_format(&p, text, 8) == -1);

    if (!mkdtemp(dir)) {
        check("a temporary directory", 0);
        return;
    }
    setenv("POCKETOS_STATE_DIR", dir, 1);
    check("the file lives in the state directory, under rift/",
          strncmp(rift_store_path(), dir, strlen(dir)) == 0 &&
              strstr(rift_store_path(), "/rift/prefs.v1") != NULL);
    check("no file is the defaults", rift_store_load(&p) == 1 && p.dm_sound == 1);
    p.dm_sound = 0;
    check("a choice is saved, creating the directory", rift_store_save(&p) == 0);
    rift_prefs_defaults(&p);
    check("and read back", rift_store_load(&p) == 0 && p.dm_sound == 0);
    p.ch_sound = 0;
    rift_prefs_set_channel_muted(&p, "#1:9a:12345678", 1);
    check("the channel setting and a mute are saved", rift_store_save(&p) == 0);
    rift_prefs_defaults(&p);
    check("and survive a reload", rift_store_load(&p) == 0 && p.ch_sound == 0 &&
                                      rift_prefs_channel_muted(&p, "#1:9a:12345678") &&
                                      p.dm_sound == 0);
    /* Somebody else's edit that this build cannot read leaves the default
     * standing, and the file where it is. */
    snprintf(path, sizeof(path), "%s", rift_store_path());
    f = fopen(path, "w");
    if (f) {
        fputs("dm_sound=2\n", f);
        fclose(f);
    }
    check("an unusable value keeps the default", rift_store_load(&p) == 0 && p.dm_sound == 1);
    check("and the file is not rewritten behind the reader's back", access(path, F_OK) == 0);
    unlink(path);
    {
        char sub[600];

        snprintf(sub, sizeof(sub), "%s/rift", dir);
        rmdir(sub);
    }
    rmdir(dir);
    /* A state directory nobody can write to: saving fails and says so. */
    setenv("POCKETOS_STATE_DIR", "/proc/rift-cannot-write-here", 1);
    check("a directory that cannot be made fails the save",
          rift_store_save(&p) == -1);
    unsetenv("POCKETOS_STATE_DIR");
}

/* ---- the activity measure ------------------------------------------------- */

static void test_pulse(void)
{
    static struct rift_model m;
    struct rift_conv conv[RIFT_MAX_CONVERSATIONS];
    int64_t ms = 0;
    int rx = -1;
    int tx = -1;
    int at_least = -1;
    int n;
    int i;

    check("never heard is unknown, not stale", rift_pulse_of(0, 0) == RIFT_PULSE_NONE);
    check("a stamp in the future is unknown, not now", rift_pulse_of(-1, 1) == RIFT_PULSE_NONE);
    check("just heard is NOW", rift_pulse_of(0, 1) == RIFT_PULSE_NOW);
    check("five minutes is still NOW", rift_pulse_of(RIFT_PULSE_NOW_MS, 1) == RIFT_PULSE_NOW);
    check("a moment later is RECENT",
          rift_pulse_of(RIFT_PULSE_NOW_MS + 1, 1) == RIFT_PULSE_RECENT);
    check("an hour is RECENT", rift_pulse_of(RIFT_PULSE_RECENT_MS, 1) == RIFT_PULSE_RECENT);
    check("past an hour is QUIET",
          rift_pulse_of(RIFT_PULSE_RECENT_MS + 1, 1) == RIFT_PULSE_QUIET);
    check("QUIET ends where NODES' stale group starts",
          rift_pulse_of(RIFT_STALE_MS, 1) == RIFT_PULSE_QUIET &&
              rift_pulse_of(RIFT_STALE_MS + 1, 1) == RIFT_PULSE_STALE);
    check("the words", strcmp(rift_pulse_word(RIFT_PULSE_NOW), "NOW") == 0 &&
                           strcmp(rift_pulse_word(RIFT_PULSE_RECENT), "RECENT") == 0 &&
                           strcmp(rift_pulse_word(RIFT_PULSE_QUIET), "QUIET") == 0 &&
                           strcmp(rift_pulse_word(RIFT_PULSE_STALE), "STALE") == 0 &&
                           strcmp(rift_pulse_word(RIFT_PULSE_NONE), RIFT_UNKNOWN) == 0);

    /* A conversation is heard from when the other side says something, or
     * when its node is heard - never when this device talks. */
    rift_model_init(&m);
    dm(&m, 1, "out", KEY_A, 1, "only mine", "sent_direct");
    n = rift_model_conversations(&m, conv, RIFT_MAX_CONVERSATIONS);
    check("a conversation of only our own messages", n == 1);
    check("has not been heard from", rift_model_conv_heard(&m, &conv[0], &ms) == 0);
    dm(&m, 2, "in", KEY_A, 2, "theirs", "received");
    n = rift_model_conversations(&m, conv, RIFT_MAX_CONVERSATIONS);
    check("their message is when it was heard from",
          rift_model_conv_heard(&m, &conv[0], &ms) == 1 && ms == 2000);
    {
        cJSON *o = cJSON_Parse("{\"nodes\":[{\"public_key\":\"" KEY_A "\",\"name\":\"A\","
                               "\"path_known\":false,\"last_heard_mono_ms\":9000}]}");

        rift_model_apply_nodes(&m, o);
        cJSON_Delete(o);
    }
    n = rift_model_conversations(&m, conv, RIFT_MAX_CONVERSATIONS);
    check("a node heard since is the later observation",
          rift_model_conv_heard(&m, &conv[0], &ms) == 1 && ms == 9000);

    /* The mesh as a whole: frames in the ring within a window. */
    rift_model_init(&m);
    for (i = 0; i < 10; i++) {
        char json[128];

        snprintf(json, sizeof(json), "{\"kind\":\"%s\",\"result\":\"ok\",\"payload_type\":\"advert\","
                                     "\"mono_ms\":%d}", i % 5 == 0 ? "tx" : "rx", 1000 * i);
        event(&m, "mesh.activity", json);
    }
    event(&m, "mesh.activity", "{\"kind\":\"rx\",\"payload_type\":\"advert\"}");
    rift_model_recent_frames(&m, 9000, 4000, &rx, &tx, &at_least);
    check("frames within the window are counted by direction", rx == 4 && tx == 1);
    check("a ring that is not full is the whole count", at_least == 0);
    for (i = 0; i < RIFT_MAX_ACTIVITY; i++) {
        char json[96];

        snprintf(json, sizeof(json), "{\"kind\":\"rx\",\"payload_type\":\"txt\",\"mono_ms\":%d}",
                 20000 + i);
        event(&m, "mesh.activity", json);
    }
    rift_model_recent_frames(&m, 20000 + RIFT_MAX_ACTIVITY, 60000, &rx, &tx, &at_least);
    check("a full ring inside the window says it may be more",
          rx == RIFT_MAX_ACTIVITY && at_least == 1);
}

/* ---- the path chain, and a route too long to write whole ------------------ */

static void test_long_chain(void)
{
    struct rift_node n;
    struct rift_path p;
    char chain[RIFT_CHAIN_MAX];
    char small[96];
    int i;

    memset(&n, 0, sizeof(n));
    n.path_known = 1;
    n.hops = 63;
    for (i = 0; i < 63; i++) {
        snprintf(n.path_hex + 2 * i, 3, "%02x", i);
    }
    check("a 63-hop path parses", rift_path_parse(&n, &p) == 0 && p.count == 63);
    rift_path_chain("K230-A", &p, "FAR-AWAY", NULL, NULL, chain, sizeof(chain));
    check("written whole when it fits", strstr(chain, "K230-A") == chain &&
                                            strstr(chain, " 3e " "\xE2\x80\xBA" " FAR-AWAY") !=
                                                NULL &&
                                            strstr(chain, "+") == NULL);
    rift_path_chain("K230-A", &p, "FAR-AWAY", NULL, NULL, small, sizeof(small));
    check("too long, both ends are kept", strncmp(small, "K230-A", 6) == 0 &&
                                              strstr(small, "FAR-AWAY") != NULL);
    check("with the last relay before the target", strstr(small, "3e") != NULL);
    check("and the middle counted, not dropped silently",
          strstr(small, "\xE2\x80\xA6 +") != NULL);
    check("inside the buffer", strlen(small) < sizeof(small));
}

/* ---- more peers than the list holds ---------------------------------------- */

static void test_many_peers(void)
{
    static struct rift_model m;
    static struct rift_conv conv[RIFT_MAX_CONVERSATIONS];
    char key[RIFT_KEY_HEX];
    int n;
    int i;
    int oldest_in = 0;
    int newest_in = 0;

    /* More peers than the list holds, one message each, peer i the i-th to
     * speak, counted in the key's first two bytes. */
    const int peers = RIFT_MAX_CONVERSATIONS + 36;

    rift_model_init(&m);
    for (i = 0; i < peers; i++) {
        snprintf(key, sizeof(key), "%04x%060d", 0x20 + i, 0);
        dm(&m, 1 + i, "in", key, 100 + i, "hi", "received");
    }
    n = rift_model_conversations(&m, conv, RIFT_MAX_CONVERSATIONS);
    check("a list of more peers than it holds is full", n == RIFT_MAX_CONVERSATIONS);
    for (i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "%04x%060d", 0x20 + peers - 1, 0);
        newest_in |= strcmp(conv[i].key, key) == 0;
        snprintf(key, sizeof(key), "%04x%060d", 0x20 + 0, 0);
        oldest_in |= strcmp(conv[i].key, key) == 0;
    }
    check("and holds the peer who spoke last", newest_in);
    check("not the one who spoke longest ago", !oldest_in);
    snprintf(key, sizeof(key), "%04x%060d", 0x20 + peers - 1, 0);
    check("newest first", strcmp(conv[0].key, key) == 0);
    /* The one who spoke longest ago speaks again, and is back in. */
    snprintf(key, sizeof(key), "%04x%060d", 0x20 + 0, 0);
    /* An id past every one used above: a repeated id is an update, not a
     * message. */
    dm(&m, peers + 100, "in", key, 300, "again", "received");
    n = rift_model_conversations(&m, conv, RIFT_MAX_CONVERSATIONS);
    check("a peer who speaks again comes back, at the top", strcmp(conv[0].key, key) == 0);
    check("with every message it has held counted", conv[0].total == 2 && conv[0].unread == 2);
}

int main(void)
{
    test_arrivals();
    test_policy();
    test_sound();
    test_tones();
    test_helper();
    test_channels();
    test_store();
    test_pulse();
    test_long_chain();
    test_many_peers();
    printf("rift_notify_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
