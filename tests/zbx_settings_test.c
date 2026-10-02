/*
 * The Zabbix connection changed on the unit (core/zabbix/zbx_settings.c):
 * what the CONNECTION screen is shown, what TEST and SAVE do with what it
 * sends, against the fake server, in temporary directories.
 *
 * What is proved: the files are read for the screen without the secret;
 * only url=, auth= and user= change and everything else in zabbix.conf is
 * kept; invalid settings are refused before anything is sent; a token and a
 * password connect, a refused one is AUTH FAILED after one login only, a
 * missing server is UNREACHABLE; SAVE stores (0644 conf, 0600 secret under
 * 0700) only what connected and the helper's own loader reads it back; a
 * save that fails - not connected, or a rename that fails half way - leaves
 * the previous files exactly as they were; an empty secret keeps the stored
 * one; no answer and no log line holds the secret.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketlog/pocketlog.h"
#include "zabbix/zbx_fake.h"
#include "zabbix/zbx_settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOKEN "tok-0123456789abcdef0123456789abcdef"
#define NEW_TOKEN "tok-fedcba9876543210fedcba9876543210"
#define PASSWORD "pa ss=wo#rd æøå "

static int checks;
static int failed;
static char dir[128];
static char conf[256];
static char secret[256];
static struct zbx_fake fake;
static struct zbx_transport tr;

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

static int64_t clk(void *user)
{
    static int64_t t = 1000;

    (void)user;
    return t += 10;
}

static void write_file(const char *path, const char *text, mode_t mode)
{
    FILE *f;

    unlink(path);
    f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
    chmod(path, mode);
}

/* The whole file, or "<none>" when it does not exist. */
static const char *slurp(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f) {
        snprintf(buf, len, "<none>");
        return buf;
    }
    n = fread(buf, 1, len - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static bool exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static mode_t mode_of(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? st.st_mode & 0777 : 0;
}

static void use_scenario(const char *name)
{
    zbx_fake_init(&fake, name);
    fake.realtime = false;
    zbx_transport_fake(&tr, &fake);
}

/* A unit set up by hand, the way docs/apps/ZABBIX.md §10 says. */
static void old_install(void)
{
    char sdir[300];

    write_file(conf, "# set up over SSH\n"
                     "url=https://old.example.com/zabbix/\n"
                     "label=Production\n"
                     "verify_tls=1\n"
                     "ca_file=/etc/pocketos/zabbix-ca.pem\n"
                     "refresh_s=45\n"
                     "future_key=kept\n",
               0644);
    snprintf(sdir, sizeof(sdir), "%s/zabbix", dir);
    mkdir(sdir, 0700);
    write_file(secret, "token=" TOKEN "\n", 0600);
}

static struct zbx_settings want(const char *url, enum zbx_auth auth, const char *user)
{
    struct zbx_settings s;

    memset(&s, 0, sizeof(s));
    snprintf(s.url, sizeof(s.url), "%s", url);
    s.auth = auth;
    snprintf(s.user, sizeof(s.user), "%s", user ? user : "");
    return s;
}

static enum zbx_cresult run(bool save, const struct zbx_settings *w, const char *new_secret,
                            bool *saved, char *text, size_t len)
{
    return zbx_settings_run(save, conf, secret, w, new_secret, &tr, clk, NULL, saved, text, len);
}

/* ---- reading -------------------------------------------------------------------- */

static void test_read(void)
{
    struct zbx_settings s;
    char note[ZBX_TEXT_MAX];

    unlink(conf);
    unlink(secret);
    zbx_settings_read(conf, secret, &s, note, sizeof(note));
    check("read: no files - nothing set, a token sign-in, nothing stored, no complaint",
          !s.url[0] && s.auth == ZBX_AUTH_TOKEN && !s.user[0] && !s.secret_stored && !note[0]);

    old_install();
    zbx_settings_read(conf, secret, &s, note, sizeof(note));
    check("read: an existing installation is shown as it is",
          strcmp(s.url, "https://old.example.com/zabbix/") == 0 && s.auth == ZBX_AUTH_TOKEN &&
              s.secret_stored && !note[0]);

    write_file(conf, "url=https://z.example.com\nauth=password\nuser=viewer\n", 0644);
    zbx_settings_read(conf, secret, &s, note, sizeof(note));
    check("read: a password sign-in whose stored secret is a token: not stored for it, and why",
          s.auth == ZBX_AUTH_PASSWORD && strcmp(s.user, "viewer") == 0 && !s.secret_stored &&
              strstr(note, "does not match") != NULL);
    check("read: the address is shown as the frontend", strcmp(s.url, "https://z.example.com/") == 0);

    write_file(conf, "url=https://bob:hunter2@z.example.com/\n", 0644);
    zbx_settings_read(conf, secret, &s, note, sizeof(note));
    check("read: an address with user:password@ in it is never shown", !s.url[0] && note[0] &&
                                                                      !strstr(note, "hunter2"));

    write_file(conf, "mode=fake\nurl=https://z.example.com/\n", 0644);
    zbx_settings_read(conf, secret, &s, note, sizeof(note));
    check("read: a demo configuration says so", strstr(note, "mode=fake") != NULL);

    write_file(conf, "url=https://z.example.com/\ntoken=" TOKEN "\n", 0644);
    zbx_settings_read(conf, secret, &s, note, sizeof(note));
    check("read: a token in zabbix.conf is a note, never a field",
          strstr(note, "must not hold") && !strstr(note, TOKEN) && !strstr(s.url, TOKEN) &&
              !strstr(s.user, TOKEN));
}

/* ---- composing ---------------------------------------------------------------------- */

static void test_compose(void)
{
    struct zbx_settings s = want("https://new.example.com/", ZBX_AUTH_PASSWORD, "viewer");
    char out[ZBX_CONF_BYTES_MAX];
    char err[ZBX_TEXT_MAX];
    struct zbx_config c;

    check("compose: works", zbx_settings_compose("# hand\nurl=https://old/\nlabel=Prod\nverify_tls=0\n"
                                                 "url=https://older/\nmode=fake\nscenario=auth\n"
                                                 "user=someone\nfuture_key=x\n",
                                                 &s, out, sizeof(out), err, sizeof(err)) == 0);
    if (strcmp(out, "# hand\nurl=https://new.example.com/\nlabel=Prod\nverify_tls=0\nscenario=auth\n"
                    "user=viewer\nfuture_key=x\nauth=password\n") != 0) {
        printf("     got:\n%s", out);
        check("compose: url, auth and user replaced where they were, once", 0);
    } else {
        check("compose: url, auth and user replaced where they were, once", 1);
    }
    zbx_config_defaults(&c);
    check("compose: and the parser reads it as a live server, the other keys as they were",
          zbx_config_parse(&c, out, err, sizeof(err)) == 0 && c.configured && !c.fake && !c.verify &&
              strcmp(c.label, "Prod") == 0 && c.auth == ZBX_AUTH_PASSWORD &&
              strcmp(c.user, "viewer") == 0);

    s = want("https://new.example.com/", ZBX_AUTH_TOKEN, "");
    zbx_settings_compose("url=https://old/\nuser=someone\nlabel=Prod", &s, out, sizeof(out), err,
                         sizeof(err));
    check("compose: no user drops the line; a last line without a newline gets one",
          strcmp(out, "url=https://new.example.com/\nlabel=Prod\nauth=token\n") == 0);

    zbx_settings_compose("", &s, out, sizeof(out), err, sizeof(err));
    check("compose: from nothing", strcmp(out, "url=https://new.example.com/\nauth=token\n") == 0);
    {
        static char big[ZBX_CONF_BYTES_MAX];

        memset(big, '#', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        check("compose: a file that would grow past the limit is refused",
              zbx_settings_compose(big, &s, out, sizeof(out), err, sizeof(err)) == -1 && !out[0]);
    }
}

/* ---- invalid --------------------------------------------------------------------------- */

static void test_invalid(void)
{
    static const struct {
        const char *what;
        const char *url;
        enum zbx_auth auth;
        const char *user;
        const char *secret;
        const char *says;
    } cases[] = {
        { "no address", "  ", ZBX_AUTH_TOKEN, "", NEW_TOKEN, "Type the server" },
        { "not http(s)", "ftp://z.example.com/", ZBX_AUTH_TOKEN, "", NEW_TOKEN, "https://" },
        { "credentials in the address", "https://a:b@z.example.com/", ZBX_AUTH_TOKEN, "", NEW_TOKEN,
          "user or password" },
        { "a query", "https://z.example.com/?x=1", ZBX_AUTH_TOKEN, "", NEW_TOKEN, "query" },
        { "plain http without allow_insecure_http", "http://z.example.com/", ZBX_AUTH_TOKEN, "",
          NEW_TOKEN, "unencrypted" },
        { "a password without a user", "https://z.example.com/", ZBX_AUTH_PASSWORD, "", "pw",
          "Type the user" },
        { "a user with a line break (a key smuggled into zabbix.conf)", "https://z.example.com/",
          ZBX_AUTH_PASSWORD, "demo\ntoken=x", "pw", "character" },
        { "a token with a space", "https://z.example.com/", ZBX_AUTH_TOKEN, "", "two words",
          "one word" },
        { "a password with a line break", "https://z.example.com/", ZBX_AUTH_PASSWORD, "demo",
          "a\nb", "one line" },
        { "a password sign-in with only a token stored", "https://z.example.com/", ZBX_AUTH_PASSWORD,
          "demo", NULL, "Type the password" },
    };
    char before_c[ZBX_CONF_BYTES_MAX];
    char before_s[512];
    char after[ZBX_CONF_BYTES_MAX];
    size_t i;

    old_install();
    slurp(conf, before_c, sizeof(before_c));
    slurp(secret, before_s, sizeof(before_s));
    use_scenario("demo");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct zbx_settings w = want(cases[i].url, cases[i].auth, cases[i].user);
        char text[ZBX_TEXT_MAX];
        char name[160];
        bool saved = true;
        unsigned before = fake.requests;
        enum zbx_cresult r = run(true, &w, cases[i].secret, &saved, text, sizeof(text));

        snprintf(name, sizeof(name), "invalid: %s - INVALID CONFIG, says why, sends nothing",
                 cases[i].what);
        check(name, r == ZBX_CRESULT_INVALID && !saved && strstr(text, cases[i].says) != NULL &&
                         fake.requests == before);
        if (r != ZBX_CRESULT_INVALID || !strstr(text, cases[i].says)) {
            printf("     %s: %s\n", zbx_cresult_word(r), text);
        }
    }
    check("invalid: zabbix.conf untouched", strcmp(slurp(conf, after, sizeof(after)), before_c) == 0);
    check("invalid: the secret untouched", strcmp(slurp(secret, after, sizeof(after)), before_s) == 0);

    /* http:// is fine where the file already allows it. */
    write_file(conf, "allow_insecure_http=1\n", 0644);
    {
        struct zbx_settings w = want("http://10.0.0.5:8080/", ZBX_AUTH_TOKEN, "");
        char text[ZBX_TEXT_MAX];
        bool saved;

        check("http:// with allow_insecure_http=1 kept from the file: tried",
              run(false, &w, NEW_TOKEN, &saved, text, sizeof(text)) == ZBX_CRESULT_CONNECTED);
    }
}

/* ---- trying -------------------------------------------------------------------------------- */

static void test_try(void)
{
    struct zbx_settings w;
    char text[ZBX_TEXT_MAX];
    char a[ZBX_CONF_BYTES_MAX];
    char b[ZBX_CONF_BYTES_MAX];
    bool saved;
    enum zbx_cresult r;

    old_install();
    slurp(conf, a, sizeof(a));

    use_scenario("demo");
    w = want("https://new.example.com/", ZBX_AUTH_TOKEN, "");
    r = run(false, &w, NEW_TOKEN, &saved, text, sizeof(text));
    check("test: a token on a good server is CONNECTED, and says which version",
          r == ZBX_CRESULT_CONNECTED && !saved && strstr(text, "7.0.31") && strstr(text, "API token"));
    check("test: it read with authentication (problem.get), not only the version",
          fake.requests >= 3);
    check("test: and stored nothing", strcmp(slurp(conf, b, sizeof(b)), a) == 0);

    use_scenario("demo");
    r = run(false, &w, NULL, &saved, text, sizeof(text));
    check("test: no new token uses the stored one", r == ZBX_CRESULT_CONNECTED);

    use_scenario("demo");
    w = want("https://new.example.com/", ZBX_AUTH_PASSWORD, ZBX_FAKE_USER);
    r = run(false, &w, PASSWORD, &saved, text, sizeof(text));
    check("test: a user and password are CONNECTED", r == ZBX_CRESULT_CONNECTED &&
                                                         strstr(text, "user and password"));
    check("test: and the session it opened is logged out", fake.logins == 1 && fake.logouts == 1 &&
                                                               !fake.session[0]);

    use_scenario("demo");
    w = want("https://new.example.com/", ZBX_AUTH_PASSWORD, "nobody");
    r = run(false, &w, "wrong", &saved, text, sizeof(text));
    check("auth: a refused password is AUTH FAILED with the server's words",
          r == ZBX_CRESULT_AUTH_FAILED && strstr(text, "Incorrect user name or password"));
    check("auth: after exactly one login (the lockout counts each)", fake.failed_logins == 1 &&
                                                                         fake.logins == 0);

    use_scenario("auth");
    w = want("https://new.example.com/", ZBX_AUTH_TOKEN, "");
    r = run(false, &w, NEW_TOKEN, &saved, text, sizeof(text));
    check("auth: a refused token is AUTH FAILED", r == ZBX_CRESULT_AUTH_FAILED &&
                                                     strstr(text, "Not authorized"));

    use_scenario("expired");
    r = run(false, &w, NEW_TOKEN, &saved, text, sizeof(text));
    check("auth: an expired token is AUTH FAILED", r == ZBX_CRESULT_AUTH_FAILED &&
                                                      strstr(text, "expired"));

    use_scenario("refused");
    r = run(false, &w, NEW_TOKEN, &saved, text, sizeof(text));
    check("unreachable: a refused connection", r == ZBX_CRESULT_UNREACHABLE &&
                                                   strstr(text, "Server not reachable"));
    use_scenario("dns");
    r = run(false, &w, NEW_TOKEN, &saved, text, sizeof(text));
    check("unreachable: a name that does not resolve", r == ZBX_CRESULT_UNREACHABLE &&
                                                           strstr(text, "name not found"));
    use_scenario("tls");
    r = run(false, &w, NEW_TOKEN, &saved, text, sizeof(text));
    check("unreachable: a TLS failure", r == ZBX_CRESULT_UNREACHABLE && strstr(text, "Secure"));
    use_scenario("http500");
    r = run(false, &w, NEW_TOKEN, &saved, text, sizeof(text));
    check("unreachable: an HTTP error (no API there)", r == ZBX_CRESULT_UNREACHABLE);
    check("and none of it stored anything", strcmp(slurp(conf, b, sizeof(b)), a) == 0);
}

/* ---- saving -------------------------------------------------------------------------------- */

static int fail_conf_rename(const char *from, const char *to)
{
    if (strcmp(to, conf) == 0) {
        errno = EIO;
        return -1;
    }
    return rename(from, to);
}

static void no_leftovers(const char *what)
{
    char p[300];
    char name[160];
    bool left = false;

    snprintf(p, sizeof(p), "%s.new", conf);
    left |= exists(p);
    snprintf(p, sizeof(p), "%s.new", secret);
    left |= exists(p);
    snprintf(p, sizeof(p), "%s.prev", secret);
    left |= exists(p);
    snprintf(name, sizeof(name), "%s: no .new or .prev file left behind", what);
    check(name, !left);
}

static void test_save(void)
{
    struct zbx_settings w;
    struct zbx_settings back;
    struct zbx_config loaded;
    char text[ZBX_TEXT_MAX];
    char err[ZBX_TEXT_MAX];
    char note[ZBX_TEXT_MAX];
    char a[ZBX_CONF_BYTES_MAX];
    char sa[512];
    char b[ZBX_CONF_BYTES_MAX];
    bool saved;
    enum zbx_cresult r;
    struct stat st1;
    struct stat st2;

    /* Not connected: nothing is written. */
    old_install();
    slurp(conf, a, sizeof(a));
    slurp(secret, sa, sizeof(sa));
    use_scenario("auth");
    w = want("https://new.example.com/", ZBX_AUTH_TOKEN, "");
    r = run(true, &w, NEW_TOKEN, &saved, text, sizeof(text));
    check("save: AUTH FAILED stores nothing", r == ZBX_CRESULT_AUTH_FAILED && !saved);
    use_scenario("refused");
    r = run(true, &w, NEW_TOKEN, &saved, text, sizeof(text));
    check("save: UNREACHABLE stores nothing", r == ZBX_CRESULT_UNREACHABLE && !saved);
    check("save: the previous configuration is exactly as it was (the fallback)",
          strcmp(slurp(conf, b, sizeof(b)), a) == 0 && strcmp(slurp(secret, b, sizeof(b)), sa) == 0);
    check("save: and still loads as it did", zbx_config_load(&loaded, conf, secret, err, sizeof(err)) == 0 &&
                                                 loaded.configured && strcmp(loaded.secret, TOKEN) == 0);
    zbx_config_forget_secret(&loaded);
    no_leftovers("save refused");

    /* Connected: stored, and read back the way the helper starts. */
    use_scenario("demo");
    w = want(" https://new.example.com/zabbix ", ZBX_AUTH_PASSWORD, " " ZBX_FAKE_USER " ");
    r = run(true, &w, PASSWORD, &saved, text, sizeof(text));
    check("save: CONNECTED is stored", r == ZBX_CRESULT_CONNECTED && saved);
    slurp(conf, b, sizeof(b));
    check("save: zabbix.conf has the new url, auth and user, trimmed",
          strstr(b, "\nurl=https://new.example.com/zabbix\n") && strstr(b, "\nauth=password\n") &&
              strstr(b, "\nuser=" ZBX_FAKE_USER "\n"));
    check("save: and kept everything else, comment and unknown key included",
          strstr(b, "# set up over SSH\n") && strstr(b, "label=Production\n") &&
              strstr(b, "ca_file=/etc/pocketos/zabbix-ca.pem\n") && strstr(b, "refresh_s=45\n") &&
              strstr(b, "future_key=kept\n"));
    check("save: no secret in zabbix.conf", !strstr(b, "password=") && !strstr(b, "pa ss"));
    check("save: zabbix.conf 0644, the secret 0600 in a 0700 directory",
          mode_of(conf) == 0644 && mode_of(secret) == 0600 &&
              (snprintf(b, sizeof(b), "%s/zabbix", dir), mode_of(b) == 0700));
    check("save: the secret is the password, byte for byte",
          strcmp(slurp(secret, b, sizeof(b)), "password=" PASSWORD "\n") == 0);
    check("save: the helper's own loader reads it back",
          zbx_config_load(&loaded, conf, secret, err, sizeof(err)) == 0 && loaded.configured &&
              loaded.auth == ZBX_AUTH_PASSWORD && strcmp(loaded.user, ZBX_FAKE_USER) == 0 &&
              strcmp(loaded.secret, PASSWORD) == 0 && loaded.refresh_s == 45 &&
              strcmp(loaded.url, "https://new.example.com/zabbix/api_jsonrpc.php") == 0);
    zbx_config_forget_secret(&loaded);
    zbx_settings_read(conf, secret, &back, note, sizeof(note));
    check("save: persisted - the screen reads the new settings, stored, without the secret",
          strcmp(back.url, "https://new.example.com/zabbix/") == 0 && back.auth == ZBX_AUTH_PASSWORD &&
              strcmp(back.user, ZBX_FAKE_USER) == 0 && back.secret_stored && !note[0]);
    no_leftovers("save");

    /* An empty secret keeps the stored one. */
    stat(secret, &st1);
    use_scenario("demo");
    w = want("https://other.example.com/", ZBX_AUTH_PASSWORD, ZBX_FAKE_USER);
    r = run(true, &w, "", &saved, text, sizeof(text));
    stat(secret, &st2);
    check("save: no new password keeps the stored one, untouched",
          r == ZBX_CRESULT_CONNECTED && saved && st1.st_ino == st2.st_ino &&
              strcmp(slurp(secret, b, sizeof(b)), "password=" PASSWORD "\n") == 0 &&
              strstr(slurp(conf, a, sizeof(a)), "url=https://other.example.com/\n"));

    /* Back to a token. */
    use_scenario("demo");
    w = want("https://other.example.com/", ZBX_AUTH_TOKEN, "");
    r = run(true, &w, NEW_TOKEN, &saved, text, sizeof(text));
    check("save: switching to a token stores it and drops the user line",
          saved && strcmp(slurp(secret, b, sizeof(b)), "token=" NEW_TOKEN "\n") == 0 &&
              strstr(slurp(conf, a, sizeof(a)), "auth=token\n") && !strstr(a, "user="));

    /* A rename that fails half way: the pair goes back to what it was. */
    slurp(conf, a, sizeof(a));
    slurp(secret, sa, sizeof(sa));
    zbx_settings_rename = fail_conf_rename;
    use_scenario("demo");
    w = want("https://third.example.com/", ZBX_AUTH_TOKEN, "");
    r = run(true, &w, TOKEN, &saved, text, sizeof(text));
    zbx_settings_rename = rename;
    check("rollback: connected but not stored, and it says so",
          r == ZBX_CRESULT_CONNECTED && !saved && strstr(text, "not stored"));
    check("rollback: zabbix.conf and the secret are the previous ones",
          strcmp(slurp(conf, b, sizeof(b)), a) == 0 && strcmp(slurp(secret, b, sizeof(b)), sa) == 0 &&
              mode_of(secret) == 0600);
    no_leftovers("rollback");

    /* The same with no secret before: the new one does not stay. */
    unlink(secret);
    zbx_settings_rename = fail_conf_rename;
    use_scenario("demo");
    r = run(true, &w, TOKEN, &saved, text, sizeof(text));
    zbx_settings_rename = rename;
    check("rollback: a secret that was not there before is removed again",
          !saved && !exists(secret) && strcmp(slurp(conf, b, sizeof(b)), a) == 0);
    no_leftovers("rollback without a secret");

    /* A first set-up, on a unit with no files at all. */
    unlink(conf);
    use_scenario("demo");
    w = want("https://first.example.com", ZBX_AUTH_TOKEN, "");
    r = run(true, &w, TOKEN, &saved, text, sizeof(text));
    check("save: a first set-up writes both files",
          saved && strcmp(slurp(conf, b, sizeof(b)), "url=https://first.example.com\nauth=token\n") == 0 &&
              strcmp(slurp(secret, b, sizeof(b)), "token=" TOKEN "\n") == 0);

    /* A demo file becomes a real one. */
    write_file(conf, "mode=fake\nscenario=large\nlabel=Desk\n", 0644);
    use_scenario("demo");
    r = run(true, &w, NULL, &saved, text, sizeof(text));
    check("save: mode=fake is dropped, the rest kept",
          saved && strcmp(slurp(conf, b, sizeof(b)),
                          "scenario=large\nlabel=Desk\nurl=https://first.example.com\nauth=token\n") == 0);
}

/* ---- the secret never comes back out ------------------------------------------------------- */

static void test_masking(void)
{
    static const char *const scen[] = { "demo", "auth", "refused", NULL };
    char logpath[300];
    char logtext[65536];
    char note[ZBX_TEXT_MAX];
    struct zbx_settings s;
    int i;
    bool clean = true;

    old_install();
    for (i = 0; scen[i]; i++) {
        struct zbx_settings w = want("https://z.example.com/", ZBX_AUTH_PASSWORD, ZBX_FAKE_USER);
        char text[ZBX_TEXT_MAX];
        bool saved;

        use_scenario(scen[i]);
        run(false, &w, PASSWORD, &saved, text, sizeof(text));
        clean = clean && !strstr(text, "pa ss") && !strstr(text, TOKEN);
        use_scenario(scen[i]);
        w.auth = ZBX_AUTH_TOKEN;
        run(true, &w, NEW_TOKEN, &saved, text, sizeof(text));
        clean = clean && !strstr(text, NEW_TOKEN) && !strstr(text, TOKEN);
    }
    check("masking: no answer to a test or save holds the secret", clean);
    zbx_settings_read(conf, secret, &s, note, sizeof(note));
    check("masking: what the screen is shown holds no secret",
          !strstr(s.url, "tok-") && !strstr(s.user, "tok-") && !strstr(note, "tok-") && s.secret_stored);
    snprintf(logpath, sizeof(logpath), "%s/zbx_settings_test.log", dir);
    slurp(logpath, logtext, sizeof(logtext));
    check("masking: the log has lines, and none holds a token or the password",
          strstr(logtext, "zabbix: settings") && !strstr(logtext, "tok-") && !strstr(logtext, "pa ss"));
}

int main(void)
{
    char cmd[300];

    snprintf(dir, sizeof(dir), "/tmp/zbx-settings-%ld", (long)getpid());
    mkdir(dir, 0755);
    snprintf(conf, sizeof(conf), "%s/zabbix.conf", dir);
    snprintf(secret, sizeof(secret), "%s/zabbix/secret", dir);
    setenv("POCKETOS_LOG_DIR", dir, 1);
    setenv("POCKETOS_LOG_LEVEL", "debug", 1);
    setenv("POCKETOS_LOG_STDERR", "0", 1);
    pocketlog_init("zbx_settings_test");
    umask(022);

    test_read();
    test_compose();
    test_invalid();
    test_try();
    test_save();
    test_masking();

    pocketlog_close();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", dir);
    }
    printf("zbx_settings_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
