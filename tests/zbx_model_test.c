/*
 * The Zabbix viewer's bounded model on its own: the words, the text cutting,
 * the orders, the counts and the ages. No JSON, no processes.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zabbix/zbx_model.h"

#include <stdio.h>
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

static struct zbx_problem prob(const char *eventid, int sev, long long clock, const char *hostid)
{
    struct zbx_problem p;

    memset(&p, 0, sizeof(p));
    snprintf(p.eventid, sizeof(p.eventid), "%s", eventid);
    snprintf(p.hostid, sizeof(p.hostid), "%s", hostid);
    p.severity = (int8_t)sev;
    p.clock = clock;
    return p;
}

static struct zbx_host host(const char *id, const char *name, enum zbx_avail a, bool maint)
{
    struct zbx_host h;

    memset(&h, 0, sizeof(h));
    snprintf(h.hostid, sizeof(h.hostid), "%s", id);
    snprintf(h.name, sizeof(h.name), "%s", name);
    h.avail = a;
    h.maintenance = maint;
    h.max_severity = ZBX_SEV_NONE;
    return h;
}

static void test_words(void)
{
    check("severity words, disaster to not classified",
          strcmp(zbx_severity_word(5), "DISASTER") == 0 && strcmp(zbx_severity_word(4), "HIGH") == 0 &&
              strcmp(zbx_severity_word(3), "AVERAGE") == 0 &&
              strcmp(zbx_severity_word(2), "WARNING") == 0 && strcmp(zbx_severity_word(1), "INFO") == 0 &&
              strcmp(zbx_severity_word(0), "N/C") == 0);
    check("no problem reads OK, and out-of-range severities too",
          strcmp(zbx_severity_word(-1), "OK") == 0 && strcmp(zbx_severity_word(9), "OK") == 0);
    check("severity names", strcmp(zbx_severity_name(0), "Not classified") == 0 &&
                                strcmp(zbx_severity_name(5), "Disaster") == 0);
    check("clamp", zbx_severity_clamp(-7) == -1 && zbx_severity_clamp(3) == 3 &&
                       zbx_severity_clamp(99) == 5);
    check("availability words", strcmp(zbx_avail_word(ZBX_AVAIL_UP), "UP") == 0 &&
                                    strcmp(zbx_avail_word(ZBX_AVAIL_DOWN), "DOWN") == 0 &&
                                    strcmp(zbx_avail_word(ZBX_AVAIL_UNKNOWN), "UNKNOWN") == 0);
}

static void test_avail(void)
{
    bool up = false;
    enum zbx_avail a = ZBX_AVAIL_UNKNOWN;

    a = zbx_avail_combine(a, 0, &up);
    check("an unchecked interface leaves a host UNKNOWN", a == ZBX_AVAIL_UNKNOWN && !up);
    a = zbx_avail_combine(a, 1, &up);
    check("one available interface makes it UP", a == ZBX_AVAIL_UP && up);
    a = zbx_avail_combine(a, 0, &up);
    check("an unchecked one beside it keeps it UP", a == ZBX_AVAIL_UP);
    a = zbx_avail_combine(a, 2, &up);
    check("any unavailable interface makes it DOWN", a == ZBX_AVAIL_DOWN);
    a = zbx_avail_combine(a, 1, &up);
    check("and nothing after that brings it back", a == ZBX_AVAIL_DOWN);
}

static void test_text(void)
{
    char s[16];
    char tiny[4];
    char one[1];
    bool fit;

    fit = zbx_copy_text(s, sizeof(s), "web-01");
    check("a short name is copied whole", fit && strcmp(s, "web-01") == 0);
    zbx_copy_text(s, sizeof(s), "a\tb\nc\rd");
    check("tab, newline and return become spaces", strcmp(s, "a b c d") == 0);
    fit = zbx_copy_text(s, sizeof(s), "abcdefghijklmnopqrstuvwxyz");
    check("a long name ends in ... and fits", !fit && strlen(s) == 15 && strcmp(s + 12, "...") == 0);
    /* 15 bytes of room: "abcdefghijk" (11) + "\xc3\xa6" would be 13, "..." 16 - one too many. */
    zbx_copy_text(s, sizeof(s), "abcdefghijk\xc3\xa6\xc3\xb8\xc3\xa5xyz");
    check("a cut never splits a UTF-8 character",
          strlen(s) <= 15 && strcmp(s + strlen(s) - 3, "...") == 0 &&
              (((unsigned char)s[strlen(s) - 4] & 0xC0) != 0xC0));
    zbx_copy_text(s, sizeof(s), "ok \xc3\xa6\xc3\xb8");
    check("whole UTF-8 characters pass untouched", strcmp(s, "ok \xc3\xa6\xc3\xb8") == 0);
    zbx_copy_text(s, sizeof(s), "bad \xff\xc3 x");
    check("invalid bytes become ?", strcmp(s, "bad ?? x") == 0);
    zbx_copy_text(tiny, sizeof(tiny), "abcdef");
    check("a 4-byte buffer holds ...", strcmp(tiny, "...") == 0);
    zbx_copy_text(one, sizeof(one), "abc");
    check("a 1-byte buffer holds nothing", one[0] == '\0');
    zbx_copy_text(s, sizeof(s), NULL);
    check("NULL copies as empty", s[0] == '\0');
}

