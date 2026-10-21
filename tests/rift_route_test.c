/*
 * RIFT's explicit route, without a display or a service (rift_route.h): what
 * a reader may put in a route and what is refused, that the route is held as
 * whole keys in the order chosen, that only the preview answer for the draft
 * as it stands can mark it valid, that a node leaving the list is reviewed
 * and never remapped, that staging, AUTO and cancelling each change only
 * what they say, that the service's capabilities are taken only when they
 * are true - and that a message sent on an explicit route is never sent
 * again by RESEND.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_format.h"
#include "rift_model.h"
#include "rift_route.h"

#include <stdio.h>
#include <stdlib.h>
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
    if (strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

/* The recipient (a chat node), three repeaters, a chat node that is not a
 * repeater, and this device. */
#define KEY_TO "b2cafe1e7d0411223344556677889900aabbccddeeff001122334455667788b2"
#define KEY_R1 "a19ac21e7d0411223344556677889900aabbccddeeff001122334455667788b1"
#define KEY_R2 "c3beef1e7d0411223344556677889900aabbccddeeff001122334455667788b3"
#define KEY_R3 "d4dead1e7d0411223344556677889900aabbccddeeff001122334455667788b4"
#define KEY_CH "e5000000000000000000000000000000000000000000000000000000000000e5"
#define KEY_ME "5f000000000000000000000000000000000000000000000000000000000000ff"

static void apply(struct rift_model *m, const char *what, const char *json)
{
    cJSON *o = cJSON_Parse(json);

    if (strcmp(what, "nodes") == 0) {
        check("the node fixture is taken", rift_model_apply_nodes(m, o) == 0);
    } else if (strcmp(what, "identity") == 0) {
        rift_model_apply_identity(m, o);
    } else if (strcmp(what, "info") == 0) {
        rift_route_apply_info(&m->route, o);
    } else if (strcmp(what, "messages") == 0) {
        check("the message fixture is taken", rift_model_apply_messages(m, o) == 0);
    }
    cJSON_Delete(o);
}

static const char *NODES_ALL =
    "{\"count\":5,\"nodes\":["
    "{\"public_key\":\"" KEY_TO "\",\"node_hash\":\"b2\",\"name\":\"HYTTA\",\"type\":1},"
    "{\"public_key\":\"" KEY_R1 "\",\"node_hash\":\"a1\",\"name\":\"RPT-NORD\",\"type\":2},"
    "{\"public_key\":\"" KEY_R2 "\",\"node_hash\":\"c3\",\"name\":\"RPT-7\",\"type\":2},"
    "{\"public_key\":\"" KEY_R3 "\",\"node_hash\":\"d4\",\"name\":\"RPT-SOR\",\"type\":2},"
    "{\"public_key\":\"" KEY_CH "\",\"node_hash\":\"e5\",\"name\":\"ANNA\",\"type\":1}]}";

/* The same list without RPT-7, and with another repeater named RPT-7: what a
 * snapshot after the old one left and a new one took its name looks like. */
static const char *NODES_R2_GONE =
    "{\"count\":5,\"nodes\":["
    "{\"public_key\":\"" KEY_TO "\",\"node_hash\":\"b2\",\"name\":\"HYTTA\",\"type\":1},"
    "{\"public_key\":\"" KEY_R1 "\",\"node_hash\":\"a1\",\"name\":\"RPT-NORD\",\"type\":2},"
    "{\"public_key\":\"f6000000000000000000000000000000000000000000000000000000000000f6\","
    "\"node_hash\":\"f6\",\"name\":\"RPT-7\",\"type\":2},"
    "{\"public_key\":\"" KEY_R3 "\",\"node_hash\":\"d4\",\"name\":\"RPT-SOR\",\"type\":2},"
    "{\"public_key\":\"" KEY_CH "\",\"node_hash\":\"e5\",\"name\":\"ANNA\",\"type\":1}]}";

static const char *VALID_2 =
    "{\"valid\":true,\"route\":\"explicit\",\"relays\":2,\"hash_bytes\":1,\"path_bytes\":2,"
    "\"path_hex\":\"a1c3\",\"transmitted\":false,\"link_evidence_verified\":false}";

static int answer(struct rift_route *r, int id, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    int rc = rift_route_check_answered(r, id, o);

    cJSON_Delete(o);
    return rc;
}

