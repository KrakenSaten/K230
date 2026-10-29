/*
 * pos-library: the Library's collection reader (docs/apps/LIBRARY.md).
 *
 *   pos-library session [--archive FILE] [--images DIR]
 *       The Library app's helper. stdin and stdout are a socketpair to the
 *       shell; the protocol is Browser's (core/web/web_proto.h), so the
 *       shell's session, view and screen are Browser's too. It lives exactly
 *       as long as the Library screen, like pos-browser: it leaves on `quit`,
 *       when the shell closes its end, on SIGTERM, and through
 *       PR_SET_PDEATHSIG when the shell dies.
 *   pos-library info [--archive FILE]
 *       The collection this unit would open, and its metadata.
 *   pos-library dump [--archive FILE] ADDRESS|WORDS
 *       One page (an address under https://library.invalid/, or the words of
 *       a search) printed as text, links numbered, pictures decoded.
 *   pos-library bench [--archive FILE] WORDS...
 *       Times opening the archive, searching each WORDS, opening the first
 *       result and decoding its pictures; prints the peak memory.
 *
 * THE COLLECTION is one Kiwix/openZIM archive, read with libzim. Which one:
 * --archive; else $POCKETOS_LIBRARY_ARCHIVE; else the line "archive=PATH" in
 * <config>/library.conf; else the first *.zim, by name, in $HOME/Library
 * (/root/Library). Content is replaced by replacing that file: nothing about
 * it is built into Doors. The archive is opened at the first page and kept
 * open for the session.
 *
 * NOTHING HERE TOUCHES A NETWORK. This program links libzim, its
 * decompressors, the HTML reader and the picture decoders, and no HTTP client
 * (tests/library_lint.sh). An article's links that leave the collection are
 * sent as unsupported: the screen shows them and never follows them.
 *
 * SEARCH. Titles always: libzim's suggestion search, which uses the archive's
 * title index when libzim was built with Xapian and the title list (titles
 * starting with the words) when it was not. The full text too when libzim
 * has Xapian and the archive has a full-text index. The results page says
 * which ran.
 *
 * Archive parsing, decompression, searching and decoding all happen here,
 * one request at a time; the shell's LVGL thread only ever reads the answer.
 *
 * libzim is GPL-2.0-or-later, so this program, which links it, is
 * distributed under the GPL (docs/apps/LIBRARY.md "Licences"); the shell and
 * the rest of Doors talk to it over a pipe and stay Apache-2.0.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
extern "C" {
#include "pocketlog/pocketlog.h"
#include "pocketpaths.h"
#include "web/web_doc.h"
#include "web/web_html.h"
#include "web/web_image.h"
#include "web/web_library.h"
#include "web/web_proto.h"
#include "web/web_url.h"
}

#include <zim/zim.h> /* LIBZIM_WITH_XAPIAN, first */
#include <zim/archive.h>
#include <zim/error.h>
#include <zim/item.h>
#ifdef LIBZIM_WITH_XAPIAN
#include <zim/search.h>
#endif
#include <zim/suggestion.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define IMAGE_MAX_H 1200
/* Pixels one page's pictures may take in the shell, all together (as
 * pos-browser). */
#define PAGE_PIXELS_MAX (1536u * 1024u)
/* The most of one entry ever read: an article's HTML, a picture. */
#define ARTICLE_BYTES_MAX (8u * 1024u * 1024u)
#define PICTURE_BYTES_MAX (4u * 1024u * 1024u)
#define TITLE_RESULTS 25
#define TEXT_RESULTS 20

static volatile sig_atomic_t term;

static void on_signal(int sig)
{
    (void)sig;
    term = 1;
}

static int64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- the collection ------------------------------------------------------------------ */

struct collection {
    std::string path;                       /* the file chosen, "" when none was found */
    std::string why;                        /* why it is not open */
    std::unique_ptr<zim::Archive> zim;
    bool tried;
    int64_t open_ms;
};

/* Which archive: see the top of this file. */
static std::string find_archive(const char *arg, std::string &why)
{
    const char *env = getenv("POCKETOS_LIBRARY_ARCHIVE");
    char conf[POCKETOS_PATH_MAX];
    const char *home = getenv("HOME");
    std::string dir;
    std::vector<std::string> names;
    FILE *f;

    if (arg && *arg) {
        return arg;
    }
    if (env && *env) {
        return env;
    }
    snprintf(conf, sizeof(conf), "%s/library.conf", pocketos_config_dir());
    f = fopen(conf, "re");
    if (f) {
        char line[POCKETOS_PATH_MAX + 16];

        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (strncmp(line, "archive=", 8) == 0 && line[8]) {
                fclose(f);
                return line + 8;
            }
        }
        fclose(f);
    }
    dir = std::string(home && *home ? home : "/root") + "/Library";
    DIR *d = opendir(dir.c_str());
    if (!d) {
        why = "there is no Library folder (" + dir + ")";
        return "";
    }
    while (struct dirent *e = readdir(d)) {
        size_t n = strlen(e->d_name);

        if (e->d_name[0] != '.' && n > 4 && strcmp(e->d_name + n - 4, ".zim") == 0) {
            names.push_back(e->d_name);
        }
    }
    closedir(d);
    if (names.empty()) {
        why = "there is no .zim archive in " + dir;
        return "";
    }
    std::sort(names.begin(), names.end());
    return dir + "/" + names[0];
}

