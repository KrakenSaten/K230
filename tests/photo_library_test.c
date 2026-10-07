/*
 * Photo's library, below the screen: the gallery model as the Photo app
 * uses it (standalone - no CAMERA anywhere, kept across open, retry and every
 * view), and the safety of the one library it shares with Camera
 * (core/pocketcam/pocketcam_store.c and `pos-camera library`).
 *
 * The delete boundary is checked twice: in the store, and in the helper
 * itself, fed raw protocol lines - past the app's own client, which never
 * sends a name with a slash or a space - so the helper is shown to hold the
 * line on its own: nothing outside the photo folder, nothing but a regular
 * photo file, never a folder, never through a link, never recursive.
 *
 * No LVGL. Needs the helper's path as argv[1] (tests/pos-camera-testhooks).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "camera_gallery.h"
#include "pocketcam/pocketcam_proto.h"
#include "pocketcam/pocketcam_store.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;
static char root[128];
static char lib[256];

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

static void path_in(char *out, size_t len, const char *dir, const char *name)
{
    snprintf(out, len, "%s/%s", dir, name);
}

static void write_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");

    if (fp) {
        fputs(text, fp);
        fclose(fp);
    }
}

static void put(const char *name, const char *text)
{
    char p[600];

    path_in(p, sizeof(p), lib, name);
    write_file(p, text);
}

static bool there(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

static bool in_lib(const char *name)
{
    char p[600];

    path_in(p, sizeof(p), lib, name);
    return there(p);
}

static void wipe(const char *dir)
{
    char cmd[700];

    snprintf(cmd, sizeof(cmd), "chmod -R u+w '%s' 2>/dev/null; rm -rf '%s'", dir, dir);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", dir);
    }
}

/* ---- the model, standalone ------------------------------------------------------ */

static struct camera_gallery G;
static char names[40][CAMERA_NAME_MAX];

static struct camera_event event(enum camera_ev_kind kind)
{
    struct camera_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    return ev;
}

/* Open as the Photo app does: standalone, n photos, 6 cells a page. */
static void open_photo(int n)
{
    struct camera_event ev = event(CAMERA_EV_READY);
    int i;

    camera_gallery_init(&G);
    camera_gallery_set_standalone(&G, true);
    camera_gallery_open(&G);
    camera_gallery_set_sizes(&G, 6, 170, 528, 800, 528, 1000);
    camera_gallery_event(&G, &ev, 0);
    for (i = 0; i < n; i++) {
        snprintf(names[i], sizeof(names[i]), "IMG_%04d.jpg", n - i);
    }
    camera_gallery_set_list(&G, names, n, (uint32_t)n);
}

static bool left_is(const char *want)
{
    struct gallery_screen s;

    camera_gallery_screen(&G, &s);
    return want ? s.left && strcmp(s.left, want) == 0 : s.left == NULL;
}

