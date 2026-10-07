/*
 * The Zabbix viewer's configuration: the conf file, the URL rules, and the
 * secret - refused when others could read it, written atomically with the
 * right modes, never accepted in the world-readable file.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "zabbix/zbx_config.h"

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
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static int parse(struct zbx_config *c, const char *text, char *err, size_t len)
{
    zbx_config_defaults(c);
    return zbx_config_parse(c, text, err, len);
}

static void test_url(void)
{
    char ep[ZBX_URL_MAX];
    char shown[ZBX_URL_MAX];
    char err[ZBX_TEXT_MAX];
    bool https = false;

    check("a frontend directory gets api_jsonrpc.php",
          zbx_config_url("https://z.example.com/zabbix/", ep, sizeof(ep), shown, sizeof(shown), &https,
                         err, sizeof(err)) == 0 &&
              strcmp(ep, "https://z.example.com/zabbix/api_jsonrpc.php") == 0 &&
              strcmp(shown, "https://z.example.com/zabbix/") == 0 && https);
    check("without the trailing slash too",
          zbx_config_url("  https://z.example.com/zabbix  ", ep, sizeof(ep), shown, sizeof(shown), &https,
                         err, sizeof(err)) == 0 &&
              strcmp(ep, "https://z.example.com/zabbix/api_jsonrpc.php") == 0);
    check("a bare host",
          zbx_config_url("https://z.example.com", ep, sizeof(ep), shown, sizeof(shown), &https, err,
                         sizeof(err)) == 0 &&
              strcmp(ep, "https://z.example.com/api_jsonrpc.php") == 0 &&
              strcmp(shown, "https://z.example.com/") == 0);
    check("the endpoint itself is kept",
          zbx_config_url("http://10.0.0.5:8080/api_jsonrpc.php", ep, sizeof(ep), shown, sizeof(shown),
                         &https, err, sizeof(err)) == 0 &&
              strcmp(ep, "http://10.0.0.5:8080/api_jsonrpc.php") == 0 &&
              strcmp(shown, "http://10.0.0.5:8080/") == 0 && !https);
    check("user:password@ is refused",
          zbx_config_url("https://admin:zabbix@z.example.com/", ep, sizeof(ep), shown, sizeof(shown),
                         &https, err, sizeof(err)) != 0 &&
              strstr(err, "user"));
    check("an @ further along the path is not a userinfo",
          zbx_config_url("https://z.example.com/a@b/", ep, sizeof(ep), shown, sizeof(shown), &https,
                         err, sizeof(err)) == 0);
    check("ftp:// is refused", zbx_config_url("ftp://z/", ep, sizeof(ep), shown, sizeof(shown), &https, err,
                                              sizeof(err)) != 0);
    check("file:// is refused", zbx_config_url("file:///etc/passwd", ep, sizeof(ep), shown, sizeof(shown),
                                               &https, err, sizeof(err)) != 0);
    check("a query is refused", zbx_config_url("https://z/?token=abc", ep, sizeof(ep), shown, sizeof(shown),
                                               &https, err, sizeof(err)) != 0);
    check("a space inside is refused", zbx_config_url("https://z/za bbix", ep, sizeof(ep), shown,
                                                      sizeof(shown), &https, err, sizeof(err)) != 0);
    check("no host", zbx_config_url("https:///x", ep, sizeof(ep), shown, sizeof(shown), &https, err,
                                    sizeof(err)) != 0);
    check("empty", zbx_config_url("", ep, sizeof(ep), shown, sizeof(shown), &https, err, sizeof(err)) != 0);
}

static void test_parse(void)
{
    struct zbx_config c;
    char err[ZBX_TEXT_MAX];

    check("an empty file is not configured, and not an error",
          parse(&c, "", err, sizeof(err)) == 0 && !c.configured);
    check("defaults", c.refresh_s == 30 && c.hosts_s == 60 && c.timeout_s == 10 && c.verify &&
                          c.auth == ZBX_AUTH_TOKEN && !c.fake);
    check("a full file",
          parse(&c,
                "# Doors Zabbix viewer\n"
                "url = https://zabbix.example.com/zabbix/\n"
                "label=Production\n"
                "refresh_s=20\n"
                "hosts_s=120\n"
                "timeout_s=5\n"
                "verify_tls=1\n"
                "ca_file=/etc/pocketos/zabbix-ca.pem\n"
                "future_key=whatever\n",
                err, sizeof(err)) == 0 &&
              c.configured && c.https && strcmp(c.label, "Production") == 0 && c.refresh_s == 20 &&
              c.hosts_s == 120 && c.timeout_s == 5 &&
              strcmp(c.ca_file, "/etc/pocketos/zabbix-ca.pem") == 0);
    check("http:// is refused without allow_insecure_http",
          parse(&c, "url=http://z/\n", err, sizeof(err)) != 0 && !c.configured && strstr(err, "unencrypted"));
    check("and taken with it", parse(&c, "url=http://z/\nallow_insecure_http=1\n", err, sizeof(err)) == 0 &&
                                   c.configured && !c.https);
    check("a token in zabbix.conf is refused",
          parse(&c, "url=https://z/\ntoken=abcdef\n", err, sizeof(err)) != 0 && strstr(err, "set-secret"));
    check("so is a password", parse(&c, "url=https://z/\npassword=x\n", err, sizeof(err)) != 0);
    check("refresh below 15 s is refused", parse(&c, "url=https://z/\nrefresh_s=5\n", err, sizeof(err)) != 0);
    check("refresh above 600 s is refused", parse(&c, "url=https://z/\nrefresh_s=601\n", err, sizeof(err)) != 0);
    check("hosts below 30 s is refused", parse(&c, "url=https://z/\nhosts_s=10\n", err, sizeof(err)) != 0);
    check("timeout 0 is refused", parse(&c, "url=https://z/\ntimeout_s=0\n", err, sizeof(err)) != 0);
    check("verify_tls must be a boolean", parse(&c, "url=https://z/\nverify_tls=maybe\n", err, sizeof(err)) != 0);
    check("verify_tls=0 is taken (and shown as such)",
          parse(&c, "url=https://z/\nverify_tls=0\n", err, sizeof(err)) == 0 && !c.verify);
    check("auth=password needs a user", parse(&c, "url=https://z/\nauth=password\n", err, sizeof(err)) != 0);
    check("auth=password with a user", parse(&c, "url=https://z/\nauth=password\nuser=viewer\n", err,
                                             sizeof(err)) == 0 &&
                                           c.auth == ZBX_AUTH_PASSWORD && strcmp(c.user, "viewer") == 0);
    check("an unknown auth is refused", parse(&c, "url=https://z/\nauth=kerberos\n", err, sizeof(err)) != 0);
    check("a line without = is refused", parse(&c, "url https://z/\n", err, sizeof(err)) != 0);
    check("mode=fake needs no url", parse(&c, "mode=fake\nscenario=flap\n", err, sizeof(err)) == 0 &&
                                        c.configured && c.fake && strcmp(c.scenario, "flap") == 0);
    check("an unknown scenario is refused", parse(&c, "mode=fake\nscenario=nope\n", err, sizeof(err)) != 0);
    check("a user:password url is refused here too",
          parse(&c, "url=https://a:b@z/\n", err, sizeof(err)) != 0 && !c.configured);
}

static void test_secret(void)
{
    char dir[] = "/tmp/zbx_config_test.XXXXXX";
    char path[256];
    char conf[256];
    char link[256];
    char err[ZBX_TEXT_MAX];
    struct zbx_config c;
    struct stat st;
    FILE *f;

    if (!mkdtemp(dir)) {
        check("temporary directory", 0);
        return;
    }
    snprintf(path, sizeof(path), "%s/state/zabbix/secret", dir);
    snprintf(conf, sizeof(conf), "%s/zabbix.conf", dir);
    snprintf(link, sizeof(link), "%s/link", dir);

    check("a secret with a space is refused", zbx_config_write_secret(path, "token", "a b", err,
                                                                      sizeof(err)) != 0);
    check("a secret with a newline is refused", zbx_config_write_secret(path, "token", "ab\ncd", err,
                                                                        sizeof(err)) != 0);
    check("a kind that is not token or password is refused",
          zbx_config_write_secret(path, "cookie", "abc", err, sizeof(err)) != 0);
    check("a token is stored", zbx_config_write_secret(path, "token", "0123456789abcdef", err,
                                                       sizeof(err)) == 0);
    check("the file is 0600", stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    {
        char d[256];

        snprintf(d, sizeof(d), "%s/state/zabbix", dir);
        check("its directory is 0700", stat(d, &st) == 0 && (st.st_mode & 0777) == 0700);
    }
    f = fopen(conf, "w");
    fprintf(f, "url=https://z.example.com/\n");
    fclose(f);
    check("configuration and token load", zbx_config_load(&c, conf, path, err, sizeof(err)) == 0 &&
                                              c.configured && c.have_secret &&
                                              strcmp(c.secret, "0123456789abcdef") == 0);
    zbx_config_forget_secret(&c);
    check("forgetting wipes it", !c.have_secret && c.secret[0] == '\0' && c.secret[5] == '\0');

    chmod(path, 0644);
    check("a secret others can read is refused, and nothing is configured",
          zbx_config_load(&c, conf, path, err, sizeof(err)) != 0 && !c.configured && !c.have_secret &&
              strstr(err, "chmod"));
    chmod(path, 0600);
    if (symlink(path, link) == 0) {
        check("a secret reached through a symlink is refused",
              zbx_config_read_secret(&c, link, err, sizeof(err)) != 0);
    }
    check("the token is refused for auth=password", (zbx_config_defaults(&c), c.auth = ZBX_AUTH_PASSWORD,
                                                     zbx_config_read_secret(&c, path, err, sizeof(err)) != 0));
    check("a missing secret says how to set one", zbx_config_load(&c, conf, "/nonexistent/secret", err,
                                                                  sizeof(err)) != 0 &&
                                                      strstr(err, "set-secret"));
    check("a missing conf file is simply not configured",
          zbx_config_load(&c, "/nonexistent/zabbix.conf", path, err, sizeof(err)) == 0 && !c.configured);
    check("replacing the token", zbx_config_write_secret(path, "token", "fedcba9876543210", err,
                                                         sizeof(err)) == 0 &&
                                     zbx_config_load(&c, conf, path, err, sizeof(err)) == 0 &&
                                     strcmp(c.secret, "fedcba9876543210") == 0);
    {
        char tmp[300];

        snprintf(tmp, sizeof(tmp), "%s.new", path);
        check("no temporary file is left behind", access(tmp, F_OK) != 0);
    }
    /* Passwords are whatever the user chose, kept byte for byte. */
    {
        static const char *const pw[] = { " correct horse battery staple ", "bl\xc3\xa5" "b\xc3\xa6r\xc3\xb8",
                                          "p@ss=word#1", "a" };
        FILE *cf = fopen(conf, "w");
        size_t i;
        bool all = true;

        fprintf(cf, "url=https://z.example.com/\nauth=password\nuser=andre@example.com\n");
        fclose(cf);
        for (i = 0; i < sizeof(pw) / sizeof(pw[0]); i++) {
            if (zbx_config_write_secret(path, "password", pw[i], err, sizeof(err)) != 0 ||
                zbx_config_load(&c, conf, path, err, sizeof(err)) != 0 || strcmp(c.secret, pw[i]) != 0) {
                printf("     password %zu: \"%s\" came back as \"%s\" (%s)\n", i, pw[i], c.secret, err);
                all = false;
            }
        }
        check("a password with spaces (at its ends too), UTF-8, = or # comes back exactly", all);
        check("the user may be an e-mail address", strcmp(c.user, "andre@example.com") == 0);
        check("a password with a newline is refused",
              zbx_config_write_secret(path, "password", "two\nlines", err, sizeof(err)) != 0);
        check("a password with a CR is refused",
              zbx_config_write_secret(path, "password", "cr\rhere", err, sizeof(err)) != 0);
        check("a token is still one word", zbx_config_write_secret(path, "token", "to ken", err, sizeof(err)) != 0);
        cf = fopen(conf, "w");
        fprintf(cf, "url=https://z.example.com/\n");
        fclose(cf);
    }
    check("removing it", zbx_config_write_secret(path, "token", "", err, sizeof(err)) == 0 &&
                             access(path, F_OK) != 0);
    check("removing it twice is fine", zbx_config_write_secret(path, "token", "", err, sizeof(err)) == 0);
    {
        char cmd[400];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", dir);
        }
    }
}

int main(void)
{
    test_url();
    test_parse();
    test_secret();
    printf("zbx_config_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
