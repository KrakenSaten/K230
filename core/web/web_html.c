/*
 * HTML into a web_doc. See web_html.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "web/web_html.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG_MAX 16
#define ATTR_NAME_MAX 24
#define ATTR_URL_MAX WEB_URL_MAX
#define ATTR_SHORT_MAX 256
/* Text is decoded in pieces of this many input bytes; one byte of
 * windows-1252 can become three of UTF-8. */
#define CHUNK 4096
#define CHUNK_OUT (CHUNK * 3 + 16)
#define LIST_MAX 8

struct attrs {
    char href[ATTR_URL_MAX];
    char src[ATTR_URL_MAX];
    char data_src[ATTR_URL_MAX];
    char content[ATTR_URL_MAX];
    char alt[ATTR_SHORT_MAX];
    char width[16];
    char height[16];
    char charset[32];
    char http_equiv[32];
    char style[ATTR_SHORT_MAX];
    char aria_hidden[8];
    bool hidden;
    bool has_href;
};

struct parser {
    const char *s;
    size_t n;
    size_t i;
    struct web_doc *d;
    struct web_html_info *info;
    struct web_url page;
    struct web_url base;
    bool base_set;
    enum web_charset cs;

    /* where the text goes */
    int link;
    int code;
    int strong;
    int em;
    int pre;
    int quote;
    int list;
    bool ordered[LIST_MAX];
    int counter[LIST_MAX];
    int heading;            /* the open h1..h6, 0 when none */
    int cells;              /* table cells so far in this row */
    bool in_head;
    bool space_pending;
    bool line_start;

    /* a skipped element: its name and how deep inside it we are */
    char skip_tag[TAG_MAX];
    int skip_depth;

    char tag[TAG_MAX];
    bool closing;
    bool self_closing;
    struct attrs a;
    char out[CHUNK_OUT];
    char col[CHUNK_OUT];
};

/* ---- characters ---------------------------------------------------------------- */

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static bool is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_alnum(int c)
{
    return is_alpha(c) || (c >= '0' && c <= '9');
}