static void test_standalone_model(void)
{
    struct camera_event ev;
    struct gallery_screen s;

    camera_gallery_init(&G);
    camera_gallery_set_standalone(&G, true);
    check("standalone: opening starts the library helper", camera_gallery_open(&G) == GALLERY_DO_START);
    check("the flag survives opening", G.standalone);
    check("opening: no CAMERA", left_is(NULL));
    check("and the left action does nothing", camera_gallery_left(&G) == GALLERY_DO_NOTHING);

    open_photo(0);
    camera_gallery_screen(&G, &s);
    check("the empty library: a panel, no CAMERA, no SLIDESHOW",
          s.show_panel && s.left == NULL && s.middle == NULL && strcmp(G.title, "No photos yet") == 0);
    check("the empty library's left action does nothing", camera_gallery_left(&G) == GALLERY_DO_NOTHING);
    check("SLIDESHOW over nothing does nothing",
          camera_gallery_slideshow(&G, 0) == GALLERY_DO_NOTHING && G.view == GALLERY_GRID);

    open_photo(1);
    camera_gallery_screen(&G, &s);
    check("one photo: the grid, no CAMERA, SLIDESHOW", s.show_grid && s.left == NULL &&
                                                           s.middle && strcmp(s.middle, "SLIDESHOW") == 0);
    check("one page: NEWER and OLDER shown, both off", s.show_newer && s.show_older &&
                                                           !s.newer_enabled && !s.older_enabled);
    camera_gallery_tap_cell(&G, 0);
    camera_gallery_screen(&G, &s);
    check("the photo view keeps BACK, EXPORT, DELETE",
          G.view == GALLERY_PHOTO && s.left && strcmp(s.left, "BACK") == 0 && s.middle &&
              strcmp(s.middle, "EXPORT") == 0 && s.right && strcmp(s.right, "DELETE") == 0);
    check("the only photo: first and last at once", !s.newer_enabled && !s.older_enabled);
    check("BACK still goes back", camera_gallery_left(&G) == GALLERY_DO_PICTURES && G.view == GALLERY_GRID);

    open_photo(8);
    camera_gallery_tap_cell(&G, 0);
    check("the newest: OLDER only", camera_gallery_newer(&G) == GALLERY_DO_NOTHING && G.current == 0);
    camera_gallery_older(&G);
    check("OLDER moves on", G.current == 1 && strcmp(camera_gallery_current_name(&G), "IMG_0007.jpg") == 0);
    G.current = 7;
    check("the oldest: OLDER goes nowhere", camera_gallery_older(&G) == GALLERY_DO_NOTHING && G.current == 7);

    /* Delete with its confirmation, as Camera's. */
    camera_gallery_right(&G);
    camera_gallery_screen(&G, &s);
    check("DELETE asks first: CANCEL and DELETE", G.confirm_delete && s.left && strcmp(s.left, "CANCEL") == 0);
    check("CANCEL keeps it", camera_gallery_left(&G) == GALLERY_DO_NOTHING && !G.confirm_delete &&
                                 G.count == 8);
    camera_gallery_right(&G);
    check("DELETE, DELETE deletes", camera_gallery_right(&G) == GALLERY_DO_DELETE && G.deleting);
    ev = event(CAMERA_EV_DELFAIL);
    camera_gallery_event(&G, &ev, 10);
    check("a refused delete keeps the photo and says so",
          G.count == 8 && !G.deleting && strcmp(G.note, "The photo could not be deleted") == 0);
    camera_gallery_right(&G);
    camera_gallery_right(&G);
    ev = event(CAMERA_EV_DELETED);
    snprintf(ev.name, sizeof(ev.name), "IMG_0001.jpg");
    camera_gallery_event(&G, &ev, 20);
    check("deleted: one fewer, the one before shown", G.count == 7 && G.current == 6 &&
                                                          strcmp(G.note, "Photo deleted") == 0);

    /* The slideshow starts and stops, and stays standalone. */
    camera_gallery_left(&G);
    check("SLIDESHOW starts from the grid", camera_gallery_middle(&G, 0) == GALLERY_DO_PICTURES &&
                                                G.view == GALLERY_SLIDESHOW);
    check("a tap stops it", camera_gallery_tap_slideshow(&G) == GALLERY_DO_PICTURES && G.view == GALLERY_GRID);
    check("still no CAMERA", left_is(NULL));

    /* The helper failing: TRY AGAIN, and the flag kept by retry. */
    ev = event(CAMERA_EV_EXITED);
    ev.reason = CAMERA_EXIT_CRASHED;
    ev.value = 128 + 11;
    camera_gallery_event(&G, &ev, 30);
    camera_gallery_screen(&G, &s);
    check("a crashed helper: TRY AGAIN, no CAMERA",
          G.view == GALLERY_FAILED && s.middle && strcmp(s.middle, "TRY AGAIN") == 0 && s.left == NULL);
    check("the left action does nothing there either", camera_gallery_left(&G) == GALLERY_DO_NOTHING);
    check("TRY AGAIN starts the helper again", camera_gallery_middle(&G, 40) == GALLERY_DO_START);
    check("and the gallery is still standalone", G.standalone && left_is(NULL));

    /* Camera's own gallery is unchanged. */
    camera_gallery_init(&G);
    camera_gallery_open(&G);
    check("Camera's gallery still offers CAMERA", left_is("CAMERA") &&
                                                      camera_gallery_left(&G) == GALLERY_DO_CAMERA);
}

