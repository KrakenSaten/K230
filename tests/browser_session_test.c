/*
 * The Browser app's helper lifecycle (apps/browser/browser_session.h)
 * against the real pos-browser on the fake network: a page and its
 * pictures, STOP, a new page replacing one that loads, a crash, a helper
 * stuck in the kernel (killed and replaced without an error when a newer
 * page waits), no hello, no helper at all, and fifty open/close cycles that
 * must leave no process, descriptor, file or directory behind.
 *
 * usage: browser_session_test tools/browser/pos-browser
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "browser_session.h"

#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;
static char helper[4096];
static char runtime[] = "/tmp/browser_session_test.XXXXXX";

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

static int64_t now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void nap(int ms)
{
    struct timespec d = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&d, NULL);
}

static struct browser_session_config cfg(void)
{
    struct browser_session_config c = { helper, true, NULL, runtime };

    return c;
}

/* Poll until cond or ms; returns the changes seen. */
#define POLL_UNTIL(s, v, cond, ms)                                                                 \
    ({                                                                                             \
        unsigned ch_ = 0;                                                                          \
        int64_t end_ = now() + (ms);                                                               \
        while (!(cond) && now() < end_) {                                                          \
            ch_ |= browser_session_poll((s), (v), now());                                          \
            if (!(cond)) {                                                                         \
                nap(10);                                                                           \
            }                                                                                      \
        }                                                                                          \
        ch_;                                                                                       \
    })

static int count_entries(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.';
    }
    closedir(d);
    return n;
}

static int children(void)
{
    char path[300];
    FILE *f;
    int n = 0;
    DIR *d = opendir("/proc");
    struct dirent *e;
    pid_t me = getpid();

    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        int ppid = 0;
        char state = 0;
        char line[512];

        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
        f = fopen(path, "r");
        if (!f) {
            continue;
        }
        if (fgets(line, sizeof(line), f)) {
            char *rp = strrchr(line, ')');

            if (rp && sscanf(rp + 2, "%c %d", &state, &ppid) == 2 && ppid == me) {
                n++;
            }
        }
        fclose(f);
    }
    closedir(d);
    return n;
}

static void open_url(struct browser_session *s, struct browser_view *v, const char *url)
{
    struct browser_cmd cmd;

    browser_view_go(v, url, now(), &cmd);
    browser_session_send(s, &cmd, now());
}

