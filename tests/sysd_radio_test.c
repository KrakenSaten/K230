/*
 * sysd's radio setup (services/sysd/sysd_radio.h): the files it writes, the
 * order it restarts and asks in, that it refuses without the antenna answer
 * and when there is nothing to do, and that every failure puts the unit back
 * exactly as it was. The init scripts and the services are fakes; the job
 * runs in a real child, so the fakes record what they were asked into a file.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "sysd_radio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
    }
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static char dir[] = "/tmp/sysd-radio-XXXXXX";
static char rdef[256], mdef[256], rinit[256], minit[256], logp[256], trace[256];

/* What the fake services say. */
static const char *radiod_backend = "sx1262"; /* NULL: radiod never answers */
static int switch_ok = 1;
static int meshcored_ok = 1;

static void record(const char *line)
{
    FILE *f = fopen(trace, "a");

    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

static int fake_run(const char *const argv[], int log_fd, void *user)
{
    char line[300];

    (void)log_fd;
    (void)user;
    snprintf(line, sizeof(line), "run %s %s", strrchr(argv[0], '/') + 1, argv[1]);
    record(line);
    return 0;
}

static cJSON *fake_call(const char *service, const char *method, cJSON *params, int timeout_ms, char *err,
                        size_t errlen, void *user)
{
    char line[300];
    cJSON *res = NULL;

    (void)timeout_ms;
    (void)user;
    snprintf(line, sizeof(line), "call %s %s%s", service, method,
             params && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "enabled")) ? " on" : "");
    cJSON_Delete(params);
    if (strcmp(method, "radio.info") == 0) {
        if (radiod_backend) {
            res = cJSON_CreateObject();
            cJSON_AddStringToObject(res, "backend", radiod_backend);
        }
    } else if (strcmp(method, "radio.set_enabled") == 0) {
        record(line);
        if (switch_ok) {
            res = cJSON_CreateObject();
        } else {
            snprintf(err, errlen, "the radio could not be switched on: BUSY stuck high");
            return NULL;
        }
    } else if (strcmp(method, "mesh.status") == 0) {
        res = meshcored_ok ? cJSON_CreateObject() : NULL;
    }
    if (!res && !err[0]) {
        snprintf(err, errlen, "%s is not answering", service);
    }
    return res;
}

