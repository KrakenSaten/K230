/*
 * Photo in the running app, driven by a real LVGL pointer device, with the
 * real library helper (pos-camera-testhooks, `library` mode) behind it.
 *
 * Hosted the way the shell hosts it (ui/shell/shell.c app_open): the
 * reference panel with its 30 px rounded corners, portrait and landscape, the
 * body under the 72 px header the NONE chrome leaves. Time is real: the
 * helper runs on the wall clock, so every pump here sleeps as long as it
 * advances LVGL's tick, and the session's deadlines mean what they say.
 *
 * The photos are made here, as the files Camera writes on a host without
 * libjpeg (PPM with Camera's `# doors-...` comment lines), plus the files a
 * library should survive: empty, cut short, text under a photo's name, a
 * link and a folder under a photo's name, a name with a space, a file deleted
 * behind the gallery's back, and 1100 photos.
 *
 * What is checked: the empty library; one photo; many, newest first by
 * number; a photo opened, NEWER and OLDER to both ends; what is known about
 * it; damaged, empty and vanished files; DELETE only after a confirmation, a
 * failed delete and a delete of a file already gone; the slideshow started
 * and stopped; no CAMERA anywhere and never a camera helper; every target
 * usable in both orientations; closing mid-photo and mid-slideshow, twenty
 * opens and closes with no helper, descriptor or heap left behind.
 *
 * Needs: CAMERA_HELPER, the path of tests/pos-camera-testhooks (built by
 * make). Built by ui/shell/CMakeLists.txt, run by tests/photo_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "chrome.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"

#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
#define PANEL_CORNER 30
#define STATUS_H                                                                                  \
    chrome_height(chrome_resolve(app_photo.chrome,                                                \
                                 pocketui_display_geometry()->width >                             \
                                     pocketui_display_geometry()->height,                         \
                                 false))

extern const struct pocketos_app app_photo;

static int failed;
static int checks;
static char root[128];
static char photos[256];

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

/* ---- the shell's side of app.h ---------------------------------------------------- */

static char g_hint[64];
static char g_logged[256];

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(g_hint, sizeof(g_hint), "%s", text ? text : "");
}

void pocketos_shell_orientation(struct pocketos_orientation *out)
{
    memset(out, 0, sizeof(*out));
    out->landscape = pocketui_display_geometry()->width > pocketui_display_geometry()->height;
    out->next_landscape = out->landscape;
}

void pocketlog_write(enum pocketlog_level level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    va_start(ap, fmt);
    vsnprintf(g_logged, sizeof(g_logged), fmt, ap);
    va_end(ap);
}

/* ---- display and finger -------------------------------------------------------------- */

static uint8_t draw_buf[PANEL_H * 40 * 4];
static lv_display_t *disp;
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;
static lv_obj_t *g_content;
static lv_obj_t *app_root;
static lv_obj_t *app_body;
static void *app_priv;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = finger_state;
    data->point = finger_point;
}

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* LVGL's tick follows the wall clock, as on the device. */
static void pump(int ms)
{
    int64_t end = mono_ms() + ms;
    int64_t last = mono_ms();

    while (mono_ms() < end) {
        struct timespec d = { 0, 5 * 1000000L };
        int64_t now;

        nanosleep(&d, NULL);
        now = mono_ms();
        lv_tick_inc((uint32_t)(now - last));
        last = now;
        lv_timer_handler();
    }
}

/* ---- finding things on screen ----------------------------------------------------------- */

static lv_obj_t *find_label_in(lv_obj_t *obj, const char *text, bool prefix)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (prefix ? strncmp(t, text, strlen(text)) == 0 : strcmp(t, text) == 0) {
            return obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *f = find_label_in(lv_obj_get_child(obj, i), text, prefix);

        if (f) {
            return f;
        }
    }
    return NULL;
}

static lv_obj_t *shown_label(const char *text)
{
    return app_body ? find_label_in(app_body, text, false) : NULL;
}

static bool shows_text(const char *prefix)
{
    return app_body && find_label_in(app_body, prefix, true) != NULL;
}

static lv_obj_t *label_with_in(lv_obj_t *obj, const char *part)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class) && strstr(lv_label_get_text(obj), part)) {
        return obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *f = label_with_in(lv_obj_get_child(obj, i), part);

        if (f) {
            return f;
        }
    }
    return NULL;
}

/* A visible label with part anywhere in its text. */
static bool label_with(const char *part)
{
    return app_body && label_with_in(app_body, part) != NULL;
}

/* A visible button by its label. */
static lv_obj_t *button(const char *label)
{
    lv_obj_t *l = shown_label(label);

    return l ? lv_obj_get_parent(l) : NULL;
}

static bool enabled(const char *label)
{
    lv_obj_t *b = button(label);

    return b && lv_obj_has_flag(b, LV_OBJ_FLAG_CLICKABLE);
}