static bool is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static size_t put_utf8(char *o, unsigned cp)
{
    if (cp == 0 || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) {
        o[0] = '?';
        return 1;
    }
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xc0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    o[0] = (char)(0xf0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* windows-1252 0x80..0x9f; the rest of the upper half is Latin-1. */
static const unsigned short cp1252[32] = {
    0x20ac, 0x81, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x8d, 0x017d, 0x8f, 0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
    0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x9d, 0x017e, 0x0178,
};

static unsigned latin1_cp(unsigned char c)
{
    return (c >= 0x80 && c <= 0x9f) ? cp1252[c - 0x80] : c;
}

struct entity {
    const char *name;
    unsigned short cp;
};

/* The named references pages actually use: markup, typography and the
 * Latin-1 letters. Anything else stays as written. */
static const struct entity entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
    { "nbsp", 0xa0 }, { "iexcl", 0xa1 }, { "cent", 0xa2 }, { "pound", 0xa3 }, { "curren", 0xa4 },
    { "yen", 0xa5 }, { "brvbar", 0xa6 }, { "sect", 0xa7 }, { "uml", 0xa8 }, { "copy", 0xa9 },
    { "ordf", 0xaa }, { "laquo", 0xab }, { "not", 0xac }, { "shy", 0xad }, { "reg", 0xae },
    { "macr", 0xaf }, { "deg", 0xb0 }, { "plusmn", 0xb1 }, { "sup2", 0xb2 }, { "sup3", 0xb3 },
    { "acute", 0xb4 }, { "micro", 0xb5 }, { "para", 0xb6 }, { "middot", 0xb7 }, { "cedil", 0xb8 },
    { "sup1", 0xb9 }, { "ordm", 0xba }, { "raquo", 0xbb }, { "frac14", 0xbc }, { "frac12", 0xbd },
    { "frac34", 0xbe }, { "iquest", 0xbf }, { "Agrave", 0xc0 }, { "Aacute", 0xc1 }, { "Acirc", 0xc2 },
    { "Atilde", 0xc3 }, { "Auml", 0xc4 }, { "Aring", 0xc5 }, { "AElig", 0xc6 }, { "Ccedil", 0xc7 },
    { "Egrave", 0xc8 }, { "Eacute", 0xc9 }, { "Ecirc", 0xca }, { "Euml", 0xcb }, { "Igrave", 0xcc },
    { "Iacute", 0xcd }, { "Icirc", 0xce }, { "Iuml", 0xcf }, { "ETH", 0xd0 }, { "Ntilde", 0xd1 },
    { "Ograve", 0xd2 }, { "Oacute", 0xd3 }, { "Ocirc", 0xd4 }, { "Otilde", 0xd5 }, { "Ouml", 0xd6 },
    { "times", 0xd7 }, { "Oslash", 0xd8 }, { "Ugrave", 0xd9 }, { "Uacute", 0xda }, { "Ucirc", 0xdb },
    { "Uuml", 0xdc }, { "Yacute", 0xdd }, { "THORN", 0xde }, { "szlig", 0xdf }, { "agrave", 0xe0 },
    { "aacute", 0xe1 }, { "acirc", 0xe2 }, { "atilde", 0xe3 }, { "auml", 0xe4 }, { "aring", 0xe5 },
    { "aelig", 0xe6 }, { "ccedil", 0xe7 }, { "egrave", 0xe8 }, { "eacute", 0xe9 }, { "ecirc", 0xea },
    { "euml", 0xeb }, { "igrave", 0xec }, { "iacute", 0xed }, { "icirc", 0xee }, { "iuml", 0xef },
    { "eth", 0xf0 }, { "ntilde", 0xf1 }, { "ograve", 0xf2 }, { "oacute", 0xf3 }, { "ocirc", 0xf4 },
    { "otilde", 0xf5 }, { "ouml", 0xf6 }, { "divide", 0xf7 }, { "oslash", 0xf8 }, { "ugrave", 0xf9 },
    { "uacute", 0xfa }, { "ucirc", 0xfb }, { "uuml", 0xfc }, { "yacute", 0xfd }, { "thorn", 0xfe },
    { "yuml", 0xff }, { "OElig", 0x152 }, { "oelig", 0x153 }, { "Scaron", 0x160 }, { "scaron", 0x161 },
    { "Yuml", 0x178 }, { "fnof", 0x192 }, { "circ", 0x2c6 }, { "tilde", 0x2dc }, { "ensp", 0x2002 },
    { "emsp", 0x2003 }, { "thinsp", 0x2009 }, { "zwnj", 0x200c }, { "zwj", 0x200d }, { "lrm", 0x200e },
    { "rlm", 0x200f }, { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "sbquo", 0x201a }, { "ldquo", 0x201c }, { "rdquo", 0x201d }, { "bdquo", 0x201e },
    { "dagger", 0x2020 }, { "Dagger", 0x2021 }, { "bull", 0x2022 }, { "hellip", 0x2026 },
    { "permil", 0x2030 }, { "prime", 0x2032 }, { "Prime", 0x2033 }, { "lsaquo", 0x2039 },
    { "rsaquo", 0x203a }, { "euro", 0x20ac }, { "trade", 0x2122 }, { "larr", 0x2190 },
    { "uarr", 0x2191 }, { "rarr", 0x2192 }, { "darr", 0x2193 }, { "harr", 0x2194 },
    { "minus", 0x2212 }, { "le", 0x2264 }, { "ge", 0x2265 }, { "ne", 0x2260 },
};

/* The references accepted without their ';', as browsers do. */
static bool legacy_entity(const char *name)
{
    return strcmp(name, "amp") == 0 || strcmp(name, "lt") == 0 || strcmp(name, "gt") == 0 ||
           strcmp(name, "quot") == 0 || strcmp(name, "nbsp") == 0 || strcmp(name, "copy") == 0;
}

/* Decode the reference at s[0] == '&'. Returns the input bytes it used (0: not
 * a reference, copy the '&'), with the code point in *cp. */
static size_t entity_at(const char *s, size_t n, unsigned *cp)
{
    size_t i = 1;

    if (n > 2 && s[1] == '#') {
        unsigned long v = 0;
        bool hex = n > 3 && (s[2] == 'x' || s[2] == 'X');
        size_t digits = 0;

        i = hex ? 3 : 2;
        for (; i < n && digits < 8; i++, digits++) {
            int c = (unsigned char)s[i];

            if (c >= '0' && c <= '9') {
                v = v * (hex ? 16 : 10) + (unsigned long)(c - '0');
            } else if (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                v = v * 16 + (unsigned long)(lower(c) - 'a' + 10);
            } else {
                break;
            }
        }
        if (digits == 0) {
            return 0;
        }
        while (i < n && digits >= 8 && is_alnum((unsigned char)s[i])) {
            i++; /* far too many digits: swallow them, the value is invalid */
            v = 0x110000;
        }
        if (i < n && s[i] == ';') {
            i++;
        }
        if (v >= 0x80 && v <= 0x9f) {
            v = cp1252[v - 0x80];
        }
        *cp = v > 0x10ffff ? 0xfffd : (unsigned)v;
        return i;
    }
    {
        char name[12];
        size_t k = 0;
        size_t e;

        while (i < n && k < sizeof(name) - 1 && is_alnum((unsigned char)s[i])) {
            name[k++] = s[i++];
        }
        name[k] = '\0';
        if (k == 0) {
            return 0;
        }
        for (e = 0; e < sizeof(entities) / sizeof(entities[0]); e++) {
            if (strcmp(entities[e].name, name) == 0) {
                if (i < n && s[i] == ';') {
                    *cp = entities[e].cp;
                    return i + 1;
                }
                if (legacy_entity(name) && !(i < n && is_alnum((unsigned char)s[i]))) {
                    *cp = entities[e].cp;
                    return i;
                }
                return 0;
            }
        }
    }
    return 0;
}

/* in[0..n) in the page's charset, with references, into UTF-8 at out
 * (capacity at least 3n + 4). Returns the bytes written. */
static size_t decode(const char *in, size_t n, enum web_charset cs, char *out)
{
    size_t i = 0;
    size_t o = 0;

    while (i < n) {
        unsigned char c = (unsigned char)in[i];

        if (c == '&') {
            unsigned cp = 0;
            size_t used = entity_at(in + i, n - i, &cp);

            if (used) {
                o += put_utf8(out + o, cp == 0xfffd ? '?' : cp);
                i += used;
                continue;
            }
        }
        if (c >= 0x80 && cs == WEB_CHARSET_LATIN1) {
            o += put_utf8(out + o, latin1_cp(c));
        } else if (c == 0) {
            out[o++] = '?';
        } else {
            out[o++] = (char)c;
        }
        i++;
    }
    return o;
}

/* An attribute value, decoded and made valid UTF-8, into out (cap bytes). */
static void decode_attr(struct parser *p, const char *in, char *out, size_t cap)
{
    size_t n = strlen(in);
    size_t w;

    if (n > CHUNK) {
        n = CHUNK;
    }
    w = decode(in, n, p->cs, p->out);
    w = web_utf8_sanitize(p->out, w, p->col, false);
    if (w >= cap) {
        w = cap - 1;
        while (w > 0 && ((unsigned char)p->col[w] & 0xc0) == 0x80) {
            w--;
        }
    }
    memcpy(out, p->col, w);
    out[w] = '\0';
}

/* ---- charset ------------------------------------------------------------------- */

enum web_charset web_charset_from_name(const char *name, size_t n)
{
    static const char *const utf8[] = { "utf-8", "utf8", "unicode-1-1-utf-8" };
    static const char *const latin[] = { "iso-8859-1", "iso8859-1", "latin1", "l1", "us-ascii",
                                         "ascii", "windows-1252", "cp1252", "iso-8859-15",
                                         "iso8859-15", "x-cp1252", "cp819", "ibm819" };
    char low[32];
    size_t i;

    if (n == 0 || n >= sizeof(low)) {
        return n == 0 ? WEB_CHARSET_UTF8 : WEB_CHARSET_UNSUPPORTED;
    }
    for (i = 0; i < n; i++) {
        low[i] = (char)lower((unsigned char)name[i]);
    }
    low[n] = '\0';
    for (i = 0; i < sizeof(utf8) / sizeof(utf8[0]); i++) {
        if (strcmp(low, utf8[i]) == 0) {
            return WEB_CHARSET_UTF8;
        }
    }
    for (i = 0; i < sizeof(latin) / sizeof(latin[0]); i++) {
        if (strcmp(low, latin[i]) == 0) {
            return WEB_CHARSET_LATIN1;
        }
    }
    return WEB_CHARSET_UNSUPPORTED;
}

/* "charset=NAME" somewhere in s: the name's start and length. */
static bool find_charset(const char *s, size_t n, const char **name, size_t *len)
{
    size_t i;

    for (i = 0; i + 7 < n; i++) {
        size_t k;
        size_t j;

        for (k = 0; k < 7 && lower((unsigned char)s[i + k]) == "charset"[k]; k++) {
        }
        if (k < 7) {
            continue;
        }
        j = i + 7;
        while (j < n && is_space((unsigned char)s[j])) {
            j++;
        }
        if (j >= n || s[j] != '=') {
            continue;
        }
        j++;
        while (j < n && (is_space((unsigned char)s[j]) || s[j] == '"' || s[j] == '\'')) {
            j++;
        }
        k = j;
        while (k < n && (is_alnum((unsigned char)s[k]) || s[k] == '-' || s[k] == '_' ||
                         s[k] == ':' || s[k] == '.')) {
            k++;
        }
        if (k > j) {
            *name = s + j;
            *len = k - j;
            return true;
        }
    }
    return false;
}

/* Whether s is valid UTF-8; a sequence cut off by the end of s is not held
 * against it (s may be the first part of a page). */
static bool valid_utf8_prefix(const char *s, size_t n)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0;

    while (i < n) {
        unsigned c = u[i];
        size_t len;
        size_t k;

        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            len = 2;
        } else if (c >= 0xe0 && c <= 0xef) {
            len = 3;
        } else if (c >= 0xf0 && c <= 0xf4) {
            len = 4;
        } else {
            return false;
        }
        for (k = 1; k < len; k++) {
            if (i + k >= n) {
                return true;
            }
            if ((u[i + k] & 0xc0) != 0x80) {
                return false;
            }
        }
        i += len;
    }
    return true;
}

