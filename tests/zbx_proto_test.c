/*
 * The Zabbix app/helper line protocol: every line written by one side read
 * back by the other, sets handed over only when complete, and every kind of
 * damage refused rather than half-applied.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "zabbix/zbx_proto.h"

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

/* Everything written to a memory stream, then fed line by line to rx; the
 * kinds seen are counted and the last message is kept. */
struct feed {
    int kinds[ZBX_RX_BAD + 1];
    struct zbx_rx_msg last;
    int lines;
};

static void feed_text(struct zbx_rx *rx, const char *text, struct feed *f)
{
    char *copy = strdup(text);
    char *save = NULL;
    char *line;

    memset(f, 0, sizeof(*f));
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        struct zbx_rx_msg m;
        enum zbx_rx_kind k = zbx_rx_line(rx, line, &m);

        f->kinds[k]++;
        f->lines++;
        if (k != ZBX_RX_NONE) {
            f->last = m;
        }
    }
    free(copy);
}

static struct zbx_problem problem(int i, int sev)
{
    struct zbx_problem p;

    memset(&p, 0, sizeof(p));
    snprintf(p.eventid, sizeof(p.eventid), "%d", 5000 + i);
    snprintf(p.objectid, sizeof(p.objectid), "%d", 7000 + i);
    snprintf(p.hostid, sizeof(p.hostid), "%d", 10000 + i % 3);
    snprintf(p.host, sizeof(p.host), "host-%d", i % 3);
    snprintf(p.name, sizeof(p.name), "Problem number %d with\ta tab", i);
    p.severity = (int8_t)sev;
    p.acknowledged = i % 2;
    p.clock = 1790000000LL - i;
    return p;
}

