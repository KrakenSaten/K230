/*
 * The Zabbix connection, changed on the unit. See zbx_settings.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "zabbix/zbx_settings.h"

#include "pocketlog/pocketlog.h"
#include "pocketpaths.h"
#include "zabbix/zbx_client.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int (*zbx_settings_rename)(const char *from, const char *to) = rename;

static void say(char *err, size_t len, const char *fmt, const char *arg)
{
    if (err && len) {
        snprintf(err, len, fmt, arg ? arg : "");
    }
}

static const char *kind_of(enum zbx_auth a)
{
    return a == ZBX_AUTH_PASSWORD ? "password" : "token";
}

/* The whole of a small file, NUL-ended. 0 (an absent file reads as ""),
 * or -1 with errno. */
static int read_text(const char *path, char *buf, size_t len)
{
    size_t got = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    buf[0] = '\0';
    if (fd < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    while (got + 1 < len) {
        ssize_t r = read(fd, buf + got, len - 1 - got);

        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0) {
            close(fd);
            return -1;
        }
        if (r == 0) {
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    if (got + 1 >= len) {
        errno = EFBIG;
        return -1;
    }
    return 0;
}

static void trim_copy(char *dst, size_t len, const char *s, size_t n)
{
    while (n > 0 && isspace((unsigned char)*s)) {
        s++;
        n--;
    }
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        n--;
    }
    if (n >= len) {
        n = len - 1;
    }
    memcpy(dst, s, n);
    dst[n] = '\0';
}

/* One line of a conf file split into its key and value (trimmed); false for
 * a blank line, a comment or a line without '='. */
static bool key_value(const char *line, size_t n, char *key, size_t klen, char *val, size_t vlen)
{
    const char *eq;
    size_t i = 0;

    while (i < n && isspace((unsigned char)line[i])) {
        i++;
    }
    if (i == n || line[i] == '#') {
        return false;
    }
    eq = memchr(line, '=', n);
    if (!eq) {
        return false;
    }
    trim_copy(key, klen, line, (size_t)(eq - line));
    trim_copy(val, vlen, eq + 1, n - (size_t)(eq + 1 - line));
    return true;
}

/* ---- reading -------------------------------------------------------------------- */

void zbx_settings_read(const char *conf_path, const char *secret_path, struct zbx_settings *s,
                       char *note, size_t notelen)
{
    static char text[ZBX_CONF_BYTES_MAX];
    struct zbx_config cfg;
    char url[ZBX_URL_MAX] = "";
    char err[ZBX_TEXT_MAX] = "";
    const char *p;

    memset(s, 0, sizeof(*s));
    s->auth = ZBX_AUTH_TOKEN;
    if (note && notelen) {
        note[0] = '\0';
    }
    if (read_text(conf_path, text, sizeof(text)) != 0) {
        say(note, notelen, "zabbix.conf cannot be read: %s", strerror(errno));
        return;
    }
    /* What the file says, even when it says something else wrong: the
     * screen shows it so it can be put right. The last of a key wins, as in
     * zbx_config_parse(). */
    for (p = text; *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char key[32];
        char val[ZBX_URL_MAX];

        if (key_value(p, n, key, sizeof(key), val, sizeof(val))) {
            if (strcmp(key, "url") == 0) {
                snprintf(url, sizeof(url), "%s", val);
            } else if (strcmp(key, "auth") == 0) {
                s->auth = strcmp(val, "password") == 0 ? ZBX_AUTH_PASSWORD : ZBX_AUTH_TOKEN;
            } else if (strcmp(key, "user") == 0) {
                /* A longer one is refused by the parser; the note says so. */
                snprintf(s->user, sizeof(s->user), "%.*s", (int)sizeof(s->user) - 1, val);
            }
        }
        p += n + (nl ? 1 : 0);
    }
    explicit_bzero(text, sizeof(text));
    if (url[0]) {
        char endpoint[ZBX_URL_MAX];
        bool https;

        if (zbx_config_url(url, endpoint, sizeof(endpoint), s->url, sizeof(s->url), &https, err,
                           sizeof(err)) != 0) {
            s->url[0] = '\0';
        }
    }
    /* Whether a secret of the kind auth= asks for is there and usable. */
    zbx_config_defaults(&cfg);
    cfg.auth = s->auth;
    if (secret_path && zbx_config_read_secret(&cfg, secret_path, NULL, 0) == 0) {
        s->secret_stored = true;
    }
    zbx_config_forget_secret(&cfg);
    /* And whether the helper could start with the files as they are. */
    if (zbx_config_load(&cfg, conf_path, secret_path, err, sizeof(err)) != 0) {
        say(note, notelen, "%s", err);
    } else if (cfg.fake) {
        say(note, notelen, "zabbix.conf runs the demo (mode=fake); SAVE sets up a real server", NULL);
    }
    zbx_config_forget_secret(&cfg);
}

/* ---- composing --------------------------------------------------------------------- */

struct out {
    char *buf;
    size_t len;
    size_t at;
    bool over;
};

static void put(struct out *o, const char *s, size_t n)
{
    if (o->over || o->at + n + 1 > o->len) {
        o->over = true;
        return;
    }
    memcpy(o->buf + o->at, s, n);
    o->at += n;
    o->buf[o->at] = '\0';
}

static void put_line(struct out *o, const char *key, const char *val)
{
    put(o, key, strlen(key));
    put(o, "=", 1);
    put(o, val, strlen(val));
    put(o, "\n", 1);
}

int zbx_settings_compose(const char *old_text, const struct zbx_settings *s, char *out,
                         size_t outlen, char *err, size_t errlen)
{
    struct out o = { out, outlen, 0, false };
    bool done_url = false;
    bool done_auth = false;
    bool done_user = false;
    const char *p;

    if (outlen) {
        out[0] = '\0';
    }
    for (p = old_text ? old_text : ""; *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char key[32];
        char val[16];

        if (key_value(p, n, key, sizeof(key), val, sizeof(val))) {
            if (strcmp(key, "url") == 0) {
                if (!done_url) {
                    put_line(&o, "url", s->url);
                }
                done_url = true;
                p += n + (nl ? 1 : 0);
                continue;
            }
            if (strcmp(key, "auth") == 0) {
                if (!done_auth) {
                    put_line(&o, "auth", kind_of(s->auth));
                }
                done_auth = true;
                p += n + (nl ? 1 : 0);
                continue;
            }
            if (strcmp(key, "user") == 0) {
                if (!done_user && s->user[0]) {
                    put_line(&o, "user", s->user);
                }
                done_user = true;
                p += n + (nl ? 1 : 0);
                continue;
            }
            if (strcmp(key, "mode") == 0 && strcmp(val, "fake") == 0) {
                p += n + (nl ? 1 : 0);
                continue;
            }
        }
        /* Anything else exactly as it was written. */
        put(&o, p, n);
        put(&o, "\n", 1);
        p += n + (nl ? 1 : 0);
    }
    if (!done_url) {
        put_line(&o, "url", s->url);
    }
    if (!done_auth) {
        put_line(&o, "auth", kind_of(s->auth));
    }
    if (!done_user && s->user[0]) {
        put_line(&o, "user", s->user);
    }
    /* zbx_config_load() reads at most ZBX_CONF_BYTES_MAX - 2 bytes. */
    if (o.over || o.at + 2 > ZBX_CONF_BYTES_MAX) {
        say(err, errlen, "zabbix.conf would be too large", NULL);
        if (outlen) {
            out[0] = '\0';
        }
        return -1;
    }
    return 0;
}

/* ---- the commit -------------------------------------------------------------------- */

static void dir_of(const char *path, char *dir, size_t len)
{
    char *slash;

    snprintf(dir, len, "%s", path);
    slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
    } else {
        snprintf(dir, len, "%s", slash ? "/" : ".");
    }
}