static void choose_charset(struct parser *p, const char *header)
{
    const char *name = NULL;
    size_t len = 0;
    bool declared = false;

    p->cs = WEB_CHARSET_UTF8;
    if (p->n >= 3 && (unsigned char)p->s[0] == 0xef && (unsigned char)p->s[1] == 0xbb &&
        (unsigned char)p->s[2] == 0xbf) {
        p->i = 3;
        snprintf(p->info->charset_name, sizeof(p->info->charset_name), "utf-8");
        return;
    }
    if (p->n >= 2 && (((unsigned char)p->s[0] == 0xff && (unsigned char)p->s[1] == 0xfe) ||
                      ((unsigned char)p->s[0] == 0xfe && (unsigned char)p->s[1] == 0xff))) {
        name = "utf-16";
        len = 6;
        declared = true;
    } else if (header && *header) {
        name = header;
        len = strlen(header);
        declared = true;
    } else if (find_charset(p->s, p->n < 1024 ? p->n : 1024, &name, &len)) {
        declared = true;
    }
    if (declared) {
        size_t k;

        if (len >= sizeof(p->info->charset_name)) {
            len = sizeof(p->info->charset_name) - 1;
        }
        for (k = 0; k < len; k++) {
            p->info->charset_name[k] = (char)lower((unsigned char)name[k]);
        }
        p->info->charset_name[len] = '\0';
        p->cs = web_charset_from_name(name, len);
        return;
    }
    /* Nothing said: UTF-8 when the start of the page is valid UTF-8, the
     * Western default otherwise. */
    if (!valid_utf8_prefix(p->s, p->n < 65536 ? p->n : 65536)) {
        p->cs = WEB_CHARSET_LATIN1;
    }
}

/* ---- the document ---------------------------------------------------------------- */

static int indent(const struct parser *p)
{
    int v = p->list + p->quote;

    return v > WEB_INDENT_MAX ? WEB_INDENT_MAX : v;
}

static unsigned flags(const struct parser *p)
{
    return (p->code > 0 ? WEB_RUN_CODE : 0) | (p->strong > 0 ? WEB_RUN_STRONG : 0) |
           (p->em > 0 ? WEB_RUN_EM : 0);
}

static void block_break(struct parser *p)
{
    web_doc_end_block(p->d);
    p->space_pending = false;
    p->line_start = true;
}

static void begin(struct parser *p, enum web_block_type type, int level)
{
    web_doc_begin_block(p->d, type, level);
    p->space_pending = false;
    p->line_start = true;
}

/* A block for text that arrives with none open: what the context says. */
static void ensure_block(struct parser *p)
{
    if (p->d->open_block) {
        return;
    }
    if (p->pre > 0) {
        begin(p, WEB_BLOCK_PRE, indent(p));
    } else if (p->heading > 0) {
        begin(p, WEB_BLOCK_HEADING, p->heading);
    } else if (p->quote > 0) {
        begin(p, WEB_BLOCK_QUOTE, indent(p));
    } else {
        begin(p, WEB_BLOCK_PARA, indent(p));
    }
}

