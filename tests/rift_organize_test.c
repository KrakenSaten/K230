/*
 * COMMS' three groups (apps/rift/rift_comms_group.h) and CONTACTS' address
 * book (rift_contacts.h): favourites, the ALL / FAVORITES / RECENT filters,
 * repeaters and sensors kept out, and the favourites kept in RIFT's
 * preferences file (rift_store.h) across a restart of the app.
 *
 * No LVGL and no socket: the model is fed as the service would feed it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_comms_group.h"
#include "rift_contacts.h"
#include "rift_model.h"
#include "rift_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    checks++;
    failed += !ok;
}

#define KEY_CHAT "a111111111111111111111111111111111111111111111111111111111111111"
#define KEY_ROOM "b222222222222222222222222222222222222222222222222222222222222222"
#define KEY_RPT "c333333333333333333333333333333333333333333333333333333333333333"
#define KEY_SNS "d444444444444444444444444444444444444444444444444444444444444444"
#define KEY_UNK "e555555555555555555555555555555555555555555555555555555555555555"
#define KEY_NEW "f666666666666666666666666666666666666666666666666666666666666666"
#define KEY_ROOM2 "0777777777777777777777777777777777777777777777777777777777777777"

static void add_node(struct rift_model *m, const char *key, const char *name, int type)
{
    char json[512];
    cJSON *o;

    /* type 0: the table has not said what it is. */
    snprintf(json, sizeof(json),
             "{\"node\":{\"public_key\":\"%s\",\"node_hash\":\"%.2s\",\"name\":\"%s\"%s%d,"
             "\"path_known\":false,\"last_heard_mono_ms\":1000},\"reason\":\"discovered\"}",
             key, key, name, type ? ",\"type\":" : ",\"x\":", type);
    o = cJSON_Parse(json);
    rift_model_apply_event(m, "mesh.node", o);
    cJSON_Delete(o);
}

static int message(struct rift_model *m, int id, const char *dir, const char *peer, int64_t mono)
{
    char json[512];
    cJSON *o;
    int rc;

    snprintf(json, sizeof(json),
             "{\"id\":%d,\"direction\":\"%s\",\"kind\":\"direct\",\"peer_public_key\":\"%s\","
             "\"text\":\"m%d\",\"state\":\"%s\",\"mono_ms\":%lld}",
             id, dir, peer, id, dir[0] == 'i' ? "received" : "sent_direct", (long long)mono);
    o = cJSON_Parse(json);
    rc = rift_model_apply_message(m, o);
    cJSON_Delete(o);
    return rc;
}

static int channel_message(struct rift_model *m, int id, int slot, const char *hash,
                           const char *name, int64_t mono)
{
    char json[512];
    cJSON *o;
    int rc;

    snprintf(json, sizeof(json),
             "{\"id\":%d,\"direction\":\"in\",\"kind\":\"channel\",\"channel\":%d,"
             "\"channel_name\":\"%s\",\"channel_hash\":\"%s\",\"sender_name\":\"X\","
             "\"text\":\"X: hi\",\"state\":\"received\",\"ack_expected\":false,\"mono_ms\":%lld}",
             id, slot, name, hash, (long long)mono);
    o = cJSON_Parse(json);
    rc = rift_model_apply_message(m, o);
    cJSON_Delete(o);
    return rc;
}

