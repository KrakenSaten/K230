/*
 * netd's Wi-Fi store. See wifi_store.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "wifi_store.h"

#include "pocketpaths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LINE_MAX_LEN 256

void wifi_store_init(struct wifi_store *st)
{
    memset(st, 0, sizeof(*st));
}

void wifi_store_wipe(struct wifi_store *st)
{
    int i;

    for (i = 0; i < WIFI_STORE_MAX; i++) {
        explicit_bzero(st->net[i].passphrase, sizeof(st->net[i].passphrase));
    }
}

static int entry_valid(const struct wifi_saved *n)
{
    if (n->ssid.len == 0 || n->ssid.len > WIFI_SSID_MAX || wifi_ssid_is_hidden(&n->ssid)) {
        return 0;
    }
    if (wifi_security_needs_passphrase(n->security)) {
        return wifi_passphrase_valid(n->passphrase) == 0;
    }
    if (n->security != WIFI_SEC_OPEN) {
        /* Nothing else can be joined, so nothing else is ever stored. */
        return 0;
    }
    return n->passphrase[0] == '\0' && (n->hidden == 0 || n->hidden == 1);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* network=<ssid hex> <security> <hidden> <passphrase hex or -> */
static int parse_network(char *value, struct wifi_saved *n)
{
    char *save = NULL;
    char *ssid = strtok_r(value, " ", &save);
    char *sec = strtok_r(NULL, " ", &save);
    char *hidden = strtok_r(NULL, " ", &save);
    char *pass = strtok_r(NULL, " ", &save);
    size_t plen;
    size_t i;

    memset(n, 0, sizeof(*n));
    if (!ssid || !sec || !hidden || !pass || strtok_r(NULL, " ", &save)) {
        return -1;
    }
    if (wifi_ssid_from_hex(ssid, &n->ssid) < 0 || wifi_security_parse(sec, &n->security) < 0) {
        return -1;
    }
    if (strcmp(hidden, "0") != 0 && strcmp(hidden, "1") != 0) {
        return -1;
    }
    n->hidden = hidden[0] == '1';
    if (strcmp(pass, "-") != 0) {
        plen = strlen(pass);
        if (plen % 2 != 0 || plen / 2 > WIFI_PASSPHRASE_MAX) {
            return -1;
        }
        for (i = 0; i < plen; i += 2) {
            int hi = hex_val(pass[i]);
            int lo = hex_val(pass[i + 1]);

            if (hi < 0 || lo < 0) {
                explicit_bzero(n->passphrase, sizeof(n->passphrase));
                return -1;
            }
            n->passphrase[i / 2] = (char)(hi * 16 + lo);
        }
        explicit_bzero(pass, plen);
    }
    return entry_valid(n) ? 0 : -1;
}

enum wifi_store_load_result wifi_store_load(const char *dir, struct wifi_store *st, int *too_open)
{
    char path[POCKETOS_PATH_MAX];
    char line[LINE_MAX_LEN];
    struct stat sb;
    FILE *f;
    int fd;
    int have_version = 0;
    int damaged = 0;