static void add(struct parser *p, const char *s, size_t n, int link)
{
    ensure_block(p);
    web_doc_add_text(p->d, s, n, flags(p), link);
}

/* Decoded text, whitespace collapsed unless in pre, into the document. */
static void text_utf8(struct parser *p, const char *s, size_t n)
{
    size_t i;
    size_t o = 0;

    if (p->skip_depth > 0 || p->in_head) {
        return;
    }
    if (p->pre > 0) {
        if (n) {
            add(p, s, n, p->link);
            p->line_start = false;
        }
        return;
    }
    for (i = 0; i < n && is_space((unsigned char)s[i]); i++) {
    }
    if (i < n) {
        /* Before the words are collapsed: opening a block starts a line. */
        ensure_block(p);
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (is_space(c)) {
            p->space_pending = true;
            continue;
        }
        if (p->space_pending && !p->line_start) {
            if (o == 0) {
                /* The space between two pieces belongs to neither: plain,
                 * so a link's underline does not start with it. */
                web_doc_add_text(p->d, " ", 1, 0, -1);
            } else {
                p->col[o++] = ' ';
            }
        }
        p->space_pending = false;
        p->line_start = false;
        p->col[o++] = (char)c;
    }
    if (o) {
        add(p, p->col, o, p->link);
    }
}

static void text_raw(struct parser *p, const char *s, size_t n)
{
    while (n > 0 && !p->d->truncated) {
        size_t take = n > CHUNK ? CHUNK : n;
        size_t w;

        if (take < n) {
            size_t k;

            /* Never split a character or a reference between pieces. */
            while (take > 1 && ((unsigned char)s[take] & 0xc0) == 0x80) {
                take--;
            }
            for (k = take > 12 ? take - 12 : 0; k < take; k++) {
                if (s[k] == '&') {
                    if (!memchr(s + k, ';', take - k) && k > 0) {
                        take = k;
                    }
                    break;
                }
            }
        }
        w = decode(s, take, p->cs, p->out);
        text_utf8(p, p->out, w);
        s += take;
        n -= take;
    }
}

static void note(struct parser *p, const char *words, const char *url)
{
    int link = -1;

    begin(p, WEB_BLOCK_NOTE, 0);
    web_doc_add_text(p->d, words, strlen(words), 0, -1);
    if (url) {
        struct web_url u;

        if (web_url_resolve(&p->base, url, &u) == WEB_URL_OK) {
            char abs[WEB_URL_MAX];

            if (web_url_format(&u, abs, sizeof(abs)) > 0) {
                link = web_doc_add_link(p->d, abs, true);
                web_doc_add_text(p->d, " ", 1, 0, -1);
                web_doc_add_text(p->d, abs, strlen(abs), 0, link);
            }
        }
    }
    block_break(p);
}

/* ---- tags ---------------------------------------------------------------------- */

static bool tag_is(const struct parser *p, const char *name)
{
    return strcmp(p->tag, name) == 0;
}

static bool tag_in(const struct parser *p, const char *const *names)
{
    for (; *names; names++) {
        if (strcmp(p->tag, *names) == 0) {
            return true;
        }
    }
    return false;
}

static const char *const block_tags[] = {
    "address", "article", "aside", "body", "center", "details", "dialog", "dir", "div", "dl",
    "fieldset", "figcaption", "figure", "footer", "header", "hgroup", "main", "menu", "nav",
    "p", "section", "summary", "table", "tbody", "thead", "tfoot", "caption", "legend", "dt",
    "html", "noscript", "search", NULL
};

/* Elements whose content is never shown. */
static const char *const skipped_tags[] = {
    "svg", "math", "select", "button", "video", "audio", "canvas", "object", "map", "datalist",
    "template", "option", "applet", NULL
};

/* Elements with no content and no end tag. */
static const char *const void_tags[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
    "source", "track", "wbr", "keygen", NULL
};

static int number(const char *s)
{
    long v = 0;
    int digits = 0;

    while (*s == ' ') {
        s++;
    }
    while (*s >= '0' && *s <= '9' && digits < 6) {
        v = v * 10 + (*s++ - '0');
        digits++;
    }
    return digits ? (int)v : 0;
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    size_t i;

    for (i = 0; hay[i]; i++) {
        size_t k;

        for (k = 0; k < n && hay[i + k] && lower((unsigned char)hay[i + k]) == needle[k]; k++) {
        }
        if (k == n) {
            return true;
        }
    }
    return false;
}

static bool hidden_element(const struct parser *p)
{
    char style[ATTR_SHORT_MAX];
    size_t i;
    size_t o = 0;

    if (p->a.hidden || strcmp(p->a.aria_hidden, "true") == 0) {
        return true;
    }
    for (i = 0; p->a.style[i] && o + 1 < sizeof(style); i++) {
        if (!is_space((unsigned char)p->a.style[i])) {
            style[o++] = p->a.style[i];
        }
    }
    style[o] = '\0';
    return contains_ci(style, "display:none") || contains_ci(style, "visibility:hidden");
}

static void start_skip(struct parser *p)
{
    web_copy(p->skip_tag, sizeof(p->skip_tag), p->tag);
    p->skip_depth = 1;
}

