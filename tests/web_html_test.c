/*
 * The Browser's HTML reader (core/web/web_html.h, web_doc.h): what an
 * ordinary page becomes, and that hostile or broken input - unterminated
 * markup, deep nesting, huge attributes, invalid UTF-8, NUL bytes, tag soup,
 * pages past every limit - is read in one bounded pass into a document whose
 * every index is in range. Run under ASan/UBSan by `make web-san-test`.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "web/web_html.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Every index a consumer follows is in range. */
static int sound(const struct web_doc *d)
{
    size_t i;

    if (d->text_len > WEB_DOC_TEXT_MAX || d->nblocks > WEB_DOC_BLOCK_MAX || d->nruns > WEB_DOC_RUN_MAX ||
        d->nlinks > WEB_DOC_LINK_MAX || d->nimages > WEB_DOC_IMAGE_MAX) {
        return 0;
    }
    for (i = 0; i < d->nblocks; i++) {
        const struct web_block *b = &d->blocks[i];

        if (b->type >= WEB_BLOCK_TYPE_COUNT || b->run_first + b->run_count > d->nruns ||
            (b->type == WEB_BLOCK_IMAGE && (b->image < 0 || (size_t)b->image >= d->nimages)) ||
            (b->type != WEB_BLOCK_IMAGE && b->image != -1) ||
            (b->type == WEB_BLOCK_HEADING ? b->level > 6 : b->level > WEB_INDENT_MAX)) {
            return 0;
        }
    }
    for (i = 0; i < d->nruns; i++) {
        const struct web_run *r = &d->runs[i];

        if (r->off + r->len > d->text_len || r->link < -1 || (r->link >= 0 && (size_t)r->link >= d->nlinks) ||
            r->len == 0) {
            return 0;
        }
    }
    for (i = 0; i < d->nlinks; i++) {
        if (d->links[i].off + d->links[i].len > d->text_len) {
            return 0;
        }
    }
    for (i = 0; i < d->nimages; i++) {
        if (d->images[i].src_off + d->images[i].src_len > d->text_len ||
            (d->images[i].link >= 0 && (size_t)d->images[i].link >= d->nlinks)) {
            return 0;
        }
    }
    return 1;
}

/* The document as text: one line per block, "{link text|url}" for links,
 * "<img alt>" for images, "---" for rules. */
static char *flat(const struct web_doc *d)
{
    size_t cap = d->text_len * 3 + 4096;
    char *out = malloc(cap);
    size_t n = 0;
    size_t i;

    for (i = 0; i < d->nblocks; i++) {
        const struct web_block *b = &d->blocks[i];
        size_t r;

        if (b->type == WEB_BLOCK_RULE) {
            n += (size_t)snprintf(out + n, cap - n, "---\n");
            continue;
        }
        if (b->type == WEB_BLOCK_IMAGE) {
            n += (size_t)snprintf(out + n, cap - n, "<img %s>\n", d->images[b->image].alt);
            continue;
        }
        n += (size_t)snprintf(out + n, cap - n, "%c%d:", "PHCQIRGN"[b->type], b->level);
        for (r = b->run_first; r < b->run_first + b->run_count; r++) {
            size_t len;
            const char *t = web_doc_run_text(d, r, &len);

            if (d->runs[r].link >= 0) {
                size_t ul;
                const char *u = web_doc_link_url(d, d->runs[r].link, &ul);

                n += (size_t)snprintf(out + n, cap - n, "{%.*s|%.*s%s}", (int)len, t, (int)ul, u,
                                      d->links[d->runs[r].link].supported ? "" : "!");
            } else {
                n += (size_t)snprintf(out + n, cap - n, "%.*s", (int)len, t);
            }
        }
        n += (size_t)snprintf(out + n, cap - n, "\n");
    }
    out[n] = '\0';
    return out;
}

static struct web_url page_url(const char *u)
{
    struct web_url p;

    web_url_parse(u, &p);
    return p;
}

static char *parse(const char *html, const char *charset, struct web_doc *d, struct web_html_info *info)
{
    struct web_url p = page_url("https://example.com/dir/page.html");

    web_doc_init(d);
    web_html_parse(html, strlen(html), &p, charset, d, info);
    return flat(d);
}

static void expect(const char *name, const char *html, const char *want)
{
    struct web_doc d;
    char *got = parse(html, NULL, &d, NULL);
    int ok = strcmp(got, want) == 0 && sound(&d);

    check(name, ok);
    if (!ok) {
        printf("     want: %s     got:  %s", want, got);
    }
    free(got);
    web_doc_free(&d);
}