/* Open the archive once; afterwards c->zim or c->why. */
static void open_collection(struct collection *c, const char *arg)
{
    struct stat st;
    int64_t t0 = mono_ms();

    if (c->tried) {
        return;
    }
    c->tried = true;
    c->path = find_archive(arg, c->why);
    if (c->path.empty()) {
        LOG_WARN("library: no collection: %s", c->why.c_str());
        return;
    }
    if (stat(c->path.c_str(), &st) != 0) {
        c->why = c->path + ": " + strerror(errno);
    } else if (!S_ISREG(st.st_mode)) {
        c->why = c->path + ": not a file";
    } else {
        try {
            c->zim.reset(new zim::Archive(c->path));
            /* Reading the header and one entry proves more than opening. */
            if (c->zim->getEntryCount() == 0) {
                c->why = c->path + ": an empty archive";
                c->zim.reset();
            }
        } catch (const std::exception &e) {
            c->why = c->path + ": not a readable ZIM archive (" + e.what() + ")";
            c->zim.reset();
        }
    }
    c->open_ms = mono_ms() - t0;
    if (c->zim) {
        LOG_INFO("library: collection open in %lld ms, %u entries", (long long)c->open_ms,
                 c->zim->getEntryCount());
    } else {
        LOG_WARN("library: collection not opened: %s", c->why.c_str());
    }
}

static std::string meta(const zim::Archive &a, const char *key)
{
    try {
        return a.getMetadata(key);
    } catch (const std::exception &) {
        return "";
    }
}

/* ---- addresses -------------------------------------------------------------------------- */

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                                                                      : -1;
}

