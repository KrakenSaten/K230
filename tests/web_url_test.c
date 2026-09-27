/*
 * Browser addresses (core/web/web_url.h): what the address field turns
 * into an address, what it refuses and why, how links resolve against a
 * page, and that nothing but http(s) and the Browser's own about: pages is
 * ever let through.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "web/web_url.h"

#include <stdio.h>
#include <string.h>

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

/* typed -> the address it becomes, or NULL for "refused with want_err". */
static void typed(const char *in, const char *want, enum web_url_err want_err)
{
    struct web_url u;
    char out[WEB_URL_MAX];
    char name[WEB_URL_MAX * 3];
    enum web_url_err e = web_url_from_input(in, &u);

    if (want) {
        int ok = e == WEB_URL_OK && web_url_format(&u, out, sizeof(out)) > 0 && strcmp(out, want) == 0;

        snprintf(name, sizeof(name), "typed \"%s\" -> %s%s%s", in, want, ok ? "" : " (got ",
                 ok ? "" : (e == WEB_URL_OK ? out : web_url_err_text(e)));
        if (!ok) {
            strncat(name, ")", sizeof(name) - strlen(name) - 1);
        }
        check(name, ok);
    } else {
        snprintf(name, sizeof(name), "typed \"%s\" is refused: %s", in, web_url_err_text(want_err));
        check(name, e == want_err);
    }
}

static void resolves(const char *base, const char *ref, const char *want)
{
    struct web_url b;
    struct web_url u;
    char out[WEB_URL_MAX];
    char name[WEB_URL_MAX * 3];
    int ok = web_url_parse(base, &b) == WEB_URL_OK;

    ok = ok && web_url_resolve(&b, ref, &u) == WEB_URL_OK && web_url_format(&u, out, sizeof(out)) > 0 &&
         strcmp(out, want) == 0;
    snprintf(name, sizeof(name), "\"%s\" on %s -> %s", ref, base, want);
    check(name, ok);
    if (!ok) {
        printf("     got %s\n", out);
    }
}

static void refused_link(const char *base, const char *ref)
{
    struct web_url b;
    struct web_url u;
    char name[512];

    web_url_parse(base, &b);
    snprintf(name, sizeof(name), "a link to \"%s\" is never an address", ref);
    check(name, web_url_resolve(&b, ref, &u) != WEB_URL_OK);
}