static void open_link(struct parser *p)
{
    char href[ATTR_URL_MAX];
    struct web_url u;
    enum web_url_err e;

    p->link = -1;
    if (!p->a.has_href) {
        return;
    }
    decode_attr(p, p->a.href, href, sizeof(href));
    e = web_url_resolve(&p->base, href, &u);
    if (e == WEB_URL_OK) {
        char abs[WEB_URL_MAX];

        if (u.scheme == WEB_SCHEME_ABOUT) {
            /* A page cannot point into the Browser's own pages. */
            p->link = web_doc_add_link(p->d, "about:", false);
        } else if (web_url_format(&u, abs, sizeof(abs)) > 0) {
            p->link = web_doc_add_link(p->d, abs, true);
        }
    } else if (href[0] && href[0] != '#') {
        /* mailto:, javascript: and the rest: kept to say what they are, and
         * never opened. Only the scheme is worth keeping. */
        char what[40];
        size_t k = 0;

        while (href[k] && href[k] != ':' && k < sizeof(what) - 2) {
            what[k] = href[k];
            k++;
        }
        what[k] = href[k] == ':' ? ':' : '\0';
        what[k + (href[k] == ':')] = '\0';
        p->link = web_doc_add_link(p->d, what, false);
    }
}

static bool image_src_usable(const char *src)
{
    size_t n = strlen(src);
    const char *q = strchr(src, '?');
    size_t pn = q ? (size_t)(q - src) : n;

    if (!src[0] || strncmp(src, "data:", 5) == 0) {
        return false;
    }
    /* Formats the helper cannot decode: say so in words, fetch nothing. */
    if (pn >= 4 && (contains_ci(src + pn - 4, ".svg") || contains_ci(src + pn - 4, ".gif"))) {
        return false;
    }
    if (pn >= 5 && contains_ci(src + pn - 5, ".webp")) {
        return false;
    }
    return true;
}

static void image(struct parser *p)
{
    char src[ATTR_URL_MAX];
    char alt[ATTR_SHORT_MAX];
    int w = number(p->a.width);
    int h = number(p->a.height);
    struct web_url u;
    char abs[WEB_URL_MAX];
    bool usable;

    decode_attr(p, p->a.src[0] && strncmp(p->a.src, "data:", 5) != 0 ? p->a.src : p->a.data_src,
                src, sizeof(src));
    decode_attr(p, p->a.alt, alt, sizeof(alt));
    if ((w > 0 && w <= 2) || (h > 0 && h <= 2)) {
        return; /* a tracking pixel */
    }
    usable = image_src_usable(src) && web_url_resolve(&p->base, src, &u) == WEB_URL_OK &&
             u.scheme != WEB_SCHEME_ABOUT && web_url_format(&u, abs, sizeof(abs)) > 0;
    if (usable && !(w > 0 && w < 40 && h > 0 && h < 40) && p->d->nimages < WEB_DOC_IMAGE_MAX) {
        int link = p->link;

        if (web_doc_add_image(p->d, abs, alt, link, w, h) >= 0) {
            p->line_start = true;
            p->space_pending = false;
            return;
        }
    }
    /* An icon, a format that cannot be shown, or no room: its words. */
    if (alt[0]) {
        char words[ATTR_SHORT_MAX + 4];
        int n = snprintf(words, sizeof(words), "[%s]", alt);

        ensure_block(p);
        if (p->space_pending && !p->line_start) {
            add(p, " ", 1, p->link);
        }
        add(p, words, n < 0 ? 0 : (size_t)n >= sizeof(words) ? sizeof(words) - 1 : (size_t)n, p->link);
        p->space_pending = false;
        p->line_start = false;
    }
}

static void meta(struct parser *p)
{
    char content[ATTR_URL_MAX];
    const char *s;

    if (!contains_ci(p->a.http_equiv, "refresh") || p->info->has_refresh) {
        return;
    }
    decode_attr(p, p->a.content, content, sizeof(content));
    s = strchr(content, ';');
    if (!s) {
        s = strchr(content, ',');
    }
    if (!s) {
        return;
    }
    s++;
    while (is_space((unsigned char)*s)) {
        s++;
    }
    if (lower((unsigned char)s[0]) == 'u' && lower((unsigned char)s[1]) == 'r' &&
        lower((unsigned char)s[2]) == 'l') {
        s += 3;
        while (is_space((unsigned char)*s)) {
            s++;
        }
        if (*s == '=') {
            s++;
        }
        while (is_space((unsigned char)*s)) {
            s++;
        }
    }
    if (*s == '"' || *s == '\'') {
        char q = *s++;
        char *end = strchr(s, q);

        if (end) {
            *end = '\0';
        }
    }
    if (*s) {
        p->info->has_refresh = true;
        note(p, "This page sends you on to:", s);
    }
}

static void list_item(struct parser *p)
{
    char marker[16];
    int depth = p->list < 1 ? 1 : p->list;
    int slot = depth > LIST_MAX ? LIST_MAX - 1 : depth - 1;

    begin(p, WEB_BLOCK_ITEM, depth > WEB_INDENT_MAX ? WEB_INDENT_MAX : depth);
    if (p->list > 0 && p->ordered[slot]) {
        snprintf(marker, sizeof(marker), "%d. ", ++p->counter[slot]);
    } else {
        snprintf(marker, sizeof(marker), "\xe2\x80\xa2 ");
    }
    web_doc_add_text(p->d, marker, strlen(marker), 0, -1);
    web_doc_mark_prefix(p->d);
    p->line_start = true;
}

