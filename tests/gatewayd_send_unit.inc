/* ---- gw_send: sending (M4.2) -------------------------------------------- */
/* Included by gatewayd_unit_test.c. */

#define CM1 "0123456789abcdef"
#define SEND1 "{\"to\":\"" K64 "\",\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}"

static struct gw_send GS;

static bool nobody_paired(const uint8_t fp[GW_FP_LEN])
{
    (void)fp;
    return false;
}

static bool all_paired(const uint8_t fp[GW_FP_LEN])
{
    (void)fp;
    return true;
}

static int send_bad(const char *text, bool resend)
{
    struct gw_send_req q;
    cJSON *p = parse(text);
    int r = gw_send_parse(p, resend, &q);

    cJSON_Delete(p);
    return r;
}

/* A request to a node (or a channel when chan >= 0) under the n-th id. */
static struct gw_send_req send_req(int n, const char *text, int chan)
{
    struct gw_send_req q;
    char buf[512];
    cJSON *p;

    if (chan >= 0) {
        snprintf(buf, sizeof(buf), "{\"channel\":%d,\"text\":\"%s\",\"client_msg_id\":\"cmid-%012d\"}", chan, text, n);
    } else {
        snprintf(buf, sizeof(buf), "{\"to\":\"" K64 "\",\"text\":\"%s\",\"client_msg_id\":\"cmid-%012d\"}", text, n);
    }
    p = parse(buf);
    CHECK(gw_send_parse(p, false, &q) == 0);
    cJSON_Delete(p);
    return q;
}

static enum gw_send_outcome send_accept(struct gw_send_rec *r, double id)
{
    char buf[160];
    cJSON *p;
    enum gw_send_outcome o;

    snprintf(buf, sizeof(buf),
             "{\"accepted\":true,\"message_id\":%.0f,\"route\":\"direct\",\"ack_expected\":true,"
             "\"ack_timeout_ms\":9000,\"secret\":\"LEAK\"}", id);
    p = parse(buf);
    o = gw_send_answered(r, p, 0);
    cJSON_Delete(p);
    return o;
}

static enum gw_send_look send_look(struct gw_send_rec *r, const char *list)
{
    cJSON *p = parse(list);
    enum gw_send_look l = gw_send_looked(&GS, r, p);

    cJSON_Delete(p);
    return l;
}

#define OUT(id, text) "{\"id\":" #id ",\"direction\":\"out\",\"kind\":\"direct\",\"peer_public_key\":\"" K64 "\"," \
                      "\"text\":\"" text "\",\"state\":\"sent_flood\",\"ack_expected\":true,\"attempts\":1}"