static std::string pct_decode(const char *s, size_t n, bool plus_space)
{
    std::string out;

    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out += (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else {
            out += plus_space && s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

/* The address of an entry: its path, encoded for an address. */
static std::string entry_url(const std::string &path)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out = WEB_LIBRARY_ORIGIN;

    for (unsigned char c : path) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~/!$&'()*+,;=:@", c)) {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

/* The entry path an address of the collection names, or false. */
static bool url_entry(const char *url, std::string &path)
{
    struct web_url u;

    if (web_url_parse(url, &u) != WEB_URL_OK || u.scheme != WEB_SCHEME_HTTPS || strcmp(u.host, WEB_LIBRARY_HOST) != 0 ||
        u.port != 0) {
        return false;
    }
    path = pct_decode(u.path + 1, strlen(u.path + 1), false);
    return true;
}

/* ---- output ------------------------------------------------------------------------------ */

static int emit(void *ctx, const char *line, size_t n)
{
    static char buf[WEB_LINE_MAX + 2];
    const char *p = buf;
    size_t left;

    (void)ctx;
    if (n > WEB_LINE_MAX) {
        return -1;
    }
    memcpy(buf, line, n);
    buf[n] = '\n';
    left = n + 1;
    while (left > 0) {
        ssize_t w = write(1, p, left);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            _exit(0); /* the shell has gone: so do we */
        }
        p += w;
        left -= (size_t)w;
    }
    return 0;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
    char line[WEB_LINE_MAX];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof(line)) {
        emit(NULL, line, (size_t)n);
    }
}

static void say_fail(int seq, enum web_fail f, const char *url, const std::string &text)
{
    static char eu[WEB_URL_MAX * 2];
    static char et[WEB_FAIL_TEXT_MAX * 2];
    std::string t = text.substr(0, WEB_FAIL_TEXT_MAX - 1);

    if (web_escape(url, strlen(url), eu, sizeof(eu)) < 0) {
        eu[0] = '\0';
    }
    if (web_escape(t.c_str(), t.size(), et, sizeof(et)) < 0) {
        et[0] = '\0';
    }
    say("fail\t%d\t%s\t0\t%s\t%s", seq, web_fail_name(f), eu, et);
}

/* ---- pages ------------------------------------------------------------------------------- */

struct page {
    struct web_doc doc;
    std::string url;            /* where it really is, after the main page's redirect */
    enum web_fail fail;
    std::string why;
    bool ok;
    int64_t search_title_ms;
    int64_t search_text_ms;
    int title_hits;
    int text_hits;
};

static void text(struct web_doc *d, enum web_block_type type, int level, const std::string &s, int link = -1)
{
    web_doc_begin_block(d, type, level);
    web_doc_add_text(d, s.data(), s.size(), 0, link);
    web_doc_end_block(d);
}

static void item_link(struct web_doc *d, const std::string &title, const std::string &path, const std::string &more)
{
    int link = web_doc_add_link(d, entry_url(path).c_str(), true);

    web_doc_begin_block(d, WEB_BLOCK_ITEM, 0);
    web_doc_add_text(d, "\xe2\x80\xa2 ", 4, 0, -1);
    web_doc_mark_prefix(d);
    web_doc_add_text(d, title.data(), title.size(), WEB_RUN_STRONG, link);
    if (!more.empty()) {
        std::string m = "\n" + more;

        web_doc_add_text(d, m.data(), m.size(), 0, -1);
    }
    web_doc_end_block(d);
}

#ifdef LIBZIM_WITH_XAPIAN
/* A snippet is HTML with the matched words in <b>: the words only. */
static std::string plain(const std::string &html)
{
    std::string out;
    bool tag = false;

    for (char c : html) {
        if (c == '<') {
            tag = true;
        } else if (c == '>') {
            tag = false;
        } else if (!tag) {
            out += c == '\n' ? ' ' : c;
        }
    }
    return out;
}
#endif

static bool full_text_available(const zim::Archive &a)
{
#ifdef LIBZIM_WITH_XAPIAN
    try {
        return a.hasFulltextIndex();
    } catch (const std::exception &) {
        return false;
    }
#else
    (void)a;
    return false;
#endif
}

static void search_page(const zim::Archive &a, const std::string &q, struct page *p)
{
    struct web_doc *d = &p->doc;
    std::string title = meta(a, "Title");
    bool fts = full_text_available(a);
    int64_t t0 = mono_ms();
    std::vector<std::pair<std::string, std::string>> titles;   /* title, path */
    std::vector<std::vector<std::string>> texts;                /* title, path, snippet */
    int title_est = 0;
    int text_est = 0;

    web_doc_set_title(d, ("Search: " + q).c_str(), q.size() + 8);
    zim::SuggestionSearcher suggest(a);
    /* With Xapian the title index matches words in any case. Without it
     * libzim matches the start of a title exactly, case included, so the
     * words are also tried as titles are usually written: first letter
     * capitalised, then every word (ASCII letters only: no ICU here). */
    std::vector<std::string> tries = { q };
#ifndef LIBZIM_WITH_XAPIAN
    {
        std::string first = q;
        std::string words = q;

        if (!first.empty()) {
            first[0] = (char)toupper((unsigned char)first[0]);
        }
        for (size_t i = 0; i < words.size(); i++) {
            if (i == 0 || words[i - 1] == ' ') {
                words[i] = (char)toupper((unsigned char)words[i]);
            }
        }
        for (const std::string &t : { first, words }) {
            if (std::find(tries.begin(), tries.end(), t) == tries.end()) {
                tries.push_back(t);
            }
        }
    }
#endif
    for (const std::string &t : tries) {
        auto ts = suggest.suggest(t);
        auto tr = ts.getResults(0, TITLE_RESULTS);

        title_est += ts.getEstimatedMatches();
        for (auto it = tr.begin(); it != tr.end() && titles.size() < TITLE_RESULTS; ++it) {
            std::string path = it->getPath();
            bool seen = false;

            for (auto &have : titles) {
                seen = seen || have.second == path;
            }
            if (!seen) {
                titles.emplace_back(it->getTitle(), path);
            }
        }
    }
    p->search_title_ms = mono_ms() - t0;
#ifdef LIBZIM_WITH_XAPIAN
    if (fts) {
        int64_t t1 = mono_ms();
        zim::Searcher searcher(a);
        zim::Query query(q);
        auto s = searcher.search(query);
        auto r = s.getResults(0, TEXT_RESULTS);

        text_est = s.getEstimatedMatches();
        for (auto it = r.begin(); it != r.end(); ++it) {
            texts.push_back({ it.getTitle(), it.getPath(), plain(it.getSnippet()) });
        }
        p->search_text_ms = mono_ms() - t1;
    }
#endif
    p->title_hits = (int)titles.size();
    p->text_hits = (int)texts.size();

    text(d, WEB_BLOCK_HEADING, 1, "Search: \xe2\x80\x9c" + q + "\xe2\x80\x9d");
    {
        char words[400];

        if (fts) {
            snprintf(words, sizeof(words), "Searched the titles and the full text of %s: %d title match%s, about %d "
                     "in the text.", title.empty() ? "the collection" : title.c_str(), title_est,
                     title_est == 1 ? "" : "es", text_est);
        } else {
            snprintf(words, sizeof(words), "Title search only: %s. %d title match%s.",
#ifdef LIBZIM_WITH_XAPIAN
                     "this collection has no full-text index",
#else
                     "this build of Library has no full-text search, so only titles that begin with the words (as "
                     "typed, or capitalised) are "
                     "found",
#endif
                     title_est, title_est == 1 ? "" : "es");
        }
        text(d, WEB_BLOCK_NOTE, 0, words);
    }
    text(d, WEB_BLOCK_HEADING, 2, "Titles");
    for (auto &t : titles) {
        item_link(d, t.first, t.second, "");
    }
    if (titles.empty()) {
        text(d, WEB_BLOCK_PARA, 0, "No article title matches.");
    }
    if (fts) {
        text(d, WEB_BLOCK_HEADING, 2, "In the text");
        for (auto &t : texts) {
            item_link(d, t[0], t[1], t[2]);
        }
        if (texts.empty()) {
            text(d, WEB_BLOCK_PARA, 0, "No article has these words.");
        }
    }
    p->ok = true;
}

static std::string kb(uint64_t bytes)
{
    char b[48];

    if (bytes >= 10u * 1024 * 1024) {
        snprintf(b, sizeof(b), "%llu MB", (unsigned long long)(bytes / (1024 * 1024)));
    } else {
        snprintf(b, sizeof(b), "%llu KB", (unsigned long long)((bytes + 1023) / 1024));
    }
    return b;
}

static void about_page(const struct collection *c, struct page *p)
{
    const zim::Archive &a = *c->zim;
    struct web_doc *d = &p->doc;
    std::string title = meta(a, "Title");
    static const struct {
        const char *key;
        const char *label;
    } rows[] = {
        { "Title", "Title" },     { "Date", "Date" },         { "Source", "Source" },     { "Creator", "Creator" },
        { "Publisher", "Publisher" }, { "Language", "Language" }, { "Name", "Name" },       { "Flavour", "Flavour" },
        { "License", "Licence" }, { "Scraper", "Made with" },
    };

    web_doc_set_title(d, "About this collection", 21);
    text(d, WEB_BLOCK_HEADING, 1, title.empty() ? "This collection" : title);
    if (!meta(a, "Description").empty()) {
        text(d, WEB_BLOCK_PARA, 0, meta(a, "Description"));
    }
    for (auto &r : rows) {
        std::string v = meta(a, r.key);

        if (r.key == std::string("License") && v.empty()) {
            v = "not stated in the archive: see the source's own terms";
        }
        if (!v.empty()) {
            text(d, WEB_BLOCK_ITEM, 0, std::string(r.label) + ": " + v);
        }
    }
    {
        char line[200];
        struct stat st;

        /* The archive's own count of pages by type (its "Counter"), when it
         * has one: getArticleCount() also counts redirect titles. */
        std::string counter = meta(a, "Counter");
        size_t k = counter.find("text/html=");

        if (k != std::string::npos) {
            snprintf(line, sizeof(line), "Articles: %ld pages, %u titles with redirects; media: %u",
                     atol(counter.c_str() + k + 10), a.getArticleCount(), a.getMediaCount());
        } else {
            snprintf(line, sizeof(line), "Articles: %u titles; media: %u", a.getArticleCount(), a.getMediaCount());
        }
        text(d, WEB_BLOCK_ITEM, 0, line);
        text(d, WEB_BLOCK_ITEM, 0, std::string("Search: ") +
                                       (full_text_available(a) ? "titles and full text" : "titles only"));
        text(d, WEB_BLOCK_ITEM, 0, "File: " + c->path +
                                       (stat(c->path.c_str(), &st) == 0 ? ", " + kb((uint64_t)st.st_size) : ""));
    }
    text(d, WEB_BLOCK_NOTE, 0, "Read offline from this unit with libzim " + std::string(LIBZIM_VERSION) +
                                   ". Library shows the archive as it is: the content and its licence are the "
                                   "source's, not Doors'. Replace the file to change the collection.");
    if (a.hasMainEntry()) {
        int link = web_doc_add_link(d, WEB_LIBRARY_MAIN, true);

        text(d, WEB_BLOCK_PARA, 0, "Open the main page", link);
    }
    p->ok = true;
}

/* Every link that does not name an entry of the collection is shown, never
 * followed. */
static void mark_outside_links(struct web_doc *d)
{
    for (size_t i = 0; i < d->nlinks; i++) {
        size_t n;
        const char *u = web_doc_link_url(d, (int)i, &n);
        std::string s(u ? u : "", u ? n : 0);
        std::string path;

        if (!url_entry(s.c_str(), path)) {
            d->links[i].supported = false;
        }
    }
}

static std::string mime_charset(const std::string &mime)
{
    size_t k = mime.find("charset=");

    return k == std::string::npos ? "" : mime.substr(k + 8);
}

static void entry_page(const zim::Archive &a, const std::string &path, struct page *p)
{
    zim::Entry e = a.getEntryByPath(path);
    zim::Item it = e.getItem(true);
    std::string mime = it.getMimetype();
    std::string type = mime.substr(0, mime.find(';'));
    uint64_t size = it.getSize();
    struct web_url final;

    p->url = entry_url(it.getPath());
    if (web_url_parse(p->url.c_str(), &final) != WEB_URL_OK) {
        p->fail = WEB_FAIL_URL;
        p->why = "an entry name Library cannot address";
        return;
    }
    if (type == "text/html" || type == "text/plain") {
        zim::Blob b = it.getData(0, std::min<uint64_t>(size, ARTICLE_BYTES_MAX));
        std::string cs = mime_charset(mime);

        if (type == "text/html") {
            web_html_parse(b.data(), b.size(), &final, cs.empty() ? NULL : cs.c_str(), &p->doc, NULL);
        } else {
            web_text_parse(b.data(), b.size(), cs.empty() ? NULL : cs.c_str(), &p->doc);
        }
        if (size > ARTICLE_BYTES_MAX) {
            p->doc.truncated = true;
        }
        if (!p->doc.title[0]) {
            std::string t = it.getTitle();

            web_doc_set_title(&p->doc, t.data(), t.size());
        }
        mark_outside_links(&p->doc);
    } else if (type.compare(0, 6, "image/") == 0) {
        std::string t = it.getTitle();

        web_doc_set_title(&p->doc, t.data(), t.size());
        web_doc_add_image(&p->doc, p->url.c_str(), t.c_str(), -1, 0, 0);
    } else {
        p->fail = WEB_FAIL_TYPE;
        p->why = mime + ", " + kb(size) + ": Library shows articles and pictures; PDF, video and other files are not "
                                          "supported";
        return;
    }
    p->ok = true;
}

/* One address of the collection into p. */
static void make_page(struct collection *c, const char *arg, const char *url, struct page *p)
{
    std::string path;
    const std::string own = WEB_LIBRARY_OWN;

    p->ok = false;
    p->fail = WEB_FAIL_INTERNAL;
    p->url = url;
    if (!url_entry(url, path)) {
        p->fail = WEB_FAIL_URL;
        p->why = "not an address of the collection";
        return;
    }
    open_collection(c, arg);
    if (!c->zim) {
        p->fail = WEB_FAIL_ARCHIVE;
        p->why = c->why;
        return;
    }
    try {
        if (path == own + "main") {
            if (!c->zim->hasMainEntry()) {
                p->fail = WEB_FAIL_NOT_FOUND;
                p->why = "this collection names no main page";
                return;
            }
            entry_page(*c->zim, c->zim->getMainEntry().getItem(true).getPath(), p);
        } else if (path == own + "about") {
            about_page(c, p);
        } else if (path.compare(0, own.size() + 7, own + "search?") == 0) {
            struct web_url u;
            const char *q;

            web_url_parse(url, &u);
            q = strstr(u.path, "?q=");
            std::string words = q ? pct_decode(q + 3, strcspn(q + 3, "&"), true) : "";
            search_page(*c->zim, words, p);
            p->url = url;
        } else {
            entry_page(*c->zim, path, p);
        }
    } catch (const zim::EntryNotFound &) {
        p->ok = false;
        p->fail = WEB_FAIL_NOT_FOUND;
        p->why = "no entry \"" + path.substr(0, 120) + "\" in this collection";
    } catch (const std::exception &e) {
        p->ok = false;
        p->fail = WEB_FAIL_ARCHIVE;
        p->why = std::string("the archive could not be read: ") + e.what();
    }
}

/* One picture of the page, decoded, or why not. */
static enum web_image_err picture(const zim::Archive &a, const struct web_doc *d, size_t i, int maxw, size_t budget,
                                  struct web_pixels *px, std::string &why)
{
    const struct web_image *im = &d->images[i];
    std::string src(d->text + im->src_off, im->src_len);
    std::string path;

    if (!url_entry(src.c_str(), path)) {
        why = src.compare(0, 5, "data:") == 0 ? "a drawing in the page, not shown" : "outside the collection: not loaded";
        return WEB_IMAGE_FORMAT;
    }
    try {
        zim::Item it = a.getEntryByPath(path).getItem(true);

        if (it.getSize() > PICTURE_BYTES_MAX) {
            why = "picture too large";
            return WEB_IMAGE_TOO_LARGE;
        }
        if (it.getMimetype() == "image/svg+xml") {
            why = "SVG drawings are not shown";
            return WEB_IMAGE_FORMAT;
        }
        zim::Blob b = it.getData();
        enum web_image_err e = web_image_decode((const unsigned char *)b.data(), b.size(), im->width, maxw,
                                                IMAGE_MAX_H, budget < WEB_IMAGE_PIXELS_MAX ? budget
                                                                                           : WEB_IMAGE_PIXELS_MAX,
                                                px);
        if (e != WEB_IMAGE_OK) {
            why = web_image_err_text(e);
        }
        return e;
    } catch (const zim::EntryNotFound &) {
        why = "missing from the collection";
    } catch (const std::exception &) {
        why = "could not be read";
    }
    return WEB_IMAGE_BROKEN;
}

/* ---- the session -------------------------------------------------------------------------- */

struct helper {
    struct collection c;
    const char *archive;
    const char *img_dir;
    char in[WEB_LINE_MAX];
    size_t in_len;
    bool in_over;
    bool quit;
    bool have_open;
    int open_seq;
    int open_maxw;
    char open_flags[8];
    char open_url[WEB_URL_MAX];
    int seq;
    bool active;
    bool stop;
};

static void command(struct helper *h, char *line)
{
    char *f[5];
    int n = 0;
    char *p = line;

    while (n < 5) {
        char *t = strchr(p, '\t');

        f[n++] = p;
        if (!t) {
            break;
        }
        *t = '\0';
        p = t + 1;
    }
    if (strcmp(f[0], "quit") == 0) {
        h->quit = true;
    } else if (strcmp(f[0], "stop") == 0 && n == 2) {
        int seq = atoi(f[1]);

        if (h->active && seq == h->seq) {
            h->stop = true;
        }
        if (h->have_open && seq == h->open_seq) {
            h->have_open = false;
        }
    } else if (strcmp(f[0], "open") == 0 && n == 5) {
        h->have_open = true;
        h->open_seq = atoi(f[1]);
        h->open_maxw = atoi(f[2]);
        web_copy(h->open_flags, sizeof(h->open_flags), f[3]);
        web_copy(h->open_url, sizeof(h->open_url), f[4]);
        if (h->active) {
            h->stop = true; /* a new page replaces the one loading */
        }
    }
}

/* What the shell sent; wait up to timeout_ms (-1: until something comes). */
static void read_commands(struct helper *h, int timeout_ms)
{
    struct pollfd pfd = { 0, POLLIN, 0 };
    char buf[4096];

    for (;;) {
        int r = poll(&pfd, 1, timeout_ms);
        ssize_t n;

        if (r < 0 && errno == EINTR) {
            if (term) {
                return;
            }
            continue;
        }
        if (r <= 0) {
            return;
        }
        n = read(0, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            h->quit = true; /* the shell closed its end */
            return;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (!h->in_over) {
                    h->in[h->in_len] = '\0';
                    command(h, h->in);
                }
                h->in_len = 0;
                h->in_over = false;
            } else if (h->in_len + 1 >= sizeof(h->in)) {
                h->in_over = true;
            } else if (!h->in_over) {
                h->in[h->in_len++] = buf[i];
            }
        }
        timeout_ms = 0;
    }
}

