/*
 * sysd_services tests: the supervised-service table read from pos-supervise's
 * state files, against a fake runtime directory ($POCKETOS_RUNTIME_DIR).
 *
 * One file per case, each the shape the supervisor writes at a particular
 * moment of a service's life (tools/supervise/pos-supervise). The supervisor
 * side of the contract — that these files are written whole and at the right
 * moments — is tests/supervise_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "sysd_services.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failed;
static char run[] = "/tmp/pos_svc.XXXXXX";

static void check(const char *name, int ok)
{
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

/* A file in the fake runtime directory, written exactly as given. */
static void put(const char *name, const char *text)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", run, name);
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        exit(1);
    }
    fputs(text, f);
    fclose(f);
}

static const cJSON *get(const cJSON *o, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(o, key);
}

static int str_is(const cJSON *o, const char *key, const char *want)
{
    const cJSON *v = get(o, key);

    return cJSON_IsString(v) && strcmp(v->valuestring, want) == 0;
}

static int num_is(const cJSON *o, const char *key, double want)
{
    const cJSON *v = get(o, key);

    return cJSON_IsNumber(v) && v->valuedouble > want - 1e-6 && v->valuedouble < want + 1e-6;
}

static const cJSON *find_named(const cJSON *arr, const char *name)
{
    const cJSON *e;

    cJSON_ArrayForEach(e, arr) {
        if (str_is(e, "name", name)) {
            return e;
        }
    }
    return NULL;
}

/* Every entry carries the same six keys, whatever the supervisor knew. */
static int has_all_keys(const cJSON *e)
{
    return e && get(e, "name") && get(e, "pid") && get(e, "running") &&
           get(e, "crashloop") && get(e, "last_exit_code") && get(e, "restarts");
}

static cJSON *services(void)
{
    cJSON *status = cJSON_CreateObject();

    sysd_services_add(status);
    return status;
}