/* ---- the store ------------------------------------------------------------------- */

static void test_store(void)
{
    struct pocketcam_store st;
    char p[600];
    char outside[400];
    char listed[8][POCKETCAM_STORE_NAME_MAX];
    uint32_t total = 0;
    int n;
    int i;

    wipe(lib);
    check("the store opens the folder", pocketcam_store_open(&st, lib) == 0);
    put("IMG_0001.ppm", "P6\n1 1\n255\nabc");
    put("IMG_0002.ppm", "P6\n1 1\n255\nabc");
    put("IMG_0003 copy.ppm", "P6\n1 1\n255\nabc");
    put("notes.txt", "no");
    snprintf(outside, sizeof(outside), "%s/outside.ppm", root);
    write_file(outside, "P6\n1 1\n255\nabc");
    path_in(p, sizeof(p), lib, "IMG_0004.ppm");
    check("a link under a photo's name can be made", symlink(outside, p) == 0);
    path_in(p, sizeof(p), lib, "IMG_0005.ppm");
    mkdir(p, 0755);
    path_in(p, sizeof(p), lib, "IMG_0005.ppm/IMG_0001.ppm");
    write_file(p, "inside a folder");

    n = pocketcam_store_list(&st, listed, 8, &total);
    check("only the two regular photos are listed, newest first",
          n == 2 && total == 2 && strcmp(listed[0], "IMG_0002.ppm") == 0 &&
              strcmp(listed[1], "IMG_0001.ppm") == 0);

    check("'../outside.ppm' is not a name", pocketcam_store_delete(&st, "../outside.ppm") == -EINVAL);
    check("an absolute path is not a name", pocketcam_store_delete(&st, outside) == -EINVAL);
    check("'IMG_0001.ppm/../x' is not a name",
          pocketcam_store_delete(&st, "IMG_0001.ppm/../../outside.ppm") == -EINVAL);
    check("a name with a space is not a photo's", pocketcam_store_delete(&st, "IMG_0003 copy.ppm") == -EINVAL);
    check("nor 'notes.txt'", pocketcam_store_delete(&st, "notes.txt") == -EINVAL);
    check("nor an empty name", pocketcam_store_delete(&st, "") == -EINVAL && pocketcam_store_delete(&st, NULL) == -EINVAL);
    check("a link under a photo's name is refused", pocketcam_store_delete(&st, "IMG_0004.ppm") == -EPERM);
    check("and it and what it points at are both still there", in_lib("IMG_0004.ppm") && there(outside));
    check("a folder under a photo's name is refused", pocketcam_store_delete(&st, "IMG_0005.ppm") == -EPERM);
    path_in(p, sizeof(p), lib, "IMG_0005.ppm/IMG_0001.ppm");
    check("and neither it nor anything in it is removed", in_lib("IMG_0005.ppm") && there(p));
    check("a photo that is not there: -ENOENT", pocketcam_store_delete(&st, "IMG_0099.ppm") == -ENOENT);
    check("a photo is deleted", pocketcam_store_delete(&st, "IMG_0002.ppm") == 0 && !in_lib("IMG_0002.ppm"));
    check("only that photo", in_lib("IMG_0001.ppm") && in_lib("IMG_0003 copy.ppm") && in_lib("notes.txt"));
    check("and the count follows", st.files == 1);
    if (geteuid() != 0) {
        chmod(lib, 0555);
        check("a read-only folder: -EACCES, and the photo stays",
              pocketcam_store_delete(&st, "IMG_0001.ppm") == -EACCES && in_lib("IMG_0001.ppm"));
        chmod(lib, 0755);
    } else {
        printf("note: running as root: the read-only folder case is not checked\n");
    }

    /* Many: the list is capped, the total is not, and it stays newest first. */
    wipe(lib);
    mkdir(lib, 0755);
    for (i = 1; i <= 1100; i++) {
        char name[32];

        snprintf(name, sizeof(name), "IMG_%04d.ppm", i);
        put(name, "");
    }
    pocketcam_store_open(&st, lib);
    {
        static char many[CAMERA_LIBRARY_MAX][POCKETCAM_STORE_NAME_MAX];

        n = pocketcam_store_list(&st, many, CAMERA_LIBRARY_MAX, &total);
        check("1100 photos: the newest 1000 listed, 1100 counted",
              n == 1000 && total == 1100 && strcmp(many[0], "IMG_1100.ppm") == 0 &&
                  strcmp(many[999], "IMG_0101.ppm") == 0);
    }
    unlink(outside);
}

