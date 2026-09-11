/*
 * PocketNotes in the running app, driven by a real LVGL pointer device and a
 * real touch keyboard, against a real (temporary) store.
 *
 * Nothing here sets the field's text to make a note: every character is
 * tapped on the keyboard, travels the logical stream, and lands in the
 * focused field, the way it will on the panel. What is then read back off
 * the disk is what the store actually kept.
 *
 * The app is hosted the way the shell hosts it, but the shell itself is not
 * here, so the three app.h keyboard entry points are implemented below
 * against the real pos_keyboard. That is exactly what the shell does, and it
 * keeps the app honest: it can only ask, and it never sees a keyboard.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/notes_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "notes_store.h"
#include "notes_view.h"
#include "pocketui.h"
#include "pos_keyboard.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
#define STATUS_H POCKETUI_STATUS_BAR_H

extern const struct pocketos_app app_notes;

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

/* ---- the shell's side of app.h, as the shell implements it ------------- */

static lv_obj_t *g_keyboard;
static lv_obj_t *g_content;
static char g_hint[64];

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(g_hint, sizeof(g_hint), "%s", text ? text : "");
}

void pocketos_shell_go_home(void) { }
int pocketos_shell_reduced_motion(void) { return 0; }
const char *pocketos_shell_radio_state(void) { return NULL; }

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret,
                                  void (*on_done)(void *user), void *user)
{
    (void)on_done;
    (void)user;
    pos_keyboard_set_return(g_keyboard, ret == POCKETOS_KB_NEWLINE ? POS_KB_RETURN_NEWLINE
                                                                   : POS_KB_RETURN_DONE);
    lv_obj_set_height(g_content, PANEL_H - STATUS_H - POS_KB_H);
    pos_keyboard_show(g_keyboard);
}

void pocketos_shell_keyboard_hide(void)
{
    pos_keyboard_hide(g_keyboard);
    lv_obj_set_height(g_content, PANEL_H - STATUS_H);
}

int pocketos_shell_keyboard_visible(void)
{
    return pos_keyboard_is_shown(g_keyboard);
}

/* ---- display and finger ------------------------------------------------ */

static uint8_t draw_buf[PANEL_W * 40 * 2];
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;

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

static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void drain(void)
{
    int t;

    for (t = 0; t < 300 && pos_input_queued() > 0; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
    pump(40);
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
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
    drain();
}

static void tap_key(const char *label)
{
    tap_obj(pos_keyboard_key(g_keyboard, label));
}

/* Type it the way a person would: the alpha layer shows lower case, so a
 * capital needs Shift first. One-shot Shift clears itself after the letter,
 * which is why no capital here ever leaks into the next one. */
static void type_text(const char *s)
{
    char one[2] = { 0, 0 };

    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            tap_key("SHIFT");
        }
        one[0] = *s;
        tap_key(one);
    }
}

/* ---- finding things in the app's tree ---------------------------------- */

/* Depth-first search for a label with this text; returns its clickable
 * ancestor, which is what a finger would press. */
static lv_obj_t *find_labelled(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    /* A hidden screen is still in the tree, and both the editor and the
     * confirmation carry a button labelled Delete. A finger cannot press
     * what is not shown, so neither may this. */
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && strcmp(t, text) == 0) {
            lv_obj_t *p = obj;

            while (p && !lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) {
                p = lv_obj_get_parent(p);
            }
            return p ? p : obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_labelled(lv_obj_get_child(obj, i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static lv_obj_t *find_field(lv_obj_t *obj)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        return obj;
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_field(lv_obj_get_child(obj, i));

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static int label_present(lv_obj_t *obj, const char *text)
{
    return find_labelled(obj, text) != NULL;
}

/* ---- the app, hosted the way the shell hosts it ------------------------ */

/* The shell's own frame (ui/shell/shell.c, app_open): a header row, then a
 * body that grows into what is left, padded the same way. Whether a long
 * list fits is a question about exactly these pixels. */
static lv_obj_t *app_root;
static lv_obj_t *app_body;
static void *app_priv;

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
    lv_obj_set_style_pad_row(app_body, POCKETUI_PAD, 0);
    lv_obj_add_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(app_body, LV_DIR_VER);
    app_priv = app_notes.create(app_body);
    pump(60);
}

/* The shell's app_close(): hide the keyboard, destroy, delete the root. */
static void app_stop(void)
{
    pocketos_shell_keyboard_hide();
    app_notes.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_root);
    app_root = NULL;
    app_body = NULL;
    pump(60);
}

static void wipe(void)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        /* nothing there yet */
    }
}