static void capabilities(void)
{
    struct rift_model m;

    rift_model_init(&m);
    check("before mesh.info nothing is supported", !rift_route_supported(&m.route) &&
                                                       !m.route.caps_known);
    apply(&m, "info", "{\"service\":\"meshcored\",\"version\":\"0.0.9\"}");
    check("a service that names no capabilities is known and unsupported",
          m.route.caps_known && !rift_route_supported(&m.route));
    apply(&m, "info", "{\"capabilities\":{\"manual_path_preview\":\"true\","
                      "\"explicit_direct_send\":1}}");
    check("a string or a number is not true", !rift_route_supported(&m.route));
    apply(&m, "info", "{\"capabilities\":{\"manual_path_preview\":true}}");
    check("preview without explicit send is not enough",
          m.route.can_preview && !m.route.can_explicit && !rift_route_supported(&m.route));
    apply(&m, "info", "{\"capabilities\":{\"manual_path_preview\":true,"
                      "\"explicit_direct_send\":true}}");
    check("both true is supported", rift_route_supported(&m.route));
    rift_route_service_lost(&m.route);
    check("the connection going forgets them", !rift_route_supported(&m.route) &&
                                                   !m.route.caps_known);
    apply(&m, "info", "{\"capabilities\":{\"manual_path_preview\":true,"
                      "\"explicit_direct_send\":false}}");
    check("and the next service is read for itself", !rift_route_supported(&m.route));
}

static void supported(struct rift_model *m)
{
    rift_model_init(m);
    apply(m, "nodes", NODES_ALL);
    apply(m, "identity", "{\"public_key\":\"" KEY_ME "\",\"node_hash\":\"5f\",\"name\":\"K230-A\"}");
    apply(m, "info", "{\"capabilities\":{\"manual_path_preview\":true,"
                     "\"explicit_direct_send\":true}}");
}