int main(int argc, char **argv)
{
    struct browser_session *s = malloc(sizeof(*s));
    struct browser_view *v = malloc(sizeof(*v));
    struct browser_session_config c;
    struct browser_cmd cmd;
    char state[600];
    char err[160];
    char dir[BROWSER_PATH_MAX];
    struct stat st;
    unsigned ch;
    int64_t t0;
    int fds0;
    int i;

    if (argc < 2 || !s || !v || !mkdtemp(runtime)) {
        fprintf(stderr, "usage: browser_session_test tools/browser/pos-browser\n");
        return 2;
    }
    if (!realpath(argv[1], helper)) {
        fprintf(stderr, "no helper at %s\n", argv[1]);
        return 2;
    }
    setenv("POCKETOS_LOG_DIR", runtime, 1);
    setenv("POCKETOS_LOG_STDERR", "0", 1);
    snprintf(state, sizeof(state), "%s/state", runtime);
    browser_view_init(v, state, NULL, 0);
    browser_session_init(s);
    c = cfg();
    fds0 = count_entries("/proc/self/fd");

    /* ---- a page with a picture ------------------------------------------------------------ */
    check("the helper starts", browser_session_start(s, &c, now(), err, sizeof(err)) == 0);
    web_copy(dir, sizeof(dir), s->img_dir);
    check("with a private picture directory (0700) under the runtime directory",
          dir[0] && strncmp(dir, runtime, strlen(runtime)) == 0 && stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    POLL_UNTIL(s, v, v->helper_up, 3000);
    check("it says hello, on the fake network", v->helper_up && strstr(v->features, "fake") != NULL);
    t0 = now();
    open_url(s, v, "doors.test");
    POLL_UNTIL(s, v, v->state == BROWSER_PAGE && !v->images_loading, 5000);
    check("the demo page arrives", v->state == BROWSER_PAGE && strcmp(v->doc.title, "Doors Browser demo") == 0 &&
                                        v->doc.nlinks == 5);
    printf("     page and picture in %lld ms\n", (long long)(now() - t0));
    check("its picture arrives as pixels, scaled to the width asked for",
          v->img[0].state == BROWSER_IMAGE_READY && v->img[0].w == 480 && v->img[0].h == 270);
    check("and the picture file is gone once read", count_entries(dir) == 0);

    /* ---- stop and replace ------------------------------------------------------------------- */
    open_url(s, v, "doors.test/slow");
    POLL_UNTIL(s, v, 0, 200);
    t0 = now();
    browser_view_stop(v, &cmd);
    browser_session_send(s, &cmd, now());
    POLL_UNTIL(s, v, s->stop_seq == 0, 3000);
    check("STOP on a slow page is answered within a second", s->stop_seq == 0 && now() - t0 < 1000);
    check("the demo page is still on screen", strcmp(v->doc.title, "Doors Browser demo") == 0);
    POLL_UNTIL(s, v, 0, 5500);
    check("and nothing from the stopped load turns up later", strcmp(v->doc.title, "Doors Browser demo") == 0 &&
                                                                  v->state == BROWSER_PAGE);
    open_url(s, v, "doors.test/slow");
    POLL_UNTIL(s, v, 0, 200);
    t0 = now();
    open_url(s, v, "doors.test/about");
    POLL_UNTIL(s, v, v->state == BROWSER_PAGE && strcmp(v->doc.title, "About") == 0, 3000);
    check("a new page replaces one still loading, at once", strcmp(v->doc.title, "About") == 0 &&
                                                                now() - t0 < 1000);

    /* ---- failures through the helper ------------------------------------------------------- */
    open_url(s, v, "doors.test/zip");
    POLL_UNTIL(s, v, !v->loading, 3000);
    check("a zip is not shown: downloads are not supported", v->state == BROWSER_ERROR &&
                                                                 v->err.fail == WEB_FAIL_TYPE &&
                                                                 strstr(v->err.detail, "downloads") != NULL);
    open_url(s, v, "doors.test/insecure");
    POLL_UNTIL(s, v, !v->loading, 3000);
    check("an https page redirecting to http is refused", v->err.fail == WEB_FAIL_INSECURE_REDIRECT);
    open_url(s, v, "doors.test/file");
    POLL_UNTIL(s, v, !v->loading, 3000);
    check("a redirect to file:// is refused", v->err.fail == WEB_FAIL_BAD_REDIRECT);
    open_url(s, v, "doors.test/loop");
    POLL_UNTIL(s, v, !v->loading, 3000);
    check("a redirect loop ends", v->err.fail == WEB_FAIL_REDIRECTS);
    open_url(s, v, "nowhere.example");
    POLL_UNTIL(s, v, !v->loading, 3000);
    check("an unknown host is server-not-found", v->err.fail == WEB_FAIL_DNS);
    open_url(s, v, "tls.doors.test");
    POLL_UNTIL(s, v, !v->loading, 3000);
    check("a refused certificate", v->err.fail == WEB_FAIL_TLS);
    open_url(s, v, "doors.test/garbage");
    POLL_UNTIL(s, v, !v->loading, 3000);
    check("random bytes as a page: shown as whatever text they make, nothing breaks",
          v->state == BROWSER_PAGE && browser_session_active(s));
    open_url(s, v, "doors.test/big");
    POLL_UNTIL(s, v, !v->loading, 5000);
    check("a 3 MB page is shown cut, and says so", v->state == BROWSER_PAGE && v->doc.truncated &&
                                                       strstr(v->status, "cut") != NULL);
    open_url(s, v, "doors.test/long");
    POLL_UNTIL(s, v, !v->loading, 3000);
    check("a long page arrives whole", v->state == BROWSER_PAGE && !v->doc.truncated && v->doc.nblocks > 300);

    /* ---- a crash ----------------------------------------------------------------------------- */
    open_url(s, v, "doors.test/crash");
    ch = POLL_UNTIL(s, v, !browser_session_active(s), 3000);
    check("a helper crash ends the session and shows an error page, not a hang",
          !browser_session_active(s) && v->state == BROWSER_ERROR && v->err.helper && (ch & BROWSER_CHANGED_STATE));
    check("its picture directory is removed", stat(dir, &st) != 0);
    check("a new helper starts for the next page", browser_view_may_start_helper(v, now()) &&
                                                       browser_session_start(s, &c, now(), err, sizeof(err)) == 0);
    POLL_UNTIL(s, v, v->helper_up, 3000);
    open_url(s, v, "doors.test/about");
    POLL_UNTIL(s, v, v->state == BROWSER_PAGE, 3000);
    check("and it loads", v->state == BROWSER_PAGE && strcmp(v->doc.title, "About") == 0);

    /* ---- stuck in the kernel ------------------------------------------------------------------ */
    open_url(s, v, "doors.test/hang");
    POLL_UNTIL(s, v, 0, 200);
    browser_view_stop(v, &cmd);
    browser_session_send(s, &cmd, now());
    t0 = now();
    ch = POLL_UNTIL(s, v, !browser_session_active(s), BROWSER_STOP_GRACE_MS + 2000);
    check("a helper that cannot be stopped is ended after the grace period",
          !browser_session_active(s) && now() - t0 >= BROWSER_STOP_GRACE_MS - 100);
    check("with nothing waiting: no error page, no restart asked, the page stays",
          !(ch & BROWSER_CHANGED_RESTART) && v->state == BROWSER_PAGE && strcmp(v->doc.title, "About") == 0);
    browser_session_start(s, &c, now(), err, sizeof(err));
    POLL_UNTIL(s, v, v->helper_up, 3000);
    open_url(s, v, "doors.test/hang");
    POLL_UNTIL(s, v, 0, 200);
    browser_view_stop(v, &cmd);
    browser_session_send(s, &cmd, now());
    open_url(s, v, "doors.test/about");
    ch = POLL_UNTIL(s, v, !browser_session_active(s), BROWSER_STOP_GRACE_MS + 2000);
    check("stuck while a newer page waits: ended, and a restart is asked for instead of an error",
          (ch & BROWSER_CHANGED_RESTART) && v->loading && v->state != BROWSER_ERROR);
    browser_session_start(s, &c, now(), err, sizeof(err));
    browser_view_resend(v, &cmd);
    browser_session_send(s, &cmd, now());
    POLL_UNTIL(s, v, !v->loading, 3000);
    check("the fresh helper loads the page that waited", v->state == BROWSER_PAGE &&
                                                             strcmp(v->doc.title, "About") == 0);
    t0 = now();
    open_url(s, v, "doors.test/slow");
    POLL_UNTIL(s, v, 0, 100);
    browser_session_abandon(s, 300);
    check("closing the app while a page loads takes under half a second", now() - t0 - 100 < 500 &&
                                                                              !browser_session_active(s));

    /* ---- no hello, no helper -------------------------------------------------------------------- */
    {
        char script[700];
        FILE *f;
        struct browser_session_config q = c;

        snprintf(script, sizeof(script), "%s/silent.sh", runtime);
        f = fopen(script, "w");
        fputs("#!/bin/sh\nexec sleep 30\n", f);
        fclose(f);
        chmod(script, 0755);
        q.helper = script;
        browser_session_start(s, &q, now(), err, sizeof(err));
        POLL_UNTIL(s, v, !browser_session_active(s), BROWSER_HELLO_MS + 2000);
        check("a helper that never says hello is ended", !browser_session_active(s));
        q.helper = "/nonexistent/pos-browser";
        browser_session_start(s, &q, now(), err, sizeof(err));
        open_url(s, v, "doors.test");
        POLL_UNTIL(s, v, !browser_session_active(s), 3000);
        check("no helper installed: an error page that names pos-browser",
              !browser_session_active(s) && v->state == BROWSER_ERROR && strstr(v->err.text, "pos-browser") != NULL);
        unlink(script);
    }

    /* ---- fifty open/close cycles ------------------------------------------------------------- */
    for (i = 0; i < 50; i++) {
        browser_session_start(s, &c, now(), err, sizeof(err));
        POLL_UNTIL(s, v, v->helper_up, 3000);
        if (i % 2) {
            open_url(s, v, "doors.test");
            POLL_UNTIL(s, v, !v->loading, 3000);
        }
        browser_session_abandon(s, 300);
        v->helper_up = false;
    }
    check("fifty open/close cycles leave no helper process", children() == 0);
    check("no descriptor", count_entries("/proc/self/fd") == fds0);
    {
        DIR *d = opendir(runtime);
        struct dirent *e;
        int left = 0;

        while ((e = readdir(d)) != NULL) {
            left += strncmp(e->d_name, "browser.", 8) == 0;
        }
        closedir(d);
        check("and no picture directory", left == 0);
    }

    browser_view_free(v);
    free(v);
    free(s);
    {
        char cmdline[700];

        snprintf(cmdline, sizeof(cmdline), "rm -rf '%s'", runtime);
        if (system(cmdline) != 0) {
            printf("warning: %s not removed\n", runtime);
        }
    }
    printf("browser_session_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
