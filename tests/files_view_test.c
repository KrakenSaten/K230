/*
 * Files: what an entry says on screen (apps/files/files_view.h).
 * Run with TZ=UTC so a time reads the same on every host.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "files_view.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

static void size_is(int64_t bytes, const char *want)
{
    char got[32];
    char what[64];

    files_view_size(got, sizeof(got), bytes);
    snprintf(what, sizeof(what), "size %lld", (long long)bytes);
    check_str(what, got, want);
}

static struct files_entry entry(const char *name, enum files_kind kind, int64_t size, int64_t mtime)
{
    struct files_entry e;

    memset(&e, 0, sizeof(e));
    snprintf(e.name, sizeof(e.name), "%s", name);
    e.kind = kind;
    e.size = size;
    e.mtime = mtime;
    return e;
}

static void type_is(const struct files_entry *e, const char *want)
{
    char got[48];

    files_view_type(got, sizeof(got), e);
    check_str(e->name, got, want);
}

int main(void)
{
    char out[FILES_PATH_MAX + 8];
    struct files_entry e;

    /* ---- sizes ---- */
    size_is(-1, "");
    size_is(0, "0 B");
    size_is(1023, "1023 B");
    size_is(1024, "1.0 KB");
    size_is(1536, "1.5 KB");
    size_is(10180, "9.9 KB");
    size_is(10200, "10 KB");     /* 9.96: rounds up, and says so without a decimal */
    size_is(123456, "121 KB");
    size_is(5 * 1024 * 1024, "5.0 MB");
    size_is((int64_t)3 * 1024 * 1024 * 1024, "3.0 GB");

    /* ---- types ---- */
    e = entry("notes.txt", FILES_KIND_FILE, 10, 0);
    type_is(&e, "TXT file");
    e = entry("archive.tar.gz", FILES_KIND_FILE, 10, 0);
    type_is(&e, "GZ file");
    e = entry("README", FILES_KIND_FILE, 10, 0);
    type_is(&e, "File");
    e = entry(".profile", FILES_KIND_FILE, 10, 0);
    type_is(&e, "File");
    e = entry("trailing.", FILES_KIND_FILE, 10, 0);
    type_is(&e, "File");
    e = entry("odd.ex-t", FILES_KIND_FILE, 10, 0);
    type_is(&e, "File");
    e = entry("a.verylongextension", FILES_KIND_FILE, 10, 0);
    type_is(&e, "File");
    e = entry("docs", FILES_KIND_DIR, -1, 0);
    type_is(&e, "Folder");
    e = entry("tty0", FILES_KIND_OTHER, -1, 0);
    type_is(&e, "Device or pipe");
    e = entry("l", FILES_KIND_LINK, -1, 0);
    type_is(&e, "Link");
    e.link_dir = true;
    type_is(&e, "Link to folder");
    e.link_dir = false;
    e.link_broken = true;
    type_is(&e, "Broken link");

    /* ---- time and captions ---- */
    files_view_when(out, sizeof(out), 0);
    check_str("an unknown time is left out", out, "");
    files_view_when(out, sizeof(out), 1790000000); /* 2026-09-21 14:13:20 UTC */
    check_str("a time", out, "2026-09-21 14:13");
    e = entry("a.txt", FILES_KIND_FILE, 2048, 1790000000);
    files_view_caption(out, sizeof(out), &e);
    check_str("a file's caption is its size and time", out, "2.0 KB \xC2\xB7 2026-09-21 14:13");
    e = entry("d", FILES_KIND_DIR, -1, 1790000000);
    files_view_caption(out, sizeof(out), &e);
    check_str("a folder's caption is its type and time", out, "Folder \xC2\xB7 2026-09-21 14:13");
    e = entry("d", FILES_KIND_DIR, -1, 0);
    files_view_caption(out, sizeof(out), &e);
    check_str("with no time, only the first part", out, "Folder");

    /* ---- names made drawable ---- */
    files_view_name(out, sizeof(out), "plain.txt");
    check_str("a plain name is unchanged", out, "plain.txt");
    files_view_name(out, sizeof(out), "bl\xC3\xA5" "b\xC3\xA6r.txt");
    check_str("UTF-8 is kept", out, "bl\xC3\xA5" "b\xC3\xA6r.txt");
    files_view_name(out, sizeof(out), "bad\xFF\xC3name\x01.txt");
    check_str("bytes that are not UTF-8, and controls, become ?", out, "bad??name?.txt");
    files_view_name(out, 6, "\xC3\xA6\xC3\xB8\xC3\xA5");
    check_str("a short buffer never ends in half a character", out, "\xC3\xA6\xC3\xB8");

    /* ---- the path bar ---- */
    files_view_path(out, sizeof(out), "/root/docs", 40);
    check_str("a path that fits is shown whole", out, "/root/docs");
    files_view_path(out, sizeof(out), "/root/documents/projects/doors/notes", 20);
    check_str("a long path keeps its end", out, "\xE2\x80\xA6/doors/notes");
    files_view_path(out, sizeof(out), "/a/averyveryveryverylongfoldername", 10);
    check_str("the last folder is kept whole even when alone it is longer", out,
              "\xE2\x80\xA6/averyveryveryverylongfoldername");
    files_view_path(out, sizeof(out), "/", 10);
    check_str("the top", out, "/");

    check_str("sort names", files_view_sort_name(FILES_SORT_DATE), "Date");

    printf("files_view_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