/* ---- the helper, fed raw lines ---------------------------------------------------- */

struct helper {
    pid_t pid;
    int in;   /* its stdin */
    int out;  /* its stdout */
    char buf[4096];
    size_t len;
};

static int start_helper(struct helper *h, const char *bin)
{
    int to[2];
    int from[2];
    char shm_path[300];
    int shm;
    struct pocketcam_shm_header hdr = { POCKETCAM_SHM_MAGIC, POCKETCAM_PROTO_VERSION, POCKETCAM_SLOTS,
                                        POCKETCAM_SLOT_BYTES, POCKETCAM_VIEW_MAX_W,
                                        POCKETCAM_VIEW_MAX_H };

    memset(h, 0, sizeof(*h));
    snprintf(shm_path, sizeof(shm_path), "%s/shm", root);
    shm = open(shm_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (shm < 0 || ftruncate(shm, POCKETCAM_SHM_BYTES) != 0 ||
        pwrite(shm, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr) || pipe2(to, O_CLOEXEC) != 0 ||
        pipe2(from, O_CLOEXEC) != 0) {
        return -1;
    }
    unlink(shm_path);
    h->pid = fork();
    if (h->pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 1);
        if (shm == POCKETCAM_SHM_FD) {
            fcntl(shm, F_SETFD, 0); /* dup2 onto itself would keep close-on-exec */
        } else {
            dup2(shm, POCKETCAM_SHM_FD);
        }
        execl(bin, bin, "library", "--dir", lib, (char *)NULL);
        _exit(127);
    }
    close(shm);
    close(to[0]);
    close(from[1]);
    h->in = to[1];
    h->out = from[0];
    return h->pid > 0 ? 0 : -1;
}

/* The next line from the helper, within ms; "" on a timeout. */
static const char *line_from(struct helper *h, int ms, char *line, size_t cap)
{
    int64_t waited = 0;

    line[0] = '\0';
    for (;;) {
        char *nl = memchr(h->buf, '\n', h->len);
        struct pollfd pfd = { h->out, POLLIN, 0 };
        ssize_t r;

        if (nl) {
            size_t n = (size_t)(nl - h->buf);

            snprintf(line, cap, "%.*s", (int)n, h->buf);
            memmove(h->buf, nl + 1, h->len - n - 1);
            h->len -= n + 1;
            return line;
        }
        if (waited >= ms || poll(&pfd, 1, 50) < 0) {
            return line;
        }
        waited += 50;
        if (pfd.revents) {
            r = read(h->out, h->buf + h->len, sizeof(h->buf) - 1 - h->len);
            if (r <= 0) {
                return line;
            }
            h->len += (size_t)r;
        }
    }
}

/* Send a line; its answer (shown, so a failure says what came back). */
static const char *ask(struct helper *h, const char *cmd, char *line, size_t cap)
{
    char out[300];

    snprintf(out, sizeof(out), "%s\n", cmd);
    if (write(h->in, out, strlen(out)) < 0) {
        line[0] = '\0';
        return line;
    }
    line_from(h, 3000, line, cap);
    printf("     %s -> %s\n", cmd, line);
    return line;
}

static void stop_helper(struct helper *h)
{
    int status = 0;

    if (write(h->in, "quit\n", 5) < 0) {
        kill(h->pid, SIGTERM);
    }
    close(h->in);
    waitpid(h->pid, &status, 0);
    close(h->out);
}

