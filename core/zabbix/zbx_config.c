/*
 * The Zabbix viewer's configuration and its secret. See zbx_config.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "zabbix/zbx_config.h"

#include "pocketpaths.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ENDPOINT "api_jsonrpc.php"

static void say(char *err, size_t len, const char *fmt, const char *arg)
{
    if (err && len) {
        snprintf(err, len, fmt, arg ? arg : "");
    }
}

void zbx_config_defaults(struct zbx_config *c)
{
    memset(c, 0, sizeof(*c));
    c->auth = ZBX_AUTH_TOKEN;
    c->verify = true;
    c->refresh_s = ZBX_REFRESH_DEFAULT_S;
    c->hosts_s = ZBX_HOSTS_DEFAULT_S;
    c->timeout_s = ZBX_TIMEOUT_DEFAULT_S;
    snprintf(c->scenario, sizeof(c->scenario), "%s", ZBX_FAKE_DEFAULT);
}

void zbx_config_forget_secret(struct zbx_config *c)
{
    explicit_bzero(c->secret, sizeof(c->secret));
    c->have_secret = false;
}

void zbx_config_paths(char *conf, size_t conf_len, char *secret, size_t secret_len)
{
    if (conf) {
        snprintf(conf, conf_len, "%s/%s", pocketos_config_dir(), ZBX_CONF_FILE);
    }
    if (secret) {
        snprintf(secret, secret_len, "%s/%s/%s", pocketos_state_dir(), ZBX_SECRET_DIR,
                 ZBX_SECRET_FILE);
    }
}

/* ---- the URL ------------------------------------------------------------------ */

int zbx_config_url(const char *in, char *endpoint, size_t endpoint_len, char *shown,
                   size_t shown_len, bool *https, char *err, size_t errlen)
{
    char buf[ZBX_URL_MAX];
    const char *rest;
    const char *slash;
    size_t n;
    size_t i;

    while (in && isspace((unsigned char)*in)) {
        in++;
    }
    if (!in || !*in) {
        say(err, errlen, "no url", NULL);
        return -1;
    }
    n = strlen(in);
    while (n > 0 && isspace((unsigned char)in[n - 1])) {
        n--;
    }
    if (n + sizeof(ENDPOINT) + 2 > sizeof(buf)) {
        say(err, errlen, "the url is too long", NULL);
        return -1;
    }
    memcpy(buf, in, n);
    buf[n] = '\0';
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)buf[i];

        if (ch <= 0x20 || ch == 0x7F) {
            say(err, errlen, "the url has a space or a control character in it", NULL);
            return -1;
        }
        if (ch == '?' || ch == '#') {
            say(err, errlen, "the url has a query or a fragment; give the frontend's address", NULL);
            return -1;
        }
    }
    if (strncasecmp(buf, "https://", 8) == 0) {
        *https = true;
        rest = buf + 8;
    } else if (strncasecmp(buf, "http://", 7) == 0) {
        *https = false;
        rest = buf + 7;
    } else {
        say(err, errlen, "the url must start with https:// (or http://)", NULL);
        return -1;
    }
    slash = strchr(rest, '/');
    {
        size_t alen = slash ? (size_t)(slash - rest) : strlen(rest);

        if (alen == 0) {
            say(err, errlen, "the url has no host", NULL);
            return -1;
        }
        if (memchr(rest, '@', alen)) {
            say(err, errlen, "the url must not carry a user or password (user@host)", NULL);
            return -1;
        }
    }
    /* The endpoint: the frontend's directory plus api_jsonrpc.php. */
    n = strlen(buf);
    if (n >= strlen(ENDPOINT) && strcmp(buf + n - strlen(ENDPOINT), ENDPOINT) == 0) {
        snprintf(endpoint, endpoint_len, "%s", buf);
        buf[n - strlen(ENDPOINT)] = '\0';
        snprintf(shown, shown_len, "%s", buf);
    } else {
        if (!slash) {
            buf[n++] = '/';
            buf[n] = '\0';
        } else if (buf[n - 1] != '/') {
            buf[n++] = '/';
            buf[n] = '\0';
        }
        snprintf(shown, shown_len, "%s", buf);
        snprintf(endpoint, endpoint_len, "%s%s", buf, ENDPOINT);
    }
    return 0;
}

/* ---- the conf file --------------------------------------------------------------- */