int main(void)
{
    char self[64];
    char alive[512];
    cJSON *st;
    const cJSON *arr;
    const cJSON *e;
    char cmd[600];

    if (!mkdtemp(run)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("POCKETOS_RUNTIME_DIR", run, 1);

    /* ---- nothing supervised: the key is there, the array is empty ---- */
    st = services();
    arr = get(st, "services");
    check("services is always present", cJSON_IsArray(arr));
    check("no state files gives an empty array", cJSON_GetArraySize(arr) == 0);
    cJSON_Delete(st);

    /* ---- one file per moment in a service's life ---- */
    snprintf(self, sizeof(self), "%ld", (long)getpid());
    snprintf(alive, sizeof(alive),
             "state_version=1\nname=alive\nsupervisor_pid=1\nchild_pid=%s\nrunning=1\n"
             "crashloop=0\nlast_exit_code=\nrestarts=0\nbackoff_s=\n"
             "started_uptime_s=10\nupdated_uptime_s=11\n", self);
    put("alive.state", alive);
    /* the supervisor thinks it has a child; the kernel disagrees */
    put("dead.state",
        "state_version=1\nname=dead\nsupervisor_pid=1\nchild_pid=999999\nrunning=1\n"
        "crashloop=0\nlast_exit_code=\nrestarts=0\nbackoff_s=\n"
        "started_uptime_s=10\nupdated_uptime_s=11\n");
    /* the child has exited and the restart has not happened yet */
    put("exited.state",
        "state_version=1\nname=exited\nsupervisor_pid=1\nchild_pid=\nrunning=0\n"
        "crashloop=0\nlast_exit_code=3\nrestarts=1\nbackoff_s=\n"
        "started_uptime_s=\nupdated_uptime_s=20\n");
    /* waiting out the backoff before the next start */
    put("backoff.state",
        "state_version=1\nname=backoff\nsupervisor_pid=1\nchild_pid=\nrunning=0\n"
        "crashloop=0\nlast_exit_code=1\nrestarts=2\nbackoff_s=4\n"
        "started_uptime_s=\nupdated_uptime_s=25\n");
    /* supervision has given up */
    put("looping.state",
        "state_version=1\nname=looping\nsupervisor_pid=1\nchild_pid=\nrunning=0\n"
        "crashloop=1\nlast_exit_code=139\nrestarts=6\nbackoff_s=\n"
        "started_uptime_s=\nupdated_uptime_s=30\n");
    /* stopped on SIGTERM, having exited cleanly: 0 is a value, not an absence */
    put("stopped.state",
        "state_version=1\nname=stopped\nsupervisor_pid=1\nchild_pid=\nrunning=0\n"
        "crashloop=0\nlast_exit_code=0\nrestarts=0\nbackoff_s=\n"
        "started_uptime_s=\nupdated_uptime_s=40\n");
    /* started, never exited, so the exit code and count are not yet known */
    put("fresh.state",
        "state_version=1\nname=fresh\nsupervisor_pid=1\nchild_pid=\nrunning=0\n"
        "crashloop=0\nlast_exit_code=\nrestarts=\nbackoff_s=\n"
        "started_uptime_s=\nupdated_uptime_s=1\n");

    st = services();
    arr = get(st, "services");

    e = find_named(arr, "alive");
    check("a running service has all six keys", has_all_keys(e));
    check("a running service reports its child pid", num_is(e, "pid", (double)getpid()));
    check("a running service is running", cJSON_IsTrue(get(e, "running")));
    check("a running service is not in a crash loop", cJSON_IsFalse(get(e, "crashloop")));
    check("a service that has not exited has a null exit code",
          cJSON_IsNull(get(e, "last_exit_code")));
    check("a service that has not restarted reports 0, not null",
          num_is(e, "restarts", 0));

    e = find_named(arr, "dead");
    check("a pid the kernel does not have is not running", cJSON_IsFalse(get(e, "running")));
    check("the pid is still reported so the operator can see it",
          num_is(e, "pid", 999999));

    e = find_named(arr, "exited");
    check("an exited child leaves a null pid", cJSON_IsNull(get(e, "pid")));
    check("an exited child is not running", cJSON_IsFalse(get(e, "running")));
    check("the exit code is reported", num_is(e, "last_exit_code", 3));
    check("the restart count is reported", num_is(e, "restarts", 1));

    e = find_named(arr, "backoff");
    check("a service waiting out a backoff is not running", cJSON_IsFalse(get(e, "running")));
    check("its restart count has advanced", num_is(e, "restarts", 2));
    check("a backoff does not change the entry's shape", has_all_keys(e));

    e = find_named(arr, "looping");
    check("a crash-looped service says so", cJSON_IsTrue(get(e, "crashloop")));
    check("a crash-looped service is not running", cJSON_IsFalse(get(e, "running")));
    check("its last exit code survives", num_is(e, "last_exit_code", 139));
    check("its restart count survives", num_is(e, "restarts", 6));

    e = find_named(arr, "stopped");
    check("exit code 0 is a number, not a null", num_is(e, "last_exit_code", 0));

    e = find_named(arr, "fresh");
    check("an unknown exit code is null", cJSON_IsNull(get(e, "last_exit_code")));
    check("an unknown restart count is null", cJSON_IsNull(get(e, "restarts")));
    cJSON_Delete(st);

    /* ---- files this reader must not trust or must not see ---- */
    /* a supervisor from a future PocketOS: the service exists, its state
     * does not parse, and nothing about it is invented */
    put("future.state",
        "state_version=2\nname=future\nchild_pid=1\nrunning=1\ncrashloop=1\n"
        "last_exit_code=7\nrestarts=9\n");
    /* not a state file at all */
    put("garbage.state", "this is not a state file\n\n=\n=x\nrunning\n");
    /* half a file, as a reader would see if writes were not atomic */
    put("truncated.state", "state_version=1\nname=truncated\nchild_p");
    /* the supervisor's own pid and marker files must not make entries:
     * only .state names a service now */
    put("legacy.pid", "1234\n");
    put("legacy.crashloop", "2026-09-09T16:14:23Z rc=139 restarts=6\n");
    /* gone between the readdir and the open */
    snprintf(cmd, sizeof(cmd), "ln -s '%s/no_such_file' '%s/gone.state'", run, run);
    if (system(cmd) != 0) {
        fprintf(stderr, "cannot create the dangling state link\n");
        return 1;
    }

    st = services();
    arr = get(st, "services");

    e = find_named(arr, "future");
    check("a state file from a newer supervisor is listed", e != NULL);
    check("nothing from it is claimed", has_all_keys(e) && cJSON_IsNull(get(e, "pid")) &&
                                            cJSON_IsFalse(get(e, "running")) &&
                                            cJSON_IsFalse(get(e, "crashloop")) &&
                                            cJSON_IsNull(get(e, "last_exit_code")) &&
                                            cJSON_IsNull(get(e, "restarts")));
    e = find_named(arr, "garbage");
    check("a state file with no version is listed but not believed",
          has_all_keys(e) && cJSON_IsNull(get(e, "pid")) && cJSON_IsFalse(get(e, "running")));
    e = find_named(arr, "truncated");
    check("a half-written state file yields nulls, never a partial value",
          has_all_keys(e) && cJSON_IsNull(get(e, "pid")) && cJSON_IsFalse(get(e, "running")) &&
              cJSON_IsNull(get(e, "last_exit_code")) && cJSON_IsNull(get(e, "restarts")));
    check("a state file that vanished before the open is not listed",
          find_named(arr, "gone") == NULL);
    check("a pid file alone does not name a service", find_named(arr, "legacy") == NULL);

    /* ---- the array is sorted and holds one entry per state file ---- */
    check("one entry per readable state file", cJSON_GetArraySize(arr) == 10);
    check("sorted by name",
          str_is(cJSON_GetArrayItem(arr, 0), "name", "alive") &&
              str_is(cJSON_GetArrayItem(arr, 1), "name", "backoff") &&
              str_is(cJSON_GetArrayItem(arr, 2), "name", "dead") &&
              str_is(cJSON_GetArrayItem(arr, 9), "name", "truncated"));
    cJSON_Delete(st);

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", run);
    if (system(cmd) != 0) {
        fprintf(stderr, "cleanup failed\n");
    }
    printf("sysd_services_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
