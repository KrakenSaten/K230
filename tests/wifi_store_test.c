/*
 * netd's Wi-Fi store (services/netd/wifi_store.c): absence, round trip,
 * restrictive permissions, atomic replacement, damaged files left alone,
 * ordering and limits, and that passphrases survive exactly.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "wifi_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static struct wifi_saved net(const char *ssid, enum wifi_security sec, const char *pass)
{
    struct wifi_saved n;

    memset(&n, 0, sizeof(n));
    wifi_ssid_from_text(ssid, &n.ssid);
    n.security = sec;
    if (pass) {
        snprintf(n.passphrase, sizeof(n.passphrase), "%s", pass);
    }
    return n;
}

static void put_file(const char *dir, const char *name, const char *text, mode_t mode)
{
    char path[600];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(1);
    }
    fputs(text, f);
    fclose(f);
    chmod(path, mode);
}

static mode_t mode_of(const char *dir, const char *name)
{
    char path[600];
    struct stat sb;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return lstat(path, &sb) == 0 ? (sb.st_mode & 07777) : (mode_t)-1;
}

static int file_contains(const char *dir, const char *name, const char *needle)
{
    char path[600];
    char buf[4096];
    FILE *f;
    size_t n;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

int main(void)
{
    char base[] = "/tmp/pos_wifi_store.XXXXXX";
    char dir[600];
    char cmd[700];
    struct wifi_store st;
    struct wifi_store back;
    struct wifi_saved n;
    int too_open;
    int i;

    if (!mkdtemp(base)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(dir, sizeof(dir), "%s/netd", base);

    /* ---- absence ------------------------------------------------------------ */
    check("absent store loads as absent", wifi_store_load(dir, &st, &too_open) == WIFI_STORE_ABSENT);
    check("absent store is off with no networks", st.enabled == 0 && st.count == 0);

    /* ---- round trip and permissions --------------------------------------------- */
    wifi_store_init(&st);
    st.enabled = 1;
    n = net("Home", WIFI_SEC_WPA2_PSK, "correct horse battery");
    check("put a WPA2 network", wifi_store_put(&st, &n) == 0 && st.count == 1);
    n = net("Café ø", WIFI_SEC_WPA2_WPA3, "p\"a ss#w=o\\rd");
    check("put a network with a UTF-8 SSID and awkward passphrase", wifi_store_put(&st, &n) == 0);
    n = net("Guest", WIFI_SEC_OPEN, NULL);
    n.hidden = 1;
    check("put a hidden open network", wifi_store_put(&st, &n) == 0 && st.count == 3);
    check("save creates the store", wifi_store_save(dir, &st) == 0);
    check("directory is 0700", mode_of(base, "netd") == 0700);
    check("file is 0600", mode_of(dir, WIFI_STORE_FILE) == 0600);
    check("no temp file is left", mode_of(dir, WIFI_STORE_FILE ".tmp") == (mode_t)-1);
    check("the file says it holds secrets", file_contains(dir, WIFI_STORE_FILE, "Contains Wi-Fi passphrases"));
    check("the passphrase is not in the file as text", !file_contains(dir, WIFI_STORE_FILE, "correct horse"));
    check("the SSID is stored as hex", file_contains(dir, WIFI_STORE_FILE, "network=486f6d65 wpa2 0 "));

    check("reload", wifi_store_load(dir, &back, &too_open) == WIFI_STORE_LOADED);
    check("reload: not too open", too_open == 0);
    check("reload: enabled", back.enabled == 1);
    check("reload: three networks", back.count == 3);
    check("reload: most recent first", back.net[0].ssid.len == 5 && memcmp(back.net[0].ssid.bytes, "Guest", 5) == 0);
    check("reload: hidden flag", back.net[0].hidden == 1 && back.net[0].security == WIFI_SEC_OPEN);
    check("reload: open network has no passphrase", back.net[0].passphrase[0] == '\0');
    check("reload: UTF-8 SSID exact", wifi_ssid_equal(&back.net[1].ssid, &st.net[1].ssid));
    check("reload: awkward passphrase exact", strcmp(back.net[1].passphrase, "p\"a ss#w=o\\rd") == 0);
    check("reload: security kept", back.net[1].security == WIFI_SEC_WPA2_WPA3);
    check("reload: WPA2 passphrase exact", strcmp(back.net[2].passphrase, "correct horse battery") == 0);

    /* permissions are tightened on save */
    chmod(dir, 0755);
    put_file(dir, WIFI_STORE_FILE, "version=1\nenabled=0\n", 0644);
    check("a world-readable store loads but is reported", wifi_store_load(dir, &back, &too_open) ==
                                                              WIFI_STORE_LOADED && too_open == 1);
    check("save tightens the directory", wifi_store_save(dir, &st) == 0 && mode_of(base, "netd") == 0700);
    check("save tightens the file", mode_of(dir, WIFI_STORE_FILE) == 0600);
    {
        mode_t old = umask(0);

        check("save under umask 0 is still 0600", wifi_store_save(dir, &st) == 0 &&
                                                     mode_of(dir, WIFI_STORE_FILE) == 0600);
        umask(old);
    }

    /* a stale temp file from an interrupted save does not block the next */
    put_file(dir, WIFI_STORE_FILE ".tmp", "junk", 0644);
    check("a leftover temp file is replaced", wifi_store_save(dir, &st) == 0 &&
                                                  mode_of(dir, WIFI_STORE_FILE ".tmp") == (mode_t)-1);

    /* ---- ordering and limits ------------------------------------------------------- */
    wifi_store_init(&st);
    for (i = 0; i < WIFI_STORE_MAX; i++) {
        char name[16];

        snprintf(name, sizeof(name), "net%02d", i);
        n = net(name, WIFI_SEC_WPA2_PSK, "password123");
        wifi_store_put(&st, &n);
    }
    check("store fills to its limit", st.count == WIFI_STORE_MAX);
    n = net("newest", WIFI_SEC_WPA2_PSK, "password123");
    check("one more still fits", wifi_store_put(&st, &n) == 0 && st.count == WIFI_STORE_MAX);
    check("the newest is first", memcmp(st.net[0].ssid.bytes, "newest", 6) == 0);
    n = net("net00", WIFI_SEC_WPA2_PSK, "password123");
    check("the oldest was dropped to make room", wifi_store_find(&st, &n.ssid) == -1);
    n = net("net05", WIFI_SEC_WPA2_PSK, "changed-pass");
    check("re-putting an existing network replaces it", wifi_store_put(&st, &n) == 0 && st.count == WIFI_STORE_MAX);
    check("and moves it to the front", memcmp(st.net[0].ssid.bytes, "net05", 5) == 0 &&
                                           strcmp(st.net[0].passphrase, "changed-pass") == 0);
    check("with no duplicate left behind", wifi_store_find(&st, &n.ssid) == 0);
    {
        int dup = 0;
        int j;

        for (i = 0; i < st.count; i++) {
            for (j = i + 1; j < st.count; j++) {
                dup += wifi_ssid_equal(&st.net[i].ssid, &st.net[j].ssid);
            }
        }
        check("no SSID appears twice", dup == 0);
    }
    check("remove", wifi_store_remove(&st, &n.ssid) == 0 && st.count == WIFI_STORE_MAX - 1 &&
                        wifi_store_find(&st, &n.ssid) == -1);
    check("remove an absent network", wifi_store_remove(&st, &n.ssid) == -1);
    check("a full store round-trips", wifi_store_save(dir, &st) == 0 &&
                                          wifi_store_load(dir, &back, NULL) == WIFI_STORE_LOADED &&
                                          back.count == WIFI_STORE_MAX - 1);

    /* ---- invalid entries are refused ----------------------------------------------------- */
    wifi_store_init(&st);
    n = net("Short", WIFI_SEC_WPA2_PSK, "short");
    check("a passphrase under 8 characters is refused", wifi_store_put(&st, &n) == -1);
    n = net("NoPass", WIFI_SEC_WPA2_PSK, NULL);
    check("a WPA2 network without a passphrase is refused", wifi_store_put(&st, &n) == -1);
    n = net("OpenPass", WIFI_SEC_OPEN, "something1");
    check("an open network with a passphrase is refused", wifi_store_put(&st, &n) == -1);
    n = net("Old", WIFI_SEC_WEP, "abcdefghij");
    check("a WEP network is never stored", wifi_store_put(&st, &n) == -1);
    n = net("Corp", WIFI_SEC_ENTERPRISE, "abcdefghij");
    check("an enterprise network is never stored", wifi_store_put(&st, &n) == -1);
    memset(&n, 0, sizeof(n));
    n.security = WIFI_SEC_OPEN;
    check("an empty SSID is refused", wifi_store_put(&st, &n) == -1);
    n.ssid.len = 3;
    check("an all-NUL SSID is refused", wifi_store_put(&st, &n) == -1);
    n = net("Ctl", WIFI_SEC_WPA2_PSK, "abc\ndefghi");
    check("a passphrase with a newline is refused", wifi_store_put(&st, &n) == -1);
    check("nothing was stored", st.count == 0);

    /* ---- damaged files ------------------------------------------------------------------ */
    {
        static const struct {
            const char *name;
            const char *text;
        } bad[] = {
            { "no version", "enabled=1\n" },
            { "unknown version", "version=2\nenabled=1\n" },
            { "two versions", "version=1\nversion=1\n" },
            { "bad enabled", "version=1\nenabled=yes\n" },
            { "unknown key", "version=1\ncolour=blue\n" },
            { "line without =", "version=1\nnonsense\n" },
            { "network with too few fields", "version=1\nnetwork=486f6d65 wpa2 0\n" },
            { "network with too many fields", "version=1\nnetwork=486f6d65 wpa2 0 6162636465666768 x\n" },
            { "network with bad hex SSID", "version=1\nnetwork=48zz wpa2 0 6162636465666768\n" },
            { "network with bad security", "version=1\nnetwork=486f6d65 wpa9 0 6162636465666768\n" },
            { "network with bad hidden flag", "version=1\nnetwork=486f6d65 wpa2 2 6162636465666768\n" },
            { "network with odd passphrase hex", "version=1\nnetwork=486f6d65 wpa2 0 616\n" },
            { "network with upper-case passphrase hex", "version=1\nnetwork=486f6d65 wpa2 0 6162636465666A68\n" },
            { "network with a short passphrase", "version=1\nnetwork=486f6d65 wpa2 0 61626364\n" },
            { "network with a control byte in the passphrase", "version=1\nnetwork=486f6d65 wpa2 0 616263640a656667\n" },
            { "open network with a passphrase", "version=1\nnetwork=486f6d65 open 0 6162636465666768\n" },
            { "duplicate network", "version=1\nnetwork=486f6d65 open 0 -\nnetwork=486f6d65 open 1 -\n" },
        };
        size_t k;

        for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
            char name[160];

            put_file(dir, WIFI_STORE_FILE, bad[k].text, 0600);
            st.enabled = 1;
            st.count = 5;
            snprintf(name, sizeof(name), "damaged (%s) is reported, not partly believed", bad[k].name);
            check(name, wifi_store_load(dir, &st, NULL) == WIFI_STORE_DAMAGED && st.enabled == 0 && st.count == 0);
        }
        put_file(dir, WIFI_STORE_FILE, "", 0600);
        check("an empty file is damaged (no version)", wifi_store_load(dir, &st, NULL) == WIFI_STORE_DAMAGED);
        {
            char longline[400];

            memset(longline, 'a', sizeof(longline) - 2);
            longline[sizeof(longline) - 2] = '\n';
            longline[sizeof(longline) - 1] = '\0';
            put_file(dir, WIFI_STORE_FILE, longline, 0600);
            check("an overlong line is damaged", wifi_store_load(dir, &st, NULL) == WIFI_STORE_DAMAGED);
        }
        put_file(dir, WIFI_STORE_FILE, "# comment\n\nversion=1\r\nenabled=1\r\n", 0600);
        check("comments, blank lines and CRLF are fine", wifi_store_load(dir, &st, NULL) == WIFI_STORE_LOADED &&
                                                             st.enabled == 1);
    }

    /* a damaged file is moved aside, not overwritten */
    put_file(dir, WIFI_STORE_FILE, "version=7\nnetwork=precious\n", 0644);
    check("setting aside a damaged store", wifi_store_set_aside(dir) == 0);
    check("the damaged copy keeps its bytes", file_contains(dir, WIFI_STORE_DAMAGED_FILE, "network=precious"));
    check("the damaged copy is made 0600", mode_of(dir, WIFI_STORE_DAMAGED_FILE) == 0600);
    check("and the store path is free", mode_of(dir, WIFI_STORE_FILE) == (mode_t)-1);
    check("setting aside nothing reports ENOENT", wifi_store_set_aside(dir) == -1 && errno == ENOENT);

    /* a symlink is not followed, in either direction */
    put_file(base, "elsewhere", "version=1\nenabled=1\n", 0644);
    snprintf(cmd, sizeof(cmd), "%s/elsewhere", base);
    {
        char link[700];

        snprintf(link, sizeof(link), "%s/%s", dir, WIFI_STORE_FILE);
        unlink(link);
        check("symlink planted", symlink(cmd, link) == 0);
        check("a symlinked store is not followed on load", wifi_store_load(dir, &st, NULL) == WIFI_STORE_DAMAGED);
        wifi_store_init(&st);
        n = net("Home", WIFI_SEC_WPA2_PSK, "correct horse battery");
        wifi_store_put(&st, &n);
        check("save replaces the link itself", wifi_store_save(dir, &st) == 0 &&
                                                   mode_of(dir, WIFI_STORE_FILE) == 0600);
        check("and never writes through it", !file_contains(base, "elsewhere", "network="));
    }

    /* the directory path must be a directory */
    {
        char notdir[620];

        snprintf(notdir, sizeof(notdir), "%s/elsewhere", base);
        check("a file where the directory should be fails the save", wifi_store_save(notdir, &st) == -1);
    }

    /* an unreadable file is reported as such (skipped when running as root) */
    if (geteuid() != 0) {
        chmod(dir, 0700);
        put_file(dir, WIFI_STORE_FILE, "version=1\nenabled=1\n", 0000);
        check("an unreadable store is unreadable, not absent or damaged",
              wifi_store_load(dir, &st, NULL) == WIFI_STORE_UNREADABLE);
        chmod(dir, 0700);
    } else {
        check("(as root: unreadable case covered by the damaged and symlink cases)", 1);
    }

    /* ---- wipe --------------------------------------------------------------------------- */
    wifi_store_init(&st);
    n = net("Home", WIFI_SEC_WPA2_PSK, "correct horse battery");
    wifi_store_put(&st, &n);
    wifi_store_wipe(&st);
    {
        size_t k;
        int zero = 1;

        for (k = 0; k < sizeof(st.net[0].passphrase); k++) {
            zero &= st.net[0].passphrase[k] == 0;
        }
        check("wipe zeroes every passphrase", zero);
    }

    snprintf(cmd, sizeof(cmd), "chmod -R u+rwx '%s' 2>/dev/null; rm -rf '%s'", base, base);
    if (system(cmd) != 0) {
        fprintf(stderr, "could not remove %s\n", base);
    }
    printf("wifi_store_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