static void test_send(void)
{
    static const char *const bad_send[] = {
        "[1]",
        "{\"to\":\"" K64 "\",\"text\":\"hello\"}",
        "{\"to\":\"" K64 "\",\"text\":\"hello\",\"client_msg_id\":\"0123456789abcde\"}",
        "{\"to\":\"" K64 "\",\"text\":\"hello\",\"client_msg_id\":\"0123456789abcdef0123456789abcdef01234\"}",
        "{\"to\":\"" K64 "\",\"text\":\"hello\",\"client_msg_id\":\"0123456789 abcdef\"}",
        "{\"to\":\"" K64 "\",\"text\":\"hello\",\"client_msg_id\":1234567890123456}",
        "{\"to\":\"" K64 "\",\"channel\":0,\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"aa00\",\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"zz00000000000000000000000000000000000000000000000000000000000001\",\"text\":\"hello\","
        "\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":7,\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"" K64 "\",\"text\":\"\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"" K64 "\",\"text\":7,\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"" K64 "\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"" K64 "\",\"text\":\"a\\u0001b\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"" K64 "\",\"text\":\"a\\u001bb\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"" K64 "\",\"text\":\"a\\rb\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"" K64 "\",\"text\":\"a\\u007fb\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"" K64 "\",\"text\":\"a\\u0085b\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"channel\":-1,\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"channel\":1.5,\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"channel\":256,\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"channel\":\"0\",\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"channel\":0,\"via\":[],\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"" K64 "\",\"via\":null,\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"" K64 "\",\"via\":\"" K64B "\",\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"" K64 "\",\"via\":[\"bb00\"],\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"to\":\"" K64 "\",\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\",\"resend\":3}",
        "{\"to\":\"" K64 "\",\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\",\"zero_hop\":true}",
        "{\"message_id\":3,\"run_id\":\"run1\",\"client_msg_id\":\"" CM1 "\"}",
        NULL};
    static const char *const bad_resend[] = {
        "{\"message_id\":0,\"run_id\":\"run1\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"message_id\":1.5,\"run_id\":\"run1\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"message_id\":\"3\",\"run_id\":\"run1\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"message_id\":1e300,\"run_id\":\"run1\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"message_id\":3,\"client_msg_id\":\"" CM1 "\"}",
        "{\"message_id\":3,\"run_id\":\"bad run!\",\"client_msg_id\":\"" CM1 "\"}",
        "{\"message_id\":3,\"run_id\":\"run1\"}",
        "{\"message_id\":3,\"run_id\":\"run1\",\"client_msg_id\":\"" CM1 "\",\"text\":\"x\"}",
        "{\"message_id\":3,\"run_id\":\"run1\",\"client_msg_id\":\"" CM1 "\",\"via\":[]}",
        SEND1,
        NULL};
    static const char *const rows[] = {"mesh.send", "mesh.resend", NULL};
    uint8_t fa[GW_FP_LEN] = {1}, fb[GW_FP_LEN] = {2}, fx[GW_FP_LEN];
    struct gw_send_req q, q2;
    struct gw_send_client *c, *c2;
    struct gw_send_rec *r, *r2;
    const struct gw_method *m;
    char big[1024], via[5000];
    cJSON *p, *up, *res;
    int64_t t = 1000;
    int i, n;

    /* The rows: mesh_send and nothing else opens them; mesh_read does not,
     * and mesh_send opens no reading. */
    for (i = 0; rows[i] != NULL; i++) {
        m = gw_method_find(rows[i]);
        CHECK(m != NULL && m->scope == GW_S_MESH_SEND && m->who == GW_W_PAIRED && m->filter == NULL &&
              m->params == NULL);
        CHECK(gw_policy_check(m, GW_W_PAIRED, GW_S_STATUS | GW_S_MESH_READ) == GW_V_FORBIDDEN);
        CHECK(gw_policy_check(m, GW_W_PAIRED, GW_S_ALL & ~GW_S_MESH_SEND) == GW_V_FORBIDDEN);
        CHECK(gw_policy_check(m, GW_W_PAIRED, 0) == GW_V_FORBIDDEN);
        CHECK(gw_policy_check(m, GW_W_PAIRED, GW_S_MESH_SEND) == GW_V_OK);
        CHECK(gw_policy_check(m, GW_W_PAIRED, GW_S_DEFAULT) == GW_V_OK);
        CHECK(gw_policy_check(m, GW_W_CANDIDATE, GW_S_ALL) == GW_V_UNKNOWN);
    }
    CHECK(gw_method_find("mesh.send")->kind == GW_K_SEND && gw_method_find("mesh.resend")->kind == GW_K_RESEND);
    CHECK(gw_policy_check(gw_method_find("mesh.sync"), GW_W_PAIRED, GW_S_MESH_SEND) == GW_V_FORBIDDEN);
    CHECK(gw_policy_check(gw_method_find("mesh.nodes"), GW_W_PAIRED, GW_S_MESH_SEND | GW_S_STATUS) == GW_V_FORBIDDEN);
    CHECK(!gw_scopes_valid(GW_S_MESH_SEND) && gw_scopes_valid(GW_S_MESH_SEND | GW_S_MESH_READ));

    /* What a client may ask to have sent. */
    for (i = 0; bad_send[i] != NULL; i++) {
        CHECK(send_bad(bad_send[i], false) == -1);
    }
    for (i = 0; bad_resend[i] != NULL; i++) {
        CHECK(send_bad(bad_resend[i], true) == -1);
    }
    CHECK(gw_send_parse(NULL, false, &q) == -1 && gw_send_parse(NULL, true, &q) == -1);
    memset(big, 'a', sizeof(big));
    snprintf(big + GW_SEND_TEXT_MAX + 1, 8, "%s", "");
    snprintf(via, sizeof(via), "{\"to\":\"" K64 "\",\"text\":\"%s\",\"client_msg_id\":\"" CM1 "\"}", big);
    CHECK(send_bad(via, false) == -1); /* 161 bytes */
    big[GW_SEND_TEXT_MAX] = '\0';
    snprintf(via, sizeof(via), "{\"to\":\"" K64 "\",\"text\":\"%s\",\"client_msg_id\":\"" CM1 "\"}", big);
    CHECK(send_bad(via, false) == 0);  /* 160 */
    n = snprintf(via, sizeof(via), "{\"to\":\"" K64 "\",\"text\":\"x\",\"client_msg_id\":\"" CM1 "\",\"via\":[");
    for (i = 0; i < 64; i++) {
        n += snprintf(via + n, sizeof(via) - (size_t)n, "%s\"" K64B "\"", i ? "," : "");
    }
    snprintf(via + n, sizeof(via) - (size_t)n, "]}");
    CHECK(send_bad(via, false) == -1); /* 64 relays */

    p = parse(SEND1);
    CHECK(gw_send_parse(p, false, &q) == 0 && strcmp(q.to, K64) == 0 && q.channel == -1 && !q.has_via &&
          !q.resend && strcmp(q.text, "hello") == 0 && strcmp(q.cmid, CM1) == 0);
    up = gw_send_upstream(&q, p);
    CHECK(up != NULL && cJSON_GetArraySize(up) == 2 && has(up, "to") && has(up, "text") &&
          !has(up, "client_msg_id"));
    cJSON_Delete(up);
    cJSON_Delete(p);
    /* The key in upper case is the same message; another text, another
     * place, a route or none are not. */
    p = parse("{\"to\":\"AA00000000000000000000000000000000000000000000000000000000000001\",\"text\":\"hello\","
              "\"client_msg_id\":\"" CM1 "\"}");
    CHECK(gw_send_parse(p, false, &q2) == 0 && q2.digest == q.digest && strcmp(q2.to, K64) == 0);
    cJSON_Delete(p);
    p = parse("{\"to\":\"" K64 "\",\"text\":\"hellp\",\"client_msg_id\":\"" CM1 "\"}");
    CHECK(gw_send_parse(p, false, &q2) == 0 && q2.digest != q.digest);
    cJSON_Delete(p);
    p = parse("{\"to\":\"" K64B "\",\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\"}");
    CHECK(gw_send_parse(p, false, &q2) == 0 && q2.digest != q.digest);
    cJSON_Delete(p);
    p = parse("{\"to\":\"" K64 "\",\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\",\"via\":[]}");
    CHECK(gw_send_parse(p, false, &q2) == 0 && q2.digest != q.digest && q2.has_via);
    up = gw_send_upstream(&q2, p);
    CHECK(up != NULL && cJSON_IsArray(cJSON_GetObjectItem(up, "via")) &&
          cJSON_GetArraySize(cJSON_GetObjectItem(up, "via")) == 0);
    cJSON_Delete(up);
    cJSON_Delete(p);
    p = parse("{\"to\":\"" K64 "\",\"text\":\"hello\",\"client_msg_id\":\"" CM1 "\",\"via\":[\"" K64B "\"]}");
    CHECK(gw_send_parse(p, false, &q2) == 0 && q2.digest != q.digest);
    up = gw_send_upstream(&q2, p);
    CHECK(up != NULL && cJSON_GetArraySize(cJSON_GetObjectItem(up, "via")) == 1 && cJSON_GetArraySize(up) == 3);
    cJSON_Delete(up);
    cJSON_Delete(p);
    p = parse("{\"channel\":3,\"text\":\"bl\\u00e5b\\u00e6r\\n\\tok\",\"client_msg_id\":\"A-b_0123456789abcd\"}");
    CHECK(gw_send_parse(p, false, &q2) == 0 && q2.channel == 3 && q2.to[0] == '\0' && q2.digest != q.digest);
    up = gw_send_upstream(&q2, p);
    CHECK(up != NULL && cJSON_GetArraySize(up) == 2 && cJSON_GetObjectItem(up, "channel")->valuedouble == 3);
    cJSON_Delete(up);
    cJSON_Delete(p);
    p = parse("{\"message_id\":3,\"run_id\":\"run1\",\"client_msg_id\":\"" CM1 "\"}");
    CHECK(gw_send_parse(p, true, &q2) == 0 && q2.resend && q2.message_id == 3 && strcmp(q2.run_id, "run1") == 0);
    up = gw_send_upstream(&q2, p);
    CHECK(up != NULL && cJSON_GetArraySize(up) == 1 && cJSON_GetObjectItem(up, "resend")->valuedouble == 3);
    cJSON_Delete(up);
    cJSON_Delete(p);

    /* One id, one transmission. */
    gw_send_init(&GS);
    c = gw_send_client(&GS, fa, NULL);
    CHECK(c != NULL && gw_send_client(&GS, fa, NULL) == c && gw_send_client(&GS, fb, NULL) != c);
    CHECK(gw_send_begin(c, &q, NULL, 5, t, &r) == GW_SA_UNAVAILABLE && r == NULL && gw_send_inflight(c) == 0);
    CHECK(gw_send_begin(c, &q, "run1", 5, t, &r) == GW_SA_FORWARD && r != NULL && r->state == GW_SS_PENDING &&
          r->id_before == 5 && gw_send_inflight(c) == 1);
    r->tag = 77;
    CHECK(gw_send_by_tag(&GS, 77, &c2) == r && c2 == c && gw_send_by_tag(&GS, 78, NULL) == NULL);
    CHECK(gw_send_begin(c, &q, "run1", 5, t, &r2) == GW_SA_IN_PROGRESS && r2 == r);
    q2 = q;
    q2.digest ^= 1;
    CHECK(gw_send_begin(c, &q2, "run1", 5, t, &r2) == GW_SA_MISMATCH && r->state == GW_SS_PENDING);
    CHECK(send_accept(r, 6) == GW_SO_DONE && r->state == GW_SS_DONE && r->message_id == 6);
    CHECK(gw_send_by_tag(&GS, 77, NULL) == NULL && gw_send_inflight(c) == 0);
    res = gw_send_result(r, false);
    CHECK(res != NULL && cJSON_GetArraySize(res) == 7 && !has(res, "secret") &&
          cJSON_IsFalse(cJSON_GetObjectItem(res, "duplicate")) &&
          strcmp(cJSON_GetObjectItem(res, "run_id")->valuestring, "run1") == 0 &&
          cJSON_GetObjectItem(res, "message_id")->valuedouble == 6 &&
          strcmp(cJSON_GetObjectItem(res, "route")->valuestring, "direct") == 0 &&
          cJSON_GetObjectItem(res, "ack_timeout_ms")->valuedouble == 9000);
    cJSON_Delete(res);
    /* A repeat is answered from the record: now, with meshcored away, and
     * after it started again. Under another message it is a mistake. */
    CHECK(gw_send_begin(c, &q, "run1", 9, t, &r2) == GW_SA_REPLY && r2 == r);
    CHECK(gw_send_begin(c, &q, NULL, 9, t, &r2) == GW_SA_REPLY && gw_send_begin(c, &q, "run2", 0, t, &r2) == GW_SA_REPLY);
    res = gw_send_result(r, true);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(res, "duplicate")) && cJSON_GetObjectItem(res, "message_id")->valuedouble == 6);
    cJSON_Delete(res);
    CHECK(gw_send_begin(c, &q2, "run1", 9, t, &r2) == GW_SA_MISMATCH);
    /* The same id of another client is another message. */
    c2 = gw_send_client(&GS, fb, NULL);
    CHECK(gw_send_begin(c2, &q, "run1", 6, t, &r2) == GW_SA_FORWARD && r2 != r);
    gw_send_release(r2);

    /* Refused by meshcored: nothing was sent, the id may be tried again. */
    q = send_req(2, "second", -1);
    CHECK(gw_send_begin(c, &q, "run1", 6, t, &r) == GW_SA_FORWARD);
    CHECK(gw_send_answered(r, NULL, 5) == GW_SO_REFUSED && r->state == GW_SS_FREE);
    CHECK(gw_send_begin(c, &q, "run1", 6, t, &r) == GW_SA_FORWARD);
    /* No answer: not known. Nothing is sent before meshcored's list says. */
    CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN && r->state == GW_SS_UNCERTAIN);
    CHECK(gw_send_begin(c, &q, NULL, 6, t, &r2) == GW_SA_UNAVAILABLE && r->state == GW_SS_UNCERTAIN);
    CHECK(gw_send_begin(c, &q, "run1", 6, t, &r2) == GW_SA_LOOKUP && r2 == r && r->state == GW_SS_RESOLVING);
    CHECK(gw_send_begin(c, &q, "run1", 6, t, &r2) == GW_SA_IN_PROGRESS);
    /* The list reaches back to message 6 and holds nothing like it: an
     * incoming one with the same words, one to another node, another text,
     * and the message of the first send. */
    CHECK(send_look(r, "[" OUT(6, "hello") ",{\"id\":7,\"direction\":\"in\",\"kind\":\"direct\",\"peer_public_key\":\""
                       K64 "\",\"text\":\"second\"},{\"id\":8,\"direction\":\"out\",\"kind\":\"direct\","
                       "\"peer_public_key\":\"" K64B "\",\"text\":\"second\"}]") == GW_SL_ABSENT);
    CHECK(r->state == GW_SS_RESOLVING);
    r->state = GW_SS_PENDING; /* as main.c does when it passes it on */
    r->id_before = 9;
    CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
    CHECK(gw_send_begin(c, &q, "run1", 9, t, &r2) == GW_SA_LOOKUP);
    /* Now it is there: found, never sent again. */
    CHECK(send_look(r, "[" OUT(9, "secondly") "," OUT(10, "second") "]") == GW_SL_FOUND && r->state == GW_SS_DONE &&
          r->message_id == 10);
    res = gw_send_result(r, true);
    CHECK(res != NULL && cJSON_GetObjectItem(res, "message_id")->valuedouble == 10 &&
          strcmp(cJSON_GetObjectItem(res, "route")->valuestring, "flood") == 0 &&
          cJSON_IsTrue(cJSON_GetObjectItem(res, "ack_expected")) && cJSON_IsTrue(cJSON_GetObjectItem(res, "accepted")));
    cJSON_Delete(res);
    CHECK(gw_send_begin(c, &q, "run1", 10, t, &r2) == GW_SA_REPLY);

    /* Two sends of the same words are two messages: the one meshcored
     * answered is not taken for the one it did not. */
    q = send_req(3, "twice", -1);
    q2 = send_req(4, "twice", -1);
    CHECK(gw_send_begin(c, &q, "run1", 10, t, &r) == GW_SA_FORWARD);
    CHECK(gw_send_begin(c2, &q2, "run1", 10, t, &r2) == GW_SA_FORWARD);
    CHECK(send_accept(r, 11) == GW_SO_DONE);
    CHECK(gw_send_answered(r2, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
    CHECK(gw_send_begin(c2, &q2, "run1", 11, t, &r2) == GW_SA_LOOKUP);
    CHECK(send_look(r2, "[" OUT(10, "second") "," OUT(11, "twice") "]") == GW_SL_ABSENT);
    gw_send_unresolved(r2);
    CHECK(r2->state == GW_SS_UNCERTAIN && gw_send_begin(c2, &q2, "run1", 11, t, &r2) == GW_SA_LOOKUP);
    CHECK(send_look(r2, "[" OUT(11, "twice") "," OUT(12, "twice") "]") == GW_SL_FOUND && r2->message_id == 12);

    /* A list that does not reach back proves nothing: never sent again. */
    q = send_req(5, "far", -1);
    CHECK(gw_send_begin(c, &q, "run1", 12, t, &r) == GW_SA_FORWARD);
    CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
    CHECK(gw_send_begin(c, &q, "run1", 30, t, &r) == GW_SA_LOOKUP);
    CHECK(send_look(r, "[" OUT(14, "other") "," OUT(15, "other") "]") == GW_SL_UNKNOWN && r->state == GW_SS_UNKNOWN);
    CHECK(gw_send_begin(c, &q, "run1", 30, t, &r) == GW_SA_UNKNOWN && gw_send_begin(c, &q, NULL, 0, t, &r) == GW_SA_UNKNOWN);
    /* ... unless the message is in it. */
    q = send_req(6, "near", -1);
    CHECK(gw_send_begin(c, &q, "run1", 12, t, &r) == GW_SA_FORWARD);
    CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
    CHECK(gw_send_begin(c, &q, "run1", 30, t, &r) == GW_SA_LOOKUP);
    CHECK(send_look(r, "[" OUT(14, "other") "," OUT(15, "near") "]") == GW_SL_FOUND);
    /* An empty list proves absence only when nothing was ever seen; what is
     * not a list proves nothing. */
    q = send_req(7, "empty", -1);
    CHECK(gw_send_begin(c, &q, "run1", 15, t, &r) == GW_SA_FORWARD);
    CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
    CHECK(gw_send_begin(c, &q, "run1", 15, t, &r) == GW_SA_LOOKUP && send_look(r, "[]") == GW_SL_UNKNOWN);
    /* An outgoing message to the same place that came after, in other words
     * (meshcored reports text as it sanitised it): it may be the one. A list
     * whose entries cannot be read says nothing either. Nor does one whose
     * ring of that kind holds nothing from before and 16 from after: it may
     * have gone round. With 15 from after it cannot have: proven absent. */
    for (i = 0; i < 6; i++) {
        static const char *const lists[] = {
            "[" OUT(15, "x") "," OUT(16, "ambiguous?") "]",
            "[" OUT(15, "x") ",{\"id\":16,\"direction\":\"out\",\"kind\":\"direct\",\"text\":\"ambiguous\"}]",
            "[" OUT(15, "x") ",{\"id\":16,\"direction\":\"out\",\"text\":\"zzz\"}]",
            "[" OUT(15, "x") ",{\"direction\":\"in\",\"kind\":\"direct\",\"text\":\"zzz\"}]",
            "[{\"id\":3,\"direction\":\"out\",\"kind\":\"channel\",\"channel\":0,\"text\":\"old\"},{\"id\":20,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":21,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":22,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":23,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":24,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":25,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":26,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":27,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":28,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":29,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":30,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":31,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":32,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":33,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":34,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":35,\"direction\":\"in\",\"kind\":\"direct\"}]",
            "[{\"id\":3,\"direction\":\"out\",\"kind\":\"channel\",\"channel\":0,\"text\":\"old\"},{\"id\":20,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":21,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":22,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":23,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":24,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":25,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":26,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":27,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":28,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":29,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":30,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":31,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":32,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":33,\"direction\":\"in\",\"kind\":\"direct\"},{\"id\":34,\"direction\":\"in\",\"kind\":\"direct\"}]"};

        q = send_req(60 + i, "ambiguous", -1);
        CHECK(gw_send_begin(c2, &q, "run1", 15, t, &r) == GW_SA_FORWARD);
        CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
        CHECK(gw_send_begin(c2, &q, "run1", 15, t, &r) == GW_SA_LOOKUP);
        CHECK(send_look(r, lists[i]) == (i == 5 ? GW_SL_ABSENT : GW_SL_UNKNOWN));
    }
    q = send_req(8, "empty", -1);
    CHECK(gw_send_begin(c2, &q, "runX", 0, t, &r) == GW_SA_FORWARD);
    CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
    CHECK(gw_send_begin(c2, &q, "runX", 0, t, &r) == GW_SA_LOOKUP && send_look(r, "[]") == GW_SL_ABSENT);
    gw_send_unresolved(r);
    CHECK(gw_send_begin(c2, &q, "runX", 0, t, &r) == GW_SA_LOOKUP && send_look(r, "{\"a\":1}") == GW_SL_UNKNOWN);

    /* meshcored started again: what the old run took cannot be asked. */
    q = send_req(9, "restart", -1);
    CHECK(gw_send_begin(c, &q, "run1", 15, t, &r) == GW_SA_FORWARD);
    CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
    CHECK(gw_send_begin(c, &q, "run2", 0, t, &r) == GW_SA_UNKNOWN && r->state == GW_SS_UNKNOWN);
    CHECK(gw_send_begin(c, &q, "run1", 15, t, &r) == GW_SA_UNKNOWN);

    /* An answer that is neither a refusal nor an acceptance with an id. */
    q = send_req(10, "odd", -1);
    CHECK(gw_send_begin(c, &q, "run1", 15, t, &r) == GW_SA_FORWARD);
    p = parse("{\"accepted\":true}");
    CHECK(gw_send_answered(r, p, 0) == GW_SO_UNKNOWN && gw_send_begin(c, &q, "run1", 15, t, &r) == GW_SA_UNKNOWN);
    cJSON_Delete(p);
    q = send_req(11, "odd", -1);
    CHECK(gw_send_begin(c, &q, "run1", 15, t, &r) == GW_SA_FORWARD);
    p = parse("{\"accepted\":false,\"message_id\":16}");
    CHECK(gw_send_answered(r, p, 0) == GW_SO_UNKNOWN);
    cJSON_Delete(p);

    /* A channel: reported with the sender's name before the text. */
    gw_send_free(&GS);
    gw_send_init(&GS);
    c = gw_send_client(&GS, fa, NULL);
    t += 100000;
    q = send_req(20, "to all", 2);
    CHECK(gw_send_begin(c, &q, "run1", 3, t, &r) == GW_SA_FORWARD);
    p = parse("{\"accepted\":true,\"message_id\":4,\"route\":\"flood\",\"channel\":2,\"ack_expected\":false}");
    CHECK(gw_send_answered(r, p, 0) == GW_SO_DONE);
    cJSON_Delete(p);
    res = gw_send_result(r, false);
    CHECK(res != NULL && !has(res, "ack_timeout_ms") && cJSON_GetObjectItem(res, "channel")->valuedouble == 2 &&
          cJSON_IsFalse(cJSON_GetObjectItem(res, "ack_expected")));
    cJSON_Delete(res);
    q = send_req(21, "to all", 2);
    CHECK(gw_send_begin(c, &q, "run1", 4, t, &r) == GW_SA_FORWARD);
    CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
    CHECK(gw_send_begin(c, &q, "run1", 4, t, &r) == GW_SA_LOOKUP);
    CHECK(send_look(r, "[{\"id\":4,\"direction\":\"out\",\"kind\":\"channel\",\"channel\":2,\"text\":\"unit: to all\"},"
                       "{\"id\":5,\"direction\":\"out\",\"kind\":\"channel\",\"channel\":1,\"text\":\"unit: to all\"},"
                       "{\"id\":6,\"direction\":\"in\",\"kind\":\"channel\",\"channel\":2,\"text\":\"other: to all\"},"
                       "{\"id\":7,\"direction\":\"out\",\"kind\":\"direct\",\"peer_public_key\":\"" K64 "\",\"text\":\"to all\"}]")
          == GW_SL_ABSENT);
    gw_send_unresolved(r);
    CHECK(gw_send_begin(c, &q, "run1", 4, t, &r) == GW_SA_LOOKUP);
    /* "<sender_name>: <text>" exactly: a text that only ends the same way
     * is another message to the same place, and then nothing is proven. */
    CHECK(send_look(r, "[{\"id\":4,\"direction\":\"out\",\"kind\":\"channel\",\"channel\":2,\"text\":\"unit: to all\"},"
                       "{\"id\":8,\"direction\":\"out\",\"kind\":\"channel\",\"channel\":2,\"sender_name\":\"unit\","
                       "\"text\":\"unit: note: to all\"}]") == GW_SL_UNKNOWN);
    q = send_req(22, "to all", 2);
    CHECK(gw_send_begin(c, &q, "run1", 4, t, &r) == GW_SA_FORWARD);
    CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
    CHECK(gw_send_begin(c, &q, "run1", 4, t, &r) == GW_SA_LOOKUP);
    CHECK(send_look(r, "[{\"id\":4,\"direction\":\"out\",\"kind\":\"channel\",\"channel\":2,\"text\":\"unit: to all\"},"
                       "{\"id\":8,\"direction\":\"out\",\"kind\":\"channel\",\"channel\":2,\"sender_name\":\"unit\","
                       "\"text\":\"unit: to all\",\"state\":\"sent_flood\",\"ack_expected\":false}]") == GW_SL_FOUND &&
          r->message_id == 8);
    res = gw_send_result(r, true);
    CHECK(res != NULL && cJSON_GetObjectItem(res, "channel")->valuedouble == 2 &&
          cJSON_IsFalse(cJSON_GetObjectItem(res, "ack_expected")));
    cJSON_Delete(res);

    /* Resend: of this run only; the attempts say whether it was taken. */
    p = parse("{\"message_id\":3,\"run_id\":\"run1\",\"client_msg_id\":\"resend-0123456789\"}");
    CHECK(gw_send_parse(p, true, &q) == 0);
    cJSON_Delete(p);
    CHECK(gw_send_begin(c, &q, "run2", 0, t, &r) == GW_SA_STALE_RUN);
    CHECK(gw_send_begin(c, &q, "run1", 8, t, &r) == GW_SA_FORWARD && r->resend && r->attempts_before == -1);
    p = parse("{\"accepted\":true,\"message_id\":4,\"route\":\"flood\"}");
    CHECK(gw_send_answered(r, p, 0) == GW_SO_UNKNOWN); /* another message than the one named */
    cJSON_Delete(p);
    p = parse("{\"message_id\":3,\"run_id\":\"run1\",\"client_msg_id\":\"resend-0123456780\"}");
    CHECK(gw_send_parse(p, true, &q) == 0);
    cJSON_Delete(p);
    CHECK(gw_send_begin(c, &q, "run1", 8, t, &r) == GW_SA_FORWARD);
    p = parse("{\"accepted\":true,\"message_id\":3,\"route\":\"flood\",\"ack_expected\":true,\"ack_timeout_ms\":5000,"
              "\"resent\":true}");
    CHECK(gw_send_answered(r, p, 0) == GW_SO_DONE);
    cJSON_Delete(p);
    res = gw_send_result(r, false);
    CHECK(res != NULL && cJSON_IsTrue(cJSON_GetObjectItem(res, "resent")) && cJSON_GetArraySize(res) == 8);
    cJSON_Delete(res);
    for (i = 0; i < 4; i++) {
        static const char *const lists[] = {
            "[" OUT(3, "x") "]",                                                            /* attempts 1: not taken */
            "[{\"id\":3,\"direction\":\"out\",\"kind\":\"direct\",\"attempts\":2,\"state\":\"sent_direct\"}]", /* taken */
            "[" OUT(4, "x") "]",                                                            /* the message is gone */
            "[" OUT(3, "x") "]"};                                                           /* attempts not known before */
        static const enum gw_send_look want[] = {GW_SL_ABSENT, GW_SL_FOUND, GW_SL_UNKNOWN, GW_SL_UNKNOWN};
        char buf[160];

        snprintf(buf, sizeof(buf), "{\"message_id\":3,\"run_id\":\"run1\",\"client_msg_id\":\"resend-u-%08d\"}", i);
        p = parse(buf);
        CHECK(gw_send_parse(p, true, &q) == 0);
        cJSON_Delete(p);
        CHECK(gw_send_begin(c, &q, "run1", 8, t, &r) == GW_SA_FORWARD);
        r->attempts_before = i == 3 ? -1 : 1;
        CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
        CHECK(gw_send_begin(c, &q, "run1", 8, t, &r) == GW_SA_LOOKUP);
        CHECK(send_look(r, lists[i]) == want[i]);
        if (want[i] == GW_SL_FOUND) {
            res = gw_send_result(r, true);
            CHECK(res != NULL && cJSON_IsTrue(cJSON_GetObjectItem(res, "resent")) &&
                  strcmp(cJSON_GetObjectItem(res, "route")->valuestring, "direct") == 0);
            cJSON_Delete(res);
        }
    }

    /* The rate: 20 in a minute, whatever became of them; a repeat answered
     * from the record is not one. */
    gw_send_free(&GS);
    gw_send_init(&GS);
    c = gw_send_client(&GS, fa, NULL);
    c2 = gw_send_client(&GS, fb, NULL);
    t = 500000;
    for (i = 0; i < GW_SEND_PER_MIN; i++) {
        q = send_req(100 + i, "rate", -1);
        CHECK(gw_send_begin(c, &q, "run1", 0, t + i, &r) == GW_SA_FORWARD);
        if (i % 2) {
            CHECK(gw_send_answered(r, NULL, 5) == GW_SO_REFUSED);
        } else {
            CHECK(send_accept(r, 1 + i) == GW_SO_DONE);
        }
    }
    q2 = send_req(100, "rate", -1);
    CHECK(gw_send_begin(c, &q2, "run1", 0, t + 30, &r) == GW_SA_REPLY);
    q = send_req(200, "rate", -1);
    CHECK(gw_send_begin(c, &q, "run1", 0, t + 59000, &r) == GW_SA_RATE);
    CHECK(gw_send_begin(c2, &q, "run1", 0, t + 59000, &r) == GW_SA_FORWARD); /* another client's own budget */
    CHECK(gw_send_begin(c, &q, "run1", 0, t + GW_SEND_WINDOW_MS - 1, &r) == GW_SA_RATE);
    CHECK(gw_send_begin(c, &q, "run1", 0, t + GW_SEND_WINDOW_MS, &r) == GW_SA_FORWARD);
    CHECK(gw_send_begin(c, &q2, "run1", 0, t + GW_SEND_WINDOW_MS, &r) == GW_SA_REPLY);
    q = send_req(201, "rate", -1);
    CHECK(gw_send_begin(c, &q, "run1", 0, t + GW_SEND_WINDOW_MS, &r) == GW_SA_RATE);
    /* Finding out about an unanswered send is under the same limit. */
    q = send_req(119, "rate", -1); /* was refused: gone; make it uncertain */
    t += 10 * GW_SEND_WINDOW_MS;
    CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_FORWARD);
    CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
    for (i = 0; i < GW_SEND_PER_MIN - 1; i++) {
        CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_LOOKUP);
        gw_send_unresolved(r);
    }
    CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_RATE && r->state == GW_SS_UNCERTAIN);

    /* At most 8 of one client wait at once. */
    gw_send_free(&GS);
    gw_send_init(&GS);
    c = gw_send_client(&GS, fa, NULL);
    for (i = 0; i < GW_SEND_INFLIGHT; i++) {
        q = send_req(300 + i, "wait", -1);
        CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_FORWARD);
    }
    q = send_req(399, "wait", -1);
    CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_BUSY && gw_send_inflight(c) == GW_SEND_INFLIGHT);

    /* The last 64 ids are remembered; the oldest that waits for nothing
     * makes room. */
    gw_send_free(&GS);
    gw_send_init(&GS);
    c = gw_send_client(&GS, fa, NULL);
    q = send_req(1000, "keep", -1);
    CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_FORWARD); /* stays PENDING throughout */
    for (i = 1; i <= GW_SEND_RECORDS; i++) {
        t += GW_SEND_WINDOW_MS;
        q = send_req(1000 + i, "keep", -1);
        CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_FORWARD);
        CHECK(send_accept(r, i) == GW_SO_DONE);
    }
    t += GW_SEND_WINDOW_MS;
    q = send_req(1000, "keep", -1);
    CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_IN_PROGRESS); /* a waiting record is never replaced */
    q = send_req(1002, "keep", -1);
    CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_REPLY);
    q = send_req(1001, "keep", -1);
    CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_FORWARD);    /* the oldest was forgotten */
    /* An id whose outcome is not settled outlives the settled ones: 64
     * further sends do not make a repeat of it a new message. */
    gw_send_free(&GS);
    gw_send_init(&GS);
    c = gw_send_client(&GS, fa, NULL);
    q = send_req(2000, "open", -1);
    CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_FORWARD);
    CHECK(gw_send_answered(r, NULL, GW_ERR_UPSTREAM) == GW_SO_UNCERTAIN);
    q = send_req(2001, "open", -1);
    t += GW_SEND_WINDOW_MS;
    CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_FORWARD);
    p = parse("{\"accepted\":true}");
    CHECK(gw_send_answered(r, p, 0) == GW_SO_UNKNOWN);
    cJSON_Delete(p);
    for (i = 2; i < 2 + 2 * GW_SEND_RECORDS; i++) {
        t += GW_SEND_WINDOW_MS;
        q = send_req(2000 + i, "open", -1);
        CHECK(gw_send_begin(c, &q, "run1", 0, t, &r) == GW_SA_FORWARD);
        CHECK(send_accept(r, i) == GW_SO_DONE);
    }
    t += GW_SEND_WINDOW_MS;
    q = send_req(2000, "open", -1);
    CHECK(gw_send_begin(c, &q, "run1", 200, t, &r) == GW_SA_LOOKUP);
    q = send_req(2001, "open", -1);
    CHECK(gw_send_begin(c, &q, "run1", 200, t, &r) == GW_SA_UNKNOWN);

    /* A client's records live as long as it is paired, when room is short. */
    gw_send_free(&GS);
    gw_send_init(&GS);
    memset(fx, 0, sizeof(fx));
    for (i = 0; i < GW_MAX_CLIENTS; i++) {
        fx[0] = (uint8_t)(10 + i);
        CHECK(gw_send_client(&GS, fx, all_paired) != NULL);
    }
    fx[0] = 99;
    CHECK(gw_send_client(&GS, fx, all_paired) == NULL && gw_send_client(&GS, fx, NULL) == NULL);
    CHECK(gw_send_client(&GS, fx, nobody_paired) != NULL);
    gw_send_free(&GS);
}