static void test_helper(const char *bin)
{
    struct helper h;
    char line[512];
    char outside[400];
    char p[600];

    wipe(lib);
    mkdir(lib, 0755);
    put("IMG_0001.ppm", "P6\n1 1\n255\nabc");
    put("IMG_0002.ppm", "P6\n1 1\n255\nabc");
    snprintf(outside, sizeof(outside), "%s/outside.ppm", root);
    write_file(outside, "P6\n1 1\n255\nabc");
    path_in(p, sizeof(p), lib, "IMG_0003.ppm");
    if (symlink(outside, p) != 0) {
        printf("note: no symlink: %s\n", strerror(errno));
    }
    path_in(p, sizeof(p), lib, "IMG_0004.ppm");
    mkdir(p, 0755);
    path_in(p, sizeof(p), lib, "IMG_0004.ppm/IMG_0001.ppm");
    write_file(p, "inside a folder");

    if (start_helper(&h, bin) != 0) {
        check("the library helper starts", 0);
        return;
    }
    line_from(&h, 3000, line, sizeof(line));
    check("the helper says hello in library mode", strncmp(line, "hello ", 6) == 0 && strstr(line, "library"));
    line_from(&h, 3000, line, sizeof(line));
    printf("     %s\n", line);
    check("and is ready with the two photos", strncmp(line, "ready library", 13) == 0 &&
                                                  strstr(line, " 2 IMG_0002.ppm") != NULL);

    ask(&h, "delete ../outside.ppm", line, sizeof(line));
    check("raw 'delete ../outside.ppm': refused", strncmp(line, "delfail", 7) == 0 && there(outside));
    snprintf(p, sizeof(p), "delete %s", outside);
    ask(&h, p, line, sizeof(line));
    check("raw delete of an absolute path: refused", strncmp(line, "delfail", 7) == 0 && there(outside));
    ask(&h, "delete IMG_0001.ppm/../../outside.ppm", line, sizeof(line));
    check("raw delete through a photo's name: refused", strncmp(line, "delfail", 7) == 0 && there(outside));
    ask(&h, "delete IMG_0003.ppm", line, sizeof(line));
    check("raw delete of a link under a photo's name: refused, link and target kept",
          strncmp(line, "delfail", 7) == 0 && in_lib("IMG_0003.ppm") && there(outside));
    ask(&h, "delete IMG_0004.ppm", line, sizeof(line));
    path_in(p, sizeof(p), lib, "IMG_0004.ppm/IMG_0001.ppm");
    check("raw delete of a folder under a photo's name: refused, nothing in it removed",
          strncmp(line, "delfail", 7) == 0 && in_lib("IMG_0004.ppm") && there(p));
    ask(&h, "delete IMG_0002.ppm", line, sizeof(line));
    check("a photo is deleted, and the count said", strcmp(line, "deleted IMG_0002.ppm 1") == 0 &&
                                                        !in_lib("IMG_0002.ppm"));
    ask(&h, "delete IMG_0002.ppm", line, sizeof(line));
    check("deleting it again: already gone counts as deleted", strncmp(line, "deleted IMG_0002.ppm", 20) == 0);
    check("nothing else was touched", in_lib("IMG_0001.ppm") && in_lib("IMG_0003.ppm") &&
                                          in_lib("IMG_0004.ppm") && there(outside));
    stop_helper(&h);
    check("the helper leaves on quit", 1);
    unlink(outside);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2 || access(argv[1], X_OK) != 0) {
        printf("FAIL usage: photo_library_test <pos-camera-testhooks>\n");
        return 2;
    }
    snprintf(root, sizeof(root), "/tmp/photo-lib-%ld", (long)getpid());
    snprintf(lib, sizeof(lib), "%s/camera", root);
    mkdir(root, 0755);
    setenv("POCKETOS_STATE_DIR", root, 1);
    setenv("HOME", root, 1);

    test_standalone_model();
    test_store();
    test_helper(argv[1]);

    wipe(root);
    printf("photo_library_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
