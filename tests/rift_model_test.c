/*
 * RIFT's model: what it keeps, what it refuses, and what it never invents.
 *
 * mesh.* is protocol-oriented and leaves out what nobody measured
 * (docs/api/mesh.md). The failures worth testing are all one mistake in two
 * directions: filling a gap in - a missing RSSI read as 0 dBm, a missing hop
 * count read as 0 hops, a node the service has forgotten still on screen -
 * or throwing away something real, which is what a second row for a node
 * that adverted twice would do to a list somebody is counting.
 *
 * No LVGL, no socket, no service.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_model.h"

#include "rift_format.h"

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
#define KEY_C "c3beef1e7d0411223344556677889900aabbccddeeff001122334455667788b3"
#define KEY_SELF "5f000000000000000000000000000000000000000000000000000000000000ff"

/* Apply a JSON literal, and say whether the model took it. */
static int apply_nodes(struct rift_model *m, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_nodes(m, o);

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

static int apply_status(struct rift_model *m, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_status(m, o);

    cJSON_Delete(o);
    return rc;
}

static int apply_identity(struct rift_model *m, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_model_apply_identity(m, o);

    cJSON_Delete(o);
    return rc;
}

int main(void)
{
    struct rift_model m;
    const struct rift_node *n;
    const struct rift_node *order[RIFT_MAX_NODES];
    const struct rift_activity *act;
    int count;
    int i;

    /* ---- an empty model knows nothing, and says so ---------------------- */
    rift_model_init(&m);
    check("an empty model holds no nodes", m.node_count == 0);
    check("has no identity", !m.have_identity);
    check("has read no snapshot", !m.snapshot_valid);
    check("and its service state is unknown, not absent",
          m.state == RIFT_SVC_UNKNOWN && !m.stale);

    /* ---- the initial snapshot -------------------------------------------- */
    check("a snapshot is taken",
          apply_nodes(&m, "{\"count\":2,\"nodes\":["
                          "{\"public_key\":\"" KEY_A "\",\"node_hash\":\"a1\",\"name\":\"OSLO-01\","
                          "\"type\":1,\"path_known\":true,\"hops\":0,\"direct\":true,"
                          "\"last_heard_mono_ms\":1000,\"last_rssi_dbm\":-71.0,"
                          "\"last_snr_db\":9.5},"
                          "{\"public_key\":\"" KEY_B "\",\"node_hash\":\"b2\",\"name\":\"HYTTA\","
                          "\"type\":2,\"path_known\":true,\"hops\":3,\"direct\":false,"
                          "\"path_hex\":\"a1c2d3\",\"last_heard_mono_ms\":500}]}") == 0);
    check("both nodes are held", m.node_count == 2);
    check("and the snapshot is now valid", m.snapshot_valid && !m.stale);

    n = rift_model_find(&m, KEY_A);
    check("a node is found by its public key", n != NULL);
    text_is("with the name the service gave", n->name, "OSLO-01");
    check("its measured RSSI is kept", n->have_rssi && n->rssi_dbm < -70.0);
    check("its measured SNR is kept", n->have_snr);
    check("zero relays is direct", n->path_known && n->direct && n->hops == 0);
    check("and an unreported advert time stays unreported", !n->have_advert);

    n = rift_model_find(&m, KEY_B);
    check("the relayed node is held too", n != NULL);
    check("with its hop count", n->hops == 3 && !n->direct);
    /* The one that matters: this node's frame carried no signal metadata,
     * and nothing here turns that into 0 dBm at full scale. */
    check("an RSSI the service did not report is absent, not zero", !n->have_rssi);
    check("and so is the SNR", !n->have_snr);

    /* ---- a duplicate updates; it does not duplicate ---------------------- */
    check("an event for a node already held is applied",
          apply_event(&m, "mesh.node",
                      "{\"reason\":\"path\",\"node\":{\"public_key\":\"" KEY_B "\","
                      "\"node_hash\":\"b2\",\"name\":\"HYTTA\",\"type\":2,"
                      "\"path_known\":true,\"hops\":5,\"direct\":false,"
                      "\"path_hex\":\"a1c2d3e4f5\",\"last_heard_mono_ms\":9000,"
                      "\"last_rssi_dbm\":-88.0}}") == 0);
    check("and the list is still two nodes long", m.node_count == 2);
    n = rift_model_find(&m, KEY_B);
    check("the node's path is the new one", n->hops == 5);
    text_is("with the new bytes", n->path_hex, "a1c2d3e4f5");
    check("its signal is now known", n->have_rssi);
    check("and RIFT has counted the observation", n->observations == 1);

    /* Three more events for the same node, one of them the same path. */
    apply_event(&m, "mesh.node",
                "{\"reason\":\"discovered\",\"node\":{\"public_key\":\"" KEY_B "\","
                "\"path_known\":true,\"hops\":5,\"direct\":false,\"path_hex\":\"a1c2d3e4f5\","
                "\"last_heard_mono_ms\":9500}}");
    check("a repeated event still does not add a row", m.node_count == 2);
    n = rift_model_find(&m, KEY_B);
    check("but it is counted as another observation", n->observations == 2);
    check("and an unchanged path is not recorded as a change", n->hist_count == 1);
    apply_event(&m, "mesh.node",
                "{\"reason\":\"path\",\"node\":{\"public_key\":\"" KEY_B "\","
                "\"path_known\":true,\"hops\":2,\"direct\":false,\"path_hex\":\"a1c2\","
                "\"last_heard_mono_ms\":9800}}");
    check("a different path is", rift_model_find(&m, KEY_B)->hist_count == 2);
    check("newest first", rift_model_find(&m, KEY_B)->hist[0].hops == 2);

    /* A node that has never been heard of arrives as an event. */
    check("an event for an unknown node adds it",
          apply_event(&m, "mesh.node",
                      "{\"reason\":\"discovered\",\"node\":{\"public_key\":\"" KEY_C "\","
                      "\"node_hash\":\"c3\",\"name\":\"RPT-7\",\"type\":2,"
                      "\"path_known\":false}}") == 0);
    check("as a third row", m.node_count == 3);
    n = rift_model_find(&m, KEY_C);
    check("with no route back", !n->path_known);
    check("and no hop count at all", n->hops == 0 && !n->direct);

    /* ---- malformed events are refused whole, and counted ---------------- */
    {
        unsigned before = m.events_malformed;
        int nodes_before = m.node_count;

        check("an event whose data is not an object is refused",
              apply_event(&m, "mesh.node", "[1,2,3]") == -1);
        check("a node with no public key is refused",
              apply_event(&m, "mesh.node", "{\"node\":{\"name\":\"nobody\"}}") == -1);
        check("a key that is not hex is refused",
              apply_event(&m, "mesh.node",
                          "{\"node\":{\"public_key\":\"zzzz\",\"name\":\"nobody\"}}") == -1);
        check("a key of the wrong length is refused",
              apply_event(&m, "mesh.node",
                          "{\"node\":{\"public_key\":\"a1b2\",\"name\":\"nobody\"}}") == -1);
        check("an unknown event name is refused",
              apply_event(&m, "mesh.nonsense", "{}") == -1);
        check("a state event with no state is refused",
              apply_event(&m, "mesh.state", "{\"reason\":\"why\"}") == -1);
        check("an activity with no kind is refused",
              apply_event(&m, "mesh.activity", "{\"bytes\":10}") == -1);
        check("and an activity of a kind this build does not know is too",
              apply_event(&m, "mesh.activity", "{\"kind\":\"telepathy\"}") == -1);
        check("none of them added a node", m.node_count == nodes_before);
        check("and every one of them was counted", m.events_malformed == before + 8);
        /* Phase 1 ignored mesh.message, because COMMS was not drawn, and
         * this asserted that ignoring it was not a fault. COMMS is drawn
         * now, so the assertion has become untrue and is replaced by the
         * one that matters: a message event is applied, and a *malformed*
         * one is still refused and counted like any other.
         *
         * What a message event does with a well-formed message is
         * tests/rift_comms_test.c's subject; this is the event path. */
        check("an empty message is refused",
              apply_event(&m, "mesh.message", "{\"message\":{}}") == -1);
        check("and counted", m.events_malformed == before + 9);
        check("a message with everything it needs is applied",
              apply_event(&m, "mesh.message",
                          "{\"message\":{\"id\":1,\"direction\":\"in\","
                          "\"peer_public_key\":\"" KEY_A "\",\"text\":\"hei\","
                          "\"state\":\"received\",\"mono_ms\":1000}}") == 0);
        check("and is not counted as malformed", m.events_malformed == before + 9);
        check("nor did it become a node", m.node_count == nodes_before);
    }

    /* ---- a snapshot replaces; it does not merge -------------------------- */
    check("a second snapshot is taken",
          apply_nodes(&m, "{\"count\":1,\"nodes\":[{\"public_key\":\"" KEY_A "\","
                          "\"node_hash\":\"a1\",\"name\":\"OSLO-01\",\"path_known\":true,"
                          "\"hops\":0,\"direct\":true,\"last_heard_mono_ms\":1000}]}") == 0);
    check("a node the service no longer holds is gone from here too", m.node_count == 1);
    check("because nobody could be asked about it", rift_model_find(&m, KEY_B) == NULL);
    /* A snapshot carrying one entry this build will not read keeps the
     * rest: one bad entry costs one entry. */
    check("a snapshot with one unusable entry is still taken",
          apply_nodes(&m, "{\"nodes\":[{\"public_key\":\"" KEY_A "\",\"name\":\"OSLO-01\","
                          "\"last_heard_mono_ms\":1000},"
                          "{\"name\":\"no key at all\"},"
                          "{\"public_key\":\"" KEY_B "\",\"name\":\"HYTTA\","
                          "\"last_heard_mono_ms\":2000}]}") == 0);
    check("and holds the two that were usable", m.node_count == 2);
    check("an array that is not there is not a snapshot", apply_nodes(&m, "{\"count\":0}") == -1);
    check("and the nodes it would have replaced are still here", m.node_count == 2);

    /* ---- the cache is bounded -------------------------------------------- */
    {
        char json[512];
        char key[RIFT_KEY_HEX];
        int over = RIFT_MAX_NODES + 8;

        rift_model_init(&m);
        for (i = 0; i < over; i++) {
            snprintf(key, sizeof(key),
                     "%02x%02x000000000000000000000000000000000000000000000000000000000000",
                     i & 0xff, (i >> 8) & 0xff);
            snprintf(json, sizeof(json),
                     "{\"node\":{\"public_key\":\"%s\",\"name\":\"n%d\","
                     "\"path_known\":false,\"last_heard_mono_ms\":%d}}",
                     key, i, 1000 + i);
            apply_event(&m, "mesh.node", json);
        }
        check("the cache never grows past its bound", m.node_count == RIFT_MAX_NODES);
        check("and says how many it had to drop", m.nodes_dropped == (unsigned)(over -
                                                                                RIFT_MAX_NODES));
        /* The ones dropped are the ones heard longest ago. */
        check("the most recently heard node is still held",
              rift_model_find(&m,
                              "4700000000000000000000000000000000000000000000000000000000000000") !=
                  NULL);
    }

    /* ---- the activity ring ------------------------------------------------ */
    rift_model_init(&m);
    check("an rx activity is taken",
          apply_event(&m, "mesh.activity",
                      "{\"kind\":\"rx\",\"payload_type\":\"advert\",\"bytes\":48,"
                      "\"mono_ms\":1000,\"rssi_dbm\":-88.0,\"snr_db\":6.0}") == 0);
    check("a tx activity is taken",
          apply_event(&m, "mesh.activity",
                      "{\"kind\":\"tx\",\"submit_id\":7,\"result\":\"rx_resume_failed\","
                      "\"bytes\":72,\"mono_ms\":2000}") == 0);
    act = rift_model_activity_at(&m, 0);
    check("the newest is first", act != NULL && act->kind == RIFT_ACT_TX);
    /* The result, never the kind: three of the five results a tx can carry
     * are not "it went out". */
    text_is("and a transmit is shown by its result", act->word, "rx_resume_failed");
    check("a transmit carries no signal", !act->have_rssi && !act->have_snr);
    act = rift_model_activity_at(&m, 1);
    check("then the one before it", act != NULL && act->kind == RIFT_ACT_RX);
    text_is("named by its payload type", act->word, "advert");
    check("with the signal that was measured", act->have_rssi && act->have_snr);
    check("there is no third", rift_model_activity_at(&m, 2) == NULL);

    check("an rx with no signal metadata is still taken",
          apply_event(&m, "mesh.activity",
                      "{\"kind\":\"rx\",\"payload_type\":\"text\",\"bytes\":30,"
                      "\"mono_ms\":3000}") == 0);
    act = rift_model_activity_at(&m, 0);
    check("and its signal stays unknown rather than becoming zero",
          !act->have_rssi && !act->have_snr);

    for (i = 0; i < RIFT_MAX_ACTIVITY * 2; i++) {
        apply_event(&m, "mesh.activity", "{\"kind\":\"rx\",\"payload_type\":\"ack\"}");
    }
    check("the feed is a window, not a log", m.activity_count == RIFT_MAX_ACTIVITY);
    check("and counts everything it saw", m.activity_total == RIFT_MAX_ACTIVITY * 2 + 3);
    act = rift_model_activity_at(&m, 0);
    check("an activity with no time is kept with the time unknown", !act->have_mono);

    /* ---- service state ----------------------------------------------------- */
    rift_model_init(&m);
    check("a status is taken",
          apply_status(&m, "{\"state\":\"degraded\",\"reason\":\"the receiver did not come back\","
                           "\"state_since_mono_ms\":500,\"radio\":{\"connected\":true,"
                           "\"lease_held\":true,\"online\":false},\"nodes\":4,"
                           "\"counters\":{\"rx_events\":10,\"nodes_unretained\":2}}") == 0);
    check("with the state it named", m.state == RIFT_SVC_DEGRADED);
    text_is("and the reason in the service's own words", m.reason,
            "the receiver did not come back");
    check("radiod's state word is absent until radiod has said", !m.have_radio_state);
    check("which is not the same as off", m.radio_connected && !m.radio_online);
    check("the service's own node count is kept", m.have_nodes_reported && m.nodes_reported == 4);
    check("and so is what the contact table had no room for", m.nodes_unretained == 2);
    check("a status with no state is not a status", apply_status(&m, "{\"reason\":\"x\"}") == -1);
    check("and the state it had is untouched", m.state == RIFT_SVC_DEGRADED);

    check("every state word the API defines is understood",
          apply_status(&m, "{\"state\":\"waiting_for_lease\"}") == 0 &&
              m.state == RIFT_SVC_WAITING_LEASE);
    check("a state word this build does not know is not guessed at",
          apply_status(&m, "{\"state\":\"transcendent\"}") == 0 &&
              m.state == RIFT_SVC_UNKNOWN);

    /* ---- the service goes away, and comes back ---------------------------- */
    apply_nodes(&m, "{\"nodes\":[{\"public_key\":\"" KEY_A "\",\"name\":\"OSLO-01\","
                    "\"last_heard_mono_ms\":1000}]}");
    rift_model_service_lost(&m, "meshcored closed the connection");
    check("the service is absent", m.state == RIFT_SVC_ABSENT);
    /* The nodes stay. An empty list would say something less true than a
     * stale one - and would lose the only record of what was there. */
    check("what was known is still shown", m.node_count == 1);
    check("marked as cached", m.stale);
    check("and the snapshot is no longer valid", !m.snapshot_valid);
    check("nothing is claimed about the radio", !m.radio_connected && !m.have_radio_state);

    rift_model_service_found(&m);
    check("on reconnecting it is live again", !m.stale);
    check("with no snapshot read yet", !m.snapshot_valid);
    check("and the state unknown rather than still absent", m.state == RIFT_SVC_UNKNOWN);
    check("an event brings it out of unknown",
          apply_event(&m, "mesh.state",
                      "{\"state\":\"online\",\"reason\":\"receiving\",\"mono_ms\":99}") == 0 &&
              m.state == RIFT_SVC_ONLINE);

    /* ---- identity ---------------------------------------------------------- */
    check("an identity is taken",
          apply_identity(&m, "{\"public_key\":\"" KEY_SELF "\",\"node_hash\":\"5f\","
                             "\"name\":\"K230-A\"}") == 0);
    text_is("with this device's name", m.self_name, "K230-A");
    text_is("and its hash", m.self_hash, "5f");
    check("an identity with no key is refused",
          apply_identity(&m, "{\"name\":\"K230-A\"}") == -1);

    /* ---- naming a hop ------------------------------------------------------ */
    rift_model_init(&m);
    apply_identity(&m, "{\"public_key\":\"" KEY_SELF "\",\"name\":\"K230-A\"}");
    apply_nodes(&m, "{\"nodes\":["
                    "{\"public_key\":\"" KEY_A "\",\"name\":\"OSLO-01\"},"
                    "{\"public_key\":\"" KEY_B "\",\"name\":\"HYTTA\"}]}");
    text_is("a hop hash that matches one node is named", rift_model_name_for_hash(&m, "a1"),
            "OSLO-01");
    text_is("this device names itself", rift_model_name_for_hash(&m, "5f"), "K230-A");
    check("a hash that matches nothing is not named", rift_model_name_for_hash(&m, "ff") == NULL);
    /* Two nodes sharing a first byte: mesh.node refuses an ambiguous prefix
     * rather than answering one, and so does this. A guessed name on a hop
     * is a wrong route drawn confidently. */
    apply_nodes(&m, "{\"nodes\":["
                    "{\"public_key\":\"a1000000000000000000000000000000000000000000000000000000"
                    "00000001\",\"name\":\"ONE\"},"
                    "{\"public_key\":\"a1000000000000000000000000000000000000000000000000000000"
                    "00000002\",\"name\":\"TWO\"}]}");
    check("and a hash two nodes share is not named at all",
          rift_model_name_for_hash(&m, "a1") == NULL);

    /* ---- the order the list is drawn in ------------------------------------ */
    rift_model_init(&m);
    apply_nodes(&m,
                "{\"nodes\":["
                "{\"public_key\":\"" KEY_A "\",\"name\":\"fresh-old\","
                "\"last_heard_mono_ms\":1000},"
                "{\"public_key\":\"" KEY_B "\",\"name\":\"fresh-new\","
                "\"last_heard_mono_ms\":9000},"
                "{\"public_key\":\"" KEY_C "\",\"name\":\"never\"}]}");
    /* now is inside the stale boundary of both heard nodes. */
    count = rift_model_order(&m, 10000, order, RIFT_MAX_NODES);
    check("every node is in the order", count == 3);
    text_is("the most recently heard is first", order[0]->name, "fresh-new");
    text_is("then the one heard longer ago", order[1]->name, "fresh-old");
    text_is("and a node never heard is last", order[2]->name, "never");
    check("two of them are fresh", rift_model_fresh_count(&m, 10000) == 2);

    /* Move the clock past the stale boundary for one of them only. */
    count = rift_model_order(&m, RIFT_STALE_MS + 5000, order, RIFT_MAX_NODES);
    text_is("a node that has gone stale drops below the fresh ones", order[0]->name,
            "fresh-new");
    text_is("but stays above one never heard", order[1]->name, "fresh-old");
    text_is("which is still last", order[2]->name, "never");
    check("and only one is fresh now", rift_model_fresh_count(&m, RIFT_STALE_MS + 5000) == 1);

    {
        const struct rift_node *again[RIFT_MAX_NODES];

        rift_model_order(&m, 10000, again, RIFT_MAX_NODES);
        rift_model_order(&m, 10000, order, RIFT_MAX_NODES);
        check("the order is total, so a list rebuilt every second does not reshuffle",
              memcmp(again, order, sizeof(order[0]) * 3) == 0);
    }

    printf("rift_model_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
