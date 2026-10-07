/*
 * The page document. See web_doc.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "web/web_doc.h"

#include <stdlib.h>
#include <string.h>

void web_doc_init(struct web_doc *d)
{
    memset(d, 0, sizeof(*d));
}

void web_doc_free(struct web_doc *d)
{
    free(d->text);
    free(d->blocks);
    free(d->runs);
    free(d->links);
    web_doc_init(d);
}

/* Grow *arr (of elem-sized items, *cap of them) to hold want, never past max. */
static bool grow(void **arr, size_t *cap, size_t want, size_t elem, size_t max, size_t first)
{
    size_t c = *cap;
    void *n;

    if (want <= c) {
        return true;
    }
    if (want > max) {
        return false;
    }
    if (c == 0) {
        c = first;
    }
    while (c < want) {
        c *= 2;
    }
    if (c > max) {
        c = max;
    }
    n = realloc(*arr, c * elem);
    if (!n) {
        return false;
    }
    *arr = n;
    *cap = c;
    return true;
}

static bool text_room(struct web_doc *d, size_t n)
{
    void *p = d->text;
    bool ok = grow(&p, &d->text_cap, d->text_len + n + 1, 1, WEB_DOC_TEXT_MAX, 4096);

    d->text = p;
    return ok;
}

/* ---- UTF-8 ---------------------------------------------------------------------- */

size_t web_utf8_sanitize(const char *s, size_t n, char *out, bool keep_newlines)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0;
    size_t o = 0;

    while (i < n) {
        unsigned c = u[i];
        unsigned cp;
        size_t len;
        size_t k;

        if (c < 0x80) {
            if ((c < 0x20 && !(keep_newlines && c == '\n') && c != '\t') || c == 0x7f) {
                i++;
                continue;
            }
            out[o++] = (char)(c == '\t' ? ' ' : c);
            i++;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            len = 2;
            cp = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            len = 3;
            cp = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            len = 4;
            cp = c & 0x07;
        } else {
            out[o++] = '?';
            i++;
            continue;
        }
        if (i + len > n) {
            out[o++] = '?';
            i++;
            continue;
        }
        for (k = 1; k < len; k++) {
            if ((u[i + k] & 0xc0) != 0x80) {
                break;
            }
            cp = (cp << 6) | (u[i + k] & 0x3f);
        }
        if (k < len || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
            (cp >= 0xd800 && cp <= 0xdfff)) {
            out[o++] = '?';
            i++;
            continue;
        }
        /* C1 controls, the soft hyphen and the zero-width characters say
         * nothing a reader sees. */
        if ((cp >= 0x80 && cp <= 0x9f) || cp == 0xad || (cp >= 0x200b && cp <= 0x200f) ||
            cp == 0x2060 || cp == 0xfeff) {
            i += len;
            continue;
        }
        memcpy(out + o, u + i, len);
        o += len;
        i += len;
    }
    return o;
}

/* ---- building ------------------------------------------------------------------- */

static bool text_block(uint8_t type)
{
    return type != WEB_BLOCK_RULE && type != WEB_BLOCK_IMAGE;
}

void web_doc_end_block(struct web_doc *d)
{
    struct web_block *b;

    if (!d->open_block || d->nblocks == 0) {
        d->open_block = false;
        return;
    }
    d->open_block = false;
    b = &d->blocks[d->nblocks - 1];
    if (text_block(b->type)) {
        size_t r;
        size_t skip = b->prefix;
        bool visible = false;

        for (r = b->run_first; r < b->run_first + b->run_count && !visible; r++) {
            size_t k;

            for (k = 0; k < d->runs[r].len; k++) {
                if (skip > 0) {
                    skip--;
                    continue;
                }
                char c = d->text[d->runs[r].off + k];

                if (c != ' ' && c != '\n') {
                    visible = true;
                    break;
                }
            }
        }
        /* A marker alone is not content either. */
        if (!visible) {
            d->nruns = b->run_first;
            d->nblocks--;
        }
    }
}

void web_doc_mark_prefix(struct web_doc *d)
{
    struct web_block *b;
    size_t r;
    size_t n = 0;

    if (!d->open_block || d->nblocks == 0) {
        return;
    }
    b = &d->blocks[d->nblocks - 1];
    for (r = b->run_first; r < b->run_first + b->run_count; r++) {
        n += d->runs[r].len;
    }
    b->prefix = (uint8_t)(n > 255 ? 255 : n);
}

int web_doc_begin_block(struct web_doc *d, enum web_block_type type, int level)
{
    void *p = d->blocks;
    struct web_block *b;
    bool ok;

    web_doc_end_block(d);
    ok = grow(&p, &d->blocks_cap, d->nblocks + 1, sizeof(struct web_block), WEB_DOC_BLOCK_MAX, 32);
    d->blocks = p;
    if (!ok) {
        d->truncated = true;
        return -1;
    }
    b = &d->blocks[d->nblocks];
    memset(b, 0, sizeof(*b));
    b->type = (uint8_t)type;
    b->level = (uint8_t)(level < 0 ? 0 : level > 255 ? 255 : level);
    b->image = -1;
    b->run_first = (uint32_t)d->nruns;
    d->open_block = text_block((uint8_t)type);
    return (int)d->nblocks++;
}