/* ---- what is on disk, and what a finger can reach ---------------------- */

/* A file's bytes, read straight off the disk rather than through the store:
 * the point is to see what is actually there. Returns how many, or -1. */
static long read_raw(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t got;

    if (!f) {
        return -1;
    }
    got = fread(buf, 1, cap, f);
    fclose(f);
    return (long)got;
}

/* Back-date a file, so a rewrite shows up as a changed time. */
static void set_mtime(const char *path, time_t when)
{
    struct timespec times[2];

    times[0].tv_sec = when;
    times[0].tv_nsec = 0;
    times[1] = times[0];
    if (utimensat(AT_FDCWD, path, times, 0) != 0) {
        printf("FAIL cannot set the time of %s\n", path);
        failed++;
        checks++;
    }
}

static int has_mtime(const char *path, time_t when)
{
    struct stat st;

    return stat(path, &st) == 0 && st.st_mtime == when;
}

/* Whether obj lies wholly inside clip's box: on screen, where a finger can
 * reach it, rather than drawn past an edge that cuts it off. */
static int inside(lv_obj_t *obj, lv_obj_t *clip)
{
    lv_area_t o;
    lv_area_t c;

    if (!obj || !clip) {
        return 0;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &o);
    lv_obj_get_coords(clip, &c);
    return o.x1 >= c.x1 && o.x2 <= c.x2 && o.y1 >= c.y1 && o.y2 <= c.y2;
}

/* A finger drawn dy pixels across obj in small moves, the way a scroll
 * reaches LVGL from the panel, then time for the scroll to come to rest. */
static void drag(lv_obj_t *obj, int32_t dy)
{
    lv_area_t a;
    int32_t y0;
    int step;

    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    y0 = a.y1 + lv_area_get_height(&a) * 3 / 4;
    finger_point.y = y0;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    for (step = 1; step <= 20; step++) {
        finger_point.y = y0 + dy * step / 20;
        pump(20);
    }
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(1500);
}

