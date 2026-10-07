/*
 * HTML into a web_doc (web_doc.h): the Browser's "rendering engine", which
 * is a reader, not a layout engine.
 *
 * WHAT IT DOES. One pass over the bytes, no tree, no recursion, nothing kept
 * but a few counters:
 *
 *   - text, with whitespace collapsed (kept inside <pre>), entities decoded
 *     (numeric, and the common named ones), and the character set turned into
 *     UTF-8: UTF-8, US-ASCII, ISO-8859-1/-15 and windows-1252. Anything else
 *     is read as UTF-8 and the page gets a note saying so;
 *   - blocks from the block elements (p, div, h1-h6, li, pre, blockquote,
 *     hr, tr ...), table cells separated by " | ";
 *   - links (<a href>) resolved against the page (or <base href>), images
 *     (<img src alt width height>), the <title>, <meta charset>, and a
 *     <meta http-equiv=refresh> shown as a link rather than followed;
 *   - inline code, bold and italic as run flags.
 *
 * WHAT IT DOES NOT. No CSS (only the hidden attribute and an inline
 * display:none drop an element), no JavaScript (script is skipped and
 * noscript is shown), no forms (a form is replaced by a note saying so), no
 * frames (an iframe becomes a link), no SVG, video, audio or canvas.
 *
 * Every limit is web_doc's: a page past them is cut and marked truncated,
 * and parsing stops there. Hostile input - unterminated tags, comments and
 * attributes, deep nesting, huge attribute values, broken UTF-8, NUL bytes -
 * costs at most one pass: tests/web_html_test.c.
 *
 * Pure C: the helper (pos-browser) is the only program that runs it, so a
 * fault in it never reaches the shell.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_WEB_HTML_H
#define POCKETOS_WEB_HTML_H

#include "web/web_doc.h"
#include "web/web_url.h"

#include <stdbool.h>
#include <stddef.h>

enum web_charset {
    WEB_CHARSET_UTF8 = 0,
    WEB_CHARSET_LATIN1,         /* ISO-8859-1 and US-ASCII are read as windows-1252, as browsers do */
    WEB_CHARSET_UNSUPPORTED
};

struct web_html_info {
    enum web_charset charset;
    char charset_name[32];      /* as declared, lower case; "" when none was */
    bool has_refresh;           /* a <meta http-equiv=refresh> became a link */
    unsigned forms;             /* forms replaced by a note */
    unsigned scripts;           /* scripts skipped */
};

/* Parse an HTML body into out (which must be empty). page is the address the
 * body came from, after redirects; header_charset the charset parameter of
 * the Content-Type header, or NULL. */
void web_html_parse(const char *body, size_t len, const struct web_url *page,
                    const char *header_charset, struct web_doc *out, struct web_html_info *info);

/* A text/plain body: preformatted blocks, split at blank lines. */
void web_text_parse(const char *body, size_t len, const char *header_charset, struct web_doc *out);

/* Which charset a name means; exposed for the tests. */
enum web_charset web_charset_from_name(const char *name, size_t n);

#endif