static void test_round_trip(void)
{
    static struct zbx_problem_set ps;
    static struct zbx_host_set hs;
    static struct zbx_detail d;
    static struct zbx_rx rx;
    struct feed f;
    FILE *out;
    size_t len;
    char *buf = NULL;
    int i;

    memset(&ps, 0, sizeof(ps));
    for (i = 0; i < ZBX_PROBLEM_MAX; i++) {
        ps.p[i] = problem(i, i % 6);
    }
    ps.count = ZBX_PROBLEM_MAX;
    ps.total = 1234;
    ps.total_exact = true;
    ps.sev_exact = true;
    ps.unacknowledged = 77;
    ps.sev_count[5] = 3;
    ps.ref_clock = 1790000123;
    memset(&hs, 0, sizeof(hs));
    for (i = 0; i < ZBX_HOST_MAX; i++) {
        struct zbx_host *h = &hs.h[i];

        snprintf(h->hostid, sizeof(h->hostid), "%d", 10000 + i);
        snprintf(h->name, sizeof(h->name), "h%d", i);
        h->avail = (enum zbx_avail)(i % 3);
        h->maintenance = i == 5;
        h->problems = i % 4;
        h->max_severity = (int8_t)(i % 4 ? i % 6 : -1);
    }
    hs.count = ZBX_HOST_MAX;
    hs.total = 700;
    hs.fetched = 500;
    hs.down = 9;
    hs.down_exact = true;
    memset(&d, 0, sizeof(d));
    snprintf(d.host.hostid, sizeof(d.host.hostid), "10042");
    snprintf(d.host.name, sizeof(d.host.name), "db-osl-02");
    d.host.avail = ZBX_AVAIL_DOWN;
    d.p[0] = problem(1, 4);
    d.problem_count = 1;
    snprintf(d.item[0].itemid, sizeof(d.item[0].itemid), "4001");
    snprintf(d.item[0].name, sizeof(d.item[0].name), "CPU utilization");
    snprintf(d.item[0].value, sizeof(d.item[0].value), "23.4 %%");
    snprintf(d.item[0].units, sizeof(d.item[0].units), "%%");
    d.item[0].clock = 1790000000;
    snprintf(d.item[1].itemid, sizeof(d.item[1].itemid), "4002");
    snprintf(d.item[1].name, sizeof(d.item[1].name), "Empty value");
    d.item_count = 2;

    out = open_memstream(&buf, &len);
    zbx_proto_hello(out, true, "demo");
    zbx_proto_config(out, "Prod\tuction", "https://z.example/zabbix/", "token", true, 30, 60);
    zbx_proto_state(out, ZBX_CONN_RETRYING, 3, 20, ZBX_ERR_TIMEOUT, "took too\nlong");
    zbx_proto_busy(out, "problems");
    zbx_proto_idle(out);
    zbx_proto_version(out, "7.0.31");
    zbx_proto_problems(out, 7, &ps);
    zbx_proto_hosts(out, 8, &hs);
    zbx_proto_detail(out, 9, &d);
    zbx_proto_detail_none(out, "10099", "gone");
    zbx_proto_bye(out);
    fclose(out);

    {
        char *p = buf;
        size_t longest = 0;

        while (*p) {
            char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);

            if (n > longest) {
                longest = n;
            }
            p += n + (nl ? 1 : 0);
        }
        check("the longest line is under the limit", longest < ZBX_LINE_MAX);
    }
    zbx_rx_init(&rx);
    feed_text(&rx, buf, &f);
    check("hello, config, state, busy, idle, version, bye all read",
          f.kinds[ZBX_RX_HELLO] == 1 && f.kinds[ZBX_RX_CONFIG] == 1 && f.kinds[ZBX_RX_STATE] == 1 &&
              f.kinds[ZBX_RX_BUSY] == 1 && f.kinds[ZBX_RX_IDLE] == 1 && f.kinds[ZBX_RX_VERSION] == 1 &&
              f.kinds[ZBX_RX_BYE] == 1);
    check("three sets and a dnone", f.kinds[ZBX_RX_PROBLEMS] == 1 && f.kinds[ZBX_RX_HOSTS] == 1 &&
                                        f.kinds[ZBX_RX_DETAIL] == 1 && f.kinds[ZBX_RX_DETAIL_NONE] == 1);
    check("nothing refused", f.kinds[ZBX_RX_BAD] == 0 && rx.bad == 0);
    check("the problem set arrives whole", rx.problems.count == ZBX_PROBLEM_MAX &&
                                               rx.problems.total == 1234 && rx.problems.unacknowledged == 77 &&
                                               rx.problems.sev_count[5] == 3 &&
                                               rx.problems.ref_clock == 1790000123);
    check("a problem's fields survive, its tab turned to a space",
          strcmp(rx.problems.p[3].eventid, "5003") == 0 && rx.problems.p[3].severity == 3 &&
              rx.problems.p[3].acknowledged && strcmp(rx.problems.p[3].host, "host-0") == 0 &&
              strcmp(rx.problems.p[3].name, "Problem number 3 with a tab") == 0 &&
              rx.problems.p[3].clock == 1790000000LL - 3);
    check("the host set arrives whole", rx.hosts.count == ZBX_HOST_MAX && rx.hosts.total == 700 &&
                                            rx.hosts.fetched == 500 && rx.hosts.down == 9 &&
                                            rx.hosts.down_exact &&
                                            rx.hosts.h[5].maintenance &&
                                            rx.hosts.h[5].max_severity == 5 &&
                                            rx.hosts.h[8].max_severity == -1 &&
                                            rx.hosts.h[2].avail == ZBX_AVAIL_DOWN);
    check("the detail arrives with its problem and items",
          strcmp(rx.detail.host.name, "db-osl-02") == 0 && rx.detail.host.avail == ZBX_AVAIL_DOWN &&
              rx.detail.problem_count == 1 && rx.detail.host.problems == 1 &&
              rx.detail.host.max_severity == 4 && rx.detail.item_count == 2 &&
              strcmp(rx.detail.item[0].value, "23.4 %") == 0 && rx.detail.item[1].value[0] == '\0');
    free(buf);

    /* The single-line messages, one at a time. */
    {
        char l1[] = "config\tProd\tuction\thttps://z.example/zabbix/\ttoken\t1\t30\t60";
        char l2[] = "state\tretrying\t3\t20\ttimeout\ttook too long";
        char l3[] = "hello\t1\tlive\t-";
        char l4[] = "dnone\t10099\tgone";
        struct zbx_rx_msg m;

        check("a field with a tab in it is not what config reads", zbx_rx_line(&rx, l1, &m) == ZBX_RX_BAD);
        check("state", zbx_rx_line(&rx, l2, &m) == ZBX_RX_STATE && m.state == ZBX_CONN_RETRYING &&
                           m.attempt == 3 && m.retry_s == 20 && m.err == ZBX_ERR_TIMEOUT &&
                           strcmp(m.text, "took too long") == 0);
        check("hello live, no scenario", zbx_rx_line(&rx, l3, &m) == ZBX_RX_HELLO && !m.fake &&
                                             m.word[0] == '\0' && m.proto == 1);
        check("dnone", zbx_rx_line(&rx, l4, &m) == ZBX_RX_DETAIL_NONE && strcmp(m.word, "10099") == 0 &&
                           strcmp(m.text, "gone") == 0);
    }
}