static void test_ids(void)
{
    check("ids order numerically", zbx_id_cmp("999", "1000") < 0 && zbx_id_cmp("1000", "999") > 0);
    check("equal ids", zbx_id_cmp("00123", "123") == 0);
    check("non-numeric ids sort after numeric ones", zbx_id_cmp("abc", "5") > 0 &&
                                                         zbx_id_cmp("5", "abc") < 0);
}

static void test_sorts(void)
{
    struct zbx_problem p[5];
    struct zbx_host h[6];

    p[0] = prob("10", 2, 100, "1");
    p[1] = prob("11", 5, 50, "1");
    p[2] = prob("12", 2, 300, "2");
    p[3] = prob("13", 4, 10, "2");
    p[4] = prob("14", 2, 300, "3");
    zbx_problems_sort(p, 5);
    check("problems: disaster first, then high",
          strcmp(p[0].eventid, "11") == 0 && strcmp(p[1].eventid, "13") == 0);
    check("then the newest of a severity, then the higher eventid",
          strcmp(p[2].eventid, "14") == 0 && strcmp(p[3].eventid, "12") == 0 &&
              strcmp(p[4].eventid, "10") == 0);

    h[0] = host("1", "zulu", ZBX_AVAIL_UP, false);
    h[1] = host("2", "alpha", ZBX_AVAIL_UP, false);
    h[2] = host("3", "Bravo", ZBX_AVAIL_DOWN, false);
    h[3] = host("4", "charlie", ZBX_AVAIL_UNKNOWN, false);
    h[4] = host("5", "delta", ZBX_AVAIL_DOWN, true);
    h[5] = host("6", "echo", ZBX_AVAIL_UP, false);
    h[5].max_severity = 3;
    h[5].problems = 1;
    zbx_hosts_sort(h, 6);
    check("hosts: a host with a problem first", strcmp(h[0].name, "echo") == 0);
    check("then DOWN, then UNKNOWN", strcmp(h[1].name, "Bravo") == 0 && strcmp(h[2].name, "charlie") == 0);
    check("then UP by name without case", strcmp(h[3].name, "alpha") == 0 && strcmp(h[4].name, "zulu") == 0);
    check("maintenance last, even when DOWN", strcmp(h[5].name, "delta") == 0);
    zbx_problems_sort(NULL, 3);
    zbx_hosts_sort(h, 0);
    check("sorting nothing is harmless", 1);
}