    wifi_store_init(st);
    if (too_open) {
        *too_open = 0;
    }
    if (snprintf(path, sizeof(path), "%s/%s", dir, WIFI_STORE_FILE) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return WIFI_STORE_UNREADABLE;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) {
            return WIFI_STORE_ABSENT;
        }
        /* ELOOP: a symlink where the store should be is not followed, and is
         * reported as damaged so it gets moved aside, not written through. */
        return errno == ELOOP ? WIFI_STORE_DAMAGED : WIFI_STORE_UNREADABLE;
    }
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        close(fd);
        return WIFI_STORE_DAMAGED;
    }
    if (too_open && (sb.st_mode & 077)) {
        *too_open = 1;
    }
    f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return WIFI_STORE_UNREADABLE;
    }
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        char *eq;

        if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
            damaged = 1; /* a line longer than any this code writes */
            break;
        }
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            damaged = 1;
            break;
        }
        *eq = '\0';
        if (strcmp(line, "version") == 0) {
            if (have_version || strcmp(eq + 1, "1") != 0) {
                damaged = 1;
                break;
            }
            have_version = 1;
        } else if (strcmp(line, "enabled") == 0) {
            if (strcmp(eq + 1, "0") != 0 && strcmp(eq + 1, "1") != 0) {
                damaged = 1;
                break;
            }
            st->enabled = eq[1] == '1';
        } else if (strcmp(line, "network") == 0) {
            struct wifi_saved n;

            if (st->count >= WIFI_STORE_MAX || parse_network(eq + 1, &n) < 0 ||
                wifi_store_find(st, &n.ssid) >= 0) {
                explicit_bzero(&n, sizeof(n));
                damaged = 1;
                break;
            }
            st->net[st->count++] = n;
            explicit_bzero(&n, sizeof(n));
        } else {
            damaged = 1;
            break;
        }
    }
    explicit_bzero(line, sizeof(line));
    if (ferror(f)) {
        fclose(f);
        wifi_store_wipe(st);
        wifi_store_init(st);
        return WIFI_STORE_UNREADABLE;
    }
    fclose(f);
    if (damaged || !have_version) {
        wifi_store_wipe(st);
        wifi_store_init(st);
        return WIFI_STORE_DAMAGED;
    }
    return WIFI_STORE_LOADED;
}

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int prepare_dir(const char *dir)
{
    struct stat sb;

    if (pocketos_mkdir_p(dir, 0700) < 0) {
        return -1;
    }
    if (lstat(dir, &sb) != 0) {
        return -1;
    }
    if (!S_ISDIR(sb.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    if ((sb.st_mode & 07777) != 0700 && chmod(dir, 0700) != 0) {
        return -1;
    }
    return 0;
}

int wifi_store_save(const char *dir, const struct wifi_store *st)
{
    char path[POCKETOS_PATH_MAX];
    char tmp[POCKETOS_PATH_MAX];
    char line[LINE_MAX_LEN];
    int fd;
    int dfd;
    int i;
    int saved_errno;

    if (snprintf(path, sizeof(path), "%s/%s", dir, WIFI_STORE_FILE) >= (int)sizeof(path) ||
        snprintf(tmp, sizeof(tmp), "%s/%s.tmp", dir, WIFI_STORE_FILE) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (prepare_dir(dir) < 0) {
        return -1;
    }
    unlink(tmp); /* a leftover from a save that was cut short */
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, 0600) != 0) {
        goto fail;
    }
    snprintf(line, sizeof(line),
             "# Doors Wi-Fi networks, written by netd. Contains Wi-Fi passphrases:\n"
             "# root only, mode 0600. See docs/decisions/ADR-003-wifi-credentials.md.\n"
             "version=%d\nenabled=%d\n", WIFI_STORE_VERSION, st->enabled ? 1 : 0);
    if (write_all(fd, line, strlen(line)) < 0) {
        goto fail;
    }
    for (i = 0; i < st->count && i < WIFI_STORE_MAX; i++) {
        const struct wifi_saved *n = &st->net[i];
        char ssid[WIFI_SSID_HEX_MAX];
        char pass[WIFI_PASSPHRASE_MAX * 2 + 1];
        size_t plen = strlen(n->passphrase);
        size_t k;

        if (!entry_valid(n)) {
            errno = EINVAL;
            goto fail;
        }
        wifi_ssid_to_hex(&n->ssid, ssid, sizeof(ssid));
        for (k = 0; k < plen; k++) {
            snprintf(pass + k * 2, 3, "%02x", (unsigned char)n->passphrase[k]);
        }
        pass[plen * 2] = '\0';
        snprintf(line, sizeof(line), "network=%s %s %d %s\n", ssid,
                 wifi_security_name(n->security), n->hidden ? 1 : 0, plen ? pass : "-");
        explicit_bzero(pass, sizeof(pass));
        if (write_all(fd, line, strlen(line)) < 0) {
            goto fail;
        }
    }
    explicit_bzero(line, sizeof(line));
    if (fsync(fd) != 0) {
        goto fail;
    }
    if (close(fd) != 0) {
        fd = -1;
        goto fail;
    }
    fd = -1;
    if (rename(tmp, path) != 0) {
        goto fail;
    }
    dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        /* Best effort: the rename is already durable on most filesystems. */
        if (fsync(dfd) != 0) {
            saved_errno = errno;
            close(dfd);
            errno = saved_errno;
            return -1;
        }
        close(dfd);
    }
    return 0;
fail:
    saved_errno = errno;
    explicit_bzero(line, sizeof(line));
    if (fd >= 0) {
        close(fd);
    }
    unlink(tmp);
    errno = saved_errno;
    return -1;
}

int wifi_store_set_aside(const char *dir)
{
    char path[POCKETOS_PATH_MAX];
    char aside[POCKETOS_PATH_MAX];

    if (snprintf(path, sizeof(path), "%s/%s", dir, WIFI_STORE_FILE) >= (int)sizeof(path) ||
        snprintf(aside, sizeof(aside), "%s/%s", dir, WIFI_STORE_DAMAGED_FILE) >= (int)sizeof(aside)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (rename(path, aside) != 0) {
        return -1;
    }
    /* The damaged copy may still hold passphrases. */
    chmod(aside, 0600);
    return 0;
}

int wifi_store_find(const struct wifi_store *st, const struct wifi_ssid *ssid)
{
    int i;

    for (i = 0; i < st->count; i++) {
        if (wifi_ssid_equal(&st->net[i].ssid, ssid)) {
            return i;
        }
    }
    return -1;
}

int wifi_store_put(struct wifi_store *st, const struct wifi_saved *net)
{
    int at;

    if (!entry_valid(net)) {
        return -1;
    }
    at = wifi_store_find(st, &net->ssid);
    if (at < 0) {
        /* A new network: drop the oldest when full. */
        at = st->count < WIFI_STORE_MAX ? st->count++ : WIFI_STORE_MAX - 1;
    }
    explicit_bzero(&st->net[at], sizeof(st->net[at]));
    memmove(&st->net[1], &st->net[0], (size_t)at * sizeof(st->net[0]));
    st->net[0] = *net;
    return 0;
}

int wifi_store_remove(struct wifi_store *st, const struct wifi_ssid *ssid)
{
    int at = wifi_store_find(st, ssid);

    if (at < 0) {
        return -1;
    }
    memmove(&st->net[at], &st->net[at + 1], (size_t)(st->count - at - 1) * sizeof(st->net[0]));
    st->count--;
    explicit_bzero(&st->net[st->count], sizeof(st->net[0]));
    return 0;
}