static void contains(const char *name, const char *html, const char *charset, const char *want)
{
    struct web_doc d;
    char *got = parse(html, charset, &d, NULL);
    int ok = strstr(got, want) != NULL && sound(&d);

    check(name, ok);
    if (!ok) {
        printf("     want within: %s\n     got: %s", want, got);
    }
    free(got);
    web_doc_free(&d);
}

static void survives(const char *name, const char *buf, size_t n)
{
    struct web_doc d;
    struct web_url p = page_url("https://example.com/");

    web_doc_init(&d);
    web_html_parse(buf, n, &p, NULL, &d, NULL);
    check(name, sound(&d));
    web_doc_free(&d);
}

static uint32_t rng = 12345;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

int main(void)
{
    struct web_doc d;
    struct web_html_info info;
    char *got;
    size_t i;

    /* ---- an ordinary page ------------------------------------------------------- */
    expect("paragraphs, headings and whitespace",
           "<html><head><title> The  Title </title></head><body>\n<h1>Head</h1>\n<p>One\n  two"
           "</p><p>three</p></body></html>",
           "H1:Head\nP0:One two\nP0:three\n");
    got = parse("<title> The \n Title </title><p>x", NULL, &d, NULL);
    check("the title is collapsed and trimmed", strcmp(d.title, "The Title") == 0);
    free(got);
    web_doc_free(&d);
    expect("links resolve against the page",
           "<p>See <a href=\"other.html\">the other</a> and <a href='/root'>root</a>.</p>",
           "P0:See {the other|https://example.com/dir/other.html} and {root|https://example.com/root}.\n");
    expect("unsupported links are kept as words, never as addresses",
           "<p><a href=\"javascript:alert(1)\">run</a> <a href=\"mailto:a@b.c\">mail</a></p>",
           "P0:{run|javascript:!} {mail|mailto:!}\n");
    expect("lists with markers and depth",
           "<ul><li>a</li><li>b<ol><li>c</li><li>d</li></ol></li></ul>",
           "I1:\xe2\x80\xa2 a\nI1:\xe2\x80\xa2 b\nI2:1. c\nI2:2. d\n");
    expect("pre keeps its lines", "<pre>a  b\n  c</pre><p>x  y</p>", "C0:a  b\n  c\nP0:x y\n");
    expect("blockquote, rule, table",
           "<blockquote>q</blockquote><hr><table><tr><td>1</td><td>2</td></tr><tr><th>3</th></tr></table>",
           "Q1:q\n---\nP0:1 | 2\nP0:3\n");
    expect("br is a line break", "<p>a<br>b<br/>c</p>", "P0:a\nb\nc\n");
    expect("images: kept with alt, trackers and icons dropped",
           "<p>x<img src=\"a.jpg\" alt=\"A cat\" width=300>y</p><img src=t.gif width=1 height=1>"
           "<img src=i.png width=16 height=16 alt=\"icon\"><img src=pic.svg alt=vector>",
           "P0:x\n<img A cat>\nP0:y\nP0:[icon][vector]\n");
    got = parse("<img src=\"//cdn.example.org/p.png?x=1\" alt=a width=200>", NULL, &d, NULL);
    check("an image keeps its resolved address for the helper",
          d.nimages == 1 && strncmp(d.text + d.images[0].src_off, "https://cdn.example.org/p.png?x=1",
                                    d.images[0].src_len) == 0);
    free(got);
    web_doc_free(&d);
    expect("inline flags", "<p>a <code>b</code> <b>c</b> <em>d</em></p>", "P0:a b c d\n");
    got = parse("<p>a <code>b</code> <b>c</b> <em>d</em></p>", NULL, &d, NULL);
    check("code, strong and em are run flags",
          d.nruns == 6 && d.runs[1].flags == WEB_RUN_CODE && d.runs[3].flags == WEB_RUN_STRONG &&
              d.runs[5].flags == WEB_RUN_EM);
    free(got);
    web_doc_free(&d);

    /* ---- what is not shown ------------------------------------------------------ */
    expect("script and style are skipped, noscript is shown",
           "<script>document.write('<p>no</p>')</script><style>p{color:red}</style>"
           "<noscript><p>shown</p></noscript><p>ok</p>",
           "P0:shown\nP0:ok\n");
    expect("a script cannot end itself early with a string",
           "<script>var s = \"<p>no</p>\";</script><p>ok</p>", "P0:ok\n");
    expect("hidden, aria-hidden, display:none",
           "<div hidden><p>a</p></div><span aria-hidden=\"true\">b</span>"
           "<div style=\"color: red; display : none\"><div>c</div></div><p>d</p>",
           "P0:d\n");
    expect("svg, video, select, button are skipped",
           "<svg><text>s</text><svg>t</svg></svg><video>v</video><select><option>o</select>"
           "<button>b</button><p>ok</p>",
           "P0:ok\n");
    expect("head text is not the page", "<head><meta charset=utf-8><link rel=x>junk</head><p>ok</p>",
           "P0:ok\n");
    expect("a form becomes a note, once per page",
           "<form action=/s><input name=q><button>Go</button></form><p>after</p><form><input></form>",
           "N0:This page has forms. Forms are not supported in this version of Browser.\nP0:after\n");
    expect("meta refresh is shown as a link, not followed",
           "<meta http-equiv=\"refresh\" content=\"0; URL='/next'\"><p>x</p>",
           "N0:This page sends you on to: {https://example.com/next|https://example.com/next}\nP0:x\n");
    expect("an iframe becomes a link", "<iframe src=\"/frame\">fallback</iframe><p>x</p>",
           "N0:Embedded page: {https://example.com/frame|https://example.com/frame}\nP0:x\n");
    expect("base href moves where links point",
           "<base href=\"https://other.example/x/\"><a href=y>y</a>",
           "P0:{y|https://other.example/x/y}\n");
    expect("comments, doctype and processing instructions", "<!DOCTYPE html><!-- c <p>no</p> --><?xml x?><p>ok</p>",
           "P0:ok\n");

    /* ---- characters ------------------------------------------------------------- */
    expect("entities", "<p>&amp;&lt;&gt;&quot;&#65;&#x42;&eacute;&copy;&hellip;&amp x &bogus; &#0; &#x110000;</p>",
           "P0:&<>\"AB\xc3\xa9\xc2\xa9\xe2\x80\xa6& x &bogus; ? ?\n");
    expect("a split reference is not a reference", "<p>AT&T &#</p>", "P0:AT&T &#\n");
    contains("windows-1252 declared in a meta", "<meta charset=windows-1252><p>caf\xe9 \x93q\x94</p>", NULL,
             "caf\xc3\xa9 \xe2\x80\x9cq\xe2\x80\x9d");
    contains("the header charset wins", "<meta charset=utf-8><p>\xe9</p>", "ISO-8859-1", "P0:\xc3\xa9");
    contains("undeclared invalid UTF-8 is read as windows-1252", "<p>M\xf8te</p>", NULL, "M\xc3\xb8te");
    contains("undeclared valid UTF-8 stays UTF-8", "<p>M\xc3\xb8te</p>", NULL, "M\xc3\xb8te");
    got = parse("<meta charset=shift_jis><p>x</p>", NULL, &d, &info);
    check("an unsupported charset is said, in a note",
          info.charset == WEB_CHARSET_UNSUPPORTED && strstr(got, "shift_jis") != NULL);
    free(got);
    web_doc_free(&d);
    contains("broken UTF-8 in a declared UTF-8 page becomes '?'", "<meta charset=utf-8><p>a\xc3(b\xed\xa0\x80</p>",
             NULL, "a?(b???");
    contains("control characters and zero-width spaces are dropped", "<p>a\x01\x1b" "b\xe2\x80\x8b" "c</p>",
             NULL, "P0:abc");

    /* ---- hostile and broken input -------------------------------------------------- */
#define SURVIVES(name, lit) survives(name, lit, sizeof(lit) - 1)
    SURVIVES("an empty page", "");
    SURVIVES("a lone <", "<");
    SURVIVES("an unterminated comment", "<p>a<!-- never");
    SURVIVES("an unterminated tag", "<p>a<a href='x");
    SURVIVES("an unterminated attribute", "<a href=\"https://example.com/");
    SURVIVES("an unterminated script", "<script>for(;;){}");
    SURVIVES("an unterminated title", "<title>abc");
    SURVIVES("an end tag at the very end", "<p>a</");
    SURVIVES("a reference at the very end", "<p>a&#x");
    SURVIVES("NUL bytes", "<p>a\0b<\0a href=\0>c</a>");
    {
        size_t n = 3u * 1024u * 1024u;
        char *big = malloc(n + 1);

        for (i = 0; i < n; i += 4) {
            memcpy(big + i, "<p>x", 4);
        }
        big[n] = '\0';
        web_doc_init(&d);
        {
            struct web_url p = page_url("https://example.com/");

            web_html_parse(big, n, &p, NULL, &d, NULL);
        }
        check("3 MB of paragraphs: cut at the block limit and marked truncated",
              d.truncated && d.nblocks == WEB_DOC_BLOCK_MAX && sound(&d));
        web_doc_free(&d);

        for (i = 0; i + 5 <= n; i += 5) {
            memcpy(big + i, "<div>", 5);
        }
        survives("a million nested divs", big, n - (n % 5));

        memset(big, 'x', n);
        memcpy(big, "<a href=\"", 9);
        survives("a 3 MB attribute value", big, n);

        memset(big, 'y', n);
        memcpy(big, "<p>", 3);
        web_doc_init(&d);
        {
            struct web_url p = page_url("https://example.com/");

            web_html_parse(big, n, &p, NULL, &d, NULL);
        }
        check("3 MB of text: cut at the text limit", d.truncated && d.text_len < WEB_DOC_TEXT_MAX && sound(&d));
        web_doc_free(&d);

        n = 0;
        for (i = 0; i < 2000; i++) {
            n += (size_t)sprintf(big + n, "<a href=\"/l%zu\">l%zu</a> ", i, i);
        }
        web_doc_init(&d);
        {
            struct web_url p = page_url("https://example.com/");

            web_html_parse(big, n, &p, NULL, &d, NULL);
        }
        check("2000 links: 500 addresses kept, the text of the rest stays", d.nlinks == WEB_DOC_LINK_MAX &&
                                                                              strstr(d.text, "l1999") && sound(&d));
        web_doc_free(&d);

        n = 0;
        for (i = 0; i < 100; i++) {
            n += (size_t)sprintf(big + n, "<img src=\"/i%zu.jpg\" alt=\"i%zu\" width=100 height=100>", i, i);
        }
        web_doc_init(&d);
        {
            struct web_url p = page_url("https://example.com/");

            web_html_parse(big, n, &p, NULL, &d, NULL);
        }
        check("100 images: 16 kept, the rest as their words", d.nimages == WEB_DOC_IMAGE_MAX &&
                                                                strstr(d.text, "[i99]") && sound(&d));
        web_doc_free(&d);

        /* Tag soup: random pieces of markup, many times over. */
        {
            static const char *const pieces[] = {
                "<p>", "</p>", "<a href=\"x\">", "</a>", "<li>", "<ul>", "</ul>", "<ol>", "</ol>",
                "<pre>", "</pre>", "<h2>", "</h2>", "<img src=a.png width=100>", "<br>", "<hr>",
                "<blockquote>", "</blockquote>", "<table><tr><td>", "</td><td>", "<script>", "</script>",
                "<style>", "<!--", "-->", "<", ">", "&", "&amp;", "&#", "&#x1F600;", "\xc3", "\xa9",
                "\xf0\x9f\x98\x80", "\"", "'", "=", " ", "\n", "text", "<title>", "</title>", "<svg>",
                "</svg>", "<div hidden>", "</div>", "<form>", "<meta charset=latin1>", "<base href=/b/>",
                "<code>", "</code>", "<b>", "<textarea>", "<iframe src=f>", "</iframe>", "\0",
            };
            size_t np = sizeof(pieces) / sizeof(pieces[0]);
            int round;
            int bad = 0;

            for (round = 0; round < 3000; round++) {
                size_t len = 0;
                size_t want = rnd() % 4000;
                struct web_url p = page_url("https://example.com/a/b");

                while (len < want && len + 64 < n) {
                    const char *pc = pieces[rnd() % np];
                    size_t pl = pc[0] ? strlen(pc) : 1;

                    memcpy(big + len, pc, pl);
                    len += pl;
                }
                web_doc_init(&d);
                web_html_parse(big, len, &p, NULL, &d, NULL);
                bad += !sound(&d);
                web_doc_free(&d);
            }
            check("3000 rounds of tag soup: every document sound", bad == 0);
            bad = 0;
            for (round = 0; round < 2000; round++) {
                size_t len = rnd() % 3000;
                struct web_url p = page_url("https://example.com/");

                for (i = 0; i < len; i++) {
                    big[i] = (char)(rnd() & 0xff);
                }
                web_doc_init(&d);
                web_html_parse(big, len, &p, NULL, &d, NULL);
                bad += !sound(&d);
                web_doc_free(&d);
            }
            check("2000 rounds of random bytes: every document sound", bad == 0);
        }
        free(big);
    }

    /* ---- text/plain -------------------------------------------------------------------- */
    web_doc_init(&d);
    web_text_parse("line one\nline  two\n\n\npara <b>two</b>\r\n", 37, NULL, &d);
    got = flat(&d);
    check("plain text: preformatted paragraphs, markup left alone",
          strcmp(got, "C0:line one\nline  two\nC0:para <b>two</b>\n") == 0 && sound(&d));
    free(got);
    web_doc_free(&d);

    check("charset names", web_charset_from_name("UTF-8", 5) == WEB_CHARSET_UTF8 &&
                               web_charset_from_name("Windows-1252", 12) == WEB_CHARSET_LATIN1 &&
                               web_charset_from_name("koi8-r", 6) == WEB_CHARSET_UNSUPPORTED);

    printf("web_html_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