static void tap_obj(lv_obj_t *obj)
{
    lv_area_t a;

    if (!obj) {
        printf("FAIL tap on a missing object\n");
        failed++;
        checks++;
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(40);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(40);
}

static bool wait_until(bool (*cond)(void), int ms)
{
    int64_t end = mono_ms() + ms;

    while (mono_ms() < end) {
        if (cond()) {
            return true;
        }
        pump(20);
    }
    return cond();
}

static const char *want_text;

static bool text_shown(void)
{
    return shows_text(want_text);
}

static bool wait_until_text(const char *text, int ms)
{
    want_text = text;
    return wait_until(text_shown, ms);
}

/* ---- the gallery ------------------------------------------------------------------------- *
 * Photo's body holds one child, the gallery's frame; in it, in the order
 * camera_gallery_screen.c builds them: the panel, GALLERY_PAGE_MAX cells, the
 * photo's box, three lines about it, the slideshow's box, the status line and
 * the buttons. */
#define G_CELLS 24
#define G_PHOTO_BOX (1 + G_CELLS)
#define G_INFO (G_PHOTO_BOX + 1)
#define G_SHOW_BOX (G_PHOTO_BOX + 4)

static lv_obj_t *gallery_child(int i)
{
    lv_obj_t *g = app_body ? lv_obj_get_child(app_body, 0) : NULL;

    return g && !lv_obj_has_flag(g, LV_OBJ_FLAG_HIDDEN) ? lv_obj_get_child(g, i) : NULL;
}

static lv_obj_t *cell(int i)
{
    lv_obj_t *c = gallery_child(1 + i);

    return c && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) ? c : NULL;
}

static int cells_shown(void)
{
    int i;
    int n = 0;

    for (i = 0; i < G_CELLS; i++) {
        n += cell(i) != NULL;
    }
    return n;
}

static bool box_shows_picture(lv_obj_t *box)
{
    lv_obj_t *img = box && !lv_obj_has_flag(box, LV_OBJ_FLAG_HIDDEN) ? lv_obj_get_child(box, 0) : NULL;

    return img && !lv_obj_has_flag(img, LV_OBJ_FLAG_HIDDEN) && lv_image_get_src(img) != NULL;
}

static int want_cell;

static bool cell_has_picture(void)
{
    return box_shows_picture(cell(want_cell));
}

static bool all_cells_settled(void)
{
    int i;

    for (i = 0; i < G_CELLS; i++) {
        lv_obj_t *c = cell(i);

        if (c && !box_shows_picture(c) && !label_with_in(c, "Cannot show")) {
            return false;
        }
    }
    return true;
}

static bool in_grid(void)
{
    return cells_shown() > 0 && gallery_child(G_PHOTO_BOX) &&
           lv_obj_has_flag(gallery_child(G_PHOTO_BOX), LV_OBJ_FLAG_HIDDEN) &&
           lv_obj_has_flag(gallery_child(G_SHOW_BOX), LV_OBJ_FLAG_HIDDEN);
}

static bool empty_library(void)
{
    return shown_label("No photos yet") != NULL;
}

static bool photo_view(void)
{
    return button("BACK") != NULL && button("DELETE") != NULL;
}

static bool photo_shown(void)
{
    return box_shows_picture(gallery_child(G_PHOTO_BOX)) && photo_view();
}

/* The photo view has said what the file is: shown, or why it cannot be. */
static bool photo_settled(void)
{
    return photo_view() && !label_with("Opening...");
}

static bool slide_shown(void)
{
    return box_shows_picture(gallery_child(G_SHOW_BOX));
}

static bool failed_screen(void)
{
    return button("TRY AGAIN") != NULL;
}

/* ---- the process ------------------------------------------------------------------------- */

/* Whether a child of this process runs the helper with this word on its
 * command line. */
static bool helper_in_mode(const char *mode)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    bool found = false;

    while (d && !found && (e = readdir(d)) != NULL) {
        char path[300];
        char buf[512];
        FILE *fp;
        size_t n;
        size_t i;
        int ppid = -1;

        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
        fp = fopen(path, "r");
        if (!fp) {
            continue;
        }
        if (fscanf(fp, "%*d %*s %*c %d", &ppid) != 1) {
            ppid = -1;
        }
        fclose(fp);
        if (ppid != (int)getpid()) {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        fp = fopen(path, "rb");
        if (!fp) {
            continue;
        }
        n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        for (i = 0; i < n; i++) {
            buf[i] = buf[i] ? buf[i] : ' ';
        }
        buf[n] = '\0';
        found = strstr(buf, mode) != NULL;
    }
    if (d) {
        closedir(d);
    }
    return found;
}

static bool no_child(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
}

static int open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.';
    }
    if (d) {
        closedir(d);
    }
    return n - 1; /* the directory's own */
}

/* ---- the library on disk ------------------------------------------------------------------ */

static void path_of(char *out, size_t len, const char *name)
{
    snprintf(out, len, "%s/%s", photos, name);
}