static void test_damage(void)
{
    static struct zbx_rx rx;
    struct feed f;

    zbx_rx_init(&rx);
    feed_text(&rx,
              "pbegin\t1\t100\t2\t1\t1\t1\t0\t0\t0\t0\t0\t2\n"
              "pb\t1\t2\t3\t5\t0\t0\t99\th\tname\n"
              "pb\t2\t2\t3\t5\t0\t0\t99\th\tname\n"
              "pend\t1\t2\n",
              &f);
    check("a well-formed set is delivered", f.kinds[ZBX_RX_PROBLEMS] == 1 && rx.problems.count == 2);

    feed_text(&rx,
              "pbegin\t2\t100\t2\t1\t1\t1\t0\t0\t0\t0\t0\t2\n"
              "pb\t1\t2\t3\t5\t0\t0\t99\th\tname\n",
              &f);
    check("a set without its end is not delivered", f.kinds[ZBX_RX_PROBLEMS] == 0);
    feed_text(&rx, "pend\t2\t2\n", &f);
    check("an end whose count disagrees is refused", f.kinds[ZBX_RX_BAD] == 1 && f.kinds[ZBX_RX_PROBLEMS] == 0);
    feed_text(&rx,
              "pbegin\t3\t100\t1\t1\t1\t0\t0\t0\t0\t0\t0\t1\n"
              "pb\t1\t2\t3\t5\t0\t0\t99\th\tname\n"
              "pend\t4\t1\n",
              &f);
    check("an end with another sequence number is refused", f.kinds[ZBX_RX_BAD] == 1 &&
                                                               f.kinds[ZBX_RX_PROBLEMS] == 0);
    feed_text(&rx, "pb\t1\t2\t3\t5\t0\t0\t99\th\tname\npend\t3\t1\n", &f);
    check("a record outside a set is refused, and the end with it", f.kinds[ZBX_RX_BAD] == 2);
    feed_text(&rx,
              "pbegin\t5\t100\t1\t1\t1\t0\t0\t0\t0\t0\t0\t1\n"
              "pb\t1\t2\t3\t9\t0\t0\t99\th\tname\n"
              "pend\t5\t0\n",
              &f);
    check("a severity of 9 is refused and the set dropped", f.kinds[ZBX_RX_BAD] >= 1 &&
                                                               f.kinds[ZBX_RX_PROBLEMS] == 0);
    feed_text(&rx,
              "pbegin\t6\t100\t1\t1\t1\t0\t0\t0\t0\t0\t0\t1\n"
              "pb\t1\t2\t3\t4\t0\t0\t99\th\n"
              "pend\t6\t0\n",
              &f);
    check("a record with a field missing is refused", f.kinds[ZBX_RX_BAD] >= 1 &&
                                                         f.kinds[ZBX_RX_PROBLEMS] == 0);
    feed_text(&rx,
              "pbegin\t7\t100\t1\t1\t1\t0\t0\t0\t0\t0\t0\t1\n"
              "pb\t1\t2\t3\t4\t0\t0\tx99\th\tname\n",
              &f);
    check("a clock that is not a number is refused", f.kinds[ZBX_RX_BAD] == 1);
    {
        /* More records than the model holds: the set is dropped, never
         * written past its end. */
        char *text = malloc(64 * (ZBX_PROBLEM_MAX + 10));
        char *p = text;
        int i;

        p += sprintf(p, "pbegin\t9\t100\t999\t1\t1\t0\t0\t0\t0\t0\t0\t0\n");
        for (i = 0; i < ZBX_PROBLEM_MAX + 5; i++) {
            p += sprintf(p, "pb\t%d\t2\t3\t1\t0\t0\t9\th\tn\n", i);
        }
        sprintf(p, "pend\t9\t%d\n", ZBX_PROBLEM_MAX + 5);
        feed_text(&rx, text, &f);
        check("more records than ZBX_PROBLEM_MAX drop the set", f.kinds[ZBX_RX_PROBLEMS] == 0 &&
                                                                   f.kinds[ZBX_RX_BAD] >= 1 &&
                                                                   rx.problems.count <= ZBX_PROBLEM_MAX);
        free(text);
    }
    feed_text(&rx,
              "hbegin\t1\t0\t1\t1\t1\t0\t0\t0\t1\n"
              "ho\t1\t3\t0\t0\t-1\tname\n"
              "hend\t1\t1\n",
              &f);
    check("an availability of 3 is refused", f.kinds[ZBX_RX_HOSTS] == 0 && f.kinds[ZBX_RX_BAD] >= 1);
    feed_text(&rx, "state\tsleeping\t0\t0\tnone\t-\n", &f);
    check("an unknown state is refused", f.kinds[ZBX_RX_BAD] == 1);
    feed_text(&rx, "state\tonline\t0\t0\tnope\t-\n", &f);
    check("an unknown error word is refused", f.kinds[ZBX_RX_BAD] == 1);
    feed_text(&rx, "flying\tsaucer\n", &f);
    check("an unknown word is ignored, not refused", f.kinds[ZBX_RX_NONE] == 1 && f.kinds[ZBX_RX_BAD] == 0);
    feed_text(&rx, "pbegin\t1\t2\t3\t4\t5\t6\t7\t8\t9\t10\t11\t12\t13\t14\t15\t16\t17\n", &f);
    check("a line with too many fields is refused", f.kinds[ZBX_RX_BAD] == 1);
    feed_text(&rx, "hello\t1\tweird\t-\n", &f);
    check("hello with an unknown mode is refused", f.kinds[ZBX_RX_BAD] == 1);
}