int main(void)
{
    lv_display_t *disp;
    lv_indev_t *finger;
    struct notes_entry list[NOTES_MAX_NOTES];
    char text[NOTES_MAX_BYTES + 1];
    lv_obj_t *field;
    int n;

    snprintf(root, sizeof(root), "/tmp/pocketnotes-app-%u", (unsigned)getpid());
    setenv("POCKETOS_STATE_DIR", root, 1);
    wipe();

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
    lv_obj_set_size(g_content, PANEL_W, PANEL_H - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    g_keyboard = pos_keyboard_create(lv_screen_active());
    pump(60);

    /* ---- 1. the empty state -------------------------------------------- */

    app_start();
    check("an empty store shows the empty state", label_present(app_body, "No notes yet"));
    check("and offers a new note", label_present(app_body, "New note"));
    check("the keyboard is not up on the list", !pocketos_shell_keyboard_visible());

    /* ---- 2. create a note and type into it ----------------------------- */

    tap_obj(find_labelled(app_body, "New note"));
    check("the editor opens", label_present(app_body, "Done"));
    check("the keyboard comes up with it", pocketos_shell_keyboard_visible());
    field = find_field(app_body);
    check("the editor has a field", field != NULL);
    check("and it is focused", pos_input_focused() == field);

    type_text("Shopping");
    check_str("typed characters reach the note", lv_textarea_get_text(field), "Shopping");
    check("focus stayed on the field while typing", pos_input_focused() == field);

    /* ---- 3. a newline, and the second line ----------------------------- */

    tap_key("ENTER");
    type_text("milk");
    check_str("Enter breaks the line", lv_textarea_get_text(field), "Shopping\nmilk");

    /* ---- 4. æ ø å through the symbol layer ----------------------------- */

    tap_key("ENTER");
    tap_key("?123");
    tap_key("\xC3\xA6");
    tap_key("\xC3\xB8");
    tap_key("\xC3\xA5");
    tap_key("ABC");
    check_str("Norwegian letters type like any other",
              lv_textarea_get_text(field), "Shopping\nmilk\n\xC3\xA6\xC3\xB8\xC3\xA5");

    /* ---- 5. Done saves and returns to the list ------------------------- */

    tap_obj(find_labelled(app_body, "Done"));
    check("the keyboard goes away with the editor",
          !pocketos_shell_keyboard_visible());
    check("the list is back", label_present(app_body, "New note"));
    check("titled by the note's first line", label_present(app_body, "Shopping"));

    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("one note was stored", n == 1);
    check("with the text that was typed",
          notes_store_read(list[0].id, text, sizeof(text)) ==
              (int)strlen("Shopping\nmilk\n\xC3\xA6\xC3\xB8\xC3\xA5"));
    check_str("byte for byte", text, "Shopping\nmilk\n\xC3\xA6\xC3\xB8\xC3\xA5");

    /* ---- 6. reopen it -------------------------------------------------- */

    tap_obj(find_labelled(app_body, "Shopping"));
    field = find_field(app_body);
    check_str("reopening shows what was written",
              lv_textarea_get_text(field), "Shopping\nmilk\n\xC3\xA6\xC3\xB8\xC3\xA5");
    check("the keyboard is up again", pocketos_shell_keyboard_visible());

    /* Punctuation lives on the symbol layer, and the layer the keyboard is
     * left on is its own business, not the app's. */
    tap_key("?123");
    tap_key("!");
    tap_key("ABC");
    tap_obj(find_labelled(app_body, "Done"));
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("editing does not make a second note", n == 1);
    notes_store_read(list[0].id, text, sizeof(text));
    check("the edit was saved",
          text[strlen(text) - 1] == '!');

    /* ---- 7. a blank note is not worth a file --------------------------- */

    tap_obj(find_labelled(app_body, "New note"));
    tap_key("SPACE");
    tap_key("ENTER");
    tap_obj(find_labelled(app_body, "Done"));
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("a note with only whitespace is not stored", n == 1);

    /* ---- 8. leaving the app saves what was being written ---------------- */

    tap_obj(find_labelled(app_body, "New note"));
    type_text("draft");
    app_stop();                 /* the shell closing the app */
    check("the keyboard is down after the app closes",
          !pocketos_shell_keyboard_visible());
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("an unsaved edit survives the app closing", n == 2);
    app_start();
    check("and is there on the next launch", label_present(app_body, "draft"));

    /* ---- 9. delete: cancel changes nothing ----------------------------- */

    tap_obj(find_labelled(app_body, "draft"));
    check("the keyboard is up in the editor", pocketos_shell_keyboard_visible());
    tap_obj(find_labelled(app_body, "Delete"));
    check("the confirmation asks first", label_present(app_body, "Delete this note?"));
    check("the keyboard is dismissed for the dialog (DS §17.5)",
          !pocketos_shell_keyboard_visible());
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("nothing is deleted when the dialog opens", n == 2);

    tap_obj(find_labelled(app_body, "Cancel"));
    check("Cancel returns to the editor", label_present(app_body, "Done"));
    check("and brings the keyboard back", pocketos_shell_keyboard_visible());
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("and deleted nothing", n == 2);

    /* ---- 10. delete: confirm removes it -------------------------------- */

    tap_obj(find_labelled(app_body, "Delete"));   /* editor: open the dialog */
    tap_obj(find_labelled(app_body, "Delete"));   /* dialog: confirm */
    n = notes_store_list(list, NOTES_MAX_NOTES);
    check("confirming deletes the note", n == 1);
    check("and returns to the list", label_present(app_body, "New note"));
    check("the deleted note is gone from the list", !label_present(app_body, "draft"));
    check("the other note is untouched", label_present(app_body, "Shopping"));

    /* ---- 11. a note that is not text is shown, not edited over ---------- */

    app_stop();
    {
        char path[256];
        FILE *f;

        snprintf(path, sizeof(path), "%s/notes/note-00000042.txt", root);
        f = fopen(path, "wb");
        if (f) {
            (void)!fwrite("bad \xFF byte", 1, 10, f);
            fclose(f);
        }
    }
    app_start();
    check("an unreadable note is listed", label_present(app_body, "Unreadable note"));
    tap_obj(find_labelled(app_body, "Unreadable note"));
    field = find_field(app_body);
    check("opening it does not offer an editable field",
          lv_obj_has_state(field, LV_STATE_DISABLED));
    check("and says why", label_present(app_body, "This note could not be read. "
                                                  "It is left exactly as it is."));
    check("the keyboard stays down for it", !pocketos_shell_keyboard_visible());
    tap_obj(find_labelled(app_body, "Done"));
    {
        char path[256];
        FILE *f;
        char raw[32];
        size_t got = 0;

        snprintf(path, sizeof(path), "%s/notes/note-00000042.txt", root);
        f = fopen(path, "rb");
        if (f) {
            got = fread(raw, 1, sizeof(raw), f);
            fclose(f);
        }
        check("leaving it did not overwrite it", got == 10);
    }

    /* ---- 12. a long note scrolls rather than overflowing ---------------- */

    tap_obj(find_labelled(app_body, "New note"));
    field = find_field(app_body);
    {
        int i;

        for (i = 0; i < 40; i++) {
            type_text("line");
            tap_key("ENTER");
        }
    }
    check("a long note is all there",
          strlen(lv_textarea_get_text(field)) == 40 * 5);
    lv_obj_update_layout(field);
    check("the editor scrolls rather than growing past the body",
          lv_obj_get_height(field) <= PANEL_H - STATUS_H - POS_KB_H);
    check("and the caret is still in view: the field scrolled",
          lv_obj_get_scroll_y(field) > 0);
    check("the body itself did not become scrollable",
          lv_obj_get_scroll_bottom(app_body) <= 0);
    tap_obj(find_labelled(app_body, "Done"));

    app_stop();

    /* ---- 13. a note longer than the editor holds is shown, not cut ------ */

    /* The store takes up to 4096 bytes and the editor 2000 characters, so a
     * note written somewhere other than this app can be longer than the
     * field. Opening one used to cut it to fit on the way in and save the
     * cut version on the way out (P1-2 of the v0.0.8 review). */
    wipe();
    {
        static char big[3001];
        static char raw[NOTES_MAX_BYTES + 1];
        char path[256];
        time_t old = time(NULL) - 7200;
        int i;

        memcpy(big, "Long note\n", 10);
        for (i = 10; i < 3000; i++) {
            big[i] = (i % 50 == 49) ? '\n' : 'a';
        }
        big[3000] = '\0';
        check("a 3000-character note is one the store accepts",
              notes_store_write(60, big) == 0);
        notes_store_path(60, path, sizeof(path));
        set_mtime(path, old);

        app_start();
        tap_obj(find_labelled(app_body, "Long note"));
        field = find_field(app_body);
        check("it opens", field != NULL);
        check("read-only", field && lv_obj_has_state(field, LV_STATE_DISABLED));
        check("and says why",
              label_present(app_body, "This note is too long to edit here. "
                                      "It is left exactly as it is."));
        check("showing all of it, not the first 2000 characters",
              field && strcmp(lv_textarea_get_text(field), big) == 0);
        check("the keyboard stays down for it", !pocketos_shell_keyboard_visible());

        tap_obj(find_labelled(app_body, "Done"));
        check("Done goes back to the list", label_present(app_body, "New note"));
        check("and the note is byte for byte what it was",
              read_raw(path, raw, sizeof(raw)) == 3000 && memcmp(raw, big, 3000) == 0);

        tap_obj(find_labelled(app_body, "Long note"));
        app_stop(); /* the shell's Back */
        check("leaving the app from it keeps it byte for byte too",
              read_raw(path, raw, sizeof(raw)) == 3000 && memcmp(raw, big, 3000) == 0);
        check("and nothing wrote to it at all", has_mtime(path, old));
    }

    /* ---- 14. a note nobody changed is not written again ----------------- */

    {
        static char raw[NOTES_MAX_BYTES + 1];
        char path[256];
        time_t old = time(NULL) - 3600;

        notes_store_write(61, "Untouched\nsecond line");
        notes_store_path(61, path, sizeof(path));
        set_mtime(path, old);

        app_start();
        tap_obj(find_labelled(app_body, "Untouched"));
        field = find_field(app_body);
        check("an ordinary note opens editable",
              field && !lv_obj_has_state(field, LV_STATE_DISABLED));
        tap_obj(find_labelled(app_body, "Done"));
        check("Done on a note nobody changed leaves its time alone", has_mtime(path, old));

        tap_obj(find_labelled(app_body, "Untouched"));
        app_stop();
        check("and so does leaving the app from it", has_mtime(path, old));
        check("the bytes are what they were",
              read_raw(path, raw, sizeof(raw)) == 21 &&
                  memcmp(raw, "Untouched\nsecond line", 21) == 0);

        /* A change is still a change. */
        app_start();
        tap_obj(find_labelled(app_body, "Untouched"));
        tap_key("?123");
        tap_key("!");
        tap_key("ABC");
        tap_obj(find_labelled(app_body, "Done"));
        check("an edited note is written", !has_mtime(path, old));
        check("with the edit in it", read_raw(path, raw, sizeof(raw)) == 22 && raw[21] == '!');
        app_stop();
    }

    /* ---- 15. twenty notes: the list scrolls and New note stays ---------- */

    /* From sixteen notes the list outgrew a screen that does not scroll, and
     * New note, then the oldest notes, were drawn past its bottom edge where
     * no finger could reach them (P1-1 of the v0.0.8 review). Checked in the
     * shell's own frame, in Normal and in Outdoor, whose type is larger. */
    wipe();
    {
        static const char *const modes[] = { "normal", "outdoor" };
        char path[256];
        char why[128];
        char what[96];
        time_t base = time(NULL) - 100000;
        size_t m;
        int i;

        for (i = 1; i <= 20; i++) {
            char body[32];

            snprintf(body, sizeof(body), "Note %02d\nbody", i);
            notes_store_write((uint32_t)i, body);
            notes_store_path((uint32_t)i, path, sizeof(path));
            set_mtime(path, base + i * 60); /* Note 01 is the oldest */
        }

        for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
            lv_obj_t *list_screen;
            lv_obj_t *oldest;
            lv_obj_t *rows;
            int drags;

            snprintf(what, sizeof(what), "[%s] the mode applies", modes[m]);
            check(what, pos_theme_apply(NULL, modes[m], why, sizeof(why)) == 0);
            app_start();
            list_screen = lv_obj_get_child(app_body, 0);

            snprintf(what, sizeof(what), "[%s] New note is on screen with twenty notes",
                     modes[m]);
            check(what, inside(find_labelled(app_body, "New note"), list_screen));
            snprintf(what, sizeof(what), "[%s] the body itself does not scroll", modes[m]);
            check(what, lv_obj_get_scroll_bottom(app_body) <= 0);

            oldest = find_labelled(app_body, "Note 01");
            rows = oldest ? lv_obj_get_parent(oldest) : NULL;
            snprintf(what, sizeof(what), "[%s] the oldest note starts past the list's edge",
                     modes[m]);
            check(what, oldest && !inside(oldest, rows));
            for (drags = 0; drags < 6 && rows && !inside(oldest, rows); drags++) {
                drag(rows, -400);
            }
            snprintf(what, sizeof(what), "[%s] a finger scrolls the list to it", modes[m]);
            check(what, inside(oldest, rows) && inside(rows, list_screen));
            snprintf(what, sizeof(what), "[%s] with New note still on screen", modes[m]);
            check(what, inside(find_labelled(app_body, "New note"), list_screen));

            tap_obj(oldest);
            field = find_field(app_body);
            snprintf(what, sizeof(what), "[%s] and the oldest note opens", modes[m]);
            check(what, field && strcmp(lv_textarea_get_text(field), "Note 01\nbody") == 0);
            tap_obj(find_labelled(app_body, "Done"));

            tap_obj(find_labelled(app_body, "New note"));
            field = find_field(app_body);
            snprintf(what, sizeof(what), "[%s] New note opens an empty editor", modes[m]);
            check(what, field && lv_textarea_get_text(field)[0] == '\0' &&
                            pocketos_shell_keyboard_visible());
            tap_obj(find_labelled(app_body, "Done")); /* blank, so nothing is stored */
            snprintf(what, sizeof(what), "[%s] and there are still twenty notes", modes[m]);
            check(what, notes_store_list(list, NOTES_MAX_NOTES) == 20);
            app_stop();
        }
        pos_theme_apply(NULL, "normal", why, sizeof(why));
    }

    /* ---- 16. a short list looks exactly as it did ----------------------- */

    wipe();
    notes_store_write(1, "One");
    notes_store_write(2, "Two");
    app_start();
    {
        lv_obj_t *row = find_labelled(app_body, "One");
        lv_obj_t *rows = row ? lv_obj_get_parent(row) : NULL;
        lv_obj_t *button = find_labelled(app_body, "New note");
        lv_area_t r;
        lv_area_t b;

        check("a short list has both rows", rows && label_present(rows, "Two"));
        if (rows && button) {
            lv_obj_update_layout(app_body);
            lv_obj_get_coords(rows, &r);
            lv_obj_get_coords(button, &b);
            check("it is as tall as its rows and no taller",
                  lv_area_get_height(&r) ==
                      2 * POCKETUI_ROW_H + 2 * lv_obj_get_style_border_width(rows, LV_PART_MAIN));
            check("with nothing to scroll",
                  lv_obj_get_scroll_top(rows) <= 0 && lv_obj_get_scroll_bottom(rows) <= 0);
            check("and New note right under it", b.y1 - r.y2 - 1 == POCKETUI_PAD);
        }
    }
    app_stop();
    wipe();
    printf("notes_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