/* A photo as Camera writes it on a host without libjpeg: PPM with the facts
 * as comment lines; taken is EXIF's "YYYY:MM:DD HH:MM:SS" or NULL. */
static void write_photo(const char *name, int w, int h, const char *taken, unsigned seed)
{
    char p[600];
    FILE *fp;
    int i;

    path_of(p, sizeof(p), name);
    fp = fopen(p, "wb");
    if (!fp) {
        return;
    }
    fprintf(fp, "P6\n# doors-software Doors test\n");
    if (taken) {
        fprintf(fp, "# doors-taken %s\n", taken);
    }
    fprintf(fp, "%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        fputc((int)((seed * 37 + (unsigned)i) & 0xff), fp);
        fputc((int)((seed * 91) & 0xff), fp);
        fputc((int)((unsigned)i * 3 & 0xff), fp);
    }
    fclose(fp);
}

static void write_bytes(const char *name, const char *bytes, size_t n)
{
    char p[600];
    FILE *fp;

    path_of(p, sizeof(p), name);
    fp = fopen(p, "wb");
    if (fp) {
        if (n) {
            fwrite(bytes, 1, n, fp);
        }
        fclose(fp);
    }
}

static bool exists(const char *name)
{
    char p[600];
    struct stat st;

    path_of(p, sizeof(p), name);
    return lstat(p, &st) == 0;
}

static void remove_name(const char *name)
{
    char p[600];

    path_of(p, sizeof(p), name);
    unlink(p);
}

static void empty_photos(void)
{
    char cmd[700];

    snprintf(cmd, sizeof(cmd), "chmod -R u+w '%s' 2>/dev/null; rm -rf '%s'", photos, photos);
    if (system(cmd) != 0) {
        printf("note: could not empty %s\n", photos);
    }
    mkdir(photos, 0755);
}

/* ---- the app, hosted --------------------------------------------------------------------- */

static void use_display(enum pos_rotation rotation)
{
    struct pos_panel panel = {
        .width = PANEL_W,
        .height = PANEL_H,
        .corners = { PANEL_CORNER, PANEL_CORNER, PANEL_CORNER, PANEL_CORNER },
    };
    struct pos_display_geometry g;

    pos_display_geometry_init(&g, &panel, rotation);
    pocketui_set_display_geometry(&g);
    finger_point.x = 0;
    finger_point.y = 0;
    lv_display_set_resolution(disp, g.width, g.height);
    lv_obj_set_size(g_content, g.width, g.height - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    pump(20);
}

static void app_start(void)
{
    lv_obj_t *header;

    app_root = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_root);
    lv_obj_set_size(app_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_root, LV_FLEX_FLOW_COLUMN);
    header = lv_obj_create(app_root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), POCKETUI_HEADER_H);
    app_body = lv_obj_create(app_root);
    lv_obj_remove_style_all(app_body);
    lv_obj_set_width(app_body, LV_PCT(100));
    lv_obj_set_flex_grow(app_body, 1);
    lv_obj_set_flex_flow(app_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(app_body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_top(app_body, POCKETUI_BODY_PAD_TOP, 0);
    lv_obj_add_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(app_body, LV_DIR_VER);
    lv_obj_update_layout(app_root);
    app_priv = app_photo.create(app_body);
    pump(20);
}

static int64_t app_stop(void)
{
    int64_t t0 = mono_ms();

    app_photo.destroy(app_priv);
    t0 = mono_ms() - t0;
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(20);
    return t0;
}

/* ---- layout ------------------------------------------------------------------------------ */

static int n_targets;
static lv_area_t targets[40];

static void target(lv_obj_t *obj, const lv_area_t *body, const char *what, int *bad)
{
    lv_area_t a;
    int k;

    lv_obj_get_coords(obj, &a);
    if (lv_area_get_height(&a) < POCKETUI_TOUCH_MIN || lv_area_get_width(&a) < POCKETUI_TOUCH_MIN ||
        a.x1 < body->x1 || a.x2 > body->x2 || a.y1 < body->y1 || a.y2 > body->y2 ||
        !pos_display_rect_is_safe(pocketui_display_geometry(), a.x1, a.y1, a.x2, a.y2)) {
        printf("FAIL %s: a target at %d,%d-%d,%d is not usable in %d,%d-%d,%d\n", what, (int)a.x1,
               (int)a.y1, (int)a.x2, (int)a.y2, (int)body->x1, (int)body->y1, (int)body->x2,
               (int)body->y2);
        (*bad)++;
    }
    for (k = 0; k < n_targets; k++) {
        if (a.x1 <= targets[k].x2 && targets[k].x1 <= a.x2 && a.y1 <= targets[k].y2 &&
            targets[k].y1 <= a.y2) {
            printf("FAIL %s: two targets overlap at %d,%d\n", what, (int)a.x1, (int)a.y1);
            (*bad)++;
        }
    }
    if (n_targets < (int)(sizeof(targets) / sizeof(targets[0]))) {
        targets[n_targets++] = a;
    }
}

static void walk(lv_obj_t *obj, const lv_area_t *body, const char *what, int *bad)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (obj != app_body && lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) &&
        !lv_obj_check_type(obj, &lv_label_class) && !lv_obj_check_type(obj, &lv_image_class)) {
        target(obj, body, what, bad);
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        walk(lv_obj_get_child(obj, i), body, what, bad);
    }
}