static bool parse_int(const char *v, int lo, int hi, int *out)
{
    char *end;
    long n;

    errno = 0;
    n = strtol(v, &end, 10);
    if (errno || end == v || *end || n < lo || n > hi) {
        return false;
    }
    *out = (int)n;
    return true;
}

static bool parse_bool(const char *v, bool *out)
{
    if (strcmp(v, "1") == 0 || strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(v, "0") == 0 || strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static char *trim(char *s)
{
    char *e;

    while (isspace((unsigned char)*s)) {
        s++;
    }
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = '\0';
    }
    return s;
}

int zbx_config_parse(struct zbx_config *c, const char *text, char *err, size_t errlen)
{
    char *copy = strdup(text ? text : "");
    char *save = NULL;
    char *line;
    char url[ZBX_URL_MAX] = "";
    int rc = 0;
    int lineno = 0;

    if (!copy) {
        say(err, errlen, "out of memory", NULL);
        return -1;
    }
    for (line = strtok_r(copy, "\n", &save); line && rc == 0; line = strtok_r(NULL, "\n", &save)) {
        char *eq;
        char *k;
        char *v;
        char where[48];

        lineno++;
        line = trim(line);
        if (!*line || *line == '#') {
            continue;
        }
        eq = strchr(line, '=');
        snprintf(where, sizeof(where), "line %d", lineno);
        if (!eq) {
            say(err, errlen, "zabbix.conf %s: not key=value", where);
            rc = -1;
            break;
        }
        *eq = '\0';
        k = trim(line);
        v = trim(eq + 1);
        if (strcmp(k, "url") == 0) {
            snprintf(url, sizeof(url), "%s", v);
        } else if (strcmp(k, "label") == 0) {
            zbx_copy_text(c->label, sizeof(c->label), v);
        } else if (strcmp(k, "auth") == 0) {
            if (strcmp(v, "token") == 0) {
                c->auth = ZBX_AUTH_TOKEN;
            } else if (strcmp(v, "password") == 0) {
                c->auth = ZBX_AUTH_PASSWORD;
            } else {
                say(err, errlen, "zabbix.conf: auth must be token or password", NULL);
                rc = -1;
            }
        } else if (strcmp(k, "user") == 0) {
            if (strlen(v) >= sizeof(c->user)) {
                say(err, errlen, "zabbix.conf: user is too long", NULL);
                rc = -1;
            } else {
                snprintf(c->user, sizeof(c->user), "%s", v);
            }
        } else if (strcmp(k, "verify_tls") == 0) {
            if (!parse_bool(v, &c->verify)) {
                say(err, errlen, "zabbix.conf: verify_tls must be 0 or 1", NULL);
                rc = -1;
            }
        } else if (strcmp(k, "ca_file") == 0) {
            if (strlen(v) >= sizeof(c->ca_file)) {
                say(err, errlen, "zabbix.conf: ca_file is too long", NULL);
                rc = -1;
            } else {
                snprintf(c->ca_file, sizeof(c->ca_file), "%s", v);
            }
        } else if (strcmp(k, "allow_insecure_http") == 0) {
            if (!parse_bool(v, &c->allow_http)) {
                say(err, errlen, "zabbix.conf: allow_insecure_http must be 0 or 1", NULL);
                rc = -1;
            }
        } else if (strcmp(k, "refresh_s") == 0) {
            if (!parse_int(v, ZBX_REFRESH_MIN_S, ZBX_REFRESH_MAX_S, &c->refresh_s)) {
                say(err, errlen, "zabbix.conf: refresh_s must be 15..600", NULL);
                rc = -1;
            }
        } else if (strcmp(k, "hosts_s") == 0) {
            if (!parse_int(v, ZBX_HOSTS_MIN_S, ZBX_HOSTS_MAX_S, &c->hosts_s)) {
                say(err, errlen, "zabbix.conf: hosts_s must be 30..3600", NULL);
                rc = -1;
            }
        } else if (strcmp(k, "timeout_s") == 0) {
            if (!parse_int(v, ZBX_TIMEOUT_MIN_S, ZBX_TIMEOUT_MAX_S, &c->timeout_s)) {
                say(err, errlen, "zabbix.conf: timeout_s must be 3..30", NULL);
                rc = -1;
            }
        } else if (strcmp(k, "mode") == 0) {
            if (strcmp(v, "fake") == 0) {
                c->fake = true;
            } else if (strcmp(v, "live") == 0) {
                c->fake = false;
            } else {
                say(err, errlen, "zabbix.conf: mode must be live or fake", NULL);
                rc = -1;
            }
        } else if (strcmp(k, "scenario") == 0) {
            if (!zbx_fake_known(v)) {
                say(err, errlen, "zabbix.conf: unknown scenario %s", v);
                rc = -1;
            } else {
                snprintf(c->scenario, sizeof(c->scenario), "%s", v);
            }
        } else if (strcmp(k, "token") == 0 || strcmp(k, "password") == 0) {
            /* The one mistake worth refusing loudly: a secret in the file
             * everyone can read. */
            say(err, errlen, "zabbix.conf must not hold a %s: use pos-zabbix set-secret", k);
            rc = -1;
        }
        /* Unknown keys are ignored, so an older helper reads a newer file. */
    }
    explicit_bzero(copy, strlen(copy));
    free(copy);
    if (rc != 0) {
        return -1;
    }
    if (c->fake) {
        c->configured = true;
        snprintf(c->url_shown, sizeof(c->url_shown), "fake://%s", c->scenario);
        return 0;
    }
    if (!url[0]) {
        return 0; /* not configured, and that is not an error */
    }
    if (zbx_config_url(url, c->url, sizeof(c->url), c->url_shown, sizeof(c->url_shown), &c->https,
                       err, errlen) != 0) {
        return -1;
    }
    if (!c->https && !c->allow_http) {
        say(err, errlen, "http:// would send the credentials unencrypted; use https:// or set "
                         "allow_insecure_http=1", NULL);
        c->url[0] = '\0';
        return -1;
    }
    if (c->auth == ZBX_AUTH_PASSWORD && !c->user[0]) {
        say(err, errlen, "zabbix.conf: auth=password needs user=", NULL);
        return -1;
    }
    c->configured = true;
    return 0;
}

static int read_small(int fd, char *buf, size_t len)
{
    size_t got = 0;

    while (got + 1 < len) {
        ssize_t r = read(fd, buf + got, len - 1 - got);

        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            break;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    return (int)got;
}

int zbx_config_read_secret(struct zbx_config *c, const char *path, char *err, size_t errlen)
{
    char buf[ZBX_SECRET_MAX + 32];
    struct stat st;
    char *line;
    char *eq;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int n;

    if (fd < 0) {
        if (errno == ENOENT) {
            say(err, errlen, "no secret stored (pos-zabbix set-secret)", NULL);
        } else {
            say(err, errlen, "the secret file cannot be opened: %s", strerror(errno));
        }
        return -1;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        say(err, errlen, "the secret file is not a regular file", NULL);
        return -1;
    }
    if ((st.st_mode & 077) != 0 || st.st_uid != geteuid()) {
        close(fd);
        say(err, errlen, "the secret file may be read by others: chmod 600 it", NULL);
        return -1;
    }
    n = read_small(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0 || (size_t)n >= sizeof(buf) - 1) {
        explicit_bzero(buf, sizeof(buf));
        say(err, errlen, "the secret file cannot be read", NULL);
        return -1;
    }
    /* One line: the first newline ends it (and a CR before it). */
    line = buf;
    {
        char *nl = strchr(line, '\n');

        if (nl) {
            *nl = '\0';
        }
        n = (int)strlen(line);
        if (n > 0 && line[n - 1] == '\r') {
            line[n - 1] = '\0';
        }
    }
    eq = strchr(line, '=');
    if (!eq) {
        explicit_bzero(buf, sizeof(buf));
        say(err, errlen, "the secret file is not token= or password=", NULL);
        return -1;
    }
    *eq = '\0';
    line = trim(line);
    if ((strcmp(line, "token") == 0 && c->auth != ZBX_AUTH_TOKEN) ||
        (strcmp(line, "password") == 0 && c->auth != ZBX_AUTH_PASSWORD) ||
        (strcmp(line, "token") != 0 && strcmp(line, "password") != 0)) {
        explicit_bzero(buf, sizeof(buf));
        say(err, errlen, "the stored secret does not match auth= in zabbix.conf", NULL);
        return -1;
    }
    /* A password exactly as written (spaces at its ends included); a token
     * with any stray whitespace around it dropped. */
    line = c->auth == ZBX_AUTH_PASSWORD ? eq + 1 : trim(eq + 1);
    if (!*line || strlen(line) >= sizeof(c->secret)) {
        explicit_bzero(buf, sizeof(buf));
        say(err, errlen, "the stored secret is empty or too long", NULL);
        return -1;
    }
    snprintf(c->secret, sizeof(c->secret), "%s", line);
    c->have_secret = true;
    explicit_bzero(buf, sizeof(buf));
    return 0;
}

int zbx_config_load(struct zbx_config *c, const char *conf_path, const char *secret_path,
                    char *err, size_t errlen)
{
    char buf[ZBX_CONF_BYTES_MAX];
    int fd;
    int n;

    zbx_config_defaults(c);
    if (err && errlen) {
        err[0] = '\0';
    }
    fd = open(conf_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? 0 : (say(err, errlen, "zabbix.conf cannot be read: %s",
                                          strerror(errno)), -1);
    }
    n = read_small(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0 || (size_t)n >= sizeof(buf) - 1) {
        say(err, errlen, "zabbix.conf cannot be read or is too large", NULL);
        return -1;
    }
    if (zbx_config_parse(c, buf, err, errlen) != 0) {
        c->configured = false;
        return -1;
    }
    if (c->configured && !c->fake && secret_path) {
        if (zbx_config_read_secret(c, secret_path, err, errlen) != 0) {
            c->configured = false;
            return -1;
        }
    }
    return 0;
}

/* ---- writing the secret ---------------------------------------------------------- */

static int fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int rc;

    if (fd < 0) {
        return -1;
    }
    rc = fsync(fd);
    close(fd);
    return rc;
}

int zbx_config_write_secret(const char *path, const char *kind, const char *value, char *err,
                            size_t errlen)
{
    char dir[ZBX_PATH_MAX];
    char tmp[ZBX_PATH_MAX + 16];
    char line[ZBX_SECRET_MAX + 32];
    char *slash;
    size_t i;
    size_t n;
    int fd;
    int len;

    if (strcmp(kind, "token") != 0 && strcmp(kind, "password") != 0) {
        say(err, errlen, "the secret is a token or a password", NULL);
        return -1;
    }
    if (strlen(path) >= sizeof(dir)) {
        say(err, errlen, "the secret path is too long", NULL);
        return -1;
    }
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (!slash) {
        say(err, errlen, "the secret path has no directory", NULL);
        return -1;
    }
    *slash = '\0';
    if (!value || !*value) {
        if (unlink(path) != 0 && errno != ENOENT) {
            say(err, errlen, "the secret could not be removed: %s", strerror(errno));
            return -1;
        }
        return 0;
    }
    n = strlen(value);
    if (n >= ZBX_SECRET_MAX) {
        say(err, errlen, "the secret is too long", NULL);
        return -1;
    }
    /* A token is printable ASCII without spaces. A password is whatever the
     * user chose - spaces, æøå, a space at either end - and is kept byte for
     * byte; only what cannot sit on one line (NUL, CR, LF) is refused. */
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)value[i];

        if (strcmp(kind, "token") == 0 ? (ch <= 0x20 || ch >= 0x7F) : (ch == '\r' || ch == '\n')) {
            say(err, errlen, strcmp(kind, "token") == 0
                                 ? "a token is one word of printable characters"
                                 : "a password must be on one line", NULL);
            return -1;
        }
    }
    if (pocketos_mkdir_p(dir, 0700) != 0 || chmod(dir, 0700) != 0) {
        say(err, errlen, "the secret directory cannot be made: %s", strerror(errno));
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        say(err, errlen, "the secret cannot be written: %s", strerror(errno));
        return -1;
    }
    len = snprintf(line, sizeof(line), "%s=%s\n", kind, value);
    if (fchmod(fd, 0600) != 0 || write(fd, line, (size_t)len) != len || fsync(fd) != 0) {
        explicit_bzero(line, sizeof(line));
        close(fd);
        unlink(tmp);
        say(err, errlen, "the secret cannot be written: %s", strerror(errno));
        return -1;
    }
    explicit_bzero(line, sizeof(line));
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        say(err, errlen, "the secret cannot be stored: %s", strerror(errno));
        return -1;
    }
    fsync_dir(dir);
    return 0;
}