static void start_tag(struct parser *p)
{
    static const char *const code_tags[] = { "code", "kbd", "samp", "tt", "var", NULL };
    static const char *const strong_tags[] = { "b", "strong", NULL };
    static const char *const em_tags[] = { "i", "em", "cite", "dfn", NULL };

    if (p->skip_depth > 0) {
        if (tag_is(p, p->skip_tag) && !p->self_closing) {
            p->skip_depth++;
        }
        return;
    }
    if (hidden_element(p)) {
        if (!tag_in(p, void_tags) && !p->self_closing) {
            start_skip(p);
        }
        return;
    }
    if (p->in_head && !tag_is(p, "title") && !tag_is(p, "meta") && !tag_is(p, "link") &&
        !tag_is(p, "base") && !tag_is(p, "style") && !tag_is(p, "script") &&
        !tag_is(p, "noscript") && !tag_is(p, "template")) {
        p->in_head = false; /* content: the head has ended, whether it said so or not */
    }
    if (tag_is(p, "head")) {
        p->in_head = true;
    } else if (tag_is(p, "body")) {
        p->in_head = false;
        block_break(p);
    } else if (tag_in(p, skipped_tags)) {
        if (!p->self_closing) {
            start_skip(p);
        }
    } else if (tag_is(p, "meta")) {
        meta(p);
    } else if (tag_is(p, "base")) {
        if (!p->base_set && p->a.has_href) {
            char href[ATTR_URL_MAX];
            struct web_url u;

            decode_attr(p, p->a.href, href, sizeof(href));
            if (web_url_resolve(&p->page, href, &u) == WEB_URL_OK && u.scheme != WEB_SCHEME_ABOUT) {
                p->base = u;
                p->base_set = true;
            }
        }
    } else if (tag_is(p, "a")) {
        open_link(p);
    } else if (tag_is(p, "img")) {
        image(p);
    } else if (tag_is(p, "br")) {
        if (!p->in_head) {
            add(p, "\n", 1, p->link);
            p->line_start = true;
            p->space_pending = false;
        }
    } else if (tag_is(p, "hr")) {
        block_break(p);
        begin(p, WEB_BLOCK_RULE, 0);
        block_break(p);
    } else if (p->tag[0] == 'h' && p->tag[1] >= '1' && p->tag[1] <= '6' && !p->tag[2]) {
        p->heading = p->tag[1] - '0';
        begin(p, WEB_BLOCK_HEADING, p->heading);
    } else if (tag_is(p, "ul") || tag_is(p, "ol")) {
        block_break(p);
        if (p->list < 64) {
            p->list++;
            if (p->list <= LIST_MAX) {
                p->ordered[p->list - 1] = tag_is(p, "ol");
                p->counter[p->list - 1] = 0;
            }
        }
    } else if (tag_is(p, "li")) {
        list_item(p);
    } else if (tag_is(p, "dd")) {
        begin(p, WEB_BLOCK_PARA, indent(p) + 1 > WEB_INDENT_MAX ? WEB_INDENT_MAX : indent(p) + 1);
    } else if (tag_is(p, "pre") || tag_is(p, "listing") || tag_is(p, "xmp")) {
        if (p->pre < 64) {
            p->pre++;
        }
        begin(p, WEB_BLOCK_PRE, indent(p));
    } else if (tag_is(p, "blockquote")) {
        if (p->quote < 64) {
            p->quote++;
        }
        begin(p, WEB_BLOCK_QUOTE, indent(p));
    } else if (tag_is(p, "tr")) {
        block_break(p);
        p->cells = 0;
    } else if (tag_is(p, "td") || tag_is(p, "th")) {
        if (p->cells++ > 0 && p->d->open_block) {
            add(p, " | ", 3, -1);
            p->line_start = true;
        }
    } else if (tag_is(p, "form")) {
        /* Said once, where the first form is: a note per form is noise on a
         * page with a search box in every corner. */
        if (p->info->forms++ == 0) {
            note(p, "This page has forms. Forms are not supported in this version of Browser.", NULL);
        }
    } else if (tag_is(p, "iframe") || tag_is(p, "frame")) {
        char src[ATTR_URL_MAX];

        decode_attr(p, p->a.src, src, sizeof(src));
        if (src[0] && strncmp(src, "about:", 6) != 0 && strncmp(src, "javascript:", 11) != 0) {
            note(p, "Embedded page:", src);
        }
    } else if (tag_in(p, code_tags)) {
        p->code++;
    } else if (tag_in(p, strong_tags)) {
        p->strong++;
    } else if (tag_in(p, em_tags)) {
        p->em++;
    } else if (tag_in(p, block_tags)) {
        block_break(p);
    }
}

static void end_tag(struct parser *p)
{
    static const char *const code_tags[] = { "code", "kbd", "samp", "tt", "var", NULL };
    static const char *const strong_tags[] = { "b", "strong", NULL };
    static const char *const em_tags[] = { "i", "em", "cite", "dfn", NULL };

    if (p->skip_depth > 0) {
        if (tag_is(p, p->skip_tag)) {
            p->skip_depth--;
        }
        return;
    }
    if (tag_is(p, "head")) {
        p->in_head = false;
    } else if (tag_is(p, "a")) {
        p->link = -1;
    } else if (p->tag[0] == 'h' && p->tag[1] >= '1' && p->tag[1] <= '6' && !p->tag[2]) {
        p->heading = 0;
        block_break(p);
    } else if (tag_is(p, "ul") || tag_is(p, "ol")) {
        block_break(p);
        if (p->list > 0) {
            p->list--;
        }
    } else if (tag_is(p, "li") || tag_is(p, "dd") || tag_is(p, "tr")) {
        block_break(p);
    } else if (tag_is(p, "pre") || tag_is(p, "listing") || tag_is(p, "xmp")) {
        if (p->pre > 0) {
            p->pre--;
        }
        block_break(p);
    } else if (tag_is(p, "blockquote")) {
        if (p->quote > 0) {
            p->quote--;
        }
        block_break(p);
    } else if (tag_in(p, code_tags)) {
        p->code -= p->code > 0;
    } else if (tag_in(p, strong_tags)) {
        p->strong -= p->strong > 0;
    } else if (tag_in(p, em_tags)) {
        p->em -= p->em > 0;
    } else if (tag_in(p, block_tags) || tag_is(p, "form")) {
        block_break(p);
    }
}

/* ---- the tokenizer ------------------------------------------------------------- */

static void copy_attr(char *dst, size_t cap, const char *v, size_t n)
{
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, v, n);
    dst[n] = '\0';
}

