/*
 * PocketNotes persistence: names, listing, the atomic write, and what
 * happens to a note that is not text.
 *
 * Runs against a temporary POCKETOS_STATE_DIR, so it never touches a real
 * store.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "notes_store.h"
#include "notes_view.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;
static int checks;
static char root[128];

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void check_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want);
    }
}

static void wipe(void)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        /* Nothing to remove on the first call. */
    }
}

/* Write a file into the store directly, to make states the app cannot. */
static void plant(const char *name, const void *data, size_t len)
{
    char path[256];
    FILE *f;

    mkdir(root, 0755);
    snprintf(path, sizeof(path), "%s/notes", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/notes/%s", root, name);
    f = fopen(path, "wb");
    if (f) {
        if (len) {
            (void)!fwrite(data, 1, len, f);
        }
        fclose(f);
    }
}

static int file_exists(const char *name)
{
    char path[256];
    struct stat st;

    snprintf(path, sizeof(path), "%s/notes/%s", root, name);
    return stat(path, &st) == 0;
}

int main(void)
{
    struct notes_entry list[NOTES_MAX_NOTES];
    char text[NOTES_MAX_BYTES + 1];
    char path[256];
    char big[NOTES_MAX_BYTES + 64];
    int n;

    snprintf(root, sizeof(root), "/tmp/pocketnotes-test-%u", (unsigned)getpid());
    setenv("POCKETOS_STATE_DIR", root, 1);
    wipe();

    /* ---- 1. paths are generated, never taken from a title -------------- */

    check("the store lives under the state dir",
          strstr(notes_store_dir(), root) != NULL &&
              strstr(notes_store_dir(), "/notes") != NULL);
    check("a path is built", notes_store_path(7, path, sizeof(path)) == 0);
    check_str("and is a generated name", strrchr(path, '/') + 1, "note-00000007.txt");
    check("a path that would not fit is refused",
          notes_store_path(7, path, 8) == -1);

    /* ---- 2. an absent store is empty, not broken ----------------------- */

    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("no directory yet means no notes", n == 0);
    check("the first id is 1", notes_store_next_id() == 1);
    check("reading a note that is not there says so",
          notes_store_read(1, text, sizeof(text)) == 1);

    /* ---- 3. write, read back, list ------------------------------------- */

    check("a note is written", notes_store_write(1, "Shopping\nmilk\n") == 0);
    check("the file is where the path says",
          file_exists("note-00000001.txt"));
    check("it reads back the same length",
          notes_store_read(1, text, sizeof(text)) == 14);
    check_str("and the same bytes", text, "Shopping\nmilk\n");

    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("one note is listed", n == 1);
    check("with its id", n == 1 && list[0].id == 1);
    check("marked readable", n == 1 && list[0].readable == 1);
    check_str("and titled by its first line", n == 1 ? list[0].title : "", "Shopping");
    check("and carrying a modification time", n == 1 && list[0].modified > 0);

    check("the next id skips the one in use", notes_store_next_id() == 2);

    /* ---- 4. the write is atomic and leaves no litter ------------------- */

    check("a rewrite replaces in place", notes_store_write(1, "Shopping v2") == 0);
    check_str("with the new text",
              (notes_store_read(1, text, sizeof(text)), text), "Shopping v2");
    check("no temporary file is left behind", !file_exists("note-00000001.txt.tmp"));

    /* A temporary left by an interrupted write is not a note: it is not
     * listed, and the next write overwrites it. */
    plant("note-00000001.txt.tmp", "half writ", 9);
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("a stale temporary is not listed as a note", n == 1);
    check("the note itself is untouched",
          notes_store_read(1, text, sizeof(text)) == 11);
    check("a stale temporary does not take an id", notes_store_next_id() == 2);
    check("and the next write clears it", notes_store_write(1, "again") == 0);
    check("temporary gone", !file_exists("note-00000001.txt.tmp"));

    /* ---- 5. empty and blank notes ------------------------------------- */

    check("an empty note may be stored", notes_store_write(2, "") == 0);
    check("and reads back as nothing", notes_store_read(2, text, sizeof(text)) == 0);
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("an empty note still lists", n == 2);
    {
        int i;
        int found = 0;

        for (i = 0; i < n; i++) {
            if (list[i].id == 2) {
                found = 1;
                check_str("titled Untitled", list[i].title, NOTES_UNTITLED);
            }
        }
        check("the empty note was found", found);
    }

    /* ---- 6. newest first ---------------------------------------------- */

    sleep(1); /* mtime has one-second resolution */
    check("a third note is written", notes_store_write(3, "Newest") == 0);
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("three notes", n == 3);
    check("the newest is first", n == 3 && list[0].id == 3);

    /* ---- 7. what is not text is refused, not mangled ------------------- */

    plant("note-00000010.txt", "bad \xFF byte", 11);
    check("a file that is not UTF-8 is refused",
          notes_store_read(10, text, sizeof(text)) == -1);
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("but it is still listed, so it is visibly still there", n == 4);
    {
        int i;

        for (i = 0; i < n; i++) {
            if (list[i].id == 10) {
                check("marked unreadable", list[i].readable == 0);
                check("with a title that says so",
                      strstr(list[i].title, "nreadable") != NULL);
            }
        }
    }
    check("an unreadable note still holds its id", notes_store_next_id() != 10);

    plant("note-00000011.txt", "has\0a nul", 9);
    check("a file with an embedded NUL is refused",
          notes_store_read(11, text, sizeof(text)) == -1);

    /* ---- 8. size cap --------------------------------------------------- */

    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    check("a note larger than the cap is refused rather than truncated",
          notes_store_write(12, big) == -1);
    check("and nothing was written", !file_exists("note-00000012.txt"));

    big[NOTES_MAX_BYTES] = '\0';
    check("a note exactly at the cap is accepted", notes_store_write(12, big) == 0);
    check("and reads back whole",
          notes_store_read(12, text, sizeof(text)) == NOTES_MAX_BYTES);

    /* Planted over the cap: refused on read, so a save can never shorten it. */
    memset(big, 'b', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    plant("note-00000013.txt", big, sizeof(big) - 1);
    check("a file over the cap is refused on read",
          notes_store_read(13, text, sizeof(text)) == -1);

    /* ---- 9. names this store did not make are ignored ------------------ */

    plant("notes.txt", "x", 1);
    plant("note-1.txt", "x", 1);
    plant("note-00000001.md", "x", 1);
    plant("README", "x", 1);
    n = notes_store_list(list, NOTES_MAX_NOTES);
    /* ids 1, 2, 3, 10, 11, 12 and 13 exist by now; the four names above
     * are not notes and must not be counted. */
    check("foreign names are not notes", n == 7);

    /* ---- 10. delete ---------------------------------------------------- */

    check("a note is deleted", notes_store_delete(1) == 0);
    check("the file is gone", !file_exists("note-00000001.txt"));
    check("deleting again is not an error", notes_store_delete(1) == 0);
    check("deleting one that never existed is not an error",
          notes_store_delete(999) == 0);
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("one fewer note", n == 6);
    check("and its id is free again", notes_store_next_id() == 1);

    wipe();
    printf("notes_store_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
