/*
 * sysd_logs tests: system.logs and system.crashes against a temporary log
 * directory holding the files pocketlog, pos-supervise and the crash handler
 * write (services/sysd/sysd_logs.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "sysd_logs.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;
static int checks;
static char dir[] = "/tmp/pos_logs.XXXXXX";

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

static void put(const char *name, const char *text)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        exit(1);
    }
    fputs(text, f);
    fclose(f);
}

static void rm(const char *name)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    unlink(path);
}

static cJSON *query(const char *params_json)
{
    struct sysd_logs_query q;
    char err[128] = "";
    cJSON *params = params_json ? cJSON_Parse(params_json) : NULL;
    cJSON *r;

    if (sysd_logs_parse_query(params, &q, err, sizeof(err)) < 0) {
        cJSON_Delete(params);
        return NULL;
    }
    r = sysd_logs_query(dir, &q);
    cJSON_Delete(params);
    return r;
}

static int parse_ok(const char *params_json)
{
    struct sysd_logs_query q;
    char err[128];
    cJSON *params = cJSON_Parse(params_json);
    int rc = sysd_logs_parse_query(params, &q, err, sizeof(err));

    cJSON_Delete(params);
    return rc == 0;
}

static const cJSON *entry(const cJSON *r, int i)
{
    return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(r, "entries"), i);
}

static const char *field(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int str_is(const cJSON *o, const char *k, const char *want)
{
    const char *v = field(o, k);

    return v && strcmp(v, want) == 0;
}

static double num(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsNumber(v) ? v->valuedouble : -1.0;
}

static int returned(const cJSON *r)
{
    return (int)num(r, "returned");
}

int main(void)
{
    cJSON *r;
    char big[SYSD_LOGS_TAIL_BYTES * 3];
    size_t off;
    int i;
    int ok;

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }

    /* ---- a directory that is not there ---- */
    {
        char missing[600];
        struct sysd_logs_query q = { SYSD_LOG_DEBUG, 10, NULL };

        snprintf(missing, sizeof(missing), "%s/nope", dir);
        r = sysd_logs_query(missing, &q);
        check("a missing log directory answers, unavailable",
              cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r, "available")) && returned(r) == 0);
        cJSON_Delete(r);
        r = sysd_crashes_list(missing);
        check("and has no crash reports",
              cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r, "available")) && num(r, "total") == 0);
        cJSON_Delete(r);
    }

    /* ---- an empty one ---- */
    r = query(NULL);
    check("an empty log directory is available with nothing in it",
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "available")) && returned(r) == 0);
    cJSON_Delete(r);

    /* ---- params ---- */
    check("no params is a query", parse_ok("{}"));
    check("level warn is a query", parse_ok("{\"level\":\"warn\",\"limit\":5}"));
    check("an unknown level is refused", !parse_ok("{\"level\":\"loud\"}"));
    check("a level that is not a string is refused", !parse_ok("{\"level\":2}"));
    check("limit 0 is refused", !parse_ok("{\"limit\":0}"));
    check("a limit above the maximum is refused", !parse_ok("{\"limit\":1000}"));
    check("a fractional limit is refused", !parse_ok("{\"limit\":2.5}"));
    check("a source with a slash is refused", !parse_ok("{\"source\":\"../etc/passwd\"}"));
    check("a source with a dot is refused", !parse_ok("{\"source\":\"radiod.stdio\"}"));
    check("params that are not an object are refused", !parse_ok("[1]"));

    /* ---- the formats, merged newest first ---- */
    put("radiod.log",
        "2026-09-24T10:00:00.000Z radiod INFO  start version=0.0.12 build=abc pid=10\n"
        "2026-09-24T10:00:02.500Z radiod WARN  receive recovery failed: -5\n"
        "this is not a pocketlog line\n"
        "2026-09-24T10:00:05.000Z radiod ERROR transceiver is not in receive mode; state error\n"
        "\n");
    put("sysd.log",
        "2026-09-24T10:00:01.000Z sysd  INFO  listening\n"
        "2026-09-24T10:00:03.000Z sysd  DEBUG a detail\n"
        "2026-09-24T10:00:04.000Z sysd  LOUD  a level that does not exist\n");
    put("supervise-radiod.log",
        "2026-09-24T10:00:06Z supervise radiod exited rc=1 after 3s (restart 1, backoff 1s)\n"
        "2026-09-24T10:00:07Z supervise radiod crash loop: 6 restarts within 60s, giving up\n"
        "2026-09-24T10:00:08Z supervise radiod stopped\n");
    put("radiod.stdio.log", "2026-09-24T10:00:09.000Z radiod ERROR only on stderr\n");
    put("notes.txt", "2026-09-24T10:00:10.000Z notes ERROR not a log file\n");

    r = query(NULL);
    check("all levels: every well-formed line", returned(r) == 8);
    check("newest first: the supervisor's stop",
          str_is(entry(r, 0), "source", "supervise-radiod") &&
              str_is(entry(r, 0), "level", "info") &&
              str_is(entry(r, 0), "message", "radiod stopped"));
    check("a supervisor timestamp is given milliseconds",
          str_is(entry(r, 0), "ts", "2026-09-24T10:00:08.000Z"));
    check("a crash loop is an error", str_is(entry(r, 1), "level", "error"));
    check("an exit is a warning", str_is(entry(r, 2), "level", "warn"));
    check("a pocketlog error, its message without the padding",
          str_is(entry(r, 3), "level", "error") && str_is(entry(r, 3), "source", "radiod") &&
              str_is(entry(r, 3), "message", "transceiver is not in receive mode; state error"));
    check("the oldest line last", str_is(entry(r, 7), "message", "start version=0.0.12 build=abc pid=10"));
    check("a two-space level (INFO ) parses", str_is(entry(r, 6), "message", "listening"));
    check("malformed lines are counted, not shown", num(r, "skipped") == 2);
    {
        char *text = cJSON_PrintUnformatted(r);

        check("the stdio capture is not read", text && !strstr(text, "only on stderr"));
        check("a .txt file is not read", text && !strstr(text, "not a log file"));
        free(text);
    }
    check("the sources read are named",
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "sources")) == 3);
    cJSON_Delete(r);

    r = query("{\"level\":\"warn\"}");
    check("warnings: warnings and errors only", returned(r) == 4);
    ok = 1;
    for (i = 0; i < returned(r); i++) {
        ok &= str_is(entry(r, i), "level", "warn") || str_is(entry(r, i), "level", "error");
    }
    check("and nothing below a warning", ok);
    cJSON_Delete(r);

    r = query("{\"level\":\"error\"}");
    check("errors: two", returned(r) == 2);
    cJSON_Delete(r);

    r = query("{\"level\":\"info\"}");
    check("info drops debug", returned(r) == 7);
    cJSON_Delete(r);

    r = query("{\"source\":\"sysd\"}");
    check("one source only", returned(r) == 2 && str_is(entry(r, 0), "source", "sysd"));
    cJSON_Delete(r);

    r = query("{\"source\":\"meshcored\"}");
    check("a source with no file answers empty", returned(r) == 0);
    cJSON_Delete(r);

    r = query("{\"limit\":3}");
    check("the limit keeps the newest", returned(r) == 3 &&
                                             str_is(entry(r, 2), "level", "warn") &&
                                             str_is(entry(r, 2), "source", "supervise-radiod"));
    cJSON_Delete(r);

    /* Same timestamp: the later line is the newer. */
    put("sysd.log",
        "2026-09-24T11:00:00.000Z sysd  INFO  first\n"
        "2026-09-24T11:00:00.000Z sysd  INFO  second\n");
    r = query("{\"source\":\"sysd\"}");
    check("equal times keep file order, newest first",
          str_is(entry(r, 0), "message", "second") && str_is(entry(r, 1), "message", "first"));
    cJSON_Delete(r);

    /* ---- messages are made safe for the panel ---- */
    {
        char line[1024];
        char msg[600];

        memset(msg, 'x', 500);
        msg[500] = '\0';
        snprintf(line, sizeof(line),
                 "2026-09-24T12:00:00.000Z sysd  WARN  tab\there \x1b[31mred\x1b[0m bad \xff byte\n"
                 "2026-09-24T12:00:01.000Z sysd  WARN  \xc3\xa6\xc3\xb8\xc3\xa5 ok\n"
                 "2026-09-24T12:00:02.000Z sysd  WARN  %s\n", msg);
        put("sysd.log", line);
    }
    r = query("{\"source\":\"sysd\"}");
    check("a long message is cut with an ellipsis within the bound",
          field(entry(r, 0), "message") &&
              strlen(field(entry(r, 0), "message")) <= SYSD_LOGS_MESSAGE_MAX &&
              strcmp(field(entry(r, 0), "message") + strlen(field(entry(r, 0), "message")) - 3,
                     "...") == 0);
    check("valid UTF-8 is kept", str_is(entry(r, 1), "message", "\xc3\xa6\xc3\xb8\xc3\xa5 ok"));
    check("control characters become spaces and invalid bytes '?'",
          str_is(entry(r, 2), "message", "tab here  [31mred [0m bad ? byte"));
    cJSON_Delete(r);

    /* ---- bounded: a large file is read from its tail only ---- */
    off = 0;
    for (i = 0; off + 80 < sizeof(big) - 1; i++) {
        off += (size_t)snprintf(big + off, sizeof(big) - off,
                                "2026-09-24T13:%02d:%02d.000Z sysd  INFO  line %06d\n",
                                (i / 60) % 60, i % 60, i);
    }
    put("sysd.log", big);
    r = query("{\"source\":\"sysd\",\"limit\":100}");
    check("a file larger than the budget is read from the tail",
          num(r, "scanned_bytes") <= SYSD_LOGS_TAIL_BYTES && num(r, "scanned_bytes") > 0);
    check("and says older lines were not scanned",
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "older_not_scanned")));
    check("the limit is honoured", returned(r) == 100);
    {
        char want[32];

        snprintf(want, sizeof(want), "line %06d", i - 1);
        check("the newest line comes first", str_is(entry(r, 0), "message", want));
    }
    check("the partial first line of the tail is not a skipped line", num(r, "skipped") == 0);
    cJSON_Delete(r);

    /* ---- rotation: .1 fills what the current file leaves ---- */
    put("sysd.log", "2026-09-24T14:00:01.000Z sysd  INFO  after rotation\n");
    put("sysd.log.1", "2026-09-24T14:00:00.000Z sysd  ERROR before rotation\n");
    r = query("{\"source\":\"sysd\"}");
    check("the rotated file is read too", returned(r) == 2 &&
                                              str_is(entry(r, 0), "message", "after rotation") &&
                                              str_is(entry(r, 1), "message", "before rotation"));
    cJSON_Delete(r);
    rm("sysd.log.1");

    /* ---- many files: at most SYSD_LOGS_MAX_FILES are read ---- */
    for (i = 0; i < SYSD_LOGS_MAX_FILES + 4; i++) {
        char name[32];

        snprintf(name, sizeof(name), "svc%02d.log", i);
        put(name, "2026-09-24T15:00:00.000Z svc   INFO  hello\n");
    }
    r = query(NULL);
    check("no more than the file bound is read",
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "sources")) == SYSD_LOGS_MAX_FILES);
    check("and the rest is flagged as not scanned",
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "older_not_scanned")));
    cJSON_Delete(r);

    /* ---- crash reports ---- */
    r = sysd_crashes_list(dir);
    check("no crash reports: an empty list", num(r, "total") == 0 &&
                                                 cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "reports")) == 0);
    cJSON_Delete(r);
    {
        char body[512];

        snprintf(body, sizeof(body),
                 "Doors crash report\nversion: 0.0.12\nbuild: abc123\nprocess: radiod\npid: 812\n"
                 "signal: %d\nbacktrace:\n/usr/sbin/radiod(+0x1234)[0x1]\n/lib/libc.so.6(+0x10)[0x2]\n"
                 "/lib/libc.so.6(+0x20)[0x3]\n/lib/libc.so.6(+0x30)[0x4]\n", SIGSEGV);
        put("crash-radiod-1790000000-812.txt", body);
    }
    put("crash-doors-shell-1790000100-900.txt", "Doors crash report\nversion: 0.0.12\n");
    put("crash-radiod-1789999000-700.txt", "");
    put("crash-bad.txt", "not a report name");
    put("crash-x-y-z.txt", "not numbers");
    r = sysd_crashes_list(dir);
    check("crash reports with well-formed names are counted", num(r, "total") == 3);
    {
        const cJSON *reps = cJSON_GetObjectItemCaseSensitive(r, "reports");
        const cJSON *a = cJSON_GetArrayItem(reps, 0);
        const cJSON *b = cJSON_GetArrayItem(reps, 1);
        const cJSON *c = cJSON_GetArrayItem(reps, 2);

        check("newest first, a dashed process name kept whole",
              str_is(a, "process", "doors-shell") && num(a, "pid") == 900 && num(a, "time") == 1790000100);
        check("a report cut short has null signal, version from its header",
              cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(a, "signal")) && str_is(a, "version", "0.0.12") &&
                  cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(a, "build")));
        check("a whole report: signal and its name",
              str_is(b, "process", "radiod") && num(b, "signal") == SIGSEGV && str_is(b, "signal_name", "SIGSEGV"));
        check("its build", str_is(b, "build", "abc123"));
        check("at most the first frames of its backtrace",
              cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b, "frames")) == SYSD_CRASH_FRAMES);
        check("an empty report is listed with nothing invented",
              str_is(c, "process", "radiod") && cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(c, "version")) &&
                  cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(c, "frames")) == 0);
    }
    cJSON_Delete(r);
    for (i = 0; i < SYSD_CRASHES_MAX + 5; i++) {
        char name[64];

        snprintf(name, sizeof(name), "crash-sysd-%d-%d.txt", 1800000000 + i, 100 + i);
        put(name, "Doors crash report\n");
    }
    r = sysd_crashes_list(dir);
    check("many reports: all counted",
          num(r, "total") == 3 + SYSD_CRASHES_MAX + 5);
    check("but only the newest few listed",
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "reports")) == SYSD_CRASHES_MAX);
    cJSON_Delete(r);

    {
        char cmd[600];

        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        if (system(cmd) != 0) {
            fprintf(stderr, "could not remove %s\n", dir);
        }
    }
    printf("sysd_logs_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