static int index_of(const struct rift_conv *c, int n, const char *key)
{
    int i;

    for (i = 0; i < n; i++) {
        if (strcmp(c[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---- COMMS -------------------------------------------------------------------- */

static void test_groups(void)
{
    static struct rift_model m;
    static struct rift_conv conv[RIFT_MAX_CONVERSATIONS];
    char pub[RIFT_KEY_HEX];
    char ops[RIFT_KEY_HEX];
    int counts[RIFT_GROUP_COUNT];
    int unread[RIFT_GROUP_COUNT];
    int n;
    int kept;
    cJSON *o;

    rift_model_init(&m);
    o = cJSON_Parse("{\"channels\":["
                    "{\"channel\":0,\"name\":\"OPS\",\"channel_hash\":\"4d\",\"key_bits\":128,"
                    "\"text_limit\":150},"
                    "{\"channel\":1,\"name\":\"torget\",\"channel_hash\":\"11\",\"key_bits\":128,"
                    "\"well_known\":\"public\",\"text_limit\":150}],\"count\":2,\"max\":8}");
    rift_model_apply_channels(&m, o);
    cJSON_Delete(o);
    rift_channel_key(1, "11", "torget", pub, sizeof(pub));
    rift_channel_key(0, "4d", "OPS", ops, sizeof(ops));
    add_node(&m, KEY_CHAT, "Anna", 1);
    add_node(&m, KEY_ROOM, "Hytta Room", 3);
    add_node(&m, KEY_UNK, "Room-ish name", 0);
    add_node(&m, KEY_ROOM2, "Club", 3);

    /* Oldest first: Public spoke first, so newest-first puts it last. */
    check("the fixture's messages are taken",
          channel_message(&m, 1, 1, "11", "torget", 1000) == 0 &&
              message(&m, 2, "in", KEY_ROOM, 2000) == 0 &&
              message(&m, 3, "out", KEY_CHAT, 3000) == 0 &&
              channel_message(&m, 4, 0, "4d", "OPS", 4000) == 0 &&
              message(&m, 5, "in", KEY_UNK, 5000) == 0 &&
              message(&m, 6, "out", KEY_ROOM2, 6000) == 0 &&
              message(&m, 7, "in", KEY_NEW, 7000) == 0);
    /* Read everything but the oldest room's and the unknown's. */
    rift_model_mark_read(&m, KEY_ROOM2);
    rift_model_mark_read(&m, KEY_CHAT);
    rift_model_mark_read(&m, KEY_NEW);
    rift_model_mark_read(&m, pub);
    rift_model_mark_read(&m, ops);
    n = rift_model_conversations(&m, conv, RIFT_MAX_CONVERSATIONS);
    check("seven conversations, newest first", n == 7 && strcmp(conv[0].key, KEY_NEW) == 0);

    check("a channel is a channel", rift_conv_group_of(&m, &conv[index_of(conv, n, pub)]) ==
                                        RIFT_GROUP_CHANNELS);
    check("a room server by its advertised type is a room",
          rift_conv_group_of(&m, &conv[index_of(conv, n, KEY_ROOM)]) == RIFT_GROUP_ROOMS);
    check("a node of unknown type is a direct message, never a room on a guess",
          rift_conv_group_of(&m, &conv[index_of(conv, n, KEY_UNK)]) == RIFT_GROUP_DIRECT);
    check("a peer the table does not hold is a direct message",
          rift_conv_group_of(&m, &conv[index_of(conv, n, KEY_NEW)]) == RIFT_GROUP_DIRECT);

    kept = rift_conv_group_sort(&m, conv, n, counts, unread);
    check("every conversation is kept, once", kept == 7);
    check("two channels, two direct messages... and the rest rooms",
          counts[RIFT_GROUP_CHANNELS] == 2 && counts[RIFT_GROUP_DIRECT] == 3 &&
              counts[RIFT_GROUP_ROOMS] == 2);
    check("CHANNELS first, Public first of them", strcmp(conv[0].key, pub) == 0 &&
                                                      strcmp(conv[1].key, ops) == 0);
    check("DIRECT MESSAGES next, unread first, then the most recent",
          strcmp(conv[2].key, KEY_UNK) == 0 && strcmp(conv[3].key, KEY_NEW) == 0 &&
              strcmp(conv[4].key, KEY_CHAT) == 0);
    check("CHAT ROOMS last, unread first",
          strcmp(conv[5].key, KEY_ROOM) == 0 && strcmp(conv[6].key, KEY_ROOM2) == 0);
    check("the unread in each group is counted",
          unread[RIFT_GROUP_CHANNELS] == 0 && unread[RIFT_GROUP_DIRECT] == 1 &&
              unread[RIFT_GROUP_ROOMS] == 1);
    check("a row is the same row: its unread and its preview are unchanged",
          conv[2].unread == 1 && conv[2].newest && strcmp(conv[2].newest->text, "m5") == 0);
    kept = rift_conv_group_sort(&m, conv, kept, counts, unread);
    check("sorting again moves nothing", kept == 7 && strcmp(conv[0].key, pub) == 0 &&
                                             strcmp(conv[2].key, KEY_UNK) == 0 &&
                                             strcmp(conv[6].key, KEY_ROOM2) == 0);

    /* The same conversation twice - a row COMMS added for the open peer and
     * the model's own - is one row. */
    conv[kept] = conv[3];
    kept = rift_conv_group_sort(&m, conv, kept + 1, counts, NULL);
    check("a duplicate key is one row", kept == 7 && counts[RIFT_GROUP_DIRECT] == 3);

    /* A node that adverts as a room later moves group; one that stops being
     * known as one does not become a room on a guess. */
    add_node(&m, KEY_UNK, "Now a room", 3);
    kept = rift_conv_group_sort(&m, conv, kept, counts, NULL);
    check("the group follows the advertised type", counts[RIFT_GROUP_ROOMS] == 3 &&
                                                       counts[RIFT_GROUP_DIRECT] == 2);
    check("the group words", strcmp(rift_conv_group_word(RIFT_GROUP_CHANNELS), "CHANNELS") == 0 &&
                                 strcmp(rift_conv_group_word(RIFT_GROUP_DIRECT), "DIRECT MESSAGES") == 0 &&
                                 strcmp(rift_conv_group_word(RIFT_GROUP_ROOMS), "CHAT ROOMS") == 0);
    check("nothing to sort is nothing", rift_conv_group_sort(&m, NULL, 3, counts, unread) == 0 &&
                                            counts[0] == 0 && unread[0] == 0);
}

/* ---- CONTACTS ------------------------------------------------------------------- */

static void test_favorites(void)
{
    static struct rift_model m;
    static struct rift_prefs p;
    const struct rift_node *out[RIFT_MAX_NODES];
    char upper[65];
    int n;
    int i;

    rift_model_init(&m);
    rift_prefs_defaults(&p);
    add_node(&m, KEY_CHAT, "Zulu", 1);
    add_node(&m, KEY_ROOM, "Hytta Room", 3);
    add_node(&m, KEY_RPT, "Alpha RPT", 2);
    add_node(&m, KEY_SNS, "Bravo SNS", 4);
    add_node(&m, KEY_UNK, "Mike", 0);
    add_node(&m, KEY_NEW, "alpha", 1);

    n = rift_contacts_list(&m, &p, RIFT_CONTACTS_ALL, "", out, RIFT_MAX_NODES);
    check("ALL leaves the repeater and the sensor out", n == 4);
    check("and is A to Z with no favourites",
          n == 4 && strcmp(out[0]->name, "alpha") == 0 && strcmp(out[1]->name, "Hytta Room") == 0 &&
              strcmp(out[2]->name, "Mike") == 0 && strcmp(out[3]->name, "Zulu") == 0);
    check("a room server is a contact, and a node of unknown type is too",
          rift_contacts_is_contact(rift_model_find(&m, KEY_ROOM)) &&
              rift_contacts_is_contact(rift_model_find(&m, KEY_UNK)) &&
              !rift_contacts_is_contact(rift_model_find(&m, KEY_RPT)) &&
              !rift_contacts_is_contact(rift_model_find(&m, KEY_SNS)) && !rift_contacts_is_contact(NULL));
    check("a repeater is not found by name", rift_contacts_list(&m, &p, RIFT_CONTACTS_ALL, "alpha rpt",
                                                                out, RIFT_MAX_NODES) == 0);
    check("nor by its key", rift_contacts_list(&m, &p, RIFT_CONTACTS_ALL, "c333", out,
                                                RIFT_MAX_NODES) == 0);
    check("nor a sensor", rift_contacts_list(&m, &p, RIFT_CONTACTS_ALL, "d444", out,
                                              RIFT_MAX_NODES) == 0);
    n = rift_contacts_list(&m, &p, RIFT_CONTACTS_FAVORITES, "", out, RIFT_MAX_NODES);
    check("FAVORITES is empty before any star", n == 0);

    /* Stars on Zulu and the room, by whole key; one given in capitals. */
    for (i = 0; i < 64; i++) {
        upper[i] = (char)(KEY_ROOM[i] >= 'a' ? KEY_ROOM[i] - 'a' + 'A' : KEY_ROOM[i]);
    }
    upper[64] = '\0';
    check("a whole key is a favourite", rift_prefs_set_favorite(&p, KEY_CHAT, 1) == 0 &&
                                            rift_prefs_favorite(&p, KEY_CHAT));
    check("either case, kept lowercase", rift_prefs_set_favorite(&p, upper, 1) == 0 &&
                                             rift_prefs_favorite(&p, KEY_ROOM) &&
                                             strcmp(p.fav[1], KEY_ROOM) == 0);
    check("twice is once", rift_prefs_set_favorite(&p, KEY_CHAT, 1) == 0 && p.fav_count == 2);
    check("a hash prefix is not an identity", rift_prefs_set_favorite(&p, "a1", 1) == -1 &&
                                                  rift_prefs_set_favorite(&p, "a1111111", 1) == -1);
    check("nor is a key with a stray character",
          rift_prefs_set_favorite(&p, "g111111111111111111111111111111111111111111111111111111111111111", 1) == -1 &&
              rift_prefs_set_favorite(&p, KEY_CHAT "0", 1) == -1 && rift_prefs_set_favorite(&p, NULL, 1) == -1);

    n = rift_contacts_list(&m, &p, RIFT_CONTACTS_ALL, "", out, RIFT_MAX_NODES);
    check("ALL: the favourites first, A to Z, then the rest A to Z",
          n == 4 && strcmp(out[0]->name, "Hytta Room") == 0 && strcmp(out[1]->name, "Zulu") == 0 &&
              strcmp(out[2]->name, "alpha") == 0 && strcmp(out[3]->name, "Mike") == 0);
    n = rift_contacts_list(&m, &p, RIFT_CONTACTS_FAVORITES, "", out, RIFT_MAX_NODES);
    check("FAVORITES: the favourites alone", n == 2 && strcmp(out[0]->name, "Hytta Room") == 0 &&
                                                  strcmp(out[1]->name, "Zulu") == 0);
    n = rift_contacts_list(&m, &p, RIFT_CONTACTS_FAVORITES, "zu", out, RIFT_MAX_NODES);
    check("searched by name", n == 1 && strcmp(out[0]->key, KEY_CHAT) == 0);
    n = rift_contacts_list(&m, &p, RIFT_CONTACTS_ALL, "b222", out, RIFT_MAX_NODES);
    check("and by key prefix", n == 1 && strcmp(out[0]->key, KEY_ROOM) == 0);
    check("a repeater made a favourite by its key is still not a contact",
          rift_prefs_set_favorite(&p, KEY_RPT, 1) == 0 &&
              rift_contacts_list(&m, &p, RIFT_CONTACTS_FAVORITES, "", out, RIFT_MAX_NODES) == 2);
    rift_prefs_set_favorite(&p, KEY_RPT, 0);

    /* RECENT: by conversation, never a repeater. */
    message(&m, 1, "in", KEY_UNK, 1000);
    message(&m, 2, "out", KEY_CHAT, 2000);
    message(&m, 3, "in", KEY_RPT, 3000);
    n = rift_contacts_list(&m, &p, RIFT_CONTACTS_RECENT, "", out, RIFT_MAX_NODES);
    check("RECENT: latest spoken with first, the repeater left out",
          n == 2 && strcmp(out[0]->key, KEY_CHAT) == 0 && strcmp(out[1]->key, KEY_UNK) == 0);

    /* Forgotten (FORGET in NODES, mesh.node "removed"): a favourite outlives
     * its node in the table. */
    {
        cJSON *o = cJSON_Parse("{\"node\":{\"public_key\":\"" KEY_CHAT "\",\"node_hash\":\"a1\","
                               "\"name\":\"Zulu\",\"type\":1,\"path_known\":false},"
                               "\"reason\":\"removed\"}");

        rift_model_apply_event(&m, "mesh.node", o);
        cJSON_Delete(o);
    }
    check("a favourite whose node is gone is not listed",
          rift_model_find(&m, KEY_CHAT) == NULL &&
              rift_contacts_list(&m, &p, RIFT_CONTACTS_FAVORITES, "", out, RIFT_MAX_NODES) == 1);
    check("is counted as absent", rift_contacts_favorites_absent(&m, &p) == 1);
    check("and is still a favourite", rift_prefs_favorite(&p, KEY_CHAT));
    add_node(&m, KEY_CHAT, "Zulu", 1);
    check("heard again, it is a favourite again",
          rift_contacts_list(&m, &p, RIFT_CONTACTS_FAVORITES, "", out, RIFT_MAX_NODES) == 2 &&
              rift_contacts_favorites_absent(&m, &p) == 0);
    check("nothing about any of it changed the contact table's types",
          rift_model_find(&m, KEY_ROOM)->type == 3);

    check("unmaking one", rift_prefs_set_favorite(&p, KEY_ROOM, 0) == 0 && !rift_prefs_favorite(&p, KEY_ROOM) &&
                              p.fav_count == 1);
    check("unmaking one that is not is nothing", rift_prefs_set_favorite(&p, KEY_ROOM, 0) == 0 &&
                                                     p.fav_count == 1);
    check("the filter words", strcmp(rift_contacts_filter_word(RIFT_CONTACTS_ALL), "ALL") == 0 &&
                                  strcmp(rift_contacts_filter_word(RIFT_CONTACTS_FAVORITES), "FAVORITES") == 0 &&
                                  strcmp(rift_contacts_filter_word(RIFT_CONTACTS_RECENT), "RECENT") == 0);
    check("NULL favourites are none", rift_contacts_list(&m, NULL, RIFT_CONTACTS_FAVORITES, "", out,
                                                         RIFT_MAX_NODES) == 0 &&
                                          rift_contacts_favorites_absent(&m, NULL) == 0);
}

static void test_favorites_file(void)
{
    static struct rift_prefs p;
    static struct rift_prefs back;
    char text[RIFT_STORE_FILE_MAX];
    char key[65];
    char dir[] = "/tmp/rift_organize_XXXXXX";
    int len;
    int i;

    rift_prefs_defaults(&p);
    rift_prefs_set_favorite(&p, KEY_CHAT, 1);
    rift_prefs_set_favorite(&p, KEY_ROOM, 1);
    len = rift_prefs_format(&p, text, sizeof(text));
    check("the favourites are written, a whole key a line",
          len > 0 && strstr(text, "favorite=" KEY_CHAT "\n") && strstr(text, "favorite=" KEY_ROOM "\n"));
    rift_prefs_defaults(&back);
    check("and read back as they were", rift_prefs_parse(&back, text) == 0 && back.fav_count == 2 &&
                                            rift_prefs_favorite(&back, KEY_CHAT) &&
                                            rift_prefs_favorite(&back, KEY_ROOM));
    check("a line that is not a whole key is refused, and the rest kept",
          (rift_prefs_parse(&back, "favorite=a1\nfavorite=" KEY_UNK "\n") & 32) != 0 &&
              back.fav_count == 3 && rift_prefs_favorite(&back, KEY_UNK));
    rift_prefs_defaults(&back);
    rift_prefs_parse(&back, "dm_sound=1\n");
    check("a file with no favourites has none", back.fav_count == 0);

    /* Full: the most a file holds, and a full list says so. */
    rift_prefs_defaults(&p);
    for (i = 0; i < RIFT_PREF_FAV_MAX; i++) {
        snprintf(key, sizeof(key), "%064x", i + 1);
        check("a favourite is kept", rift_prefs_set_favorite(&p, key, 1) == 0);
    }
    snprintf(key, sizeof(key), "%064x", 9999);
    check("one past the most is refused", rift_prefs_set_favorite(&p, key, 1) == -1 &&
                                              p.fav_count == RIFT_PREF_FAV_MAX);
    for (i = 0; i < RIFT_PREF_MUTE_MAX; i++) {
        char mute[40];

        snprintf(mute, sizeof(mute), "#%d:ab:%08x", i % 8, (unsigned)i * 2654435761u);
        rift_prefs_set_channel_muted(&p, mute, 1);
    }
    snprintf(p.emoji_recent, sizeof(p.emoji_recent), "%s", "\xF0\x9F\x98\x80 \xF0\x9F\x91\x8D");
    p.basemap = 1;
    len = rift_prefs_format(&p, text, sizeof(text));
    check("a file with everything at its most still fits", len > 0 && len < RIFT_STORE_FILE_MAX);

    /* Persistence: written, and read by the next session. */
    if (!mkdtemp(dir)) {
        check("a temporary state directory", 0);
        return;
    }
    setenv("POCKETOS_STATE_DIR", dir, 1);
    check("saved", rift_store_save(&p) == 0);
    rift_prefs_defaults(&back);
    check("loaded by the next session, every favourite in its order",
          rift_store_load(&back) == 0 && back.fav_count == RIFT_PREF_FAV_MAX &&
              memcmp(back.fav, p.fav, sizeof(p.fav)) == 0 && back.mute_count == p.mute_count &&
              back.basemap == 1);
    snprintf(key, sizeof(key), "%s", back.fav[0]);
    rift_prefs_set_favorite(&back, key, 0);
    check("unstarred and saved", rift_store_save(&back) == 0);
    rift_prefs_defaults(&p);
    check("is unstarred when read again, and the others are not",
          rift_store_load(&p) == 0 && p.fav_count == RIFT_PREF_FAV_MAX - 1 &&
              !rift_prefs_favorite(&p, key) && rift_prefs_favorite(&p, back.fav[0]));
    unlink(rift_store_path());
    {
        char sub[sizeof(dir) + 16];

        snprintf(sub, sizeof(sub), "%s/%s", dir, RIFT_STORE_SUBDIR);
        rmdir(sub);
    }
    rmdir(dir);
}

int main(void)
{
    test_groups();
    test_favorites();
    test_favorites_file();
    printf("rift_organize_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
