/*
 * A web page as the Browser shows it: a flat list of blocks (paragraphs,
 * headings, list items, preformatted text, quotes, rules, images), each made
 * of text runs, some of which are links. No tree, no styles beyond a few
 * flags, no scripts: what web_html.c makes of a page, what the helper sends
 * the shell (web_proto.h), and what the screen draws.
 *
 * EVERY PART IS BOUNDED. A page larger than the limits below is cut, the
 * document says so (truncated), and the rest is never read. The arrays grow
 * on demand up to their limits, so a small page costs little; the limits,
 * not the page, decide the worst case (about 0.6 MB, docs/apps/BROWSER.md).
 *
 * Text is UTF-8, validated: whatever builds a document hands in bytes that
 * web_doc_add_text() checks, and a broken sequence becomes '?'.
 *
 * Pure C: tests/web_doc_test.c (and every web_html and web_proto test).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_WEB_DOC_H
#define POCKETOS_WEB_DOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEB_DOC_TEXT_MAX (256u * 1024u)  /* page text and link addresses, bytes */
#define WEB_DOC_BLOCK_MAX 800
#define WEB_DOC_RUN_MAX 4000
#define WEB_DOC_LINK_MAX 500
#define WEB_DOC_IMAGE_MAX 16
#define WEB_TITLE_MAX 160
#define WEB_ALT_MAX 120
#define WEB_INDENT_MAX 6

enum web_block_type {
    WEB_BLOCK_PARA = 0,
    WEB_BLOCK_HEADING,  /* level 1..6 */
    WEB_BLOCK_PRE,      /* preformatted: line breaks kept, monospace */
    WEB_BLOCK_QUOTE,
    WEB_BLOCK_ITEM,     /* a list item; its marker is its first run */
    WEB_BLOCK_RULE,
    WEB_BLOCK_IMAGE,    /* image: its index in images[] */
    WEB_BLOCK_NOTE,     /* the Browser's own words about the page (a form it cannot
                         * submit, a redirect it did not follow) */
    WEB_BLOCK_TYPE_COUNT
};

/* Run flags. */
#define WEB_RUN_CODE 0x01
#define WEB_RUN_STRONG 0x02
#define WEB_RUN_EM 0x04

struct web_run {
    uint32_t off;       /* into text */
    uint32_t len;
    int16_t link;       /* index into links, or -1 */
    uint8_t flags;
};

struct web_block {
    uint8_t type;
    uint8_t level;      /* heading level, or indent (0..WEB_INDENT_MAX) */
    int16_t image;      /* WEB_BLOCK_IMAGE: index into images, else -1 */
    uint8_t prefix;     /* bytes at the start that are a marker, not content */
    uint32_t run_first;
    uint32_t run_count;
};

struct web_link {
    uint32_t off;       /* the resolved address, into text */
    uint32_t len;
    bool supported;     /* http(s) or about: - anything else only says what it is */
};

struct web_image {
    uint32_t src_off;   /* resolved address, into text; the shell never gets it */
    uint32_t src_len;
    char alt[WEB_ALT_MAX];
    int16_t link;       /* the link the image sits in, or -1 */
    uint16_t width;     /* what the page declared, 0 when it did not */
    uint16_t height;
};

struct web_doc {
    char *text;
    size_t text_len;
    size_t text_cap;
    struct web_block *blocks;
    size_t nblocks;
    size_t blocks_cap;
    struct web_run *runs;
    size_t nruns;
    size_t runs_cap;
    struct web_link *links;
    size_t nlinks;
    size_t links_cap;
    struct web_image images[WEB_DOC_IMAGE_MAX];
    size_t nimages;
    char title[WEB_TITLE_MAX];
    bool truncated;     /* a limit was reached: the page goes on past what is here */
    bool open_block;    /* the last block still takes runs */
};

void web_doc_init(struct web_doc *d);
void web_doc_free(struct web_doc *d);   /* back to an empty document */

/* Start a block. Returns its index, or -1 when full (truncated is set). */
int web_doc_begin_block(struct web_doc *d, enum web_block_type type, int level);
/* The text the open block holds so far is a marker ("1. "), not content:
 * a block with nothing after it is still empty. */
void web_doc_mark_prefix(struct web_doc *d);
/* Close the open block. An empty text block is dropped again. */
void web_doc_end_block(struct web_doc *d);

/* Append UTF-8 text to the open block (one is opened as a paragraph when
 * none is) with the given flags and link. Consecutive text with the same
 * flags and link joins one run. Returns 0, or -1 when a limit was reached. */
int web_doc_add_text(struct web_doc *d, const char *s, size_t n, unsigned flags, int link);

/* Remember a link address. Returns its index or -1 (full). */
int web_doc_add_link(struct web_doc *d, const char *url, bool supported);
/* Remember an image and add an image block for it. Returns its index or -1. */
int web_doc_add_image(struct web_doc *d, const char *src, const char *alt, int link, int width,
                      int height);

void web_doc_set_title(struct web_doc *d, const char *s, size_t n);

/* Accessors, safe with any index: NULL / "" when out of range. */
const char *web_doc_run_text(const struct web_doc *d, size_t run, size_t *len);
const char *web_doc_link_url(const struct web_doc *d, int link, size_t *len);

/* Bytes the document holds on the heap now. */
size_t web_doc_heap_bytes(const struct web_doc *d);

/* Copy at most n bytes of s into out as valid UTF-8 ('?' for anything
 * broken), dropping control characters but '\n' when keep_newlines. Returns
 * the bytes written (out is not terminated). outcap must be >= n. */
size_t web_utf8_sanitize(const char *s, size_t n, char *out, bool keep_newlines);

#endif