static void keep_attr(struct parser *p, const char *name, const char *v, size_t n)
{
    struct attrs *a = &p->a;

    if (strcmp(name, "href") == 0) {
        copy_attr(a->href, sizeof(a->href), v, n);
        a->has_href = true;
    } else if (strcmp(name, "src") == 0) {
        copy_attr(a->src, sizeof(a->src), v, n);
    } else if (strcmp(name, "data-src") == 0) {
        copy_attr(a->data_src, sizeof(a->data_src), v, n);
    } else if (strcmp(name, "content") == 0) {
        copy_attr(a->content, sizeof(a->content), v, n);
    } else if (strcmp(name, "alt") == 0) {
        copy_attr(a->alt, sizeof(a->alt), v, n);
    } else if (strcmp(name, "width") == 0) {
        copy_attr(a->width, sizeof(a->width), v, n);
    } else if (strcmp(name, "height") == 0) {
        copy_attr(a->height, sizeof(a->height), v, n);
    } else if (strcmp(name, "charset") == 0) {
        copy_attr(a->charset, sizeof(a->charset), v, n);
    } else if (strcmp(name, "http-equiv") == 0) {
        copy_attr(a->http_equiv, sizeof(a->http_equiv), v, n);
    } else if (strcmp(name, "style") == 0) {
        copy_attr(a->style, sizeof(a->style), v, n);
    } else if (strcmp(name, "aria-hidden") == 0) {
        copy_attr(a->aria_hidden, sizeof(a->aria_hidden), v, n);
    } else if (strcmp(name, "hidden") == 0) {
        a->hidden = true;
    }
}

static void clear_attrs(struct attrs *a)
{
    a->href[0] = a->src[0] = a->data_src[0] = a->content[0] = a->alt[0] = '\0';
    a->width[0] = a->height[0] = a->charset[0] = a->http_equiv[0] = '\0';
    a->style[0] = a->aria_hidden[0] = '\0';
    a->hidden = false;
    a->has_href = false;
}

/* The tag name at s[*i]; *i moves past it. */
static void read_name(struct parser *p, size_t *i)
{
    size_t k = 0;

    while (*i < p->n) {
        unsigned char c = (unsigned char)p->s[*i];

        if (is_space(c) || c == '>' || c == '/' || c == 0) {
            break;
        }
        if (k < TAG_MAX - 1) {
            p->tag[k++] = (char)lower(c);
        }
        (*i)++;
    }
    p->tag[k] = '\0';
}

/* Parse the start tag at s[i] == '<' (a letter follows). */
static void read_start_tag(struct parser *p)
{
    size_t i = p->i + 1;

    clear_attrs(&p->a);
    p->self_closing = false;
    read_name(p, &i);
    for (;;) {
        char name[ATTR_NAME_MAX];
        size_t nk = 0;
        const char *v = "";
        size_t vn = 0;

        while (i < p->n && (is_space((unsigned char)p->s[i]) || p->s[i] == '/')) {
            if (p->s[i] == '/' && i + 1 < p->n && p->s[i + 1] == '>') {
                p->self_closing = true;
            }
            i++;
        }
        if (i >= p->n) {
            break;
        }
        if (p->s[i] == '>') {
            i++;
            break;
        }
        while (i < p->n) {
            unsigned char c = (unsigned char)p->s[i];

            if (is_space(c) || c == '=' || c == '>' || (c == '/' && nk > 0)) {
                break;
            }
            if (nk < sizeof(name) - 1) {
                name[nk++] = (char)lower(c);
            }
            i++;
        }
        name[nk] = '\0';
        while (i < p->n && is_space((unsigned char)p->s[i])) {
            i++;
        }
        if (i < p->n && p->s[i] == '=') {
            i++;
            while (i < p->n && is_space((unsigned char)p->s[i])) {
                i++;
            }
            if (i < p->n && (p->s[i] == '"' || p->s[i] == '\'')) {
                char q = p->s[i++];
                const char *end = memchr(p->s + i, q, p->n - i);

                v = p->s + i;
                vn = end ? (size_t)(end - v) : p->n - i;
                i += vn + (end ? 1 : 0);
            } else {
                v = p->s + i;
                while (i < p->n && !is_space((unsigned char)p->s[i]) && p->s[i] != '>') {
                    i++;
                }
                vn = (size_t)(p->s + i - v);
            }
        }
        if (nk > 0) {
            keep_attr(p, name, v, vn);
        }
    }
    p->i = i;
}

/* From s[from], the index of "</name" (any case), or n. */
static size_t find_end_tag(const struct parser *p, size_t from, const char *name)
{
    size_t nl = strlen(name);
    size_t i = from;

    while (i < p->n) {
        const char *lt = memchr(p->s + i, '<', p->n - i);
        size_t k;

        if (!lt) {
            return p->n;
        }
        i = (size_t)(lt - p->s);
        if (i + 1 + nl < p->n && p->s[i + 1] == '/') {
            for (k = 0; k < nl && lower((unsigned char)p->s[i + 2 + k]) == name[k]; k++) {
            }
            if (k == nl) {
                return i;
            }
        }
        i++;
    }
    return p->n;
}

/* Elements whose content is text up to their own end tag, never markup. */
static void raw_text(struct parser *p)
{
    char name[TAG_MAX];
    size_t end;
    const char *gt;

    web_copy(name, sizeof(name), p->tag);
    end = find_end_tag(p, p->i, name);
    if (tag_is(p, "title") && !p->d->title[0] && p->skip_depth == 0) {
        size_t n = end - p->i;
        size_t w;

        if (n > CHUNK) {
            n = CHUNK;
        }
        w = decode(p->s + p->i, n, p->cs, p->out);
        web_doc_set_title(p->d, p->out, w);
    }
    if (tag_is(p, "script")) {
        p->info->scripts++;
    }
    if (end >= p->n) {
        p->i = p->n;
        return;
    }
    gt = memchr(p->s + end, '>', p->n - end);
    p->i = gt ? (size_t)(gt - p->s) + 1 : p->n;
}