/* Every visible tap target: large enough, inside the body, clear of the
 * panel's corners, apart from every other. want: how many there must be at
 * least (0: none needed, as on the empty library, whose only way out is the
 * shell's back slab). */
static void check_targets(const char *what, int want)
{
    lv_area_t body;
    int bad = 0;
    char msg[160];

    lv_obj_update_layout(app_body);
    lv_obj_get_content_coords(app_body, &body);
    n_targets = 0;
    walk(app_body, &body, what, &bad);
    snprintf(msg, sizeof(msg), "%s: %d targets, all usable, inside, safe and apart", what, n_targets);
    check(msg, bad == 0 && n_targets >= want);
}

/* The photo view's picture: inside its box, and of the file's shape (the
 * box's largest box of that shape, give or take a pixel of rounding). */
static bool picture_keeps_shape(int file_w, int file_h)
{
    lv_obj_t *box = gallery_child(G_PHOTO_BOX);
    lv_obj_t *img = box ? lv_obj_get_child(box, 0) : NULL;
    lv_area_t b;
    lv_area_t a;
    int32_t w;
    int32_t h;
    int64_t skew;

    if (!img || lv_obj_has_flag(img, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    lv_obj_update_layout(box);
    lv_obj_get_coords(box, &b);
    lv_obj_get_coords(img, &a);
    w = lv_area_get_width(&a);
    h = lv_area_get_height(&a);
    skew = (int64_t)w * file_h - (int64_t)h * file_w;
    if (skew < 0) {
        skew = -skew;
    }
    return a.x1 >= b.x1 && a.x2 <= b.x2 && a.y1 >= b.y1 && a.y2 <= b.y2 &&
           skew <= (int64_t)(file_w > file_h ? file_w : file_h) &&
           (w == lv_area_get_width(&b) || h == lv_area_get_height(&b));
}

/* ---- the journeys ------------------------------------------------------------------------ */

static void empty_and_one(const char *shape)
{
    char msg[160];

#define CHECK(text, ok) do { snprintf(msg, sizeof(msg), "%s: %s", shape, text); check(msg, ok); } while (0)
    empty_photos();
    app_start();
    CHECK("an empty library says so", wait_until(empty_library, 4000));
    CHECK("and where photos come from", shows_text("Photos you take with Camera appear here."));
    CHECK("no CAMERA: Photo is not the camera", button("CAMERA") == NULL);
    CHECK("no SLIDESHOW with nothing to show", button("SLIDESHOW") == NULL);
    CHECK("the library helper runs, never a camera",
          helper_in_mode(" library") && !helper_in_mode(" session"));
    check_targets("empty library", 0);
    app_stop();

    write_photo("IMG_20260930_101530_0001.ppm", 36, 64, "2026:09:30 10:15:30", 1);
    app_start();
    CHECK("one photo: one cell", wait_until(in_grid, 4000) && cells_shown() == 1);
    want_cell = 0;
    CHECK("with its thumbnail", wait_until(cell_has_picture, 4000));
    CHECK("the status counts it", shows_text("1 photo  |  page 1 of 1"));
    CHECK("NEWER and OLDER shown, neither turns a page", button("NEWER") && button("OLDER") &&
                                                             !enabled("NEWER") && !enabled("OLDER"));
    CHECK("SLIDESHOW offered", enabled("SLIDESHOW"));
    CHECK("still no CAMERA", button("CAMERA") == NULL);
    /* The cell and SLIDESHOW; NEWER and OLDER are off, so take no taps. */
    check_targets(shape, 2);

    tap_obj(cell(0));
    CHECK("a tap opens it", wait_until(photo_shown, 4000));
    CHECK("its name", shown_label("IMG_20260930_101530_0001.ppm") != NULL);
    CHECK("when it was taken, from the file", shows_text("Taken 2026-09-30 10:15:30"));
    CHECK("its size, file size and format", shows_text("36 x 64  |") && label_with("|  PPM"));
    CHECK("the only photo: neither NEWER nor OLDER goes anywhere",
          !enabled("NEWER") && !enabled("OLDER") && shows_text("1 of 1"));
    CHECK("BACK, EXPORT and DELETE offered", enabled("BACK") && enabled("EXPORT") && enabled("DELETE"));
    CHECK("a tall photo keeps its shape in the view", picture_keeps_shape(36, 64));
    check_targets(shape, 3); /* BACK, EXPORT, DELETE */
    tap_obj(button("BACK"));
    CHECK("BACK: the grid", wait_until(in_grid, 2000));
    app_stop();
    CHECK("and leaves no helper", no_child());
#undef CHECK
}

static void many(const char *shape)
{
    char msg[160];
    char p[600];
    char outside[400];
    int i;

#define CHECK(text, ok) do { snprintf(msg, sizeof(msg), "%s: %s", shape, text); check(msg, ok); } while (0)
    empty_photos();
    /* Numbers decide the order, not dates or names: 0003 has a date, the rest do not. */
    write_photo("IMG_0001.ppm", 64, 36, NULL, 1);
    write_photo("IMG_0002.ppm", 64, 36, NULL, 2);
    write_photo("IMG_20260101_080000_0003.ppm", 36, 64, "2026:01:01 08:00:00", 3);
    write_photo("IMG_0004.ppm", 64, 36, NULL, 4);
    write_photo("IMG_0005.ppm", 64, 36, NULL, 5);
    /* What is not a photo of the library, and must never be listed. */
    write_bytes("notes.txt", "not a photo\n", 12);
    write_photo("IMG_0006 copy.ppm", 8, 8, NULL, 6);
    write_bytes(".IMG_0007.ppm.tmp", "P6\n", 3);
    snprintf(outside, sizeof(outside), "%s/outside.ppm", root);
    {
        FILE *fp = fopen(outside, "wb");

        if (fp) {
            fputs("P6\n8 8\n255\n", fp);
            for (i = 0; i < 8 * 8 * 3; i++) {
                fputc(0x40, fp);
            }
            fclose(fp);
        }
    }
    path_of(p, sizeof(p), "IMG_0008.ppm");
    if (symlink(outside, p) != 0) {
        printf("note: no symlink: %s\n", strerror(errno));
    }
    path_of(p, sizeof(p), "IMG_0009.ppm");
    mkdir(p, 0755);

    app_start();
    CHECK("five photos, and only the photos", wait_until(in_grid, 4000) && cells_shown() == 5 &&
                                                  shows_text("5 photos  |  page 1 of 1"));
    want_cell = 4;
    CHECK("thumbnails for all", wait_until(all_cells_settled, 6000) && cell_has_picture());
    tap_obj(cell(0));
    CHECK("the first is the newest by number", wait_until(photo_shown, 4000) &&
                                                   shown_label("IMG_0005.ppm") != NULL &&
                                                   shows_text("1 of 5"));
    CHECK("at the newest, NEWER goes nowhere", !enabled("NEWER") && enabled("OLDER"));
    CHECK("an undated photo says why it has no date",
          shows_text("Date unknown: the clock was not set"));
    CHECK("a wide photo keeps its shape in the view", picture_keeps_shape(64, 36));
    tap_obj(button("NEWER"));
    CHECK("and a tap on it changes nothing", shown_label("IMG_0005.ppm") != NULL);
    tap_obj(button("OLDER"));
    CHECK("OLDER: the next older", wait_until(photo_shown, 4000) && shown_label("IMG_0004.ppm") &&
                                       shows_text("2 of 5"));
    tap_obj(button("OLDER"));
    CHECK("OLDER again: the dated one, by its number", wait_until(photo_shown, 4000) &&
                                                          shown_label("IMG_20260101_080000_0003.ppm") &&
                                                          shows_text("Taken 2026-01-01 08:00:00"));
    CHECK("a tall photo keeps its shape in the view", picture_keeps_shape(36, 64));
    tap_obj(button("OLDER"));
    wait_until(photo_shown, 4000);
    tap_obj(button("OLDER"));
    CHECK("the oldest", wait_until(photo_shown, 4000) && shown_label("IMG_0001.ppm") &&
                            shows_text("5 of 5"));
    CHECK("at the oldest, OLDER goes nowhere", !enabled("OLDER") && enabled("NEWER"));
    tap_obj(button("OLDER"));
    CHECK("and a tap on it changes nothing", shown_label("IMG_0001.ppm") != NULL);
    tap_obj(button("NEWER"));
    CHECK("NEWER goes back", wait_until(photo_shown, 4000) && shown_label("IMG_0002.ppm"));
    check_targets(shape, 5);
    tap_obj(button("BACK"));
    CHECK("BACK: the grid", wait_until(in_grid, 2000) && cells_shown() == 5);

    /* The slideshow: from the page's first photo, on, then stopped. */
    tap_obj(button("SLIDESHOW"));
    CHECK("SLIDESHOW shows a photo", wait_until(slide_shown, 4000));
    CHECK("from the newest", wait_until_text("Slideshow  |  1 of 5", 3000));
    if (strcmp(shape, "portrait") == 0) {
        CHECK("and moves on by itself", wait_until_text("Slideshow  |  2 of 5", 7000));
    }
    check_targets(shape, 1);
    tap_obj(gallery_child(G_SHOW_BOX));
    CHECK("a tap stops it, back on the grid", wait_until(in_grid, 2000));
    CHECK("which holds no slideshow pictures", !slide_shown());

    CHECK("nothing but photos was touched",
          exists("notes.txt") && exists("IMG_0006 copy.ppm") && exists("IMG_0009.ppm"));
    app_stop();
    CHECK("closed, no helper", no_child());
    path_of(p, sizeof(p), "IMG_0008.ppm");
    unlink(p);
    unlink(outside);
#undef CHECK
}

static void damaged(const char *shape)
{
    char msg[160];
    static const char text[] = "this is not a picture at all\n";

#define CHECK(text_, ok) do { snprintf(msg, sizeof(msg), "%s: %s", shape, text_); check(msg, ok); } while (0)
    empty_photos();
    write_photo("IMG_0001.ppm", 64, 36, NULL, 1);
    write_bytes("IMG_0002.jpg", "", 0);                          /* empty */
    write_bytes("IMG_0003.ppm", "P6\n64 36\n255\n\x10\x20", 15); /* cut short */
    write_bytes("IMG_0004.jpg", text, sizeof(text) - 1);          /* text under a photo's name */
    write_bytes("IMG_0005.jpg", "\xff\xd8\xff\xe0\x00\x10JFIF", 10); /* a JPEG that stops */
    write_photo("IMG_0006.ppm", 64, 36, NULL, 6);

    app_start();
    CHECK("six files listed", wait_until(in_grid, 4000) && cells_shown() == 6);
    CHECK("every cell settles, a picture or 'Cannot show'", wait_until(all_cells_settled, 6000));
    want_cell = 0;
    CHECK("the good ones are shown", cell_has_picture());
    want_cell = 5;
    CHECK("the oldest good one too", cell_has_picture());
    want_cell = 4; /* IMG_0002, empty */
    CHECK("an empty file cannot be shown", !cell_has_picture() && label_with_in(cell(4), "Cannot show"));
    want_cell = 1; /* IMG_0005, a JPEG that stops */
    CHECK("a JPEG with no picture in it cannot be shown", !cell_has_picture());

    tap_obj(cell(4));
    CHECK("the empty file opened: the view says it cannot be shown, and why",
          wait_until(photo_settled, 4000) && !box_shows_picture(gallery_child(G_PHOTO_BOX)) &&
              (label_with("damaged") || label_with("cannot be shown")));
    CHECK("its name is still shown", shown_label("IMG_0002.jpg") != NULL);
    CHECK("and it can still be stepped over", enabled("NEWER") && enabled("OLDER"));
    tap_obj(button("NEWER"));
    CHECK("a PPM cut short: what could be read, marked damaged, or said",
          wait_until(photo_settled, 4000) && shown_label("IMG_0003.ppm") &&
              (label_with("damaged") || label_with("cannot be shown")));
    tap_obj(button("NEWER"));
    CHECK("text under a photo's name: said, not shown",
          wait_until(photo_settled, 4000) && shown_label("IMG_0004.jpg") &&
              !box_shows_picture(gallery_child(G_PHOTO_BOX)));

    /* Gone behind the gallery's back: said when opened, and deleted as far
     * as the library is concerned. */
    remove_name("IMG_0001.ppm");
    tap_obj(button("BACK"));
    wait_until(in_grid, 2000);
    tap_obj(cell(3));
    wait_until(photo_settled, 4000);
    tap_obj(button("OLDER"));
    tap_obj(button("OLDER"));
    CHECK("a file deleted meanwhile: 'The file is gone'",
          wait_until_text("The file is gone", 4000) && shown_label("IMG_0001.ppm"));
    tap_obj(button("DELETE"));
    CHECK("DELETE asks first, even for a file that is gone",
          shows_text("Delete this photo?") && button("CANCEL") != NULL);
    tap_obj(button("DELETE"));
    CHECK("confirmed: its entry goes", wait_until_text("Photo deleted", 4000));
    tap_obj(button("BACK"));
    CHECK("five left in the grid", wait_until(in_grid, 2000) && cells_shown() == 5);
    CHECK("no damaged file was changed or removed",
          exists("IMG_0002.jpg") && exists("IMG_0003.ppm") && exists("IMG_0004.jpg"));

    /* A slideshow over damaged files skips them. */
    tap_obj(button("SLIDESHOW"));
    CHECK("the slideshow starts on a photo it can show", wait_until(slide_shown, 5000));
    tap_obj(gallery_child(G_SHOW_BOX));
    CHECK("and stops", wait_until(in_grid, 2000));
    app_stop();
    CHECK("closed, no helper", no_child());
#undef CHECK
}

static void deleting(const char *shape)
{
    char msg[160];

#define CHECK(text, ok) do { snprintf(msg, sizeof(msg), "%s: %s", shape, text); check(msg, ok); } while (0)
    empty_photos();
    write_photo("IMG_0001.ppm", 64, 36, NULL, 1);
    write_photo("IMG_0002.ppm", 64, 36, NULL, 2);
    write_photo("IMG_0003.ppm", 64, 36, NULL, 3);
    app_start();
    wait_until(in_grid, 4000);
    tap_obj(cell(1));
    CHECK("the middle photo opened", wait_until(photo_shown, 4000) && shown_label("IMG_0002.ppm"));
    tap_obj(button("DELETE"));
    CHECK("DELETE asks first", shows_text("Delete this photo?") && enabled("CANCEL") &&
                                   enabled("DELETE") && button("BACK") == NULL);
    CHECK("and nothing moves while it asks", !enabled("NEWER") && !enabled("OLDER"));
    check_targets(shape, 2);
    tap_obj(button("CANCEL"));
    CHECK("CANCEL keeps it", exists("IMG_0002.ppm") && button("EXPORT") != NULL);
    tap_obj(button("DELETE"));
    tap_obj(button("DELETE"));
    CHECK("confirmed: deleted, and said", wait_until_text("Photo deleted", 4000) &&
                                             !exists("IMG_0002.ppm"));
    CHECK("the next older takes its place", wait_until(photo_shown, 4000) &&
                                               shown_label("IMG_0001.ppm") &&
                                               /* after the note's 3 s on the status line */
                                               wait_until_text("2 of 2", 5000));
    CHECK("the others are untouched", exists("IMG_0001.ppm") && exists("IMG_0003.ppm"));
    tap_obj(button("BACK"));
    CHECK("the grid at once one fewer", wait_until(in_grid, 2000) && cells_shown() == 2);

    /* A delete the filesystem refuses: said, the photo kept. */
    if (geteuid() != 0) {
        tap_obj(cell(0));
        wait_until(photo_shown, 4000);
        chmod(photos, 0555);
        tap_obj(button("DELETE"));
        tap_obj(button("DELETE"));
        CHECK("a refused delete says so", wait_until_text("The photo could not be deleted", 4000));
        CHECK("and the photo is still there, and shown", exists("IMG_0003.ppm") &&
                                                            shown_label("IMG_0003.ppm") &&
                                                            enabled("DELETE"));
        chmod(photos, 0755);
        tap_obj(button("BACK"));
        CHECK("the grid still has both", wait_until(in_grid, 2000) && cells_shown() == 2);
    } else {
        printf("note: running as root: a read-only folder refuses no delete, not checked\n");
    }

    /* Everything deleted: the empty library. */
    tap_obj(cell(0));
    wait_until(photo_shown, 4000);
    tap_obj(button("DELETE"));
    tap_obj(button("DELETE"));
    wait_until_text("Photo deleted", 4000);
    wait_until(photo_shown, 4000);
    tap_obj(button("DELETE"));
    tap_obj(button("DELETE"));
    CHECK("the last photo deleted: the empty library", wait_until(empty_library, 4000));
    CHECK("the folder is empty and still there", !exists("IMG_0001.ppm") && !exists("IMG_0003.ppm") &&
                                                    access(photos, F_OK) == 0);
    app_stop();
    CHECK("closed, no helper", no_child());
#undef CHECK
}

static void faults(void)
{
    char *keep = strdup(getenv("POCKETOS_CAMERA_HELPER"));

    empty_photos();
    write_photo("IMG_0001.ppm", 64, 36, NULL, 1);
    setenv("POCKETOS_CAMERA_HELPER", "/nonexistent/pos-camera", 1);
    app_start();
    check("a missing helper: 'Photos unavailable' with TRY AGAIN",
          wait_until(failed_screen, 3000) && shows_text("Photos unavailable"));
    check("and no CAMERA to go to", button("CAMERA") == NULL);
    check_targets("failed", 1);
    setenv("POCKETOS_CAMERA_HELPER", keep, 1);
    free(keep);
    tap_obj(button("TRY AGAIN"));
    check("once it is there, TRY AGAIN lists the photos", wait_until(in_grid, 4000));
    app_stop();
    check("closed, no helper", no_child());
}

static void big_library(void)
{
    int i;
    int64_t t0;
    int64_t took;
    char name[64];

    empty_photos();
    for (i = 1; i <= 1100; i++) {
        snprintf(name, sizeof(name), "IMG_%04d.ppm", i);
        write_photo(name, 8, 8, NULL, (unsigned)i);
    }
    t0 = mono_ms();
    app_start();
    check("1100 photos: the grid comes up", wait_until(in_grid, 8000));
    took = mono_ms() - t0;
    printf("     1100 photos listed in %lld ms\n", (long long)took);
    check("with the newest 1000 of them, and says so", shows_text("Newest 1000 of 1100 photos"));
    tap_obj(cell(0));
    check("the newest is IMG_1100", wait_until(photo_shown, 4000) && shown_label("IMG_1100.ppm"));
    tap_obj(button("BACK"));
    wait_until(in_grid, 2000);
    for (i = 0; i < 3; i++) {
        tap_obj(button("OLDER"));
    }
    wait_until(in_grid, 2000);
    {
        lv_obj_t *status = gallery_child(G_SHOW_BOX + 1);

        printf("     after three OLDER: \"%s\"\n", status ? lv_label_get_text(status) : "?");
    }
    check("OLDER turns pages", wait_until_text("Newest 1000 of 1100 photos  |  page 4", 2000));
    want_cell = 0;
    check("and the page's thumbnails come", wait_until(cell_has_picture, 4000));
    took = app_stop();
    check("closing a big library is quick", took < 600);
    check("closed, no helper", no_child());
    empty_photos();
}

static void lifecycle(void)
{
    int i;
    int fds0;
    int fds1;
    size_t heap0;
    size_t heap1;
    int64_t took;
    int64_t worst = 0;
    bool all = true;

    empty_photos();
    for (i = 1; i <= 14; i++) {
        char name[32];

        snprintf(name, sizeof(name), "IMG_%04d.ppm", i);
        write_photo(name, 64, 36, NULL, (unsigned)i);
    }

    /* Closed mid-photo and mid-slideshow. */
    app_start();
    wait_until(in_grid, 4000);
    tap_obj(cell(2));
    wait_until(photo_shown, 4000);
    took = app_stop();
    check("closing on a photo is bounded", took < 600);
    check("and leaves no helper", no_child());
    app_start();
    wait_until(in_grid, 4000);
    tap_obj(button("SLIDESHOW"));
    wait_until(slide_shown, 4000);
    took = app_stop();
    printf("     closed mid-slideshow in %lld ms\n", (long long)took);
    check("closing mid-slideshow is bounded", took < 600);
    check("and leaves no helper", no_child());

    /* Twenty opens and closes, each with a page and a photo: nothing grows. */
    app_start();
    wait_until(all_cells_settled, 6000);
    app_stop();
    fds0 = open_fds();
    heap0 = mallinfo2().uordblks;
    for (i = 0; i < 20; i++) {
        app_start();
        all &= wait_until(in_grid, 4000);
        all &= wait_until(all_cells_settled, 6000);
        tap_obj(cell(i % 4));
        all &= wait_until(photo_shown, 4000);
        took = app_stop();
        worst = took > worst ? took : worst;
        all &= no_child();
    }
    fds1 = open_fds();
    heap1 = mallinfo2().uordblks;
    printf("     20 opens: fds %d -> %d, heap %zu -> %zu bytes, slowest close %lld ms\n", fds0, fds1,
           heap0, heap1, (long long)worst);
    check("twenty opens and closes, each with a page and a photo", all);
    check("no descriptor left behind", fds1 <= fds0);
    /* One page of thumbnails alone is 0.9 MB: anything kept would show. */
    check("no picture left behind (the heap within 128 KB)",
          heap1 <= heap0 + 128 * 1024);
    check("every close bounded", worst < 600);
}

int main(void)
{
    lv_indev_t *finger;
    const char *helper = getenv("CAMERA_HELPER");
    char cmd[300];

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!helper || access(helper, X_OK) != 0) {
        printf("FAIL set CAMERA_HELPER to tests/pos-camera-testhooks\n");
        return 2;
    }
    snprintf(root, sizeof(root), "/tmp/photo-app-%ld", (long)getpid());
    snprintf(photos, sizeof(photos), "%s/camera", root);
    mkdir(root, 0755);
    mkdir(photos, 0755);
    setenv("POCKETOS_STATE_DIR", root, 1);
    {
        char home[200];

        snprintf(home, sizeof(home), "%s/home", root);
        setenv("HOME", home, 1); /* EXPORT's $HOME/Pictures, never the real one */
    }
    setenv("POCKETOS_CAMERA_HELPER", helper, 1);
    setenv("POCKETOS_CAMERA_BACKEND", "fake", 1);
    unsetenv("POCKETOS_CAMERA_FAKE");
    unsetenv("POCKETCAM_TEST_FREE_BYTES");

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    finger = lv_indev_create();
    lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(finger, read_cb);
    pos_input_init();
    pocketui_init();
    pocketui_style_screen(lv_screen_active());
    g_content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_content);

    check("Photo is fullscreen (NONE), like Camera", app_photo.chrome == POCKETOS_CHROME_NONE);
    check("with its own icon", app_photo.icon_mask != NULL && strcmp(app_photo.id, "photo") == 0);

    use_display(POS_ROTATION_0);
    empty_and_one("portrait");
    many("portrait");
    damaged("portrait");
    deleting("portrait");
    use_display(POS_ROTATION_90);
    empty_and_one("landscape");
    many("landscape");
    damaged("landscape");
    deleting("landscape");
    use_display(POS_ROTATION_0);
    faults();
    big_library();
    lifecycle();

    snprintf(cmd, sizeof(cmd), "chmod -R u+w '%s' 2>/dev/null; rm -rf '%s'", root, root);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", root);
    }
    printf("photo_app_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