static void test_counts(void)
{
    static struct zbx_problem_set ps;
    static struct zbx_host_set hs;
    struct zbx_overview o;
    struct zbx_problem p[4];

    memset(&ps, 0, sizeof(ps));
    ps.p[0] = prob("1", 5, 1, "10");
    ps.p[1] = prob("2", 2, 1, "10");
    ps.p[2] = prob("3", 2, 1, "11");
    ps.p[1].acknowledged = true;
    ps.count = 3;
    ps.total = 3;
    ps.total_exact = true;
    zbx_problem_set_count(&ps);
    check("per-severity counts", ps.sev_count[5] == 1 && ps.sev_count[2] == 2 && ps.sev_count[0] == 0);
    check("unacknowledged count", ps.unacknowledged == 2);
    check("a complete set is exact", ps.sev_exact);
    ps.total = 50;
    zbx_problem_set_count(&ps);
    check("a set with more on the server is not", !ps.sev_exact);

    memset(&hs, 0, sizeof(hs));
    hs.h[0] = host("10", "a", ZBX_AVAIL_UP, false);
    hs.h[1] = host("11", "b", ZBX_AVAIL_DOWN, false);
    hs.h[2] = host("12", "c", ZBX_AVAIL_UNKNOWN, true);
    hs.count = 3;
    hs.total = 3;
    hs.fetched = 3;
    hs.total_exact = true;
    zbx_host_set_count(&hs);
    check("host counts", hs.down == 1 && hs.unknown == 1 && hs.maintenance == 1);
    memcpy(p, ps.p, sizeof(p[0]) * 3);
    zbx_hosts_apply(hs.h, hs.count, p, 3);
    check("problems land on their hosts, worst severity kept",
          hs.h[0].problems == 2 && hs.h[0].max_severity == 5 && hs.h[1].problems == 1 &&
              hs.h[1].max_severity == 2 && hs.h[2].problems == 0 && hs.h[2].max_severity == -1);
    p[3] = prob("9", 4, 1, "");
    zbx_hosts_apply(hs.h, hs.count, p, 4);
    check("a problem with no host counts for none", hs.h[0].problems == 2 && hs.h[1].problems == 1);

    ps.total = 3;
    zbx_problem_set_count(&ps);
    zbx_overview_build(&o, &ps, &hs);
    check("overview: highest severity is the disaster", o.max_severity == 5);
    check("overview: totals", o.problems_total == 3 && o.unacknowledged == 2 && o.hosts_total == 3 &&
                                  o.hosts_down == 1 && o.hosts_exact && o.problems_exact);
    hs.total = 900;
    zbx_overview_build(&o, &ps, &hs);
    check("overview: hosts counted over part of the estate are not exact", !o.hosts_exact);

    memset(&ps, 0, sizeof(ps));
    ps.total_exact = true;
    zbx_problem_set_count(&ps);
    zbx_overview_build(&o, &ps, NULL);
    check("overview: zero problems is severity NONE", o.max_severity == ZBX_SEV_NONE &&
                                                         o.problems_total == 0 && o.problems_exact);
    zbx_overview_build(&o, NULL, NULL);
    check("overview of nothing read yet", o.max_severity == ZBX_SEV_NONE && o.hosts_total == 0);
}

static void test_age(void)
{
    char s[24];
    const long long now = 1790000000LL;

    zbx_format_age(s, sizeof(s), now, now - 3);
    check("seconds ago is now", strcmp(s, "now") == 0);
    zbx_format_age(s, sizeof(s), now, now - 42);
    check("42s", strcmp(s, "42s") == 0);
    zbx_format_age(s, sizeof(s), now, now - 300);
    check("5m", strcmp(s, "5m") == 0);
    zbx_format_age(s, sizeof(s), now, now - 3 * 3600 - 12 * 60);
    check("3h 12m", strcmp(s, "3h 12m") == 0);
    zbx_format_age(s, sizeof(s), now, now - 7200);
    check("2h", strcmp(s, "2h") == 0);
    zbx_format_age(s, sizeof(s), now, now - 2 * 86400 - 4 * 3600);
    check("2d 4h", strcmp(s, "2d 4h") == 0);
    zbx_format_age(s, sizeof(s), now, now - 61 * 86400LL - 5000);
    check("61d", strcmp(s, "61d") == 0);
    zbx_format_age(s, sizeof(s), now, now + 30);
    check("half a minute in the future is skew: now", strcmp(s, "now") == 0);
    zbx_format_age(s, sizeof(s), now, now + 3600);
    check("an hour in the future is unknown", strcmp(s, "--") == 0);
    zbx_format_age(s, sizeof(s), 0, now);
    check("no reference time is unknown", strcmp(s, "--") == 0);
    zbx_format_age(s, sizeof(s), now, 0);
    check("no start time is unknown", strcmp(s, "--") == 0);
}

int main(void)
{
    test_words();
    test_avail();
    test_text();
    test_ids();
    test_sorts();
    test_counts();
    test_age();
    printf("zbx_model_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