static void editing(void)
{
    static struct rift_model m;
    struct rift_route *r = &m.route;
    char why[RIFT_ROUTE_WHY_MAX];
    char chain[RIFT_CHAIN_MAX];
    char word[16];
    unsigned gen;
    int i;

    supported(&m);
    check("a builder needs a whole key", rift_route_open(r, "b2cafe") != 0 && !r->draft.active);
    check("it opens on the conversation's peer", rift_route_open(r, KEY_TO) == 0 &&
                                                     r->draft.active &&
                                                     strcmp(r->draft.to, KEY_TO) == 0);
    check("empty, with nothing marked", r->draft.count == 0 && r->draft.marked == -1);
    check("an empty route wants a preview too: DIRECT with no relay",
          rift_route_wants_check(r));

    /* What may not be a relay. */
    check("a node not in the list",
          rift_route_eligible(r, &m, "0123456789012345678901234567890123456789012345678901234567890123",
                              why, sizeof(why)) != 0 &&
              strstr(why, "not in the node list"));
    check("the recipient", rift_route_eligible(r, &m, KEY_TO, why, sizeof(why)) != 0 &&
                               strstr(why, "HYTTA is the recipient"));
    check("a chat node that is not a repeater",
          rift_route_eligible(r, &m, KEY_CH, why, sizeof(why)) != 0 &&
              strstr(why, "ANNA is not a repeater"));
    check("a prefix is not a key", rift_route_eligible(r, &m, "a19ac21e", why, sizeof(why)) != 0);
    check("a refused tap changes nothing and says why",
          rift_route_add(r, &m, KEY_CH) != 0 && r->draft.count == 0 &&
              strstr(r->draft.note, "not a repeater"));

    /* Order is the order of the taps, by whole key. */
    gen = r->draft.gen;
    check("the first relay", rift_route_add(r, &m, KEY_R1) == 0 && r->draft.count == 1);
    check("every edit is a new generation", r->draft.gen != gen);
    check("and clears the last refusal", r->draft.note[0] == '\0');
    check("the second", rift_route_add(r, &m, KEY_R2) == 0);
    check("the same repeater twice is refused",
          rift_route_add(r, &m, KEY_R1) != 0 && strstr(r->draft.note, "already in the route") &&
              r->draft.count == 2);
    text_is("held as whole keys, first", r->draft.via[0], KEY_R1);
    text_is("and second", r->draft.via[1], KEY_R2);
    check("found where they are", rift_route_index(r, KEY_R2) == 1 && rift_route_index(r, KEY_R3) < 0);
    rift_route_chain(&m, r->draft.to, (const char(*)[RIFT_ROUTE_KEY])r->draft.via, r->draft.count,
                     chain, sizeof(chain));
    text_is("the chain reads sender, relays, recipient", chain,
            "YOU \xE2\x80\xBA RPT-NORD \xE2\x80\xBA RPT-7 \xE2\x80\xBA HYTTA");
    text_is("two relays", rift_route_relays_word(2, word, sizeof(word)), "2 RELAYS");
    text_is("one", rift_route_relays_word(1, word, sizeof(word)), "1 RELAY");
    text_is("none is not AUTO, it is no relay", rift_route_relays_word(0, word, sizeof(word)),
            "NO RELAY");

    check("UNDO takes the last off", rift_route_undo(r) == 0 && r->draft.count == 1 &&
                                         strcmp(r->draft.via[0], KEY_R1) == 0 &&
                                         r->draft.via[1][0] == '\0');
    rift_route_add(r, &m, KEY_R2);
    rift_route_add(r, &m, KEY_R3);
    rift_route_mark(r, 1);
    check("a waypoint can be picked in the list", r->draft.marked == 1);
    rift_route_mark(r, 7);
    check("but not one that is not there", r->draft.marked == -1);
    check("CUT removes it and every one after it", rift_route_cut(r, 1) == 0 &&
                                                       r->draft.count == 1 &&
                                                       strcmp(r->draft.via[0], KEY_R1) == 0);
    check("a cut past the end does nothing", rift_route_cut(r, 3) != 0 && r->draft.count == 1);
    rift_route_clear(r);
    check("CLEAR empties it, and the recipient stays", r->draft.count == 0 &&
                                                          strcmp(r->draft.to, KEY_TO) == 0);
    check("UNDO on an empty route does nothing", rift_route_undo(r) != 0);
    for (i = 0; i < RIFT_ROUTE_MAX; i++) {
        snprintf(r->draft.via[i], sizeof(r->draft.via[i]), "%062x%02x", 0, i);
    }
    r->draft.count = RIFT_ROUTE_MAX;
    check("a full route takes no more", rift_route_eligible(r, &m, KEY_R1, why, sizeof(why)) != 0 &&
                                            strstr(why, "63 relays"));
    rift_route_clear(r);

    /* This device, once it has said who it is - even listed as a repeater. */
    apply(&m, "nodes",
          "{\"count\":2,\"nodes\":["
          "{\"public_key\":\"" KEY_ME "\",\"node_hash\":\"5f\",\"name\":\"K230-A\",\"type\":2},"
          "{\"public_key\":\"" KEY_R1 "\",\"node_hash\":\"a1\",\"name\":\"RPT-NORD\",\"type\":2}]}");
    check("this device is never a relay", m.have_identity &&
                                              rift_route_eligible(r, &m, KEY_ME, why, sizeof(why)) != 0 &&
                                              strstr(why, "sender"));
    apply(&m, "nodes", NODES_ALL);
    rift_route_close(r);
    check("closing leaves nothing open", !r->draft.active);
}