static bool stopped(struct helper *h)
{
    read_commands(h, 0);
    return term || h->quit || h->stop;
}

static int write_pixels(const char *dir, int seq, int id, const struct web_pixels *px, char *name, size_t nlen)
{
    char path[1024];
    size_t bytes = (size_t)px->w * (size_t)px->h * sizeof(uint16_t);
    const char *p = (const char *)px->px;
    int fd;

    snprintf(name, nlen, "%d-%d.rgb565", seq, id);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    unlink(path);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    while (bytes > 0) {
        ssize_t w = write(fd, p, bytes);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            unlink(path);
            return -1;
        }
        p += w;
        bytes -= (size_t)w;
    }
    return close(fd);
}

static void images(struct helper *h, const struct web_doc *d, int maxw)
{
    size_t budget = PAGE_PIXELS_MAX;

    for (size_t i = 0; i < d->nimages && !stopped(h); i++) {
        struct web_pixels px;
        std::string why;
        char name[WEB_FILE_NAME_MAX];

        if (budget < 64 * 64) {
            say("nopixels\t%d\t%zu\tthe page's pictures used their memory", h->seq, i);
            continue;
        }
        memset(&px, 0, sizeof(px));
        if (picture(*h->c.zim, d, i, maxw, budget, &px, why) != WEB_IMAGE_OK) {
            say("nopixels\t%d\t%zu\t%s", h->seq, i, why.substr(0, 46).c_str());
            free(px.px);
            continue;
        }
        if (write_pixels(h->img_dir, h->seq, (int)i, &px, name, sizeof(name)) != 0) {
            say("nopixels\t%d\t%zu\tcould not be stored", h->seq, i);
        } else {
            budget -= (size_t)px.w * (size_t)px.h;
            say("pixels\t%d\t%zu\t%d\t%d\t%s", h->seq, i, px.w, px.h, name);
        }
        free(px.px);
    }
}