static void test_commands(void)
{
    struct zbx_cmd c;
    char a[] = "refresh";
    char b[] = "detail\t10084";
    char d[] = "detail\t-";
    char e[] = "scenario\tflap";
    char q[] = "quit";
    char x[] = "detail\t123456789012345678901234567890";
    char y[] = "refresh\tnow";
    char z[] = "reboot";

    check("refresh", zbx_cmd_parse(a, &c) == ZBX_CMD_REFRESH);
    check("detail with a host", zbx_cmd_parse(b, &c) == ZBX_CMD_DETAIL && strcmp(c.word, "10084") == 0);
    check("detail - stops it", zbx_cmd_parse(d, &c) == ZBX_CMD_DETAIL && c.word[0] == '\0');
    check("scenario", zbx_cmd_parse(e, &c) == ZBX_CMD_SCENARIO && strcmp(c.word, "flap") == 0);
    check("quit", zbx_cmd_parse(q, &c) == ZBX_CMD_QUIT);
    check("an id too long for the model is not a command", zbx_cmd_parse(x, &c) == ZBX_CMD_NONE);
    check("an extra field is not a command", zbx_cmd_parse(y, &c) == ZBX_CMD_NONE);
    check("an unknown word is not a command", zbx_cmd_parse(z, &c) == ZBX_CMD_NONE);
}

static void test_words(void)
{
    int i;
    bool all = true;

    for (i = 0; i < ZBX_ERR_COUNT; i++) {
        if (zbx_err_parse(zbx_err_word((enum zbx_err)i)) != i) {
            all = false;
        }
    }
    check("every error word parses back", all);
    all = true;
    for (i = 0; i <= ZBX_CONN_AUTH_FAILED; i++) {
        if (zbx_conn_state_parse(zbx_conn_state_word((enum zbx_conn_state)i)) != i) {
            all = false;
        }
    }
    check("every state word parses back", all);
    check("every error has a sentence", zbx_err_text(ZBX_ERR_TLS)[0] && zbx_err_text(ZBX_ERR_AUTH)[0] &&
                                            zbx_err_text(ZBX_ERR_NONE)[0] == '\0');
}

int main(void)
{
    test_round_trip();
    test_damage();
    test_commands();
    test_words();
    printf("zbx_proto_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