static void checking(void)
{
    static struct rift_model m;
    struct rift_route *r = &m.route;
    int id;

    supported(&m);
    rift_route_open(r, KEY_TO);
    rift_route_add(r, &m, KEY_R1);
    rift_route_add(r, &m, KEY_R2);
    check("not usable before it is checked", !rift_route_usable(r, &m));
    check("it wants a preview", rift_route_wants_check(r));
    rift_route_check_sent(r, 41);
    check("checking, and nothing else asked meanwhile",
          r->draft.check == RIFT_ROUTE_CHECKING && !rift_route_wants_check(r));
    check("an answer to another request is stale and changes nothing",
          answer(r, 40, VALID_2) != 0 && r->draft.check == RIFT_ROUTE_CHECKING);
    check("the answer to this one makes it valid",
          answer(r, 41, VALID_2) == 0 && r->draft.check == RIFT_ROUTE_VALID);
    check("with the service's numbers", r->draft.enc.relays == 2 && r->draft.enc.hash_bytes == 1 &&
                                            r->draft.enc.path_bytes == 2 &&
                                            strcmp(r->draft.enc.path_hex, "a1c3") == 0);
    check("and it is usable", rift_route_usable(r, &m));
    check("the same answer twice is stale", answer(r, 41, VALID_2) != 0);

    /* Edited while the answer was out: the answer is about the old route. */
    rift_route_undo(r);
    check("an edit makes it unchecked", r->draft.check == RIFT_ROUTE_UNCHECKED &&
                                            !rift_route_usable(r, &m));
    rift_route_check_sent(r, 42);
    rift_route_add(r, &m, KEY_R2);
    check("an answer for the route before the edit is not applied",
          answer(r, 42, "{\"valid\":true,\"route\":\"explicit\",\"relays\":1,"
                        "\"transmitted\":false}") != 0 &&
              r->draft.check != RIFT_ROUTE_VALID);
    check("and a new preview is wanted", rift_route_wants_check(r));

    /* What is not the service's word that the route is valid. */
    id = 50;
    rift_route_check_sent(r, id);
    answer(r, id, "{\"valid\":true,\"route\":\"explicit\",\"relays\":2,\"transmitted\":true}");
    check("a preview that says it transmitted is refused", r->draft.check == RIFT_ROUTE_REFUSED);
    rift_route_check_sent(r, ++id);
    answer(r, id, "{\"valid\":true,\"route\":\"direct\",\"relays\":2,\"transmitted\":false}");
    check("a route that is not explicit is refused", r->draft.check == RIFT_ROUTE_REFUSED);
    rift_route_check_sent(r, ++id);
    answer(r, id, "{\"valid\":true,\"route\":\"explicit\",\"relays\":3,\"transmitted\":false}");
    check("a relay count that is not this route's is refused", r->draft.check == RIFT_ROUTE_REFUSED);
    rift_route_check_sent(r, ++id);
    answer(r, id, "{\"valid\":false,\"route\":\"explicit\",\"relays\":2,\"transmitted\":false}");
    check("valid false is refused", r->draft.check == RIFT_ROUTE_REFUSED &&
                                        !rift_route_usable(r, &m));
    rift_route_check_sent(r, ++id);
    check("the service's refusal is kept in its words",
          rift_route_check_refused(r, id, "a relay hash matches another known repeater") == 0 &&
              r->draft.check == RIFT_ROUTE_REFUSED && strstr(r->draft.why, "hash matches"));
    check("a refused route is not asked again until it changes", !rift_route_wants_check(r));

    /* The service going away while a preview is out. */
    rift_route_clear(r);
    rift_route_check_sent(r, ++id);
    rift_route_service_lost(r);
    check("its answer is not coming", r->draft.check == RIFT_ROUTE_UNANSWERED &&
                                          r->draft.request == 0 && r->draft.why[0]);
    check("and nothing is asked of a service that has not said what it offers",
          !rift_route_wants_check(r));
    apply(&m, "info", "{\"capabilities\":{\"manual_path_preview\":true,"
                      "\"explicit_direct_send\":true}}");
    check("it is asked again once it has", rift_route_wants_check(r));
    check("a late answer from the old connection changes nothing", answer(r, id, VALID_2) != 0);
}

static void staging(void)
{
    static struct rift_model m;
    struct rift_route *r = &m.route;
    const struct rift_route_stage *s;

    supported(&m);
    check("a conversation starts AUTO", rift_route_staged(r, KEY_TO) == NULL);
    rift_route_open(r, KEY_TO);
    rift_route_add(r, &m, KEY_R1);
    rift_route_add(r, &m, KEY_R2);
    check("an unchecked route cannot be staged", rift_route_use(r, &m) != 0 &&
                                                     rift_route_staged(r, KEY_TO) == NULL);
    rift_route_check_sent(r, 7);
    answer(r, 7, VALID_2);
    check("a checked one can", rift_route_use(r, &m) == 0);
    s = rift_route_staged(r, KEY_TO);
    check("staged on the recipient's conversation, and the builder is closed",
          s && !r->draft.active && s->count == 2);
    check("with the keys in the order chosen",
          s && strcmp(s->via[0], KEY_R1) == 0 && strcmp(s->via[1], KEY_R2) == 0);
    check("and nowhere else", rift_route_staged(r, KEY_CH) == NULL);

    /* EDIT starts from it; CANCEL leaves it as it was. */
    rift_route_open(r, KEY_TO);
    check("editing starts from the staged route", r->draft.count == 2 &&
                                                     strcmp(r->draft.via[1], KEY_R2) == 0);
    rift_route_clear(r);
    rift_route_close(r);
    s = rift_route_staged(r, KEY_TO);
    check("cancelling an edit leaves the staged route alone", s && s->count == 2);

    /* A route for another conversation does not start from this one. */
    rift_route_open(r, KEY_CH);
    check("another conversation's builder starts empty", r->draft.count == 0);
    rift_route_close(r);

    /* Zero relays is a route, not AUTO. */
    rift_route_open(r, KEY_TO);
    rift_route_clear(r);
    rift_route_check_sent(r, 8);
    answer(r, 8, "{\"valid\":true,\"route\":\"explicit\",\"relays\":0,\"hash_bytes\":1,"
                 "\"path_bytes\":0,\"path_hex\":\"\",\"transmitted\":false}");
    check("an empty route checked valid can be staged", rift_route_use(r, &m) == 0);
    s = rift_route_staged(r, KEY_TO);
    check("and it is staged: DIRECT with no relay, which is not AUTO", s && s->count == 0);
    rift_route_auto(r, KEY_CH);
    check("AUTO for another conversation changes nothing here",
          rift_route_staged(r, KEY_TO) != NULL);
    rift_route_auto(r, KEY_TO);
    check("AUTO puts this one back", rift_route_staged(r, KEY_TO) == NULL);

    /* Unsupported: nothing is usable, whatever the draft says. */
    rift_route_open(r, KEY_TO);
    rift_route_check_sent(r, 9);
    answer(r, 9, "{\"valid\":true,\"route\":\"explicit\",\"relays\":0,\"transmitted\":false}");
    apply(&m, "info", "{\"service\":\"meshcored\"}");
    check("a service without the capability cannot have a route staged",
          !rift_route_usable(r, &m) && rift_route_use(r, &m) != 0);
    check("and is not asked for a preview", !rift_route_wants_check(r));
}