static void open_page(struct helper *h, int seq, int maxw, const char *flags, const char *url)
{
    struct page p;
    int64_t t0 = mono_ms();
    bool want_images = strchr(flags, 'i') != NULL && h->img_dir != NULL;

    h->seq = seq;
    h->active = true;
    h->stop = false;
    if (maxw < 32 || maxw > WEB_IMAGE_SIDE_MAX) {
        maxw = 560;
    }
    say("progress\t%d\treceive\t0", seq);
    web_doc_init(&p.doc);
    p.search_title_ms = p.search_text_ms = -1;
    make_page(&h->c, h->archive, url, &p);
    if (!p.ok) {
        LOG_INFO("library: failed: %s", web_fail_name(p.fail));
        say_fail(seq, p.fail, p.url.c_str(), p.why);
    } else {
        unsigned pflags = want_images && p.doc.nimages > 0 ? WEB_PAGE_IMAGES : 0;

        if (p.search_title_ms >= 0) {
            LOG_INFO("library: search in %lld ms (titles %lld ms, %d; text %lld ms, %d)",
                     (long long)(mono_ms() - t0), (long long)p.search_title_ms, p.title_hits,
                     (long long)p.search_text_ms, p.text_hits);
        } else {
            LOG_INFO("library: page in %lld ms: %zu blocks, %zu links, %zu pictures", (long long)(mono_ms() - t0),
                     p.doc.nblocks, p.doc.nlinks, p.doc.nimages);
        }
        web_proto_send_doc(emit, NULL, seq, pflags, 200, p.url.c_str(), &p.doc);
        if (pflags & WEB_PAGE_IMAGES) {
            images(h, &p.doc, maxw);
        }
    }
    web_doc_free(&p.doc);
    h->active = false;
#ifdef __GLIBC__
    malloc_trim(0);
#endif
    say("idle\t%d", seq);
}