int main(void)
{
    struct web_url u;
    char buf[WEB_URL_MAX + 64];
    char out[WEB_URL_MAX];

    /* ---- normalization of what is typed ------------------------------------- */
    typed("example.com", "https://example.com/", 0);
    typed("  Example.COM  ", "https://example.com/", 0);
    typed("example.com/a b", NULL, WEB_URL_SPACE);
    typed("http://example.com", "http://example.com/", 0);
    typed("HTTPS://Example.com:443/x", "https://example.com/x", 0);
    typed("http://example.com:80/", "http://example.com/", 0);
    typed("example.com:8080/admin", "https://example.com:8080/admin", 0);
    typed("localhost:8080", "https://localhost:8080/", 0);
    typed("192.168.1.1", "https://192.168.1.1/", 0);
    typed("http://192.168.1.1/cgi?x=1", "http://192.168.1.1/cgi?x=1", 0);
    typed("[::1]:8443/x", "https://[::1]:8443/x", 0);
    typed("example.com/a/./b/../c", "https://example.com/a/c", 0);
    typed("example.com/#frag", "https://example.com/", 0);
    typed("example.com?q=1", "https://example.com/?q=1", 0);
    typed("example.com.", "https://example.com/", 0);
    typed("about:home", "about:home", 0);
    typed("ABOUT:Blank", "about:blank", 0);
    typed("example.com/\xc3\xa6", "https://example.com/%C3%A6", 0);
    typed("exa\tmple.com", "https://example.com/", 0);

    /* ---- malformed and refused ---------------------------------------------- */
    typed("", NULL, WEB_URL_EMPTY);
    typed("   ", NULL, WEB_URL_EMPTY);
    typed("hello world", NULL, WEB_URL_SPACE);
    typed("javascript:alert(1)", NULL, WEB_URL_SCHEME);
    typed("JavaScript:alert(1)", NULL, WEB_URL_SCHEME);
    typed("file:///etc/shadow", NULL, WEB_URL_SCHEME);
    typed("data:text/html,<b>x</b>", NULL, WEB_URL_SCHEME);
    typed("ftp://example.com/", NULL, WEB_URL_SCHEME);
    typed("mailto:a@example.com", NULL, WEB_URL_SCHEME);
    typed("view-source:https://example.com", NULL, WEB_URL_SCHEME);
    typed("about:config", NULL, WEB_URL_ABOUT);
    typed("https://user:secret@example.com/", NULL, WEB_URL_USERINFO);
    typed("user@example.com", NULL, WEB_URL_USERINFO);
    typed("https://example.com:0/", NULL, WEB_URL_PORT);
    typed("https://example.com:65536/", NULL, WEB_URL_PORT);
    typed("https://example.com:12x/", NULL, WEB_URL_PORT);
    typed("https://exa$mple.com/", NULL, WEB_URL_HOST);
    typed("https://.example.com/", NULL, WEB_URL_HOST);
    typed("https://a..b/", NULL, WEB_URL_HOST);
    typed("https:///path", NULL, WEB_URL_HOST);
    typed("https:example.com", NULL, WEB_URL_HOST);
    typed("https://[zz::1]/", NULL, WEB_URL_HOST);
    typed("https://[::1/", NULL, WEB_URL_HOST);
    typed("https://b\xc3\xbc" "cher.example/", NULL, WEB_URL_IDN);
    typed("https://example.com/\x01x", NULL, WEB_URL_CHAR);
    typed("https://example.com/\x7fx", NULL, WEB_URL_CHAR);
    typed("https://example.com/\x01", "https://example.com/", 0); /* trailing controls are trimmed */

    memset(buf, 'a', sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    memcpy(buf, "https://example.com/", 20);
    check("an address longer than the limit is refused, not cut",
          web_url_from_input(buf, &u) == WEB_URL_TOO_LONG);
    memset(buf, 'a', sizeof(buf));
    memcpy(buf, "https://", 8);
    buf[8 + 300] = '\0';
    check("a host longer than 255 is refused", web_url_from_input(buf, &u) == WEB_URL_HOST);

    /* ---- resolving links ---------------------------------------------------- */
    resolves("https://example.com/a/b/c.html", "d.html", "https://example.com/a/b/d.html");
    resolves("https://example.com/a/b/c.html", "../d.html", "https://example.com/a/d.html");
    resolves("https://example.com/a/b/c.html", "../../../../d", "https://example.com/d");
    resolves("https://example.com/a/b/c.html", "/root", "https://example.com/root");
    resolves("https://example.com/a/b/c.html", "//cdn.example.org/x.png", "https://cdn.example.org/x.png");
    resolves("http://example.com/a/b/", "//cdn.example.org/x", "http://cdn.example.org/x");
    resolves("https://example.com/a/b/c.html?x=1", "?y=2", "https://example.com/a/b/c.html?y=2");
    resolves("https://example.com/a/b/c.html", "#top", "https://example.com/a/b/c.html");
    resolves("https://example.com/a/b/c.html", "", "https://example.com/a/b/c.html");
    resolves("https://example.com:8443/a/", "b", "https://example.com:8443/a/b");
    resolves("https://example.com/a/", "http://other.example/", "http://other.example/");
    resolves("https://example.com/a/", "  b c  ", "https://example.com/a/b%20c");
    resolves("https://example.com/a/", "./", "https://example.com/a/");
    resolves("https://example.com/a/b", "..", "https://example.com/");
    resolves("https://example.com/", "a\\b", "https://example.com/a/b");
    refused_link("https://example.com/", "javascript:alert(document.cookie)");
    refused_link("https://example.com/", "jAvAsCrIpT:alert(1)");
    refused_link("https://example.com/", "file:///etc/passwd");
    refused_link("https://example.com/", "data:text/html;base64,PGI+");
    refused_link("https://example.com/", "mailto:x@example.com");
    refused_link("https://example.com/", "tel:+4712345678");
    refused_link("https://example.com/", "https://user:pw@example.com/");
    refused_link("https://example.com/", "about:config");
    {
        struct web_url b;

        web_url_parse("about:home", &b);
        check("nothing relative resolves against about:home",
              web_url_resolve(&b, "x.html", &u) != WEB_URL_OK);
    }

    /* ---- the rest ------------------------------------------------------------ */
    web_url_parse("https://example.com:8443/secret?token=abc", &u);
    web_url_origin(&u, out, sizeof(out));
    check("the origin, which logs may carry, has no path and no query",
          strcmp(out, "https://example.com:8443") == 0);
    check("https is secure", web_url_is_secure(&u));
    web_url_parse("http://example.com/", &u);
    check("http is not", !web_url_is_secure(&u));
    check("every error has words", strlen(web_url_err_text(WEB_URL_SPACE)) > 10 &&
                                       strlen(web_url_err_text(WEB_URL_IDN)) > 10);

    printf("web_url_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
