/*
 * Files: navigation, the write policy and every operation, against a real
 * temporary tree (apps/files/files_fs.h, files_job.h).
 *
 * The point of most checks here is what is left on disk: that a refused or
 * failed operation changed nothing, that a copy never leaves a half-made
 * result behind, that nothing is ever replaced, and that the places the
 * policy protects cannot be changed however they are reached.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "files_fs.h"
#include "files_job.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failed;
static int checks;
static char root[128];
static char W[512];    /* the writable root */
static char SYS[512];  /* outside every writable root */
static char W2[512];   /* a second writable root, with nothing protected in it */

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void check_err(const char *what, int got, int want)
{
    checks++;
    if (got != want) {
        failed++;
        printf("FAIL %s: got %d (%s), want %d (%s)\n", what, got, files_strerror(got), want,
               files_strerror(want));
    }
}

static const char *at(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* A path under the test root; four rotating buffers, enough for one call. */
static const char *at(const char *fmt, ...)
{
    static char buf[4][1100];
    static int k;
    char rel[900];
    va_list ap;

    k = (k + 1) % 4;
    va_start(ap, fmt);
    vsnprintf(rel, sizeof(rel), fmt, ap);
    va_end(ap);
    snprintf(buf[k], sizeof(buf[k]), "%s/%s", root, rel);
    return buf[k];
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (!f) {
        printf("FAIL cannot write fixture %s: %s\n", path, strerror(errno));
        failed++;
        return;
    }
    fputs(text, f);
    fclose(f);
}

/* The whole file, or "" when it cannot be read. */
static const char *get(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n;

    buf[0] = '\0';
    if (!f) {
        return "(missing)";
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int exists(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

static void mk(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        printf("FAIL cannot make fixture %s: %s\n", path, strerror(errno));
        failed++;
    }
}

static void lnk(const char *target, const char *path)
{
    if (symlink(target, path) != 0) {
        printf("FAIL cannot make fixture link %s: %s\n", path, strerror(errno));
        failed++;
    }
}

static void set_mtime(const char *path, time_t t)
{
    struct timespec ts[2] = { { t, 0 }, { t, 0 } };

    utimensat(AT_FDCWD, path, ts, AT_SYMLINK_NOFOLLOW);
}

/* Entries in dir whose names start with prefix: a copy's leftovers. */
static int count_prefixed(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, prefix, strlen(prefix)) == 0) {
            n++;
        }
    }
    closedir(d);
    return n;
}