static const char *features(void)
{
    static char buf[32];
    const char *img = web_image_formats();

#ifdef LIBZIM_WITH_XAPIAN
    snprintf(buf, sizeof(buf), "zim,fulltext%s%s", img[0] ? ",img:" : "", img);
#else
    snprintf(buf, sizeof(buf), "zim,titles%s%s", img[0] ? ",img:" : "", img);
#endif
    return buf;
}

static int session(struct helper *h)
{
    LOG_INFO("library: session start, %s", features());
    say("hello\t%d\t%s", WEB_PROTO_VERSION, features());
    while (!term && !h->quit) {
        if (!h->have_open) {
            read_commands(h, -1);
            continue;
        }
        h->have_open = false;
        {
            char url[WEB_URL_MAX];
            char flags[8];

            web_copy(url, sizeof(url), h->open_url);
            web_copy(flags, sizeof(flags), h->open_flags);
            open_page(h, h->open_seq, h->open_maxw, flags, url);
        }
    }
    LOG_INFO("library: session end");
    return 0;
}

/* ---- the bench's modes ------------------------------------------------------------------------ */

static void print_doc(const struct web_doc *d, const char *url)
{
    printf("== %s\n== %s%s\n", d->title, url, d->truncated ? " (cut)" : "");
    for (size_t b = 0; b < d->nblocks; b++) {
        const struct web_block *bl = &d->blocks[b];

        if (bl->type == WEB_BLOCK_IMAGE) {
            printf("[picture %d: %s]\n", bl->image, d->images[bl->image].alt);
            continue;
        }
        if (bl->type == WEB_BLOCK_HEADING) {
            printf("%.*s ", bl->level, "######");
        }
        for (size_t r = bl->run_first; r < bl->run_first + bl->run_count; r++) {
            size_t n;
            const char *t = web_doc_run_text(d, r, &n);

            if (d->runs[r].link >= 0) {
                printf("[%.*s]{%d%s}", (int)n, t, d->runs[r].link, d->links[d->runs[r].link].supported ? "" : " outside");
            } else {
                printf("%.*s", (int)n, t);
            }
        }
        printf("\n");
    }
    printf("== %zu blocks, %zu links, %zu pictures, %zu KB text\n", d->nblocks, d->nlinks, d->nimages,
           d->text_len / 1024);
}