int web_doc_add_text(struct web_doc *d, const char *s, size_t n, unsigned flags, int link)
{
    struct web_block *b;
    struct web_run *last;
    size_t w;
    bool keep_nl;

    if (n == 0) {
        return 0;
    }
    if (d->truncated) {
        return -1;
    }
    if (!d->open_block && web_doc_begin_block(d, WEB_BLOCK_PARA, 0) < 0) {
        return -1;
    }
    if (d->text_len + n + 1 > WEB_DOC_TEXT_MAX) {
        /* Take what fits, cut at a character boundary. */
        n = WEB_DOC_TEXT_MAX - d->text_len - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) {
            n--;
        }
        d->truncated = true;
        if (n == 0) {
            return -1;
        }
    }
    if (!text_room(d, n)) {
        d->truncated = true;
        return -1;
    }
    b = &d->blocks[d->nblocks - 1];
    keep_nl = true;
    w = web_utf8_sanitize(s, n, d->text + d->text_len, keep_nl);
    if (w == 0) {
        return d->truncated ? -1 : 0;
    }
    last = b->run_count ? &d->runs[d->nruns - 1] : NULL;
    if (last && last->flags == flags && last->link == link &&
        last->off + last->len == d->text_len) {
        last->len += (uint32_t)w;
    } else {
        void *p = d->runs;
        bool ok = grow(&p, &d->runs_cap, d->nruns + 1, sizeof(struct web_run), WEB_DOC_RUN_MAX, 64);

        d->runs = p;
        if (!ok) {
            d->truncated = true;
            return -1;
        }
        d->runs[d->nruns].off = (uint32_t)d->text_len;
        d->runs[d->nruns].len = (uint32_t)w;
        d->runs[d->nruns].link = (int16_t)(link < 0 ? -1 : link);
        d->runs[d->nruns].flags = (uint8_t)flags;
        d->nruns++;
        b->run_count++;
    }
    d->text_len += w;
    d->text[d->text_len] = '\0';
    return d->truncated ? -1 : 0;
}

/* Store s in the text area, outside any run. */
static int store(struct web_doc *d, const char *s, uint32_t *off, uint32_t *len)
{
    size_t n = strlen(s);

    if (d->text_len + n + 1 > WEB_DOC_TEXT_MAX || !text_room(d, n)) {
        d->truncated = true;
        return -1;
    }
    memcpy(d->text + d->text_len, s, n);
    *off = (uint32_t)d->text_len;
    *len = (uint32_t)n;
    d->text_len += n;
    d->text[d->text_len] = '\0';
    return 0;
}

int web_doc_add_link(struct web_doc *d, const char *url, bool supported)
{
    void *p = d->links;
    bool ok;
    struct web_link *l;

    if (d->truncated) {
        return -1;
    }
    ok = grow(&p, &d->links_cap, d->nlinks + 1, sizeof(struct web_link), WEB_DOC_LINK_MAX, 32);
    d->links = p;
    if (!ok) {
        /* No room for another address: the text stays, as plain text. */
        return -1;
    }
    l = &d->links[d->nlinks];
    if (store(d, url, &l->off, &l->len) < 0) {
        return -1;
    }
    l->supported = supported;
    return (int)d->nlinks++;
}

int web_doc_add_image(struct web_doc *d, const char *src, const char *alt, int link, int width,
                      int height)
{
    struct web_image *im;
    int b;
    size_t n;

    if (d->truncated || d->nimages >= WEB_DOC_IMAGE_MAX) {
        return -1;
    }
    im = &d->images[d->nimages];
    memset(im, 0, sizeof(*im));
    if (store(d, src ? src : "", &im->src_off, &im->src_len) < 0) {
        return -1;
    }
    n = alt ? strlen(alt) : 0;
    if (n >= sizeof(im->alt)) {
        n = sizeof(im->alt) - 1;
        while (n > 0 && ((unsigned char)alt[n] & 0xc0) == 0x80) {
            n--;
        }
    }
    im->alt[web_utf8_sanitize(alt ? alt : "", n, im->alt, false)] = '\0';
    im->link = (int16_t)(link < 0 ? -1 : link);
    im->width = (uint16_t)(width > 0 && width < 65536 ? width : 0);
    im->height = (uint16_t)(height > 0 && height < 65536 ? height : 0);
    b = web_doc_begin_block(d, WEB_BLOCK_IMAGE, 0);
    if (b < 0) {
        return -1;
    }
    d->blocks[b].image = (int16_t)d->nimages;
    return (int)d->nimages++;
}

void web_doc_set_title(struct web_doc *d, const char *s, size_t n)
{
    char tmp[WEB_TITLE_MAX];
    size_t w;
    size_t i;
    size_t o = 0;
    bool space = true;

    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) {
            n--;
        }
    }
    w = web_utf8_sanitize(s, n, tmp, false);
    /* Collapse runs of blanks and trim, as a title bar would. */
    for (i = 0; i < w; i++) {
        if (tmp[i] == ' ' || tmp[i] == '\n') {
            if (!space) {
                d->title[o++] = ' ';
            }
            space = true;
        } else {
            d->title[o++] = tmp[i];
            space = false;
        }
    }
    while (o > 0 && d->title[o - 1] == ' ') {
        o--;
    }
    d->title[o] = '\0';
}

const char *web_doc_run_text(const struct web_doc *d, size_t run, size_t *len)
{
    if (run >= d->nruns || !d->text) {
        if (len) {
            *len = 0;
        }
        return "";
    }
    if (len) {
        *len = d->runs[run].len;
    }
    return d->text + d->runs[run].off;
}

const char *web_doc_link_url(const struct web_doc *d, int link, size_t *len)
{
    if (link < 0 || (size_t)link >= d->nlinks || !d->text) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    if (len) {
        *len = d->links[link].len;
    }
    return d->text + d->links[link].off;
}

size_t web_doc_heap_bytes(const struct web_doc *d)
{
    return d->text_cap + d->blocks_cap * sizeof(struct web_block) +
           d->runs_cap * sizeof(struct web_run) + d->links_cap * sizeof(struct web_link);
}
