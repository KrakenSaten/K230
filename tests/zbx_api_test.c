/*
 * The Zabbix JSON-RPC layer: the requests are what the documented API takes
 * (and nothing a strict 7.x server would refuse), the envelope is checked,
 * errors are told apart, and every result parser refuses a wrong shape
 * rather than inventing zeroes.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "zabbix/zbx_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static cJSON *parsed(char *s)
{
    cJSON *j = cJSON_Parse(s ? s : "");

    free(s);
    return j;
}

static const cJSON *get(const cJSON *o, const char *k)
{
    return cJSON_GetObjectItemCaseSensitive(o, k);
}

static bool str_is(const cJSON *v, const char *s)
{
    return cJSON_IsString(v) && strcmp(v->valuestring, s) == 0;
}

static void test_requests(void)
{
    struct zbx_req_ctx c = { .id = 7, .auth_in_body = NULL, .active_available = true };
    struct zbx_req_ctx old = { .id = 8, .auth_in_body = "tok60", .active_available = false };
    const char *tids[] = { "20001", "20002" };
    cJSON *j;
    const cJSON *p;

    j = parsed(zbx_req_version(1));
    check("apiinfo.version: envelope, empty params array, no auth",
          j && str_is(get(j, "jsonrpc"), "2.0") && str_is(get(j, "method"), "apiinfo.version") &&
              cJSON_IsArray(get(j, "params")) && cJSON_GetArraySize(get(j, "params")) == 0 &&
              !get(j, "auth") && get(j, "id")->valuedouble == 1);
    cJSON_Delete(j);

    j = parsed(zbx_req_login(2, "viewer", "s3cret"));
    p = get(j, "params");
    check("user.login takes username (not the removed user) and no auth",
          j && str_is(get(j, "method"), "user.login") && str_is(get(p, "username"), "viewer") &&
              str_is(get(p, "password"), "s3cret") && !get(p, "user") && !get(j, "auth"));
    cJSON_Delete(j);

    j = parsed(zbx_req_problems(&c, NULL, 500));
    p = get(j, "params");
    check("problem.get: trigger problems, unsuppressed, newest first, limited",
          j && str_is(get(j, "method"), "problem.get") && get(p, "source")->valuedouble == 0 &&
              get(p, "object")->valuedouble == 0 && cJSON_IsFalse(get(p, "suppressed")) &&
              str_is(get(p, "sortfield"), "eventid") && str_is(get(p, "sortorder"), "DESC") &&
              get(p, "limit")->valuedouble == 500 && !get(p, "hostids") && !get(p, "recent") &&
              cJSON_GetArraySize(get(p, "output")) == 7);
    check("from 6.4 the token is not in the body", !get(j, "auth"));
    cJSON_Delete(j);

    j = parsed(zbx_req_problems(&old, "10084", 100));
    p = get(j, "params");
    check("before 6.4 it is", str_is(get(j, "auth"), "tok60"));
    check("one host's problems", cJSON_GetArraySize(get(p, "hostids")) == 1 &&
                                     str_is(cJSON_GetArrayItem(get(p, "hostids"), 0), "10084"));
    cJSON_Delete(j);

    j = parsed(zbx_req_problem_count(&c, 4, false));
    p = get(j, "params");
    check("a count per severity", cJSON_IsTrue(get(p, "countOutput")) &&
                                      cJSON_GetArraySize(get(p, "severities")) == 1 &&
                                      cJSON_GetArrayItem(get(p, "severities"), 0)->valuedouble == 4 &&
                                      !get(p, "acknowledged") && !get(p, "limit"));
    cJSON_Delete(j);
    j = parsed(zbx_req_problem_count(&c, -1, true));
    p = get(j, "params");
    check("the unacknowledged count", cJSON_IsFalse(get(p, "acknowledged")) && !get(p, "severities"));
    cJSON_Delete(j);

    j = parsed(zbx_req_trigger_hosts(&c, tids, 2));
    p = get(j, "params");
    check("trigger.get for the problems' hosts",
          str_is(get(j, "method"), "trigger.get") && cJSON_GetArraySize(get(p, "triggerids")) == 2 &&
              cJSON_GetArraySize(get(p, "selectHosts")) == 2 && !get(p, "selectGroups"));
    cJSON_Delete(j);

    j = parsed(zbx_req_hosts(&c, NULL, 500));
    p = get(j, "params");
    check("host.get: monitored hosts with interfaces, active_available where known",
          cJSON_IsTrue(get(p, "monitored_hosts")) && cJSON_GetArraySize(get(p, "output")) == 5 &&
              cJSON_IsArray(get(p, "selectInterfaces")) && get(p, "limit")->valuedouble == 500 &&
              !get(p, "selectGroups"));
    cJSON_Delete(j);
    j = parsed(zbx_req_hosts(&old, "10084", 1));
    p = get(j, "params");
    check("a 6.0 server is not asked for active_available", cJSON_GetArraySize(get(p, "output")) == 4);
    check("one host by id, no limit", cJSON_GetArraySize(get(p, "hostids")) == 1 && !get(p, "limit"));
    cJSON_Delete(j);

    j = parsed(zbx_req_host_count(&c));
    p = get(j, "params");
    check("host count", cJSON_IsTrue(get(p, "countOutput")) && cJSON_IsTrue(get(p, "monitored_hosts")));
    cJSON_Delete(j);

    j = parsed(zbx_req_items(&c, "10084", 200));
    p = get(j, "params");
    check("item.get: one host's monitored items", str_is(get(j, "method"), "item.get") &&
                                                      cJSON_IsTrue(get(p, "monitored")) &&
                                                      get(p, "limit")->valuedouble == 200);
    cJSON_Delete(j);
}

static enum zbx_err reply_of(const char *body, int id, struct zbx_reply *r)
{
    return zbx_reply_parse(r, body, strlen(body), id);
}

static void test_replies(void)
{
    struct zbx_reply r;

    check("a result", reply_of("{\"jsonrpc\":\"2.0\",\"result\":\"7.0.31\",\"id\":3}", 3, &r) ==
                          ZBX_ERR_NONE && cJSON_IsString(r.result));
    zbx_reply_free(&r);
    check("an answer to another request is malformed",
          reply_of("{\"jsonrpc\":\"2.0\",\"result\":1,\"id\":4}", 3, &r) == ZBX_ERR_MALFORMED);
    zbx_reply_free(&r);
    check("not JSON-RPC 2.0", reply_of("{\"jsonrpc\":\"1.0\",\"result\":1,\"id\":3}", 3, &r) ==
                                  ZBX_ERR_MALFORMED);
    zbx_reply_free(&r);
    check("no result", reply_of("{\"jsonrpc\":\"2.0\",\"id\":3}", 3, &r) == ZBX_ERR_MALFORMED);
    zbx_reply_free(&r);
    check("half a document", reply_of("{\"jsonrpc\":\"2.0\",\"result\":[{\"eventid\":\"1\",", 3, &r) ==
                                 ZBX_ERR_MALFORMED);
    zbx_reply_free(&r);
    check("an HTML page", reply_of("<html>502 Bad Gateway</html>", 3, &r) == ZBX_ERR_MALFORMED);
    zbx_reply_free(&r);
    check("nothing at all", zbx_reply_parse(&r, "", 0, 3) == ZBX_ERR_MALFORMED);
    zbx_reply_free(&r);
    check("a bare array", reply_of("[1,2,3]", 3, &r) == ZBX_ERR_MALFORMED);
    zbx_reply_free(&r);

    check("Not authorized. is AUTH",
          reply_of("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"Invalid params.\","
                   "\"data\":\"Not authorized.\"},\"id\":3}",
                   3, &r) == ZBX_ERR_AUTH && r.code == -32602 &&
              strcmp(r.text, "Invalid params. Not authorized.") == 0);
    zbx_reply_free(&r);
    check("Not authorised. (6.0) is AUTH",
          reply_of("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"Invalid params.\","
                   "\"data\":\"Not authorised.\"},\"id\":null}",
                   3, &r) == ZBX_ERR_AUTH);
    zbx_reply_free(&r);
    check("an expired session is AUTH",
          reply_of("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"Invalid params.\","
                   "\"data\":\"Session terminated, re-login, please.\"},\"id\":3}",
                   3, &r) == ZBX_ERR_AUTH);
    zbx_reply_free(&r);
    check("an expired token is AUTH",
          reply_of("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32500,\"message\":\"Application error.\","
                   "\"data\":\"API token expired.\"},\"id\":3}",
                   3, &r) == ZBX_ERR_AUTH);
    zbx_reply_free(&r);
    check("a role without the method is an API error, not AUTH",
          reply_of("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32500,\"message\":\"Application error.\","
                   "\"data\":\"No permissions to call \\\"problem.get\\\".\"},\"id\":3}",
                   3, &r) == ZBX_ERR_API);
    zbx_reply_free(&r);
    check("an unknown parameter is an API error",
          reply_of("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"Invalid params.\","
                   "\"data\":\"Invalid parameter \\\"/\\\": unexpected parameter \\\"x\\\".\"},\"id\":3}",
                   3, &r) == ZBX_ERR_API);
    zbx_reply_free(&r);
    check("an error without a code is malformed",
          reply_of("{\"jsonrpc\":\"2.0\",\"error\":{\"message\":\"x\"},\"id\":3}", 3, &r) ==
              ZBX_ERR_MALFORMED);
    zbx_reply_free(&r);
}

static cJSON *J(const char *s)
{
    return cJSON_Parse(s);
}

static void test_results(void)
{
    struct zbx_problem p[4];
    struct zbx_host h[3];
    struct zbx_item it[3];
    char v[16];
    int n = 0;
    int count = -1;
    cJSON *j;

    j = J("\"7.0.31\"");
    check("version", zbx_parse_version(j, v, sizeof(v)) == 0 && strcmp(v, "7.0.31") == 0);
    cJSON_Delete(j);
    j = J("\"seven\"");
    check("a version that is not one", zbx_parse_version(j, v, sizeof(v)) != 0);
    cJSON_Delete(j);
    check("version numbers", zbx_version_num("7.0.31") == 70031 && zbx_version_num("6.4") == 60400 &&
                                 zbx_version_num("7.4.0rc1") == 70400 && zbx_version_num("x") == -1 &&
                                 zbx_version_num("7") == -1 && zbx_version_num(NULL) == -1);

    j = J("\"1234\"");
    check("a count in a string", zbx_parse_count(j, &count) == 0 && count == 1234);
    cJSON_Delete(j);
    j = J("12");
    check("a count as a number", zbx_parse_count(j, &count) == 0 && count == 12);
    cJSON_Delete(j);
    j = J("\"12x\"");
    check("a count that is not one", zbx_parse_count(j, &count) != 0);
    cJSON_Delete(j);
    j = J("\"-5\"");
    check("a negative count", zbx_parse_count(j, &count) != 0);
    cJSON_Delete(j);

    j = J("[{\"eventid\":\"9\",\"objectid\":\"20\",\"clock\":\"1790000000\",\"name\":\"CPU\","
          "\"severity\":\"4\",\"acknowledged\":\"1\",\"suppressed\":\"0\"},"
          "{\"eventid\":10,\"objectid\":21,\"clock\":1790000100,\"name\":\"Disk\","
          "\"severity\":2,\"acknowledged\":0},"
          "{\"eventid\":\"11\",\"objectid\":\"22\",\"clock\":\"1\",\"name\":\"Hidden\","
          "\"severity\":\"5\",\"acknowledged\":\"0\",\"suppressed\":\"1\"}]");
    check("problems, strings or numbers", zbx_parse_problems(j, p, 4, &n) == 0 && n == 2 &&
                                              strcmp(p[0].eventid, "9") == 0 && p[0].severity == 4 &&
                                              p[0].acknowledged && p[0].clock == 1790000000 &&
                                              strcmp(p[1].eventid, "10") == 0 && p[1].severity == 2 &&
                                              !p[1].acknowledged);
    check("a suppressed problem that came anyway is dropped", n == 2);
    cJSON_Delete(j);
    j = J("[{\"eventid\":\"9\",\"objectid\":\"20\",\"clock\":\"1\",\"severity\":\"4\","
          "\"acknowledged\":\"1\"}]");
    check("a problem without a name is refused", zbx_parse_problems(j, p, 4, &n) != 0);
    cJSON_Delete(j);
    j = J("[{\"eventid\":\"9\",\"objectid\":\"20\",\"clock\":\"soon\",\"name\":\"x\","
          "\"severity\":\"4\",\"acknowledged\":\"1\"}]");
    check("a clock that is not a number is refused", zbx_parse_problems(j, p, 4, &n) != 0);
    cJSON_Delete(j);
    j = J("{\"eventid\":\"9\"}");
    check("an object where an array belongs is refused", zbx_parse_problems(j, p, 4, &n) != 0);
    cJSON_Delete(j);
    j = J("[1,2]");
    check("numbers where objects belong are refused", zbx_parse_problems(j, p, 4, &n) != 0);
    cJSON_Delete(j);
    {
        char *big = malloc(200 * 10 + 64);
        char *q = big;
        int i;

        q += sprintf(q, "[");
        for (i = 0; i < 10; i++) {
            q += sprintf(q, "%s{\"eventid\":\"%d\",\"objectid\":\"1\",\"clock\":\"1\",\"name\":\"n\","
                            "\"severity\":\"1\",\"acknowledged\":\"0\"}",
                         i ? "," : "", i);
        }
        sprintf(q, "]");
        j = J(big);
        free(big);
        check("more problems than room: max kept, all counted", zbx_parse_problems(j, p, 4, &n) == 0 &&
                                                                  n == 10 && strcmp(p[3].eventid, "3") == 0);
        cJSON_Delete(j);
    }

    memset(p, 0, sizeof(p));
    snprintf(p[0].objectid, sizeof(p[0].objectid), "20");
    snprintf(p[1].objectid, sizeof(p[1].objectid), "21");
    snprintf(p[2].objectid, sizeof(p[2].objectid), "20");
    j = J("[{\"triggerid\":\"20\",\"hosts\":[{\"hostid\":\"10084\",\"name\":\"Zabbix server\"},"
          "{\"hostid\":\"10085\",\"name\":\"second\"}]},{\"triggerid\":\"21\",\"hosts\":[]}]");
    check("trigger hosts: the first host, for every problem of the trigger",
          zbx_parse_trigger_hosts(j, p, 3) == 0 && strcmp(p[0].host, "Zabbix server") == 0 &&
              strcmp(p[2].hostid, "10084") == 0 && p[1].host[0] == '\0');
    cJSON_Delete(j);
    j = J("[{\"triggerid\":\"20\"}]");
    check("a trigger without its hosts member is refused", zbx_parse_trigger_hosts(j, p, 3) != 0);
    cJSON_Delete(j);

    j = J("[{\"hostid\":\"1\",\"host\":\"tech\",\"name\":\"Visible\",\"maintenance_status\":\"1\","
          "\"interfaces\":[{\"type\":\"1\",\"main\":\"1\",\"available\":\"1\"},"
          "{\"type\":\"2\",\"main\":\"1\",\"available\":\"2\"}]},"
          "{\"hostid\":\"2\",\"host\":\"only-tech\",\"name\":\"\",\"maintenance_status\":\"0\","
          "\"interfaces\":[{\"available\":\"1\"}],\"active_available\":\"1\"},"
          "{\"hostid\":\"3\",\"host\":\"h3\",\"name\":\"h3\",\"interfaces\":[],\"active_available\":\"0\"}]");
    check("hosts: visible name, maintenance, one bad interface is DOWN",
          zbx_parse_hosts(j, h, 3, &n) == 0 && n == 3 && strcmp(h[0].name, "Visible") == 0 &&
              h[0].maintenance && h[0].avail == ZBX_AVAIL_DOWN && h[0].max_severity == ZBX_SEV_NONE);
    check("the technical name when there is no visible one, UP", strcmp(h[1].name, "only-tech") == 0 &&
                                                                     h[1].avail == ZBX_AVAIL_UP);
    check("no interfaces and nothing active: UNKNOWN", h[2].avail == ZBX_AVAIL_UNKNOWN);
    cJSON_Delete(j);
    j = J("[{\"hostid\":\"1\",\"name\":\"x\",\"interfaces\":[{\"available\":\"1\"}],"
          "\"active_available\":\"2\"}]");
    check("active checks unavailable make the host DOWN", zbx_parse_hosts(j, h, 3, &n) == 0 &&
                                                              h[0].avail == ZBX_AVAIL_DOWN);
    cJSON_Delete(j);
    j = J("[{\"hostid\":\"1\",\"name\":\"x\",\"interfaces\":{\"a\":1}}]");
    check("interfaces that are not an array are refused", zbx_parse_hosts(j, h, 3, &n) != 0);
    cJSON_Delete(j);
    j = J("[{\"name\":\"x\"}]");
    check("a host without an id is refused", zbx_parse_hosts(j, h, 3, &n) != 0);
    cJSON_Delete(j);

    j = J("[{\"itemid\":\"1\",\"name\":\"Zeta custom\",\"key_\":\"custom.thing\",\"lastvalue\":\"5\","
          "\"lastclock\":\"100\",\"units\":\"\",\"value_type\":\"3\"},"
          "{\"itemid\":\"2\",\"name\":\"CPU utilization\",\"key_\":\"system.cpu.util\","
          "\"lastvalue\":\"23.456\",\"lastclock\":\"100\",\"units\":\"%\",\"value_type\":\"0\"},"
          "{\"itemid\":\"3\",\"name\":\"Never\",\"key_\":\"agent.ping\",\"lastvalue\":\"0\","
          "\"lastclock\":\"0\",\"units\":\"\",\"value_type\":\"3\"},"
          "{\"itemid\":\"4\",\"name\":\"Ping\",\"key_\":\"agent.ping\",\"lastvalue\":\"1\","
          "\"lastclock\":\"100\",\"units\":\"\",\"value_type\":\"3\"}]");
    check("items: the known keys first, never-collected ones left out",
          zbx_parse_items(j, it, 3, &n) == 0 && n == 3 && strcmp(it[0].name, "Ping") == 0 &&
              strcmp(it[0].value, "Up") == 0 && strcmp(it[1].name, "CPU utilization") == 0 &&
              strcmp(it[1].value, "23.46 %") == 0 && strcmp(it[2].name, "Zeta custom") == 0);
    cJSON_Delete(j);
    j = J("[{\"itemid\":\"1\",\"name\":\"x\",\"key_\":\"k\",\"lastclock\":\"1\"}]");
    check("an item without a value is refused", zbx_parse_items(j, it, 3, &n) != 0);
    cJSON_Delete(j);
}

static void test_values(void)
{
    char s[40];

    zbx_format_value(s, sizeof(s), "vm.memory.size[total]", "8589934592", "B", 3);
    check("bytes in GB", strcmp(s, "8 GB") == 0);
    zbx_format_value(s, sizeof(s), "net.if.in[eth0]", "12345678", "bps", 3);
    check("bits in Mbps", strcmp(s, "12.35 Mbps") == 0);
    zbx_format_value(s, sizeof(s), "system.uptime", "1234567", "uptime", 3);
    check("uptime in days and hours", strcmp(s, "14d 6h") == 0);
    zbx_format_value(s, sizeof(s), "icmppingsec", "0.0023", "s", 0);
    check("a fraction of a second in ms", strcmp(s, "2.3 ms") == 0);
    zbx_format_value(s, sizeof(s), "system.cpu.load[all,avg1]", "0.40", "", 0);
    check("a plain number trimmed", strcmp(s, "0.4") == 0);
    zbx_format_value(s, sizeof(s), "icmpping", "0", "", 3);
    check("icmpping 0 is Down", strcmp(s, "Down") == 0);
    zbx_format_value(s, sizeof(s), "system.hostname", "web-01", "", 1);
    check("text passes through", strcmp(s, "web-01") == 0);
    zbx_format_value(s, sizeof(s), "x", "not a number", "%", 0);
    check("a number that is not one passes through", strcmp(s, "not a number") == 0);
    zbx_format_value(s, sizeof(s), "x", "1234.5", "", 0);
    check("a large number without decimals", strcmp(s, "1235") == 0 || strcmp(s, "1234") == 0);

    check("HTTP date", zbx_http_date("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777LL);
    check("HTTP date with leading space", zbx_http_date(" Thu, 25 Sep 2026 06:16:12 GMT") == 1790316972LL);
    check("not an HTTP date", zbx_http_date("yesterday") == -1 && zbx_http_date(NULL) == -1 &&
                                  zbx_http_date("Sun, 06 Foo 1994 08:49:37 GMT") == -1);
}

int main(void)
{
    test_requests();
    test_replies();
    test_results();
    test_values();
    printf("zbx_api_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