static long peak_kb(void)
{
    struct rusage ru;

    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

/* What the resident memory is made of now: the archive is memory-mapped,
 * so much of it is file pages the kernel can drop, not the helper's own. */
static void print_memory(void)
{
    FILE *f = fopen("/proc/self/status", "re");
    char line[128];

    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmHWM:", 6) == 0 || strncmp(line, "VmRSS:", 6) == 0 || strncmp(line, "RssAnon:", 8) == 0 ||
            strncmp(line, "RssFile:", 8) == 0) {
            line[strcspn(line, "\n")] = '\0';
            printf("%s  ", line);
        }
    }
    printf("\n");
    fclose(f);
}

static std::string address_of(const char *what)
{
    if (strncmp(what, "https://", 8) == 0) {
        return what;
    }
    std::string url = WEB_LIBRARY_SEARCH;
    static const char hex[] = "0123456789ABCDEF";

    for (const char *s = what; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (isalnum(c) || strchr("-._~", c)) {
            url += (char)c;
        } else {
            url += '%';
            url += hex[c >> 4];
            url += hex[c & 15];
        }
    }
    return url;
}

static int dump(struct helper *h, const char *what)
{
    struct page p;
    std::string url = address_of(what);

    web_doc_init(&p.doc);
    p.search_title_ms = p.search_text_ms = -1;
    make_page(&h->c, h->archive, url.c_str(), &p);
    if (!p.ok) {
        printf("fail %s: %s\n", web_fail_name(p.fail), p.why.c_str());
        web_doc_free(&p.doc);
        return 1;
    }
    print_doc(&p.doc, p.url.c_str());
    for (size_t i = 0; i < p.doc.nimages; i++) {
        struct web_pixels px;
        std::string why;

        memset(&px, 0, sizeof(px));
        if (picture(*h->c.zim, &p.doc, i, 528, PAGE_PIXELS_MAX, &px, why) == WEB_IMAGE_OK) {
            printf("picture %zu: %dx%d\n", i, px.w, px.h);
        } else {
            printf("picture %zu: not shown: %s\n", i, why.c_str());
        }
        free(px.px);
    }
    web_doc_free(&p.doc);
    return 0;
}