static void leaving(void)
{
    static struct rift_model m;
    struct rift_route *r = &m.route;
    char chain[RIFT_CHAIN_MAX];

    supported(&m);
    rift_route_open(r, KEY_TO);
    rift_route_add(r, &m, KEY_R1);
    rift_route_add(r, &m, KEY_R2);
    rift_route_check_sent(r, 3);
    answer(r, 3, VALID_2);
    check("valid while every waypoint is known", rift_route_usable(r, &m) &&
                                                     rift_route_missing(r, &m) == 0);
    apply(&m, "nodes", NODES_R2_GONE);
    check("a waypoint that left the list is counted", rift_route_missing(r, &m) == 1);
    check("and the route is not usable until it is reviewed", !rift_route_usable(r, &m));
    text_is("never remapped to the node that took its name", r->draft.via[1], KEY_R2);
    rift_route_chain(&m, r->draft.to, (const char(*)[RIFT_ROUTE_KEY])r->draft.via, r->draft.count,
                     chain, sizeof(chain));
    check("and named by its key, not by that name", strstr(chain, "c3beef1e?") != NULL);
    rift_route_undo(r);
    check("taking it out clears the review", rift_route_missing(r, &m) == 0);
}

static void resend(void)
{
    static struct rift_model m;
    char json[2048];
    const struct rift_message *msg;

    rift_model_init(&m);
    snprintf(json, sizeof(json),
             "{\"count\":3,\"total\":3,\"persistent\":false,\"messages\":["
             "{\"id\":1,\"direction\":\"out\",\"peer_public_key\":\"" KEY_TO "\","
             "\"text\":\"via two\",\"state\":\"no_ack\",\"route\":\"explicit\","
             "\"path_hex\":\"a1c3\",\"path_hops\":2,\"ack_expected\":true},"
             "{\"id\":2,\"direction\":\"out\",\"peer_public_key\":\"" KEY_TO "\","
             "\"text\":\"auto\",\"state\":\"no_ack\",\"ack_expected\":true},"
             "{\"id\":3,\"direction\":\"out\",\"peer_public_key\":\"" KEY_TO "\","
             "\"text\":\"no relay\",\"state\":\"failed\",\"route\":\"explicit\","
             "\"path_hex\":\"\",\"path_hops\":0,\"ack_expected\":true}]}");
    apply(&m, "messages", json);
    msg = rift_model_message(&m, 1);
    check("a message on an explicit route says so, by the service's word",
          msg && msg->explicit_route && msg->explicit_hops == 2);
    check("and is not offered RESEND, even unacknowledged", msg && !rift_model_can_resend(msg));
    check("a RESEND of it is refused", rift_model_resend_begin(&m, 1, 10) != 0 &&
                                           !rift_model_sending(&m));
    msg = rift_model_message(&m, 3);
    check("nor one with no relay", msg && msg->explicit_route && !rift_model_can_resend(msg));
    msg = rift_model_message(&m, 2);
    check("an AUTO message keeps its RESEND", msg && !msg->explicit_route &&
                                                  rift_model_can_resend(msg));
}

int main(void)
{
    capabilities();
    editing();
    checking();
    staging();
    leaving();
    resend();
    printf("rift_route_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