static const struct sysd_radio_ops fake_ops = { fake_run, fake_call, NULL };

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    static char buf[4096];
    size_t n;

    if (!f) {
        return NULL;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void wait_done(struct sysd_radio *r)
{
    int i;

    for (i = 0; i < 500 && sysd_radio_busy(r); i++) {
        struct timespec ts = { 0, 10 * 1000000L };

        nanosleep(&ts, NULL);
        sysd_radio_reap(r);
    }
}

static const char *state_of(struct sysd_radio *r, cJSON **keep)
{
    cJSON *st = sysd_radio_status(r);
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(st, "state");

    *keep = st;
    return cJSON_IsString(s) ? s->valuestring : "";
}

static void setup(struct sysd_radio *r)
{
    struct sysd_radio_paths p = { rdef, mdef, rinit, minit, logp };

    sysd_radio_init(r, &p, &fake_ops);
    r->answer_ms = 300;
    unlink(trace);
}

int main(void)
{
    struct sysd_radio r;
    struct sysd_radio_config c;
    char msg[200];
    char v[64];
    char *t;
    cJSON *st;
    const char *s;

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(rdef, sizeof(rdef), "%s/radiod", dir);
    snprintf(mdef, sizeof(mdef), "%s/meshcored", dir);
    snprintf(rinit, sizeof(rinit), "%s/S60radiod", dir);
    snprintf(minit, sizeof(minit), "%s/S65meshcored", dir);
    snprintf(logp, sizeof(logp), "%s/radio-setup.log", dir);
    snprintf(trace, sizeof(trace), "%s/trace", dir);

    /* ---- the key=value text ---- */
    check("a value is read, quotes removed, the last one winning",
          sysd_radio_value("# c\nRADIOD_BACKEND=mock\n  RADIOD_BACKEND=\"sx1262\"\n", "RADIOD_BACKEND", v, sizeof(v)) &&
              strcmp(v, "sx1262") == 0);
    check("a longer key is not the key", !sysd_radio_value("RADIOD_BACKEND_X=1\n", "RADIOD_BACKEND", v, sizeof(v)));
    t = sysd_radio_set("# keep\nRADIOD_REGION=EU868\nRADIOD_BACKEND=mock\nRADIOD_TX_POWER_DBM=2", "RADIOD_BACKEND",
                       "sx1262");
    check("set keeps the other lines and comments, and replaces the key once",
          t && strcmp(t, "# keep\nRADIOD_REGION=EU868\nRADIOD_TX_POWER_DBM=2\nRADIOD_BACKEND=sx1262\n") == 0);
    free(t);

    /* ---- a fresh card: no files ---- */
    setup(&r);
    sysd_radio_read(&r, &c);
    check("a fresh card: mock, no meshcored, setup needed",
          strcmp(c.backend, "mock") == 0 && !c.meshcored_enabled && sysd_radio_needed(&c));
    check("no setup without the antenna answer", sysd_radio_start(&r, false, msg, sizeof(msg)) == -1 &&
                                                       strstr(msg, "antenna") != NULL && !sysd_radio_busy(&r));
    check("with it, the setup starts", sysd_radio_start(&r, true, msg, sizeof(msg)) == 0);
    s = state_of(&r, &st);
    check("and says running at once", strcmp(s, "running") == 0);
    cJSON_Delete(st);
    check("a second start while it runs is busy", sysd_radio_start(&r, true, msg, sizeof(msg)) == -2);
    wait_done(&r);
    s = state_of(&r, &st);
    check("it finishes: done, nothing more needed",
          strcmp(s, "done") == 0 && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(st, "needed")));
    cJSON_Delete(st);
    check("/etc/default/radiod says sx1262", (t = slurp(rdef)) && strcmp(t, "RADIOD_BACKEND=sx1262\n") == 0);
    check("/etc/default/meshcored enables it", (t = slurp(mdef)) && strcmp(t, "MESHCORED_ENABLE=1\n") == 0);
    check("in order: radiod restarted, the radio switched on, then meshcored",
          (t = slurp(trace)) && strcmp(t, "run S60radiod restart\ncall radiod radio.set_enabled on\n"
                                          "run S65meshcored restart\n") == 0);
    check("once set up, a start is refused", sysd_radio_start(&r, true, msg, sizeof(msg)) == -1 &&
                                                 strstr(msg, "already set up") != NULL);

    /* ---- radiod does not come back on the SX1262 ---- */
    setup(&r);
    put(rdef, "RADIOD_REGION=EU868\n");
    unlink(mdef);
    radiod_backend = "mock";
    sysd_radio_start(&r, true, msg, sizeof(msg));
    wait_done(&r);
    s = state_of(&r, &st);
    check("radiod still on the mock: failed, with why",
          strcmp(s, "failed") == 0 &&
              strstr(cJSON_GetObjectItemCaseSensitive(st, "error")->valuestring, "SX1262") != NULL &&
              strstr(cJSON_GetObjectItemCaseSensitive(st, "error")->valuestring, "previous settings are back") != NULL);
    cJSON_Delete(st);
    check("the radiod file is back as it was", (t = slurp(rdef)) && strcmp(t, "RADIOD_REGION=EU868\n") == 0);
    check("the meshcored file is still absent", access(mdef, F_OK) != 0);
    check("the radio was never switched on, and both services were restarted on the old files",
          (t = slurp(trace)) && strstr(t, "set_enabled") == NULL &&
              strcmp(t, "run S60radiod restart\nrun S60radiod restart\nrun S65meshcored restart\n") == 0);
    sysd_radio_read(&r, &c);
    check("the setup is still needed", sysd_radio_needed(&c));

    /* ---- the SX1262 does not switch on ---- */
    setup(&r);
    radiod_backend = "sx1262";
    switch_ok = 0;
    sysd_radio_start(&r, true, msg, sizeof(msg));
    wait_done(&r);
    s = state_of(&r, &st);
    check("the transceiver refusing: failed, radiod's reason given",
          strcmp(s, "failed") == 0 &&
              strstr(cJSON_GetObjectItemCaseSensitive(st, "error")->valuestring, "BUSY stuck high") != NULL);
    cJSON_Delete(st);
    check("and the unit is back on the old settings", (t = slurp(rdef)) && strcmp(t, "RADIOD_REGION=EU868\n") == 0 &&
                                                          access(mdef, F_OK) != 0);

    /* ---- meshcored does not start ---- */
    setup(&r);
    switch_ok = 1;
    meshcored_ok = 0;
    put(mdef, "MESHCORED_ENABLE=0\nMESHCORED_NAME=bench\n");
    sysd_radio_start(&r, true, msg, sizeof(msg));
    wait_done(&r);
    s = state_of(&r, &st);
    check("meshcored silent: failed, said so",
          strcmp(s, "failed") == 0 &&
              strstr(cJSON_GetObjectItemCaseSensitive(st, "error")->valuestring, "meshcored did not start") != NULL);
    cJSON_Delete(st);
    check("both files back as they were",
          (t = slurp(mdef)) && strcmp(t, "MESHCORED_ENABLE=0\nMESHCORED_NAME=bench\n") == 0 &&
              (t = slurp(rdef)) && strcmp(t, "RADIOD_REGION=EU868\n") == 0);

    /* ---- radiod already on the SX1262, meshcored off: the rest is done ---- */
    setup(&r);
    meshcored_ok = 1;
    put(rdef, "RADIOD_BACKEND='sx1262'\n");
    sysd_radio_read(&r, &c);
    check("sx1262 without meshcored still needs the setup", strcmp(c.backend, "sx1262") == 0 && sysd_radio_needed(&c));
    sysd_radio_start(&r, true, msg, sizeof(msg));
    wait_done(&r);
    s = state_of(&r, &st);
    check("and it finishes", strcmp(s, "done") == 0);
    cJSON_Delete(st);
    check("keeping the other meshcored lines",
          (t = slurp(mdef)) && strcmp(t, "MESHCORED_NAME=bench\nMESHCORED_ENABLE=1\n") == 0);

    /* ---- nothing writable: nothing changes ---- */
    setup(&r);
    {
        char ro[300];
        struct sysd_radio_paths p = { ro, mdef, rinit, minit, logp };

        snprintf(ro, sizeof(ro), "%s/no-such-dir/radiod", dir);
        sysd_radio_init(&r, &p, &fake_ops);
        r.answer_ms = 300;
        put(mdef, "MESHCORED_ENABLE=0\n");
        sysd_radio_start(&r, true, msg, sizeof(msg));
        wait_done(&r);
        s = state_of(&r, &st);
        check("a radiod file that cannot be written: failed, nothing changed, nothing restarted",
              strcmp(s, "failed") == 0 &&
                  strstr(cJSON_GetObjectItemCaseSensitive(st, "error")->valuestring, "nothing was changed") != NULL &&
                  slurp(trace) == NULL);
        cJSON_Delete(st);
    }

    printf("sysd_radio_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