static int info(struct helper *h)
{
    open_collection(&h->c, h->archive);
    if (!h->c.zim) {
        printf("no collection: %s\n", h->c.why.c_str());
        return 1;
    }
    printf("archive %s\nlibzim %s\nfeatures %s\nopen_ms %lld\nentries %u\narticles %u\nmedia %u\nfulltext %s\n",
           h->c.path.c_str(), LIBZIM_VERSION, features(), (long long)h->c.open_ms, h->c.zim->getEntryCount(),
           h->c.zim->getArticleCount(), h->c.zim->getMediaCount(), full_text_available(*h->c.zim) ? "yes" : "no");
    for (const char *k : { "Title", "Date", "Source", "Creator", "Publisher", "Language", "Name", "Flavour", "License" }) {
        printf("meta %s = %s\n", k, meta(*h->c.zim, k).c_str());
    }
    return 0;
}

/* Cold numbers for one process: open, then for each query a search, the
 * first title result opened, and its pictures decoded. */
static int bench(struct helper *h, int argc, char **argv, int first)
{
    int64_t t0 = mono_ms();

    open_collection(&h->c, h->archive);
    if (!h->c.zim) {
        printf("no collection: %s\n", h->c.why.c_str());
        return 1;
    }
    printf("open_ms %lld peak_kb %ld\n", (long long)(mono_ms() - t0), peak_kb());
    for (int i = first; i < argc; i++) {
        struct page s;
        struct page a;
        int64_t t1 = mono_ms();
        std::string url = address_of(argv[i]);
        std::string target;

        web_doc_init(&s.doc);
        s.search_title_ms = s.search_text_ms = -1;
        make_page(&h->c, h->archive, url.c_str(), &s);
        printf("search \"%s\": %lld ms (titles %lld ms, %d shown; text %lld ms, %d shown)\n", argv[i],
               (long long)(mono_ms() - t1), (long long)s.search_title_ms, s.title_hits, (long long)s.search_text_ms,
               s.text_hits);
        for (size_t l = 0; l < s.doc.nlinks && target.empty(); l++) {
            size_t n;
            const char *u = web_doc_link_url(&s.doc, (int)l, &n);

            target.assign(u, n);
        }
        web_doc_free(&s.doc);
        if (target.empty()) {
            continue;
        }
        web_doc_init(&a.doc);
        t1 = mono_ms();
        make_page(&h->c, h->archive, target.c_str(), &a);
        int64_t open_ms = mono_ms() - t1;
        int64_t t2 = mono_ms();
        int shown = 0;
        size_t budget = PAGE_PIXELS_MAX;

        for (size_t k = 0; a.ok && k < a.doc.nimages; k++) {
            struct web_pixels px;
            std::string why;

            memset(&px, 0, sizeof(px));
            if (picture(*h->c.zim, &a.doc, k, 528, budget, &px, why) == WEB_IMAGE_OK) {
                shown++;
                budget -= (size_t)px.w * (size_t)px.h;
            }
            free(px.px);
        }
        printf("  open \"%s\": %lld ms, %zu blocks, %zu links; %d/%zu pictures decoded in %lld ms; peak_kb %ld\n",
               a.doc.title, (long long)open_ms, a.doc.nblocks, a.doc.nlinks, shown, a.doc.nimages,
               (long long)(mono_ms() - t2), peak_kb());
        web_doc_free(&a.doc);
    }
    printf("total_ms %lld peak_kb %ld\n", (long long)(mono_ms() - t0), peak_kb());
    print_memory();
    return 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: pos-library session [--archive FILE] [--images DIR]\n"
                    "       pos-library info [--archive FILE]\n"
                    "       pos-library dump [--archive FILE] ADDRESS|WORDS\n"
                    "       pos-library bench [--archive FILE] WORDS...\n");
    return 2;
}

int main(int argc, char **argv)
{
    struct helper h;
    struct sigaction sa;
    const char *mode;
    int i;

    if (argc < 2) {
        return usage();
    }
    h.archive = NULL;
    h.img_dir = NULL;
    h.in_len = 0;
    h.in_over = h.quit = h.have_open = h.active = h.stop = false;
    h.c.tried = false;
    h.c.open_ms = 0;
    mode = argv[1];
    for (i = 2; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "--archive") == 0 && i + 1 < argc) {
            h.archive = argv[++i];
        } else if (strcmp(argv[i], "--images") == 0 && i + 1 < argc) {
            h.img_dir = argv[++i];
        } else {
            return usage();
        }
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    pocketlog_init("pos-library");
    if (strcmp(mode, "session") == 0 && i == argc) {
        if (h.img_dir) {
            struct stat st;

            /* The shell made it for this session; anything else is not used. */
            if (lstat(h.img_dir, &st) != 0 || !S_ISDIR(st.st_mode) || (st.st_mode & 077) != 0) {
                LOG_WARN("library: image directory refused, no pictures this session");
                h.img_dir = NULL;
            }
        }
        return session(&h);
    }
    if (strcmp(mode, "info") == 0 && i == argc) {
        return info(&h);
    }
    if (strcmp(mode, "dump") == 0 && i + 1 == argc) {
        return dump(&h, argv[i]);
    }
    if (strcmp(mode, "bench") == 0 && i < argc) {
        return bench(&h, argc, argv, i);
    }
    return usage();
}