static bool is_raw_text(const struct parser *p)
{
    static const char *const raw[] = { "script", "style", "title", "textarea", "xmp",
                                       "iframe", "noembed", "noframes", NULL };

    return tag_in(p, raw);
}

/* The markup at s[i] == '<'. Returns false when it is just a '<' in text. */
static bool markup(struct parser *p)
{
    const char *s = p->s + p->i;
    size_t left = p->n - p->i;

    if (left >= 4 && memcmp(s, "<!--", 4) == 0) {
        size_t k;

        for (k = 4; k + 2 < left; k++) {
            if (s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>') {
                p->i += k + 3;
                return true;
            }
        }
        p->i = p->n;
        return true;
    }
    if (left >= 2 && (s[1] == '!' || s[1] == '?')) {
        const char *gt = memchr(s, '>', left);

        p->i = gt ? (size_t)(gt - p->s) + 1 : p->n;
        return true;
    }
    if (left >= 3 && s[1] == '/' && is_alpha((unsigned char)s[2])) {
        size_t i = p->i + 2;
        const char *gt;

        read_name(p, &i);
        gt = memchr(p->s + i, '>', p->n - i);
        p->i = gt ? (size_t)(gt - p->s) + 1 : p->n;
        p->closing = true;
        end_tag(p);
        return true;
    }
    if (left >= 2 && is_alpha((unsigned char)s[1])) {
        p->closing = false;
        read_start_tag(p);
        if (is_raw_text(p) && !p->self_closing) {
            /* iframe: its link first, then its fallback content is skipped. */
            start_tag(p);
            raw_text(p);
        } else {
            start_tag(p);
            if (tag_in(p, void_tags) && p->skip_depth > 0 && tag_is(p, p->skip_tag)) {
                p->skip_depth--; /* a void element never closes */
            }
        }
        return true;
    }
    return false;
}

void web_html_parse(const char *body, size_t len, const struct web_url *page,
                    const char *header_charset, struct web_doc *out, struct web_html_info *info)
{
    struct parser *p = calloc(1, sizeof(*p));
    struct web_html_info scratch;

    if (!info) {
        info = &scratch;
    }
    memset(info, 0, sizeof(*info));
    if (!p) {
        out->truncated = true;
        return;
    }
    p->s = body ? body : "";
    p->n = body ? len : 0;
    p->d = out;
    p->info = info;
    p->page = *page;
    p->base = *page;
    p->link = -1;
    p->line_start = true;
    choose_charset(p, header_charset);
    info->charset = p->cs;
    if (p->cs == WEB_CHARSET_UNSUPPORTED) {
        char words[160];

        snprintf(words, sizeof(words),
                 "This page is in the %s character set, which Browser cannot read: some "
                 "characters show as '?'.",
                 info->charset_name[0] ? info->charset_name : "unknown");
        note(p, words, NULL);
        p->cs = WEB_CHARSET_UTF8;
    }
    while (p->i < p->n && !out->truncated) {
        size_t start = p->i;
        const char *lt;

        if (p->s[p->i] == '<' && markup(p)) {
            continue;
        }
        lt = memchr(p->s + start + 1, '<', p->n - start - 1);
        p->i = lt ? (size_t)(lt - p->s) : p->n;
        text_raw(p, p->s + start, p->i - start);
    }
    web_doc_end_block(out);
    free(p);
}

void web_text_parse(const char *body, size_t len, const char *header_charset, struct web_doc *out)
{
    struct parser *p = calloc(1, sizeof(*p));
    struct web_html_info info;
    size_t i = 0;

    if (!p) {
        out->truncated = true;
        return;
    }
    memset(&info, 0, sizeof(info));
    p->s = body ? body : "";
    p->n = body ? len : 0;
    p->info = &info;
    p->d = out;
    choose_charset(p, header_charset);
    if (p->cs == WEB_CHARSET_UNSUPPORTED) {
        p->cs = WEB_CHARSET_UTF8;
    }
    i = p->i;
    while (i < p->n && !out->truncated) {
        /* One block per paragraph (up to a blank line), at most CHUNK bytes. */
        size_t end = i;
        size_t w;
        size_t k;
        size_t o = 0;

        while (end < p->n && end - i < CHUNK) {
            if (p->s[end] == '\n' && end + 1 < p->n &&
                (p->s[end + 1] == '\n' || (p->s[end + 1] == '\r' && end + 2 < p->n &&
                                           p->s[end + 2] == '\n'))) {
                break;
            }
            end++;
        }
        while (end < p->n && end > i && ((unsigned char)p->s[end] & 0xc0) == 0x80) {
            end--;
        }
        if (end == i) {
            end = i + 1;
        }
        /* No references in plain text: only the charset is converted. */
        for (k = i; k < end; k++) {
            unsigned char c = (unsigned char)p->s[k];

            if (c == '\r') {
                continue;
            }
            if (c >= 0x80 && p->cs == WEB_CHARSET_LATIN1) {
                o += put_utf8(p->out + o, latin1_cp(c));
            } else {
                p->out[o++] = (char)(c ? c : '?');
            }
        }
        w = o;
        while (w > 0 && (p->out[w - 1] == '\n' || p->out[w - 1] == ' ')) {
            w--;
        }
        k = 0;
        while (k < w && p->out[k] == '\n') {
            k++;
        }
        if (w > k) {
            web_doc_begin_block(out, WEB_BLOCK_PRE, 0);
            web_doc_add_text(out, p->out + k, w - k, 0, -1);
            web_doc_end_block(out);
        }
        i = end;
        while (i < p->n && (p->s[i] == '\n' || p->s[i] == '\r')) {
            i++;
        }
    }
    free(p);
}