static int find(const struct files_dir *d, const char *name)
{
    int i;

    for (i = 0; i < d->n; i++) {
        if (strcmp(d->entries[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void wipe(void)
{
    char cmd[600];

    snprintf(cmd, sizeof(cmd), "chmod -R u+rwx '%s' 2>/dev/null; rm -rf '%s'", root, root);
    if (system(cmd) != 0) {
        /* nothing there yet */
    }
}

/* ---- 1. paths and names ------------------------------------------------------ */

static void test_paths(void)
{
    char out[FILES_PATH_MAX];
    char name[FILES_NAME_MAX + 2];

    files_path_join(out, sizeof(out), "/", "etc");
    check("join under the top", strcmp(out, "/etc") == 0);
    files_path_join(out, sizeof(out), "/root", "a b");
    check("join", strcmp(out, "/root/a b") == 0);
    check("join refuses to overflow", files_path_join(out, 8, "/root", "long-name") == -ENAMETOOLONG);

    check("parent", files_path_parent(out, sizeof(out), "/root/docs") == 0 && strcmp(out, "/root") == 0);
    check("parent of a top folder is the top",
          files_path_parent(out, sizeof(out), "/root") == 0 && strcmp(out, "/") == 0);
    check("the top has no parent", files_path_parent(out, sizeof(out), "/") != 0);
    check("base", strcmp(files_path_base("/root/docs"), "docs") == 0);
    check("base of the top", strcmp(files_path_base("/"), "/") == 0);

    check("clean", files_path_clean(out, sizeof(out), "//root/./docs//x/../y/") == 0 &&
                       strcmp(out, "/root/docs/y") == 0);
    check("clean never climbs above the top",
          files_path_clean(out, sizeof(out), "/../../etc") == 0 && strcmp(out, "/etc") == 0);
    check("clean of the top", files_path_clean(out, sizeof(out), "/") == 0 && strcmp(out, "/") == 0);
    check("a relative path is refused", files_path_clean(out, sizeof(out), "root") == -EINVAL);

    check("within: itself", files_path_within("/root", "/root"));
    check("within: below", files_path_within("/root/a", "/root"));
    check("within: a longer name is not below", !files_path_within("/rootkit", "/root"));
    check("within: everything is below the top", files_path_within("/etc", "/"));

    check("a plain name is fine", files_name_problem("Report 2026.txt") == NULL);
    check("Norwegian letters are fine", files_name_problem("bl\xC3\xA5" "b\xC3\xA6r") == NULL);
    check("empty is refused", files_name_problem("") != NULL);
    check(". is refused", files_name_problem(".") != NULL);
    check(".. is refused", files_name_problem("..") != NULL);
    check("a slash is refused", files_name_problem("a/b") != NULL);
    check("a leading space is refused", files_name_problem(" a") != NULL);
    check("a trailing space is refused", files_name_problem("a ") != NULL);
    check("a control character is refused", files_name_problem("a\tb") != NULL);
    check("bytes that are not UTF-8 are refused", files_name_problem("a\xFF") != NULL);
    memset(name, 'n', FILES_NAME_MAX);
    name[FILES_NAME_MAX] = '\0';
    check("255 bytes is a name", files_name_problem(name) == NULL);
    name[FILES_NAME_MAX] = 'n';
    name[FILES_NAME_MAX + 1] = '\0';
    check("256 bytes is not", files_name_problem(name) != NULL);
}

/* ---- 2. listing and sorting ---------------------------------------------------- */

static void test_listing(void)
{
    struct files_dir d;
    int i;

    mk(at("w/list"));
    mk(at("w/list/Beta"));
    mk(at("w/list/alpha"));
    put(at("w/list/c.txt"), "12345");
    put(at("w/list/B.md"), "1");
    put(at("w/list/a.zip"), "1234567890");
    lnk("alpha", at("w/list/to-alpha"));
    lnk("c.txt", at("w/list/to-c"));
    lnk("nowhere", at("w/list/broken"));
    set_mtime(at("w/list/c.txt"), 1000);
    set_mtime(at("w/list/B.md"), 3000);
    set_mtime(at("w/list/a.zip"), 2000);
    set_mtime(at("w/list/to-c"), 500);
    set_mtime(at("w/list/broken"), 500);

    check("a folder is read", files_dir_read(&d, at("w/list")) == 0);
    check("every entry but . and ..", d.n == 8 && d.total == 8);
    i = find(&d, "alpha");
    check("a folder is a folder", i >= 0 && d.entries[i].kind == FILES_KIND_DIR);
    i = find(&d, "c.txt");
    check("a file has its size and time",
          i >= 0 && d.entries[i].kind == FILES_KIND_FILE && d.entries[i].size == 5 &&
              d.entries[i].mtime == 1000);
    i = find(&d, "to-alpha");
    check("a link to a folder opens like one",
          i >= 0 && d.entries[i].kind == FILES_KIND_LINK && files_entry_is_dir(&d.entries[i]));
    i = find(&d, "to-c");
    check("a link to a file takes the file's size",
          i >= 0 && !files_entry_is_dir(&d.entries[i]) && d.entries[i].size == 5);
    i = find(&d, "broken");
    check("a broken link says so", i >= 0 && d.entries[i].link_broken);

    files_sort(d.entries, d.n, FILES_SORT_NAME);
    check("by name: folders first, case does not matter",
          strcmp(d.entries[0].name, "alpha") == 0 && strcmp(d.entries[1].name, "Beta") == 0 &&
              strcmp(d.entries[2].name, "to-alpha") == 0 && strcmp(d.entries[3].name, "a.zip") == 0 &&
              strcmp(d.entries[4].name, "B.md") == 0 && strcmp(d.entries[5].name, "broken") == 0);
    files_sort(d.entries, d.n, FILES_SORT_SIZE);
    check("by size: folders first, then the largest",
          files_entry_is_dir(&d.entries[2]) && strcmp(d.entries[3].name, "a.zip") == 0 &&
              d.entries[4].size == 5);
    files_sort(d.entries, d.n, FILES_SORT_DATE);
    check("by date: the newest file first", strcmp(d.entries[3].name, "B.md") == 0 &&
                                                strcmp(d.entries[4].name, "a.zip") == 0);
    files_sort(d.entries, d.n, FILES_SORT_TYPE);
    check("by type: files without an extension, then md, txt, zip",
          strcmp(d.entries[3].name, "broken") == 0 && strcmp(d.entries[5].name, "B.md") == 0 &&
              strcmp(d.entries[6].name, "c.txt") == 0 && strcmp(d.entries[7].name, "a.zip") == 0);
    files_dir_free(&d);

    check_err("a folder that is not there", files_dir_read(&d, at("w/nothing")), -ENOENT);
    check("and nothing is left to free", d.entries == NULL && d.n == 0);
    check_err("a file is not a folder", files_dir_read(&d, at("w/list/c.txt")), -ENOTDIR);
    mk(at("w/empty"));
    check("an empty folder is read", files_dir_read(&d, at("w/empty")) == 0 && d.n == 0);
    files_dir_free(&d);
    if (geteuid() != 0) {
        mk(at("w/locked"));
        chmod(at("w/locked"), 0);
        check_err("a folder that may not be read", files_dir_read(&d, at("w/locked")), -EACCES);
        chmod(at("w/locked"), 0755);
    }
}

/* ---- 3. the write policy --------------------------------------------------------- */

static struct files_policy pol;

static void test_policy(void)
{
    const struct files_policy *def;

    mk(at("w/doors"));
    mk(at("w/doors/state"));
    put(at("w/doors/state/settings.conf"), "x=1\n");
    mk(at("w/keys"));
    lnk(SYS, at("w/to-sys"));
    put(at("sys/config"), "keep me\n");

    check("an entry in a writable root may change",
          files_policy_entry(&pol, at("w/list/c.txt")) == FILES_ACCESS_OK);
    check("a writable root itself may not", files_policy_entry(&pol, W2) == FILES_ACCESS_ROOT);
    check("and one with Doors' data in it is protected for that",
          files_policy_entry(&pol, W) == FILES_ACCESS_DOORS);
    check("but things may be made in it", files_policy_dir(&pol, W) == FILES_ACCESS_OK);
    check("outside every root is the system", files_policy_entry(&pol, at("sys/config")) == FILES_ACCESS_SYSTEM);
    check("and nothing may be made there", files_policy_dir(&pol, SYS) == FILES_ACCESS_SYSTEM);
    check("a protected folder may not change",
          files_policy_entry(&pol, at("w/doors/state")) == FILES_ACCESS_DOORS);
    check("nor anything inside it",
          files_policy_entry(&pol, at("w/doors/state/settings.conf")) == FILES_ACCESS_DOORS);
    check("nor may anything be made inside it",
          files_policy_dir(&pol, at("w/doors/state")) == FILES_ACCESS_DOORS);
    check("nor the folder around it, which would take it along",
          files_policy_entry(&pol, at("w/doors")) == FILES_ACCESS_DOORS);
    check("a link in a root is judged where it is: it may be removed",
          files_policy_entry(&pol, at("w/to-sys")) == FILES_ACCESS_OK);
    check("but going through it leads outside, where nothing may be made",
          files_policy_dir(&pol, at("w/to-sys")) == FILES_ACCESS_SYSTEM);
    check("nor changed", files_policy_entry(&pol, at("w/to-sys/config")) == FILES_ACCESS_SYSTEM);
    check("dot-dot does not climb out of a root",
          files_policy_dir(&pol, at("w/list/../../sys")) == FILES_ACCESS_SYSTEM);
    check("what is not there is said to be missing",
          files_policy_dir(&pol, at("w/nothing/deeper")) == FILES_ACCESS_MISSING);
    check("the top of the filesystem", files_policy_entry(&pol, "/") != FILES_ACCESS_OK &&
                                           files_policy_dir(&pol, "/") != FILES_ACCESS_OK);
    check("every refusal has a caption", files_access_text(FILES_ACCESS_DOORS)[0] != '\0');

    /* The default: Doors' own directories wherever the environment puts
     * them, and the system everywhere outside the five roots. */
    setenv("POCKETOS_STATE_DIR", at("w/doors-env"), 1);
    mk(at("w/doors-env"));
    def = files_policy_default();
    check("default: Doors' state directory is protected",
          files_policy_entry(def, at("w/doors-env")) == FILES_ACCESS_DOORS);
    check("default: and what is beside it in /tmp is not",
          files_policy_entry(def, at("w/list/c.txt")) == FILES_ACCESS_OK);
    check("default: /etc is the system", files_policy_dir(def, "/etc") == FILES_ACCESS_SYSTEM);
    check("default: /usr is the system", files_policy_entry(def, "/usr") != FILES_ACCESS_OK);
    check("default: /tmp itself may not change", files_policy_entry(def, "/tmp") != FILES_ACCESS_OK);
    check("default: a NULL policy is the default",
          files_policy_entry(NULL, at("w/doors-env")) == FILES_ACCESS_DOORS);
}

/* ---- 4. create and rename ------------------------------------------------------ */

static void test_mkdir_rename(void)
{
    struct stat st;

    check_err("a new folder", files_mkdir(&pol, W, "made"), 0);
    check("is there", stat(at("w/made"), &st) == 0 && S_ISDIR(st.st_mode));
    check_err("the same name again", files_mkdir(&pol, W, "made"), -EEXIST);
    check_err("a name that is not one", files_mkdir(&pol, W, "a/b"), -FILES_ENAME);
    check("and nothing was made", !exists(at("w/a")));
    check_err("in the system", files_mkdir(&pol, SYS, "x"), -FILES_EPOLICY);
    check("and nothing was made there", !exists(at("sys/x")));
    check_err("in a protected folder", files_mkdir(&pol, at("w/doors/state"), "x"), -FILES_EPOLICY);
    check_err("in a folder that is not there", files_mkdir(&pol, at("w/nothing"), "x"), -ENOENT);

    put(at("w/one.txt"), "one");
    put(at("w/two.txt"), "two");
    check_err("rename", files_rename(&pol, at("w/one.txt"), "uno.txt"), 0);
    check("the old name is gone and the new one has the bytes",
          !exists(at("w/one.txt")) && strcmp(get(at("w/uno.txt")), "one") == 0);
    check_err("rename onto a name that is taken", files_rename(&pol, at("w/uno.txt"), "two.txt"),
              -EEXIST);
    check("replaces nothing: both files are as they were",
          strcmp(get(at("w/uno.txt")), "one") == 0 && strcmp(get(at("w/two.txt")), "two") == 0);
    check_err("rename to the name it has", files_rename(&pol, at("w/two.txt"), "two.txt"), 0);
    check_err("rename to a name that is not one", files_rename(&pol, at("w/two.txt"), ".."),
              -FILES_ENAME);
    check_err("rename what is not there", files_rename(&pol, at("w/gone.txt"), "x"), -ENOENT);
    check_err("rename in the system", files_rename(&pol, at("sys/config"), "x"), -FILES_EPOLICY);
    check("the system file is untouched", strcmp(get(at("sys/config")), "keep me\n") == 0);
    check_err("rename Doors' data", files_rename(&pol, at("w/doors/state/settings.conf"), "x"),
              -FILES_EPOLICY);
    check_err("rename the folder that holds it", files_rename(&pol, at("w/doors"), "x"),
              -FILES_EPOLICY);
    check_err("rename a writable root", files_rename(&pol, W, "x"), -FILES_EPOLICY);
    check("a folder is renamed like a file", files_rename(&pol, at("w/made"), "made2") == 0 &&
                                                 exists(at("w/made2")));
}

/* ---- 5. copy ----------------------------------------------------------------------- */

static void test_copy(void)
{
    char name[FILES_NAME_MAX + 1];
    char longname[FILES_NAME_MAX + 1];
    struct stat a;
    struct stat b;
    atomic_int stop = 1;

    mk(at("w/src"));
    put(at("w/src/doc.txt"), "hello");
    chmod(at("w/src/doc.txt"), 0640);
    set_mtime(at("w/src/doc.txt"), 1234567);
    mk(at("w/dst"));

    check_err("copy a file", files_copy(&pol, at("w/src/doc.txt"), at("w/dst"), name, sizeof(name), NULL), 0);
    check("under its own name", strcmp(name, "doc.txt") == 0);
    stat(at("w/src/doc.txt"), &a);
    stat(at("w/dst/doc.txt"), &b);
    check("with its bytes, mode and time",
          strcmp(get(at("w/dst/doc.txt")), "hello") == 0 && (b.st_mode & 0777) == 0640 &&
              b.st_mtime == 1234567);
    check("the source is as it was", strcmp(get(at("w/src/doc.txt")), "hello") == 0 && a.st_mtime == 1234567);

    put(at("w/dst/doc.txt"), "changed");
    check_err("copy where the name is taken", files_copy(&pol, at("w/src/doc.txt"), at("w/dst"), name,
                                                         sizeof(name), NULL), 0);
    check("gets a number before the extension", strcmp(name, "doc (2).txt") == 0);
    check("and replaces nothing", strcmp(get(at("w/dst/doc.txt")), "changed") == 0 &&
                                      strcmp(get(at("w/dst/doc (2).txt")), "hello") == 0);
    files_copy(&pol, at("w/src/doc.txt"), at("w/dst"), name, sizeof(name), NULL);
    check("then the next number", strcmp(name, "doc (3).txt") == 0);
    files_copy(&pol, at("w/src/doc.txt"), at("w/src"), name, sizeof(name), NULL);
    check("a copy into its own folder is a duplicate", strcmp(name, "doc (2).txt") == 0 &&
                                                          exists(at("w/src/doc.txt")));

    /* A tree, with a link in it that must be copied as a link. */
    mk(at("w/src/tree"));
    mk(at("w/src/tree/sub"));
    put(at("w/src/tree/sub/deep.txt"), "deep");
    lnk("../../doc.txt", at("w/src/tree/sub/link"));
    lnk(SYS, at("w/src/tree/outside"));
    check_err("copy a folder", files_copy(&pol, at("w/src/tree"), at("w/dst"), name, sizeof(name), NULL), 0);
    check("with everything in it", strcmp(get(at("w/dst/tree/sub/deep.txt")), "deep") == 0);
    check("links are copied as links, not followed",
          lstat(at("w/dst/tree/outside"), &b) == 0 && S_ISLNK(b.st_mode) &&
              lstat(at("w/dst/tree/sub/link"), &b) == 0 && S_ISLNK(b.st_mode));
    check("no partial copy is left behind", count_prefixed(at("w/dst"), ".files-partial") == 0);
    files_copy(&pol, at("w/src/tree"), at("w/dst"), name, sizeof(name), NULL);
    check("a folder's number goes at the end", strcmp(name, "tree (2)") == 0);

    check_err("a folder into itself", files_copy(&pol, at("w/src/tree"), at("w/src/tree/sub"), name,
                                                 sizeof(name), NULL), -FILES_EINSIDE);
    check("made nothing", !exists(at("w/src/tree/sub/tree")) &&
                              count_prefixed(at("w/src/tree/sub"), ".files-partial") == 0);
    check_err("into the system", files_copy(&pol, at("w/src/doc.txt"), SYS, name, sizeof(name), NULL),
              -FILES_EPOLICY);
    check("made nothing there", !exists(at("sys/doc.txt")));
    check_err("into Doors' data", files_copy(&pol, at("w/src/doc.txt"), at("w/doors/state"), name,
                                             sizeof(name), NULL), -FILES_EPOLICY);
    check_err("from the system is allowed: reading is",
              files_copy(&pol, at("sys/config"), at("w/dst"), name, sizeof(name), NULL), 0);
    check_err("what is not there", files_copy(&pol, at("w/src/none"), at("w/dst"), name, sizeof(name), NULL),
              -ENOENT);

    mkfifo(at("w/src/pipe"), 0600);
    check_err("a pipe is not copied", files_copy(&pol, at("w/src/pipe"), at("w/dst"), name, sizeof(name), NULL),
              -FILES_ESPECIAL);
    check("and leaves nothing", !exists(at("w/dst/pipe")) && count_prefixed(at("w/dst"), ".files-partial") == 0);
    mkfifo(at("w/src/tree/sub/pipe"), 0600);
    check_err("a folder with a pipe in it is not copied",
              files_copy(&pol, at("w/src/tree"), at("w/empty"), name, sizeof(name), NULL), -FILES_ESPECIAL);
    check("and what was copied before the pipe is gone again",
          count_prefixed(at("w/empty"), "") == 2 /* . and .. */);
    unlink(at("w/src/tree/sub/pipe"));

    check_err("a copy told to stop", files_copy(&pol, at("w/src/tree"), at("w/empty"), name, sizeof(name), &stop),
              -FILES_ECANCELED);
    check("leaves nothing", count_prefixed(at("w/empty"), "") == 2);

    if (geteuid() != 0) {
        put(at("w/src/tree/sub/secret"), "s");
        chmod(at("w/src/tree/sub/secret"), 0);
        check_err("a file that may not be read stops the copy",
                  files_copy(&pol, at("w/src/tree"), at("w/empty"), name, sizeof(name), NULL), -EACCES);
        check("which leaves nothing half made", count_prefixed(at("w/empty"), "") == 2);
        check("and the source as it was", exists(at("w/src/tree/sub/secret")) &&
                                              strcmp(get(at("w/src/tree/sub/deep.txt")), "deep") == 0);
        chmod(at("w/src/tree/sub/secret"), 0600);
        unlink(at("w/src/tree/sub/secret"));
    }

    /* The longest name there can be still gets a valid numbered one. */
    memset(longname, 'x', FILES_NAME_MAX - 4);
    memcpy(longname + FILES_NAME_MAX - 4, ".txt", 5);
    put(at("w/src/%s", longname), "long");
    put(at("w/dst/%s", longname), "taken");
    check_err("copy a 255-byte name onto itself",
              files_copy(&pol, at("w/src/%s", longname), at("w/dst"), name, sizeof(name), NULL), 0);
    check("the numbered name is shortened, not refused",
          strlen(name) <= FILES_NAME_MAX && files_name_problem(name) == NULL &&
              strcmp(name + strlen(name) - 8, " (2).txt") == 0 &&
              strcmp(get(at("w/dst/%s", name)), "long") == 0);
}

/* ---- 6. move ------------------------------------------------------------------------ */

static void test_move(void)
{
    struct stat st;

    mk(at("w/mv"));
    mk(at("w/mv/to"));
    put(at("w/mv/a.txt"), "a");
    put(at("w/mv/to/b.txt"), "old b");
    put(at("w/mv/b.txt"), "new b");

    check_err("move a file", files_move(&pol, at("w/mv/a.txt"), at("w/mv/to"), NULL), 0);
    check("it is there and not here", strcmp(get(at("w/mv/to/a.txt")), "a") == 0 && !exists(at("w/mv/a.txt")));
    check_err("move onto a name that is taken", files_move(&pol, at("w/mv/b.txt"), at("w/mv/to"), NULL),
              -EEXIST);
    check("replaces nothing", strcmp(get(at("w/mv/to/b.txt")), "old b") == 0 &&
                                  strcmp(get(at("w/mv/b.txt")), "new b") == 0);
    check_err("move to where it is", files_move(&pol, at("w/mv/b.txt"), at("w/mv"), NULL), -FILES_ESAME);
    mk(at("w/mv/folder"));
    mk(at("w/mv/folder/inner"));
    check_err("a folder into its own subfolder",
              files_move(&pol, at("w/mv/folder"), at("w/mv/folder/inner"), NULL), -FILES_EINSIDE);
    check("stays where it was", stat(at("w/mv/folder/inner"), &st) == 0);
    check_err("a folder", files_move(&pol, at("w/mv/folder"), at("w/mv/to"), NULL), 0);
    check("goes with what is in it", stat(at("w/mv/to/folder/inner"), &st) == 0);
    check_err("the system cannot be moved", files_move(&pol, at("sys/config"), at("w/mv"), NULL),
              -FILES_EPOLICY);
    check_err("nor anything into it", files_move(&pol, at("w/mv/b.txt"), SYS, NULL), -FILES_EPOLICY);
    check_err("nor Doors' data out", files_move(&pol, at("w/doors/state"), at("w/mv"), NULL),
              -FILES_EPOLICY);
    check("which is where it was", exists(at("w/doors/state/settings.conf")));
    check_err("nor the folder around it", files_move(&pol, at("w/doors"), at("w/mv"), NULL), -FILES_EPOLICY);
    check_err("what is not there", files_move(&pol, at("w/mv/none"), at("w/mv/to"), NULL), -ENOENT);
}

/* Across filesystems, where rename cannot: when this host has a /dev/shm on
 * another device, as Linux hosts do, a tree goes there by copy and removal. */
static void test_move_across(void)
{
    char shm[128];
    struct stat a;
    struct stat b;
    struct files_policy both = pol;

    snprintf(shm, sizeof(shm), "/dev/shm/files-fs-test-%ld", (long)getpid());
    if (stat("/dev/shm", &a) != 0 || stat(W, &b) != 0 || a.st_dev == b.st_dev || mkdir(shm, 0755) != 0) {
        printf("skip moving across filesystems: no second filesystem at /dev/shm\n");
        return;
    }
    both.writable[2] = shm;
    both.writable[3] = NULL;
    mk(at("w/across"));
    mk(at("w/across/sub"));
    put(at("w/across/sub/f.txt"), "far");
    check_err("move a folder to another filesystem", files_move(&both, at("w/across"), shm, NULL), 0);
    {
        char path[256];

        snprintf(path, sizeof(path), "%s/across/sub/f.txt", shm);
        check("it arrived whole", strcmp(get(path), "far") == 0);
    }
    check("and the original is gone", !exists(at("w/across")));
    check("with no partial copy left", count_prefixed(shm, ".files-partial") == 0);
    {
        char cmd[300];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", shm);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", shm);
        }
    }
}

/* A mount point inside a writable root - the USB drive at /media/usb - is a
 * root of its own: its contents can change, the folder itself cannot be
 * renamed, moved or deleted. /dev/shm stands in for the drive: a real mount
 * point. Only the policy is asked; nothing under /dev is touched. */
static void test_mount_point(void)
{
    struct files_policy dev = { { "/dev", NULL }, { NULL } };
    char inside[128];
    struct stat a;
    struct stat b;

    if (stat("/dev/shm", &a) != 0 || stat("/dev", &b) != 0 || a.st_dev == b.st_dev) {
        printf("skip mount points: /dev/shm is not a mount point here\n");
        return;
    }
    check("a mount point inside a writable root is a root itself",
          files_policy_entry(&dev, "/dev/shm") == FILES_ACCESS_ROOT);
    check("and can still be written into", files_policy_dir(&dev, "/dev/shm") == FILES_ACCESS_OK);
    snprintf(inside, sizeof(inside), "/dev/shm/files-fs-mp-%ld", (long)getpid());
    if (mkdir(inside, 0755) == 0) {
        check("what is in it can be changed", files_policy_entry(&dev, inside) == FILES_ACCESS_OK);
        rmdir(inside);
    }
    mk(at("w/plainfolder"));
    check("an ordinary folder is still an ordinary folder",
          files_policy_entry(&pol, at("w/plainfolder")) == FILES_ACCESS_OK);
    rmdir(at("w/plainfolder"));
}

/* ---- 7. delete ------------------------------------------------------------------------ */

static void test_delete(void)
{
    mk(at("w/del"));
    put(at("w/del/f"), "f");
    mk(at("w/del/tree"));
    mk(at("w/del/tree/a"));
    put(at("w/del/tree/a/b"), "b");
    mk(at("w/del/target"));
    put(at("w/del/target/keep"), "keep");
    lnk(at("w/del/target"), at("w/del/link"));

    check_err("delete a file", files_delete(&pol, at("w/del/f"), NULL), 0);
    check("it is gone", !exists(at("w/del/f")));
    check_err("delete a folder", files_delete(&pol, at("w/del/tree"), NULL), 0);
    check("with everything in it", !exists(at("w/del/tree")));
    check_err("delete a link to a folder", files_delete(&pol, at("w/del/link"), NULL), 0);
    check("removes the link, not what it leads to",
          !exists(at("w/del/link")) && strcmp(get(at("w/del/target/keep")), "keep") == 0);
    check_err("what is not there", files_delete(&pol, at("w/del/none"), NULL), -ENOENT);
    check_err("the system", files_delete(&pol, at("sys/config"), NULL), -FILES_EPOLICY);
    check("is untouched", strcmp(get(at("sys/config")), "keep me\n") == 0);
    check_err("Doors' data", files_delete(&pol, at("w/doors/state/settings.conf"), NULL), -FILES_EPOLICY);
    check_err("the folder around it", files_delete(&pol, at("w/doors"), NULL), -FILES_EPOLICY);
    check("which is all still there", strcmp(get(at("w/doors/state/settings.conf")), "x=1\n") == 0);
    check_err("a writable root", files_delete(&pol, W2, NULL), -FILES_EPOLICY);
    check("is still there", exists(W2));
    check_err("a link out of a root is removed as a link", files_delete(&pol, at("w/to-sys"), NULL), 0);
    check("and the system behind it is untouched", strcmp(get(at("sys/config")), "keep me\n") == 0);

    if (geteuid() != 0) {
        mk(at("w/del/ro"));
        put(at("w/del/ro/stuck"), "stuck");
        chmod(at("w/del/ro"), 0555);
        check_err("a file that may not be removed", files_delete(&pol, at("w/del/ro"), NULL), -EACCES);
        check("is still there, and so is its folder", strcmp(get(at("w/del/ro/stuck")), "stuck") == 0);
        chmod(at("w/del/ro"), 0755);
    }
}

/* ---- 8. reading text ------------------------------------------------------------------ */

static void test_text(void)
{
    char buf[64];
    char big[FILES_TEXT_MAX + 1];
    bool cut;
    int n;
    FILE *f;

    put(at("w/t.txt"), "line one\r\nline\ttwo\n\xC3\xA6\xFF\x01");
    n = files_read_text(at("w/t.txt"), buf, sizeof(buf), &cut);
    check("text is read", n > 0 && !cut);
    check("CR dropped, tab a space, a bad byte and a control are ?",
          strcmp(buf, "line one\nline two\n\xC3\xA6??") == 0);
    put(at("w/bin"), "");
    f = fopen(at("w/bin"), "wb");
    fwrite("ab\0cd", 1, 5, f);
    fclose(f);
    check_err("a file with NUL bytes is not text", files_read_text(at("w/bin"), buf, sizeof(buf), &cut),
              -FILES_EBINARY);
    put(at("w/empty.txt"), "");
    check("an empty file is empty text", files_read_text(at("w/empty.txt"), buf, sizeof(buf), &cut) == 0);
    check_err("a folder is not text", files_read_text(W, buf, sizeof(buf), &cut), -FILES_ESPECIAL);
    check_err("a pipe is refused, without waiting for a writer",
              files_read_text(at("w/src/pipe"), buf, sizeof(buf), &cut), -FILES_ESPECIAL);
    check_err("what is not there", files_read_text(at("w/none"), buf, sizeof(buf), &cut), -ENOENT);

    f = fopen(at("w/long.txt"), "w");
    for (n = 0; n < FILES_TEXT_MAX; n++) {
        fputs("\xC3\xA6", f); /* two bytes each: the cap falls on a character */
    }
    fclose(f);
    n = files_read_text(at("w/long.txt"), big, sizeof(big), &cut);
    check("a long file is cut at the cap, and says so", cut && n > 0 && n <= FILES_TEXT_MAX);
    check("never in the middle of a character", n % 2 == 0 && strchr(big, '?') == NULL);
}

/* ---- 9. the worker --------------------------------------------------------------------- */

static void test_job(void)
{
    struct files_job job;
    int r = 1;
    int spins;

    memset(&job, 0, sizeof(job));
    mk(at("w/job"));
    check("nothing to collect before a start", !files_job_poll(&job, &r) && !files_job_busy(&job));
    check_err("a copy on the worker", files_job_start(&job, FILES_OP_COPY, &pol, at("w/src/doc.txt"),
                                                      at("w/job")), 0);
    check("is busy until collected", files_job_busy(&job));
    check_err("a second job waits its turn",
              files_job_start(&job, FILES_OP_DELETE, &pol, at("w/src/doc.txt"), NULL), -EBUSY);
    for (spins = 0; spins < 5000 && !files_job_poll(&job, &r); spins++) {
        usleep(1000);
    }
    check("finishes", !files_job_busy(&job));
    check_err("with the operation's result", r, 0);
    check("and the copy's name", strcmp(job.out_name, "doc.txt") == 0 &&
                                     strcmp(get(at("w/job/doc.txt")), "hello") == 0);
    check_err("a refused delete on the worker", files_job_start(&job, FILES_OP_DELETE, &pol, at("sys/config"), NULL), 0);
    for (spins = 0; spins < 5000 && !files_job_poll(&job, &r); spins++) {
        usleep(1000);
    }
    check_err("reports the refusal", r, -FILES_EPOLICY);
    check("and the file is there", strcmp(get(at("sys/config")), "keep me\n") == 0);
    files_job_start(&job, FILES_OP_DELETE, &pol, at("w/job/doc.txt"), NULL);
    files_job_abandon(&job);
    check("abandoning waits for the thread", !files_job_busy(&job));
    files_job_abandon(&job);
    check("and is harmless when idle", !files_job_busy(&job));
}

int main(void)
{
    char *real;

    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(root, sizeof(root), "/tmp/files-fs-test-%ld", (long)getpid());
    wipe();
    mkdir(root, 0755);
    /* The tree is judged by its resolved path, as the policy judges it. */
    real = realpath(root, NULL);
    if (real) {
        snprintf(root, sizeof(root), "%s", real);
        free(real);
    }
    snprintf(W, sizeof(W), "%s/w", root);
    snprintf(SYS, sizeof(SYS), "%s/sys", root);
    snprintf(W2, sizeof(W2), "%s/w2", root);
    mk(W);
    mk(SYS);
    mk(W2);
    pol.writable[0] = W;
    pol.writable[1] = W2;
    pol.protect[0] = at("w/doors/state");
    pol.protect[1] = at("w/keys");
    {
        static char state[512];

        /* at() rotates its buffers: keep the protected path in one of its own. */
        snprintf(state, sizeof(state), "%s/w/doors/state", root);
        pol.protect[0] = state;
    }
    {
        static char keys[512];

        snprintf(keys, sizeof(keys), "%s/w/keys", root);
        pol.protect[1] = keys;
    }

    test_paths();
    test_listing();
    test_policy();
    test_mkdir_rename();
    test_copy();
    test_move();
    test_move_across();
    test_mount_point();
    test_delete();
    test_text();
    test_job();

    wipe();
    printf("files_fs_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