static void sync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

/* zabbix.conf.new: the new text, 0644 like settings.conf (it holds no
 * secret), synced. */
static int stage_conf(const char *conf_path, const char *text, char *tmp, size_t tmplen, char *err,
                      size_t errlen)
{
    char dir[ZBX_PATH_MAX];
    size_t n = strlen(text);
    size_t done = 0;
    int fd;

    dir_of(conf_path, dir, sizeof(dir));
    if (pocketos_mkdir_p(dir, 0755) != 0) {
        say(err, errlen, "the configuration directory cannot be made: %s", strerror(errno));
        return -1;
    }
    if ((size_t)snprintf(tmp, tmplen, "%s.new", conf_path) >= tmplen) {
        say(err, errlen, "the configuration path is too long", NULL);
        return -1;
    }
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        say(err, errlen, "zabbix.conf cannot be written: %s", strerror(errno));
        return -1;
    }
    while (done < n) {
        ssize_t w = write(fd, text + done, n - done);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            break;
        }
        done += (size_t)w;
    }
    if (done != n || fchmod(fd, 0644) != 0 || fsync(fd) != 0) {
        say(err, errlen, "zabbix.conf cannot be written: %s", strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd) != 0) {
        say(err, errlen, "zabbix.conf cannot be written: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int commit(const char *conf_path, const char *secret_path, const char *text, const char *kind,
                  const char *new_secret, char *err, size_t errlen)
{
    char ctmp[ZBX_PATH_MAX + 16];
    char stmp[ZBX_PATH_MAX + 16];
    char prev[ZBX_PATH_MAX + 16];
    char dir[ZBX_PATH_MAX];
    bool secret = new_secret && *new_secret;
    bool had = false;

    if (stage_conf(conf_path, text, ctmp, sizeof(ctmp), err, errlen) != 0) {
        return -1;
    }
    if (secret) {
        if (zbx_config_stage_secret(secret_path, kind, new_secret, stmp, sizeof(stmp), err, errlen) != 0) {
            unlink(ctmp);
            return -1;
        }
        /* The secret in use, kept by a second name until the pair is in. */
        snprintf(prev, sizeof(prev), "%s.prev", secret_path);
        unlink(prev);
        if (link(secret_path, prev) == 0) {
            had = true;
        } else if (errno != ENOENT) {
            say(err, errlen, "the stored secret cannot be kept for a rollback: %s", strerror(errno));
            unlink(stmp);
            unlink(ctmp);
            return -1;
        }
        if (zbx_settings_rename(stmp, secret_path) != 0) {
            say(err, errlen, "the secret cannot be stored: %s", strerror(errno));
            unlink(stmp);
            unlink(ctmp);
            if (had) {
                unlink(prev);
            }
            return -1;
        }
    }
    if (zbx_settings_rename(ctmp, conf_path) != 0) {
        int e = errno;

        unlink(ctmp);
        if (secret) {
            /* Back to the pair that worked before. */
            if (had) {
                rename(prev, secret_path);
            } else {
                unlink(secret_path);
            }
        }
        say(err, errlen, "zabbix.conf cannot be stored: %s", strerror(e));
        return -1;
    }
    if (had) {
        unlink(prev);
    }
    dir_of(conf_path, dir, sizeof(dir));
    sync_dir(dir);
    if (secret) {
        dir_of(secret_path, dir, sizeof(dir));
        sync_dir(dir);
    }
    return 0;
}

/* ---- trying ------------------------------------------------------------------------ */

static enum zbx_cresult probe(const struct zbx_config *cfg, struct zbx_transport *tr,
                              int64_t (*now_ms)(void *user), void *clock_user, char *text,
                              size_t textlen)
{
    struct zbx_client *c = calloc(1, sizeof(*c));
    /* The trial client's protocol lines are not the app's business: the
     * running session's are. */
    FILE *sink = fopen("/dev/null", "w");
    enum zbx_cresult r;

    if (!c || !sink || zbx_client_init(c, cfg, tr, sink, now_ms, clock_user) != 0) {
        say(text, textlen, "out of memory", NULL);
        free(c);
        if (sink) {
            fclose(sink);
        }
        return ZBX_CRESULT_UNREACHABLE;
    }
    /* One authenticated read is the proof: the hosts can wait for the
     * session proper. */
    c->due_hosts_ms = INT64_MAX;
    zbx_client_start(c);
    zbx_client_step(c);
    switch (c->state) {
    case ZBX_CONN_ONLINE:
        r = ZBX_CRESULT_CONNECTED;
        snprintf(text, textlen, "Zabbix %s answered and accepted the %s", c->version,
                 cfg->auth == ZBX_AUTH_PASSWORD ? "user and password" : "API token");
        break;
    case ZBX_CONN_AUTH_FAILED:
        r = ZBX_CRESULT_AUTH_FAILED;
        zbx_copy_text(text, textlen, c->err_text[0] ? c->err_text : zbx_err_text(c->err));
        break;
    default:
        r = ZBX_CRESULT_UNREACHABLE;
        if (c->err_text[0] && strcmp(c->err_text, zbx_err_text(c->err)) != 0) {
            char both[2 * ZBX_TEXT_MAX];

            snprintf(both, sizeof(both), "%s: %s", zbx_err_text(c->err), c->err_text);
            zbx_copy_text(text, textlen, both);
        } else {
            zbx_copy_text(text, textlen, zbx_err_text(c->err));
        }
        break;
    }
    /* A password trial leaves no session behind on the server. */
    zbx_client_logout(c, ZBX_LOGOUT_TIMEOUT_MS);
    zbx_client_free(c);
    free(c);
    fclose(sink);
    return r;
}

static bool user_ok(const char *u)
{
    for (; *u; u++) {
        if ((unsigned char)*u < 0x20 || *u == 0x7f) {
            return false;
        }
    }
    return true;
}

enum zbx_cresult zbx_settings_run(bool save, const char *conf_path, const char *secret_path,
                                  const struct zbx_settings *want, const char *new_secret,
                                  struct zbx_transport *tr, int64_t (*now_ms)(void *user),
                                  void *clock_user, bool *saved, char *text, size_t textlen)
{
    static char old_text[ZBX_CONF_BYTES_MAX];
    static char new_text[ZBX_CONF_BYTES_MAX];
    struct zbx_settings s = *want;
    struct zbx_config cfg;
    char err[ZBX_TEXT_MAX] = "";
    bool typed = new_secret && *new_secret;
    enum zbx_cresult r;

    *saved = false;
    if (textlen) {
        text[0] = '\0';
    }
    /* What was typed, as it would be stored: no space around the address
     * or the user (a keyboard adds them easily), nothing that would break a
     * line of zabbix.conf. */
    trim_copy(s.url, sizeof(s.url), want->url, strlen(want->url));
    trim_copy(s.user, sizeof(s.user), want->user, strlen(want->user));
    if (!s.url[0]) {
        say(text, textlen, "Type the server's address, e.g. https://zabbix.example.com/", NULL);
        return ZBX_CRESULT_INVALID;
    }
    {
        char endpoint[ZBX_URL_MAX];
        char shown[ZBX_URL_MAX];
        bool https;

        if (zbx_config_url(s.url, endpoint, sizeof(endpoint), shown, sizeof(shown), &https, err,
                           sizeof(err)) != 0) {
            say(text, textlen, "Server address: %s", err);
            return ZBX_CRESULT_INVALID;
        }
    }
    if (!user_ok(s.user)) {
        say(text, textlen, "The user has a character it cannot have", NULL);
        return ZBX_CRESULT_INVALID;
    }
    if (s.auth == ZBX_AUTH_PASSWORD && !s.user[0]) {
        say(text, textlen, "Type the user: a password sign-in needs one", NULL);
        return ZBX_CRESULT_INVALID;
    }
    if (typed && zbx_config_check_secret(kind_of(s.auth), new_secret, err, sizeof(err)) != 0) {
        say(text, textlen, s.auth == ZBX_AUTH_PASSWORD ? "Password: %s" : "API token: %s", err);
        return ZBX_CRESULT_INVALID;
    }

    /* The configuration the change makes: the file as it would be written,
     * read back by the parser the helper starts with. */
    if (read_text(conf_path, old_text, sizeof(old_text)) != 0) {
        say(text, textlen, "zabbix.conf cannot be read: %s", strerror(errno));
        return ZBX_CRESULT_INVALID;
    }
    if (zbx_settings_compose(old_text, &s, new_text, sizeof(new_text), err, sizeof(err)) != 0) {
        say(text, textlen, "%s", err);
        return ZBX_CRESULT_INVALID;
    }
    zbx_config_defaults(&cfg);
    if (zbx_config_parse(&cfg, new_text, err, sizeof(err)) != 0 || !cfg.configured || cfg.fake) {
        say(text, textlen, "%s", err[0] ? err : "zabbix.conf would not be usable");
        return ZBX_CRESULT_INVALID;
    }
    if (typed) {
        snprintf(cfg.secret, sizeof(cfg.secret), "%s", new_secret);
        cfg.have_secret = true;
    } else if (zbx_config_read_secret(&cfg, secret_path, err, sizeof(err)) != 0) {
        say(text, textlen, s.auth == ZBX_AUTH_PASSWORD
                               ? "Type the password: none is stored for a password sign-in"
                               : "Type the API token: none is stored for a token sign-in", NULL);
        return ZBX_CRESULT_INVALID;
    }

    r = probe(&cfg, tr, now_ms, clock_user, text, textlen);
    zbx_config_forget_secret(&cfg);
    LOG_INFO("zabbix: settings %s: %s", save ? "save" : "test", zbx_cresult_word(r));
    if (!save || r != ZBX_CRESULT_CONNECTED) {
        return r;
    }
    if (commit(conf_path, secret_path, new_text, kind_of(s.auth), typed ? new_secret : NULL, err,
               sizeof(err)) != 0) {
        LOG_WARN("zabbix: settings not stored: %s", err);
        say(text, textlen, "Connected, but not stored: %s", err);
        return r;
    }
    *saved = true;
    LOG_INFO("zabbix: settings stored");
    return r;
}
