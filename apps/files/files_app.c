/*
 * Files: a file explorer - browse, read text, and create, rename, copy, move
 * and delete, with the places Doors and the system depend on kept read-only.
 *
 * Five screens in one body, one visible at a time: the browser, the text
 * viewer, the name entry (new folder, rename), the delete confirmation and
 * Storage (Internal Storage and the USB drive, from the drive button beside
 * Up).
 *
 * The USB drive is sysd's. Storage asks sysd for storage.status while it is
 * on screen and for storage.eject when Eject is pressed, both with the UI
 * deadline (SHELL_IPC_UI_TIMEOUT_MS); the eject runs in sysd and Storage
 * shows "Ejecting…" until sysd says it is safe to remove. Files itself never
 * mounts or unmounts anything (docs/api/system.md, "Storage").
 *
 * Nothing here touches the filesystem directly. Listing and reading text go
 * through files_fs.h on the LVGL thread - both are bounded, a directory to
 * FILES_LIST_MAX entries and a text to FILES_TEXT_MAX bytes - and copy, move
 * and delete run on the worker of files_job.h, which a timer polls. No file
 * operation ever runs inside a layout or draw pass: they are started from a
 * tap and finished from the timer.
 *
 * A tap selects an entry; a tap on the selected entry, or Open, opens it: a
 * folder is entered, a text file is shown. Copy and Move carry the selection:
 * the owner goes to the folder it should go to and presses Paste here.
 *
 * The safety rules are files_fs.h's, and the operations enforce them
 * themselves. The app only asks the same questions first, so an action that
 * would be refused is shown disabled and the reason is said on screen.
 *
 * LAYOUT. Chosen from the body the app is given, never from the orientation
 * (DS section 21.2), and chosen again whenever that body changes size:
 *
 *   TALL (portrait, 528 x 1060 on the reference panel). One column: the
 *   path bar (Up and the path), Sort and New folder, the list, a status
 *   line, and the actions for the selected entry across the foot.
 *
 *   WIDE (landscape, 1192 x 396). The list column on the left with Sort and
 *   New folder moved up into the path bar, and a details pane of FILES_SIDE_W
 *   on the right: the selected entry's name, type, size, time and access,
 *   with the actions under them. Chosen only when the list keeps at least
 *   the portrait width beside the pane.
 *
 * The objects are built once and only shaped (flow, sizes, parents) by the
 * layout, so nothing selected, typed or carried is lost when the shape
 * changes; the list's rows are the only thing rebuilt, from the directory.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "files_fs.h"
#include "files_job.h"
#include "files_storage.h"
#include "files_view.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "shell_ipc.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The rows the list shows at most; a larger folder says how many it has. */
#define FILES_ROWS_MAX 200
/* The wide shape's details pane, and the list's floor beside it: the portrait
 * body's width, so no row is ever narrower there than in portrait. */
#define FILES_SIDE_W 420
#define FILES_LIST_MIN_W 528
/* Paired and bar buttons, DS section 7; rows are POCKETUI_ROW_H. */
#define FILES_BTN_H 56
#define FILES_UP_W 72
#define FILES_TOOL_W 176
#define FILES_GAP 12
/* Name entry's two buttons beside the field in the wide shape. */
#define FILES_NAME_BTN_W 140
/* How often the timer looks at the worker and at deferred actions. */
#define FILES_TIMER_MS 30
/* How often Storage asks sysd about the drive while it is on screen, and
 * how often while an eject runs. */
#define FILES_USB_POLL_MS 1000
#define FILES_USB_EJECT_POLL_MS 300

enum files_screen {
    SCREEN_BROWSE = 0,
    SCREEN_VIEWER,
    SCREEN_NAME,
    SCREEN_CONFIRM,
    SCREEN_STORAGE,
    SCREEN_COUNT
};

enum { ACT_OPEN = 0, ACT_RENAME, ACT_COPY, ACT_MOVE, ACT_DELETE, ACT_COUNT };

enum files_carry { CARRY_NONE = 0, CARRY_COPY, CARRY_MOVE };
enum files_name_mode { NAME_NEW_FOLDER = 0, NAME_RENAME };

struct files_app {
    lv_obj_t *frame;
    lv_obj_t *screen[SCREEN_COUNT];

    /* the browser */
    lv_obj_t *main;        /* the list column */
    lv_obj_t *bar;         /* Up, Storage, the path; in the wide shape also Sort and New folder */
    lv_obj_t *up;
    lv_obj_t *drive;       /* opens Storage */
    lv_obj_t *path;
    lv_obj_t *tools;       /* Sort and New folder, in the tall shape */
    lv_obj_t *sort;
    lv_obj_t *new_folder;
    lv_obj_t *list;        /* a card; its rows are rebuilt with the directory */
    lv_obj_t *status;
    lv_obj_t *side;        /* the wide shape's details pane */
    lv_obj_t *details;
    lv_obj_t *d_name;
    lv_obj_t *d_kind;
    lv_obj_t *d_when;
    lv_obj_t *d_access;
    lv_obj_t *actions;
    lv_obj_t *act[ACT_COUNT];
    lv_obj_t *paste;       /* in place of the actions while something is carried */
    lv_obj_t *paste_label;
    lv_obj_t *paste_here;

    /* the viewer */
    lv_obj_t *v_title;
    lv_obj_t *v_card;
    lv_obj_t *v_text;
    lv_obj_t *v_note;

    /* the name entry */
    lv_obj_t *n_title;
    lv_obj_t *n_form;
    lv_obj_t *n_buttons;
    lv_obj_t *n_field;
    lv_obj_t *n_ok;

    /* the delete confirmation */
    lv_obj_t *dialog;
    lv_obj_t *c_title;
    lv_obj_t *c_body;

    /* Storage */
    lv_obj_t *s_internal;  /* Internal Storage's caption */
    lv_obj_t *s_usb;       /* the USB drive's caption */
    lv_obj_t *s_eject;
    lv_obj_t *s_status;
    struct files_usb usb;
    uint32_t usb_polled_at;

    struct pocketui_layout_guard layout_guard;
    bool wide;
    int32_t width;         /* the frame's content width the shape was chosen for */
    lv_timer_t *timer;
    const struct files_policy *pol;

    char cwd[FILES_PATH_MAX];
    struct files_dir dir;
    enum files_sort sort_key;
    lv_obj_t *rows[FILES_ROWS_MAX];
    int nrows;
    int selected;          /* index into dir.entries, -1 for none */
    bool open_pending;     /* a row asked to be opened: done from the timer */
    bool can_create;       /* the policy lets things be made in cwd */

    enum files_carry carry;
    char carry_path[FILES_PATH_MAX];
    char carry_name[FILES_NAME_MAX + 1];

    enum files_name_mode name_mode;

    struct files_job job;
    char job_name[FILES_NAME_MAX + 1];

    char text[FILES_TEXT_MAX + 1]; /* the viewer's scratch */
};

static void refresh(struct files_app *a, const char *select_name);
static void update_selection(struct files_app *a);
static void start_dir(char *out, size_t out_len);

/* ---- small things ---------------------------------------------------------- */

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* A button of the bar height; secondary ones do not carry the accent. The
 * role is kept in its user data so it can be disabled and given back. */
static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user,
                        bool primary)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, user);

    lv_obj_set_height(b, FILES_BTN_H);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    if (!primary) {
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
        pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
    }
    lv_obj_set_user_data(b, (void *)(uintptr_t)primary);
    lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_CLIP);
    return b;
}

/* The disable treatment of DS section 9: the disabled role in place of the
 * button's own, and no taps. Only a change is applied. */
static void button_enable(lv_obj_t *b, bool on)
{
    enum pos_style_role role = lv_obj_get_user_data(b) ? POS_STYLE_BUTTON_PRIMARY
                                                       : POS_STYLE_BUTTON_SECONDARY;

    if (on == lv_obj_has_flag(b, LV_OBJ_FLAG_CLICKABLE)) {
        return;
    }
    if (on) {
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_DISABLED), 0);
        pos_style_add(b, role, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_style(b, pos_style(role), 0);
        pos_style_add(b, POS_STYLE_BUTTON_DISABLED, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_invalidate(b);
}

/* One line, cut with an ellipsis. LV_LABEL_LONG_DOT alone does not do that:
 * a label that sizes its own height wraps instead, which on unit A put a
 * 235-character name over three lines and out of its row. The height is one
 * line of the label's own font, so the dots have a line to end. */
static void one_line(lv_obj_t *lb)
{
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_height(lb, lv_font_get_line_height(lv_obj_get_style_text_font(lb, LV_PART_MAIN)));
}

/* The status line: what just happened, or why this place is read-only. */
static void say(struct files_app *a, const char *text, bool error)
{
    lv_label_set_text(a->status, text ? text : "");
    lv_obj_remove_style(a->status, pos_style(POS_STYLE_STATUS_ERROR_TEXT), 0);
    if (error) {
        pos_style_add(a->status, POS_STYLE_STATUS_ERROR_TEXT, 0);
    }
}

static void sayf_name(struct files_app *a, bool error, const char *fmt, const char *name,
                      const char *tail)
{
    char safe[FILES_NAME_MAX + 1];
    char line[FILES_NAME_MAX + 160];

    files_view_name(safe, sizeof(safe), name);
    snprintf(line, sizeof(line), fmt, safe, tail ? tail : "");
    say(a, line, error);
}

static const struct files_entry *selected_entry(const struct files_app *a)
{
    if (a->selected < 0 || a->selected >= a->dir.n) {
        return NULL;
    }
    return &a->dir.entries[a->selected];
}

static int selected_path(const struct files_app *a, char *out, size_t out_len)
{
    const struct files_entry *e = selected_entry(a);

    if (!e) {
        return -1;
    }
    return files_path_join(out, out_len, a->cwd, e->name);
}

static void show_screen(struct files_app *a, enum files_screen which)
{
    int i;

    for (i = 0; i < SCREEN_COUNT; i++) {
        set_hidden(a->screen[i], i != (int)which);
    }
}

/* ---- the path bar ------------------------------------------------------------ */

/* The path, shortened from the front to what the bar has room for: the end
 * is where the owner is. The room is worked out from the width the shape was
 * chosen for, never measured, so it cannot depend on a half-done layout. */
static void show_path(struct files_app *a)
{
    char text[FILES_PATH_MAX + 4];
    int32_t room = a->width - 2 * (FILES_UP_W + FILES_GAP);

    if (a->wide) {
        room -= FILES_SIDE_W + POCKETUI_PAD + 2 * (FILES_TOOL_W + FILES_GAP);
    }
    files_view_path(text, sizeof(text), a->cwd, room > 0 ? (int)(room / 11) : 8);
    lv_label_set_text(a->path, text);
}

/* ---- the details pane and the actions --------------------------------------- */

static void show_details(struct files_app *a)
{
    const struct files_entry *e = selected_entry(a);
    char name[FILES_NAME_MAX + 1];
    char kind[48];
    char size[32];
    char line[128];
    char path[FILES_PATH_MAX];
    enum files_access acc;

    if (!e) {
        lv_label_set_text(a->d_name, "Nothing selected");
        lv_label_set_text(a->d_kind, "Tap a file or folder to see it here.");
        lv_label_set_text(a->d_when, "");
        lv_label_set_text(a->d_access, "");
        return;
    }
    files_view_name(name, sizeof(name), e->name);
    lv_label_set_text(a->d_name, name);
    files_view_type(kind, sizeof(kind), e);
    files_view_size(size, sizeof(size), e->size);
    if (size[0] && !files_entry_is_dir(e)) {
        snprintf(line, sizeof(line), "%s \xC2\xB7 %s", kind, size);
    } else {
        snprintf(line, sizeof(line), "%s", kind);
    }
    lv_label_set_text(a->d_kind, line);
    files_view_when(size, sizeof(size), e->mtime);
    snprintf(line, sizeof(line), size[0] ? "Modified %s" : "%s", size);
    lv_label_set_text(a->d_when, line);
    acc = selected_path(a, path, sizeof(path)) == 0 ? files_policy_entry(a->pol, path)
                                                    : FILES_ACCESS_MISSING;
    lv_label_set_text(a->d_access, acc == FILES_ACCESS_OK ? "Can be changed" : files_access_text(acc));
}

/* What can be done, from the selection, the policy and the worker. The same
 * questions the operations ask themselves, asked first so the answer can be
 * shown rather than discovered. */
static void update_actions(struct files_app *a)
{
    const struct files_entry *e = selected_entry(a);
    bool busy = files_job_busy(&a->job);
    bool modifiable = false;
    bool openable = false;
    char path[FILES_PATH_MAX];

    if (e && selected_path(a, path, sizeof(path)) == 0) {
        modifiable = files_policy_entry(a->pol, path) == FILES_ACCESS_OK;
        openable = e->kind != FILES_KIND_OTHER && !e->link_broken;
    }
    button_enable(a->act[ACT_OPEN], !busy && openable);
    button_enable(a->act[ACT_RENAME], !busy && e && modifiable);
    button_enable(a->act[ACT_COPY], !busy && openable);
    button_enable(a->act[ACT_MOVE], !busy && e && modifiable);
    button_enable(a->act[ACT_DELETE], !busy && e && modifiable);
    button_enable(a->new_folder, !busy && a->can_create);
    button_enable(a->paste_here, !busy && a->can_create);
    set_hidden(a->actions, a->carry != CARRY_NONE);
    set_hidden(a->paste, a->carry == CARRY_NONE);
}

static void update_selection(struct files_app *a)
{
    int i;

    for (i = 0; i < a->nrows; i++) {
        lv_obj_remove_style(a->rows[i], pos_style(POS_STYLE_SELECTED), 0);
        if (i == a->selected) {
            pos_style_add(a->rows[i], POS_STYLE_SELECTED, 0);
        }
    }
    show_details(a);
    update_actions(a);
}

/* ---- the list ------------------------------------------------------------------ */

static void on_row(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);
    int i = (int)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));

    if (i == a->selected) {
        /* Opening rebuilds the list, this row with it, so it is not done
         * inside the row's own event: the timer does it. */
        a->open_pending = true;
        return;
    }
    a->selected = i;
    update_selection(a);
}

static lv_obj_t *add_row(struct files_app *a, int i)
{
    const struct files_entry *e = &a->dir.entries[i];
    char name[FILES_NAME_MAX + 1];
    char caption[96];
    lv_obj_t *row = lv_obj_create(a->list);
    lv_obj_t *glyph;
    lv_obj_t *text;
    lv_obj_t *lb;

    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), POCKETUI_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);
    /* Room inside the row for the selection outline, which is drawn on its
     * edge: without it the outline ran through the glyph. */
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* The whole row is the hit area (DS section 9). */
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    if (i + 1 < a->dir.n && i + 1 < FILES_ROWS_MAX) {
        pos_style_add(row, POS_STYLE_DIVIDER, 0);
    }
    lv_obj_set_user_data(row, (void *)(uintptr_t)i);
    lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, a);

    /* The colour role first and the symbol font after it: the text roles
     * carry the body font, which has no symbol glyphs, and the later style
     * wins. */
    glyph = pocketui_label(row, files_entry_is_dir(e) ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                           files_entry_is_dir(e) ? POS_STYLE_ACCENT_TEXT : POS_STYLE_TEXT_SECONDARY);
    pos_style_add(glyph, POS_STYLE_SYMBOL, 0);
    lv_obj_set_width(glyph, 28);

    text = lv_obj_create(row);
    lv_obj_remove_style_all(text);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(text, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    files_view_name(name, sizeof(name), e->name);
    lb = pocketui_label(text, name, POS_STYLE_ROW_TITLE);
    lv_obj_set_width(lb, LV_PCT(100));
    one_line(lb);
    files_view_caption(caption, sizeof(caption), e);
    lb = pocketui_label(text, caption, POS_STYLE_CAPTION);
    lv_obj_set_width(lb, LV_PCT(100));
    one_line(lb);
    return row;
}

static void build_rows(struct files_app *a)
{
    int i;

    lv_obj_clean(a->list);
    a->nrows = 0;
    if (a->dir.n == 0) {
        lv_obj_t *empty = pocketui_label(a->list, "This folder is empty", POS_STYLE_CAPTION);

        lv_obj_set_style_pad_ver(empty, 24, 0);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }
    for (i = 0; i < a->dir.n && i < FILES_ROWS_MAX; i++) {
        a->rows[a->nrows++] = add_row(a, i);
    }
    lv_obj_scroll_to_y(a->list, 0, LV_ANIM_OFF);
}

/* The status line when nothing has just happened: whether this place can be
 * changed, and whether the list is all of it. */
static void say_place(struct files_app *a)
{
    char line[160];
    enum files_access acc = files_policy_dir(a->pol, a->cwd);

    if (a->dir.total > a->dir.n || a->dir.n > FILES_ROWS_MAX) {
        snprintf(line, sizeof(line), "Showing %d of %d items%s%s",
                 a->dir.n < FILES_ROWS_MAX ? a->dir.n : FILES_ROWS_MAX, a->dir.total,
                 acc == FILES_ACCESS_OK ? "" : " \xC2\xB7 ", files_access_text(acc));
        say(a, line, false);
    } else {
        say(a, files_access_text(acc), false);
    }
}

/* Select the entry called name (none when NULL or absent) and bring its row
 * into view. */
static void select_name(struct files_app *a, const char *name)
{
    int i;

    a->selected = -1;
    for (i = 0; name && i < a->dir.n; i++) {
        if (strcmp(a->dir.entries[i].name, name) == 0) {
            a->selected = i;
            break;
        }
    }
    update_selection(a);
    if (a->selected >= 0 && a->selected < a->nrows) {
        lv_obj_update_layout(a->list);
        lv_obj_scroll_to_view(a->rows[a->selected], LV_ANIM_OFF);
    }
}

/* Go to path. Returns 0, or the error with nothing changed: a folder that
 * cannot be read is not entered, and the one on screen stays as it was. */
static int go(struct files_app *a, const char *path, const char *select)
{
    struct files_dir d;
    char clean[FILES_PATH_MAX];
    int r;

    r = files_path_clean(clean, sizeof(clean), path);
    if (r == 0) {
        r = files_dir_read(&d, clean);
    }
    if (r != 0) {
        return r;
    }
    files_dir_free(&a->dir);
    a->dir = d;
    snprintf(a->cwd, sizeof(a->cwd), "%s", clean);
    files_sort(a->dir.entries, a->dir.n, a->sort_key);
    a->can_create = files_policy_dir(a->pol, a->cwd) == FILES_ACCESS_OK;
    build_rows(a);
    show_path(a);
    say_place(a);
    select_name(a, select);
    return 0;
}

/* The folder on screen read again. One that has gone is left for the
 * nearest parent that is still there. */
static void refresh(struct files_app *a, const char *select)
{
    char path[FILES_PATH_MAX];
    int r;

    snprintf(path, sizeof(path), "%s", a->cwd);
    r = go(a, path, select);
    while (r != 0 && files_path_parent(path, sizeof(path), path) == 0) {
        r = go(a, path, NULL);
        if (r == 0) {
            say(a, "The folder is no longer there", true);
        }
    }
}

/* ---- opening ------------------------------------------------------------------- */

static void open_viewer(struct files_app *a, const char *path, const char *name)
{
    char safe[FILES_NAME_MAX + 1];
    bool truncated = false;
    int r = files_read_text(path, a->text, sizeof(a->text), &truncated);

    if (r < 0) {
        sayf_name(a, true, "\xE2\x80\x9C%s\xE2\x80\x9D: %s", name, files_strerror(r));
        return;
    }
    files_view_name(safe, sizeof(safe), name);
    lv_label_set_text(a->v_title, safe);
    lv_label_set_text(a->v_text, r ? a->text : "(empty file)");
    set_hidden(a->v_note, !truncated);
    show_screen(a, SCREEN_VIEWER);
    lv_obj_scroll_to_y(a->v_card, 0, LV_ANIM_OFF);
}

static void open_selected(struct files_app *a)
{
    const struct files_entry *e = selected_entry(a);
    char path[FILES_PATH_MAX];
    char name[FILES_NAME_MAX + 1];
    int r;

    if (!e || selected_path(a, path, sizeof(path)) != 0) {
        return;
    }
    snprintf(name, sizeof(name), "%s", e->name);
    if (files_entry_is_dir(e)) {
        r = go(a, path, NULL);
        if (r != 0) {
            sayf_name(a, true, "\xE2\x80\x9C%s\xE2\x80\x9D cannot be opened: %s", name,
                      files_strerror(r));
        }
        return;
    }
    open_viewer(a, path, name);
}

static void on_open(lv_event_t *e)
{
    open_selected(lv_event_get_user_data(e));
}

static void on_up(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);
    char parent[FILES_PATH_MAX];
    char came_from[FILES_NAME_MAX + 1];
    int r;

    if (files_path_parent(parent, sizeof(parent), a->cwd) != 0) {
        return;
    }
    /* Back in the parent, the folder just left is the one selected. */
    snprintf(came_from, sizeof(came_from), "%s", files_path_base(a->cwd));
    r = go(a, parent, came_from);
    if (r != 0) {
        say(a, files_strerror(r), true);
    }
}

static void on_sort(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);
    char label[32];
    char keep[FILES_NAME_MAX + 1] = "";
    const struct files_entry *sel = selected_entry(a);

    if (sel) {
        snprintf(keep, sizeof(keep), "%s", sel->name);
    }
    a->sort_key = (enum files_sort)((a->sort_key + 1) % FILES_SORT_COUNT);
    snprintf(label, sizeof(label), "Sort: %s", files_view_sort_name(a->sort_key));
    lv_label_set_text(lv_obj_get_child(a->sort, 0), label);
    files_sort(a->dir.entries, a->dir.n, a->sort_key);
    build_rows(a);
    select_name(a, keep[0] ? keep : NULL);
}

static void on_viewer_close(lv_event_t *e)
{
    show_screen(lv_event_get_user_data(e), SCREEN_BROWSE);
}

/* ---- Storage: Internal Storage and the USB drive ------------------------------- */

static void storage_say(struct files_app *a, const char *text, bool error)
{
    lv_label_set_text(a->s_status, text ? text : "");
    lv_obj_remove_style(a->s_status, pos_style(POS_STYLE_STATUS_ERROR_TEXT), 0);
    if (error) {
        pos_style_add(a->s_status, POS_STYLE_STATUS_ERROR_TEXT, 0);
    }
}

/* Both places' second lines, from the root filesystem and from sysd. Run when
 * Storage opens, from the timer while it is on screen, never in a layout or
 * draw pass; the sysd call has the UI deadline. */
static void storage_update(struct files_app *a)
{
    enum files_usb_state was = a->usb.state;
    char line[200];
    char err[128] = "";
    int64_t total;
    int64_t avail;
    cJSON *r;

    files_space("/", &total, &avail);
    files_space_caption(line, sizeof(line), total, avail);
    lv_label_set_text(a->s_internal, line[0] ? line : "The microSD card");

    r = shell_ipc_call_timeout("sysd", "storage.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
    files_usb_parse(r, &a->usb);
    cJSON_Delete(r);
    files_usb_caption(line, sizeof(line), &a->usb);
    lv_label_set_text(a->s_usb, line);
    button_enable(a->s_eject, files_usb_can_eject(&a->usb) && !files_job_busy(&a->job));
    if (was == FILES_USB_EJECTING && a->usb.state == FILES_USB_EJECTED) {
        storage_say(a, "Safe to remove the USB drive", false);
    } else if (was == FILES_USB_EJECTING && a->usb.state == FILES_USB_MOUNTED) {
        storage_say(a, a->usb.error[0] ? a->usb.error : "The drive was not ejected", true);
    } else if (was != a->usb.state) {
        /* What was said belonged to the drive as it was: "Safe to remove"
         * outlived a reinserted drive on unit B. */
        storage_say(a, "", false);
    }
    a->usb_polled_at = lv_tick_get();
}

static void on_storage(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);

    a->usb.state = FILES_USB_UNKNOWN;
    storage_say(a, "", false);
    show_screen(a, SCREEN_STORAGE);
    storage_update(a);
}

/* Back to the browser, which reads its folder again: a folder on a drive that
 * has just been ejected is gone, and the browser says so and goes up. */
static void on_storage_close(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);
    const struct files_entry *sel = selected_entry(a);
    char keep[FILES_NAME_MAX + 1] = "";

    if (sel) {
        snprintf(keep, sizeof(keep), "%s", sel->name);
    }
    show_screen(a, SCREEN_BROWSE);
    refresh(a, keep[0] ? keep : NULL);
}

static void open_place(struct files_app *a, const char *path)
{
    int r = go(a, path, NULL);

    if (r != 0) {
        storage_say(a, files_strerror(r), true);
        return;
    }
    show_screen(a, SCREEN_BROWSE);
}

static void on_internal(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);
    char start[FILES_PATH_MAX];

    start_dir(start, sizeof(start));
    open_place(a, start);
}

static void on_usb(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);

    switch (a->usb.state) {
    case FILES_USB_MOUNTED:
        open_place(a, a->usb.path);
        return;
    case FILES_USB_ABSENT:
        storage_say(a, "Connect a FAT32 USB drive", false);
        return;
    case FILES_USB_EJECTED:
        storage_say(a, "Ejected. Remove the drive, then connect it again to use it.", false);
        return;
    case FILES_USB_EJECTING:
        storage_say(a, "Ejecting\xE2\x80\xA6", false);
        return;
    default: {
        char line[200];

        files_usb_caption(line, sizeof(line), &a->usb);
        storage_say(a, line, true);
        return;
    }
    }
}

static void on_eject(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);
    char err[128] = "";
    char line[200];
    cJSON *r;

    if (files_job_busy(&a->job)) {
        storage_say(a, "Wait until the copy, move or delete has finished", true);
        return;
    }
    r = shell_ipc_call_timeout("sysd", "storage.eject", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
    if (!r) {
        storage_say(a, err[0] ? err : "sysd is not answering", true);
        return;
    }
    cJSON_Delete(r);
    /* Accepted: sysd syncs and unmounts, and storage.status says when it is
     * done. Until then the drive is shown ejecting and cannot be opened. */
    a->usb.state = FILES_USB_EJECTING;
    files_usb_caption(line, sizeof(line), &a->usb);
    lv_label_set_text(a->s_usb, line);
    button_enable(a->s_eject, false);
    storage_say(a, "Ejecting\xE2\x80\xA6 Do not remove the drive yet.", false);
    a->usb_polled_at = lv_tick_get();
}

/* ---- the worker ------------------------------------------------------------------ */

static void start_job(struct files_app *a, enum files_op op, const char *src, const char *dst,
                      const char *name)
{
    static const char *const doing[] = { "Copying", "Moving", "Deleting" };
    char tail[8] = "\xE2\x80\xA6";
    char fmt[48];
    int r;

    snprintf(a->job_name, sizeof(a->job_name), "%s", name);
    r = files_job_start(&a->job, op, a->pol, src, dst);
    if (r != 0) {
        sayf_name(a, true, "\xE2\x80\x9C%s\xE2\x80\x9D: %s", name, files_strerror(r));
        return;
    }
    snprintf(fmt, sizeof(fmt), "%s \xE2\x80\x9C%%s\xE2\x80\x9D%%s", doing[op]);
    sayf_name(a, false, fmt, name, tail);
    update_actions(a);
}

static void finish_job(struct files_app *a, int r)
{
    static const char *const did[] = { "Copied", "Moved", "Deleted" };
    static const char *const fail[] = { "copied", "moved", "deleted" };
    enum files_op op = a->job.op;
    char fmt[64];
    char as[FILES_NAME_MAX + 32];
    const char *select = NULL;

    if (op == FILES_OP_COPY || op == FILES_OP_MOVE) {
        a->carry = CARRY_NONE;
    }
    if (r == 0 && op == FILES_OP_COPY) {
        select = a->job.out_name;
    } else if (r == 0 && op == FILES_OP_MOVE) {
        select = a->job_name;
    }
    if (r == -FILES_ESOURCE_KEPT) {
        select = a->job_name;
    }
    refresh(a, select);
    if (r == 0) {
        as[0] = '\0';
        if (op == FILES_OP_COPY && strcmp(a->job.out_name, a->job_name) != 0) {
            char safe[FILES_NAME_MAX + 1];

            files_view_name(safe, sizeof(safe), a->job.out_name);
            snprintf(as, sizeof(as), " as \xE2\x80\x9C%s\xE2\x80\x9D", safe);
        }
        snprintf(fmt, sizeof(fmt), "%s \xE2\x80\x9C%%s\xE2\x80\x9D%%s", did[op]);
        sayf_name(a, false, fmt, a->job_name, as);
    } else if (r == -FILES_ESOURCE_KEPT) {
        /* Both copies exist: nothing is lost, and the owner is told which. */
        sayf_name(a, true, "\xE2\x80\x9C%s\xE2\x80\x9D is here now, but the original could not "
                  "all be removed%s", a->job_name, "");
    } else {
        snprintf(fmt, sizeof(fmt), "\xE2\x80\x9C%%s\xE2\x80\x9D was not %s: %%s", fail[op]);
        sayf_name(a, true, fmt, a->job_name, files_strerror(r));
    }
    update_actions(a);
}

static void on_timer(lv_timer_t *t)
{
    struct files_app *a = lv_timer_get_user_data(t);
    int r;

    if (files_job_poll(&a->job, &r)) {
        finish_job(a, r);
    }
    if (a->open_pending) {
        a->open_pending = false;
        open_selected(a);
    }
    if (!lv_obj_has_flag(a->screen[SCREEN_STORAGE], LV_OBJ_FLAG_HIDDEN) &&
        lv_tick_elaps(a->usb_polled_at) >= (a->usb.state == FILES_USB_EJECTING ? FILES_USB_EJECT_POLL_MS
                                                                                : FILES_USB_POLL_MS)) {
        storage_update(a);
    }
}

/* ---- copy and move: carrying ---------------------------------------------------- */

static void carry(struct files_app *a, enum files_carry how)
{
    const struct files_entry *e = selected_entry(a);
    char name[FILES_NAME_MAX + 1];
    char line[FILES_NAME_MAX + 64];

    if (!e || selected_path(a, a->carry_path, sizeof(a->carry_path)) != 0) {
        return;
    }
    snprintf(a->carry_name, sizeof(a->carry_name), "%s", e->name);
    a->carry = how;
    files_view_name(name, sizeof(name), e->name);
    snprintf(line, sizeof(line), "%s \xE2\x80\x9C%s\xE2\x80\x9D", how == CARRY_COPY ? "Copy" : "Move",
             name);
    lv_label_set_text(a->paste_label, line);
    say(a, "Go to the folder it should go to, then Paste here", false);
    update_actions(a);
}

static void on_copy(lv_event_t *e)
{
    carry(lv_event_get_user_data(e), CARRY_COPY);
}

static void on_move(lv_event_t *e)
{
    carry(lv_event_get_user_data(e), CARRY_MOVE);
}

static void on_paste(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);

    if (a->carry == CARRY_NONE || files_job_busy(&a->job)) {
        return;
    }
    start_job(a, a->carry == CARRY_COPY ? FILES_OP_COPY : FILES_OP_MOVE, a->carry_path, a->cwd,
              a->carry_name);
}

static void on_paste_cancel(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);

    a->carry = CARRY_NONE;
    say_place(a);
    update_actions(a);
}

/* ---- the name entry: new folder and rename ------------------------------------- */

static void name_commit(struct files_app *a)
{
    const char *text = lv_textarea_get_text(a->n_field);
    const char *problem = files_name_problem(text);
    char name[FILES_NAME_MAX + 1];
    char path[FILES_PATH_MAX];
    int r;

    if (problem) {
        pocketui_text_field_set_error(a->n_field, problem);
        return;
    }
    snprintf(name, sizeof(name), "%s", text);
    if (a->name_mode == NAME_NEW_FOLDER) {
        r = files_mkdir(a->pol, a->cwd, name);
    } else if (selected_path(a, path, sizeof(path)) == 0) {
        r = files_rename(a->pol, path, name);
    } else {
        r = -ENOENT;
    }
    if (r != 0) {
        /* The field keeps what was typed; nothing on disk changed. */
        pocketui_text_field_set_error(a->n_field, files_strerror(r));
        return;
    }
    pocketui_text_field_set_error(a->n_field, NULL);
    pocketos_shell_keyboard_hide();
    show_screen(a, SCREEN_BROWSE);
    refresh(a, name);
    sayf_name(a, false, a->name_mode == NAME_NEW_FOLDER ? "Created \xE2\x80\x9C%s\xE2\x80\x9D%s"
                                                        : "Renamed to \xE2\x80\x9C%s\xE2\x80\x9D%s",
              name, "");
}

/* Enter from any source (DS 17.4): the touch keyboard's Done pushes Enter
 * into the one key stream, and so does a physical keyboard, and either way
 * the single-line field reports itself ready. So the name is committed here,
 * once, and the shell is given no Done callback of its own. */
static void on_name_ready(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);

    if (lv_obj_has_flag(a->screen[SCREEN_NAME], LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    name_commit(a);
}

static void on_name_ok(lv_event_t *e)
{
    name_commit(lv_event_get_user_data(e));
}

static void on_name_cancel(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);

    pocketui_text_field_set_error(a->n_field, NULL);
    pocketos_shell_keyboard_hide();
    show_screen(a, SCREEN_BROWSE);
}

static void on_name_field_clicked(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);

    /* Enter commits (on_name_ready) and then reaches the field as a click as
     * well, after the name entry has already closed: only a field on show
     * brings the keyboard back. */
    if (lv_obj_has_flag(a->screen[SCREEN_NAME], LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (!pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, NULL);
    }
}

static void open_name(struct files_app *a, enum files_name_mode mode)
{
    const struct files_entry *e = selected_entry(a);

    if (mode == NAME_RENAME && !e) {
        return;
    }
    a->name_mode = mode;
    lv_label_set_text(a->n_title, mode == NAME_NEW_FOLDER ? "New folder" : "Rename");
    lv_textarea_set_placeholder_text(a->n_field, mode == NAME_NEW_FOLDER ? "Folder name" : "New name");
    lv_label_set_text(lv_obj_get_child(a->n_ok, 0), mode == NAME_NEW_FOLDER ? "Create" : "Rename");
    pocketui_text_field_set_error(a->n_field, NULL);
    pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, NULL);
    show_screen(a, SCREEN_NAME);
    lv_textarea_set_text(a->n_field, mode == NAME_RENAME ? e->name : "");
    pos_input_focus(a->n_field);
}

static void on_new_folder(lv_event_t *e)
{
    open_name(lv_event_get_user_data(e), NAME_NEW_FOLDER);
}

static void on_rename(lv_event_t *e)
{
    open_name(lv_event_get_user_data(e), NAME_RENAME);
}

/* ---- the delete confirmation (DS section 17.5) ----------------------------------- */

static void on_delete(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);
    const struct files_entry *sel = selected_entry(a);
    char name[FILES_NAME_MAX + 1];
    char line[FILES_NAME_MAX + 32];

    if (!sel) {
        return;
    }
    files_view_name(name, sizeof(name), sel->name);
    snprintf(line, sizeof(line), "Delete \xE2\x80\x9C%s\xE2\x80\x9D?", name);
    lv_label_set_text(a->c_title, line);
    lv_label_set_text(a->c_body, sel->kind == FILES_KIND_DIR
                                     ? "The folder and everything in it are removed from this "
                                       "device. There is no undo."
                                     : "It is removed from this device. There is no undo.");
    show_screen(a, SCREEN_CONFIRM);
}

static void on_confirm_cancel(lv_event_t *e)
{
    show_screen(lv_event_get_user_data(e), SCREEN_BROWSE);
}

static void on_confirm_delete(lv_event_t *e)
{
    struct files_app *a = lv_event_get_user_data(e);
    const struct files_entry *sel = selected_entry(a);
    char path[FILES_PATH_MAX];

    /* Only here, and only on this press: nothing was removed when the dialog
     * opened (DS section 17.5). */
    show_screen(a, SCREEN_BROWSE);
    if (!sel || selected_path(a, path, sizeof(path)) != 0) {
        return;
    }
    start_job(a, FILES_OP_DELETE, path, NULL, sel->name);
}

/* ---- building ------------------------------------------------------------------ */

static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *b = lv_obj_create(parent);

    lv_obj_remove_style_all(b);
    lv_obj_set_flex_flow(b, flow);
    lv_obj_set_style_pad_row(b, FILES_GAP, 0);
    lv_obj_set_style_pad_column(b, FILES_GAP, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

static lv_obj_t *make_screen(lv_obj_t *parent)
{
    lv_obj_t *s = box(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_set_width(s, LV_PCT(100));
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_style_pad_row(s, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_column(s, POCKETUI_PAD, 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    return s;
}

static lv_obj_t *caption(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, "", role);

    lv_obj_set_width(lb, LV_PCT(100));
    one_line(lb);
    return lb;
}

/* A square button in the path bar with one accent symbol: Up, Storage. */
static lv_obj_t *slab_button(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_t *glyph;

    lv_obj_remove_style_all(b);
    pos_style_add(b, POS_STYLE_SLAB, 0);
    pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_size(b, FILES_UP_W, FILES_BTN_H);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_set_user_data(b, (void *)(uintptr_t)0);
    glyph = pocketui_label(b, symbol, POS_STYLE_SYMBOL);
    pos_style_add(glyph, POS_STYLE_ACCENT_TEXT, 0);
    lv_obj_center(glyph);
    return b;
}

static void build_browser(struct files_app *a)
{
    lv_obj_t *s = a->screen[SCREEN_BROWSE];
    static const char *const labels[ACT_COUNT] = { "Open", "Rename", "Copy", "Move", "Delete" };
    static const lv_event_cb_t cbs[ACT_COUNT] = { on_open, on_rename, on_copy, on_move, on_delete };
    int i;

    a->main = box(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(a->main, 1);

    a->bar = box(a->main, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(a->bar, LV_PCT(100), FILES_BTN_H);
    lv_obj_set_flex_align(a->bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    a->up = slab_button(a->bar, LV_SYMBOL_UP, on_up, a);
    /* Storage: Internal Storage and the USB drive. */
    a->drive = slab_button(a->bar, LV_SYMBOL_DRIVE, on_storage, a);
    a->path = pocketui_label(a->bar, "", POS_STYLE_TEXT_PRIMARY);
    one_line(a->path);
    lv_obj_set_flex_grow(a->path, 1);

    a->tools = box(a->main, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(a->tools, LV_PCT(100), FILES_BTN_H);
    a->sort = button(a->tools, "Sort: Name", on_sort, a, false);
    a->new_folder = button(a->tools, "New folder", on_new_folder, a, false);

    a->list = pocketui_card(a->main);
    /* The rows carry 8 px of their own on each side (add_row), so the text
     * keeps its place; 4 px above and below keep the first and last row's
     * selection outline off the card's border. */
    lv_obj_set_style_pad_hor(a->list, POCKETUI_PAD - 8, 0);
    lv_obj_set_style_pad_ver(a->list, 4, 0);
    lv_obj_set_flex_grow(a->list, 1);
    /* The rows scroll inside the list and nowhere else (DS section 17.1). */
    lv_obj_add_flag(a->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->list, LV_DIR_VER);

    /* What just happened, on up to two lines before the dots: a message is
     * a name and then the reason, and the reason is the part that must not
     * be cut - at a larger text size one line held only the name (DS §46.5).
     * A message one line holds is one line, as it always was. */
    a->status = pocketui_label(a->main, "", POS_STYLE_CAPTION);
    lv_obj_set_width(a->status, LV_PCT(100));
    lv_label_set_long_mode(a->status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_height(a->status, 2 * pocketui_role_line_height(POS_STYLE_CAPTION), 0);

    a->side = box(s, LV_FLEX_FLOW_COLUMN);
    a->details = pocketui_card(a->side);
    lv_obj_set_flex_grow(a->details, 1);
    lv_obj_set_style_pad_row(a->details, 6, 0);
    a->d_name = pocketui_label(a->details, "", POS_STYLE_ROW_TITLE);
    lv_label_set_long_mode(a->d_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(a->d_name, LV_PCT(100));
    /* Two lines of a long name, then the dots. */
    lv_obj_set_height(a->d_name, 2 * lv_font_get_line_height(lv_obj_get_style_text_font(a->d_name, 0)));
    a->d_kind = caption(a->details, POS_STYLE_TEXT_SECONDARY);
    a->d_when = caption(a->details, POS_STYLE_TEXT_SECONDARY);
    a->d_access = caption(a->details, POS_STYLE_TEXT_SECONDARY);

    a->actions = box(a->main, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(a->actions, LV_PCT(100));
    lv_obj_set_height(a->actions, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(a->actions, 8, 0);
    lv_obj_set_style_pad_row(a->actions, 8, 0);
    for (i = 0; i < ACT_COUNT; i++) {
        a->act[i] = button(a->actions, labels[i], cbs[i], a, i == ACT_OPEN);
    }

    a->paste = box(a->main, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(a->paste, LV_PCT(100));
    lv_obj_set_height(a->paste, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(a->paste, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(a->paste, 8, 0);
    lv_obj_set_style_pad_row(a->paste, 8, 0);
    a->paste_label = pocketui_label(a->paste, "", POS_STYLE_TEXT_PRIMARY);
    one_line(a->paste_label);
    a->paste_here = button(a->paste, "Paste here", on_paste, a, true);
    button(a->paste, "Cancel", on_paste_cancel, a, false);
    lv_obj_add_flag(a->paste, LV_OBJ_FLAG_HIDDEN);
}

static void build_viewer(struct files_app *a)
{
    lv_obj_t *s = a->screen[SCREEN_VIEWER];
    lv_obj_t *head = box(s, LV_FLEX_FLOW_ROW);
    lv_obj_t *close;

    lv_obj_set_size(head, LV_PCT(100), FILES_BTN_H);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    close = button(head, "Close", on_viewer_close, a, true);
    lv_obj_set_width(close, FILES_NAME_BTN_W);
    a->v_title = pocketui_label(head, "", POS_STYLE_ROW_TITLE);
    one_line(a->v_title);
    lv_obj_set_flex_grow(a->v_title, 1);

    a->v_card = pocketui_card(s);
    lv_obj_set_flex_grow(a->v_card, 1);
    lv_obj_add_flag(a->v_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->v_card, LV_DIR_VER);
    a->v_text = pocketui_label(a->v_card, "", POS_STYLE_TEXT_PRIMARY);
    /* A file's contents are the file's: they keep the size Small draws them
     * at, while Files' own lists, names and buttons follow the text size
     * (DS §46.4). */
    lv_obj_add_style(a->v_text, pos_style_fixed_size(POS_STYLE_TEXT_PRIMARY), 0);
    lv_label_set_long_mode(a->v_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->v_text, LV_PCT(100));

    a->v_note = pocketui_label(s, "Read-only. Showing the first 32 KB of this file.", POS_STYLE_CAPTION);
}

static void build_name(struct files_app *a)
{
    lv_obj_t *s = a->screen[SCREEN_NAME];
    lv_obj_t *cancel;

    a->n_title = pocketui_label(s, "", POS_STYLE_TITLE);
    a->n_form = box(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(a->n_form, LV_PCT(100));
    lv_obj_set_height(a->n_form, LV_SIZE_CONTENT);
    a->n_field = pocketui_text_field(a->n_form, "Folder name", true);
    lv_textarea_set_max_length(a->n_field, FILES_NAME_MAX);
    lv_obj_add_event_cb(a->n_field, on_name_field_clicked, LV_EVENT_CLICKED, a);
    lv_obj_add_event_cb(a->n_field, on_name_ready, LV_EVENT_READY, a);
    a->n_buttons = box(a->n_form, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(a->n_buttons, FILES_BTN_H);
    lv_obj_set_style_pad_column(a->n_buttons, 8, 0);
    cancel = button(a->n_buttons, "Cancel", on_name_cancel, a, false);
    lv_obj_set_flex_grow(cancel, 1);
    a->n_ok = button(a->n_buttons, "Create", on_name_ok, a, true);
    lv_obj_set_flex_grow(a->n_ok, 1);
}

static void build_confirm(struct files_app *a)
{
    lv_obj_t *buttons;
    lv_obj_t *cancel;
    lv_obj_t *confirm;

    a->dialog = pocketui_card(a->screen[SCREEN_CONFIRM]);
    a->c_title = pocketui_label(a->dialog, "", POS_STYLE_TITLE);
    lv_label_set_long_mode(a->c_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->c_title, LV_PCT(100));
    a->c_body = pocketui_label(a->dialog, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(a->c_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->c_body, LV_PCT(100));
    lv_obj_set_style_pad_top(a->c_body, 12, 0);
    lv_obj_set_style_pad_bottom(a->c_body, 20, 0);

    buttons = box(a->dialog, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(buttons, LV_PCT(100), FILES_BTN_H);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    /* Cancel first and accented: the bright treatment goes to the safe
     * choice (DS section 17.5, as Notes does). */
    cancel = button(buttons, "Cancel", on_confirm_cancel, a, true);
    lv_obj_set_flex_grow(cancel, 1);
    confirm = button(buttons, "Delete", on_confirm_delete, a, false);
    lv_obj_set_flex_grow(confirm, 1);
    pos_input_add_obj(cancel);
    pos_input_add_obj(confirm);
}

/* One place: a symbol, its name and a second line, the whole row the hit
 * area (DS section 9), drawn like a row of the list. */
static lv_obj_t *place_row(lv_obj_t *parent, const char *symbol, const char *title, lv_obj_t **caption_out,
                           lv_event_cb_t cb, void *user, bool divider)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *glyph;
    lv_obj_t *text;
    lv_obj_t *lb;

    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), POCKETUI_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    pos_style_add(row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    if (divider) {
        pos_style_add(row, POS_STYLE_DIVIDER, 0);
    }
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user);

    /* The colour role first and the symbol font after it, as in add_row. */
    glyph = pocketui_label(row, symbol, POS_STYLE_ACCENT_TEXT);
    pos_style_add(glyph, POS_STYLE_SYMBOL, 0);
    lv_obj_set_width(glyph, 28);

    text = lv_obj_create(row);
    lv_obj_remove_style_all(text);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(text, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lb = pocketui_label(text, title, POS_STYLE_ROW_TITLE);
    lv_obj_set_width(lb, LV_PCT(100));
    one_line(lb);
    *caption_out = pocketui_label(text, "", POS_STYLE_CAPTION);
    lv_obj_set_width(*caption_out, LV_PCT(100));
    one_line(*caption_out);
    return row;
}

/* Storage: Close, the title and Eject in one row, the two places in a card
 * under it, and a line for what just happened. One column in either shape:
 * it is three rows, which fit the wide body as they are. */
static void build_storage(struct files_app *a)
{
    lv_obj_t *s = a->screen[SCREEN_STORAGE];
    lv_obj_t *head = box(s, LV_FLEX_FLOW_ROW);
    lv_obj_t *close;
    lv_obj_t *title;
    lv_obj_t *card;

    lv_obj_set_size(head, LV_PCT(100), FILES_BTN_H);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    close = button(head, "Close", on_storage_close, a, true);
    lv_obj_set_width(close, FILES_NAME_BTN_W);
    title = pocketui_label(head, "Storage", POS_STYLE_ROW_TITLE);
    one_line(title);
    lv_obj_set_flex_grow(title, 1);
    a->s_eject = button(head, "Eject", on_eject, a, false);
    lv_obj_set_width(a->s_eject, FILES_NAME_BTN_W);
    button_enable(a->s_eject, false);

    card = pocketui_card(s);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(card, POCKETUI_PAD - 8, 0);
    lv_obj_set_style_pad_ver(card, 4, 0);
    /* A list like the browser's, which a third place would scroll. */
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    place_row(card, LV_SYMBOL_SD_CARD, "Internal Storage", &a->s_internal, on_internal, a, true);
    place_row(card, LV_SYMBOL_USB, "USB Drive", &a->s_usb, on_usb, a, false);

    a->s_status = pocketui_label(s, "", POS_STYLE_CAPTION);
    lv_obj_set_width(a->s_status, LV_PCT(100));
    lv_label_set_long_mode(a->s_status, LV_LABEL_LONG_WRAP);
}

/* ---- the layout ------------------------------------------------------------------ */

static void shape_browser(struct files_app *a)
{
    bool w = a->wide;
    int i;

    lv_obj_set_flex_flow(a->screen[SCREEN_BROWSE], w ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(a->main, w ? 0 : LV_PCT(100));
    lv_obj_set_height(a->main, w ? LV_PCT(100) : 0);
    lv_obj_set_flex_grow(a->main, 1);

    /* Sort and New folder: up in the bar when wide, their own row when tall.
     * The empty row goes to the end of the screen, where a hidden sibling
     * costs the growing list nothing. */
    lv_obj_set_parent(a->sort, w ? a->bar : a->tools);
    lv_obj_set_parent(a->new_folder, w ? a->bar : a->tools);
    lv_obj_set_flex_grow(a->sort, w ? 0 : 1);
    lv_obj_set_flex_grow(a->new_folder, w ? 0 : 1);
    lv_obj_set_width(a->sort, FILES_TOOL_W);
    lv_obj_set_width(a->new_folder, FILES_TOOL_W);
    lv_obj_set_parent(a->tools, w ? a->screen[SCREEN_BROWSE] : a->main);
    if (!w) {
        lv_obj_move_to_index(a->tools, 1);
    }
    set_hidden(a->tools, w);

    /* The details pane, and the actions under it when wide; across the foot
     * of the list when tall. */
    set_hidden(a->side, !w);
    lv_obj_set_size(a->side, FILES_SIDE_W, LV_PCT(100));
    lv_obj_set_parent(a->actions, w ? a->side : a->main);
    lv_obj_set_parent(a->paste, w ? a->side : a->main);
    if (w) {
        lv_obj_move_to_index(a->side, 1);
    }
    for (i = 0; i < ACT_COUNT; i++) {
        /* Wide: Open across the pane, the other four in pairs under it. */
        lv_obj_set_flex_grow(a->act[i], w ? 0 : 1);
        lv_obj_set_width(a->act[i], !w ? 1 : i == ACT_OPEN ? LV_PCT(100) : (FILES_SIDE_W - 8) / 2);
    }
    lv_obj_set_width(a->paste_label, w ? LV_PCT(100) : 0);
    lv_obj_set_flex_grow(a->paste_label, w ? 0 : 1);
    lv_obj_set_width(a->paste_here, w ? (FILES_SIDE_W - 8) / 2 : 160);
    lv_obj_set_width(lv_obj_get_child(a->paste, 2), w ? (FILES_SIDE_W - 8) / 2 : 120);
    show_path(a);
}

/* Wide: the field and its two buttons in one row, which is what fits above
 * the keyboard there (the body is about 100 px tall), and no title. */
static void shape_name(struct files_app *a)
{
    set_hidden(a->n_title, a->wide);
    lv_obj_set_flex_flow(a->n_form, a->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->n_form, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_flex_grow(lv_obj_get_parent(a->n_field), a->wide ? 1 : 0);
    lv_obj_set_width(lv_obj_get_parent(a->n_field), a->wide ? 0 : LV_PCT(100));
    lv_obj_set_width(a->n_buttons, a->wide ? 2 * FILES_NAME_BTN_W + 8 : LV_PCT(100));
    lv_obj_set_height(a->n_buttons, a->wide ? POCKETUI_ROW_H : FILES_BTN_H);
}

static void shape_confirm(struct files_app *a)
{
    lv_flex_align_t across = a->wide ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START;

    lv_obj_set_flex_align(a->screen[SCREEN_CONFIRM], LV_FLEX_ALIGN_START, across, across);
    lv_obj_set_width(a->dialog, a->wide ? FILES_LIST_MIN_W : LV_PCT(100));
}

static void layout(struct files_app *a)
{
    struct pos_insets in;
    const lv_area_t *area;
    int32_t w;
    int32_t h;

    /* Nothing to lay out in, or nothing the layout is chosen from has
     * changed: PocketUI owns that decision (pocketui.h), and hands back the
     * corner clearance the platform rule gives this box. */
    if (!pocketui_layout_begin(&a->layout_guard, a->frame, &in)) {
        return;
    }
    area = &a->layout_guard.area;
    lv_obj_set_style_pad_left(a->frame, in.left, 0);
    lv_obj_set_style_pad_top(a->frame, in.top, 0);
    lv_obj_set_style_pad_right(a->frame, in.right, 0);
    lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
    w = lv_area_get_width(area) - in.left - in.right;
    h = lv_area_get_height(area) - in.top - in.bottom;
    a->width = w;
    a->wide = w > h && w >= FILES_LIST_MIN_W + POCKETUI_PAD + FILES_SIDE_W;
    shape_browser(a);
    shape_name(a);
    shape_confirm(a);
}

static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

/* ---- the app --------------------------------------------------------------------- */

/* Where Files opens: the owner's home when it can be read, else /root,
 * else the top. */
static void start_dir(char *out, size_t out_len)
{
    const char *home = getenv("HOME");
    struct files_dir probe;
    const char *try[3] = { home, "/root", "/" };
    int i;

    for (i = 0; i < 3; i++) {
        if (try[i] && try[i][0] == '/' && files_dir_read(&probe, try[i]) == 0) {
            files_dir_free(&probe);
            snprintf(out, out_len, "%s", try[i]);
            return;
        }
    }
    snprintf(out, out_len, "/");
}

static void *files_create(lv_obj_t *root)
{
    struct files_app *a = lv_malloc_zeroed(sizeof(*a));
    char start[FILES_PATH_MAX];
    int i;
    int r;

    if (!a) {
        return NULL;
    }
    a->selected = -1;
    a->pol = files_policy_default();
    a->frame = lv_obj_create(root);
    lv_obj_remove_style_all(a->frame);
    /* Exactly the body's content box, so the shape is chosen from the room
     * the shell gives and never from what the shape itself put there. */
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->frame, LV_FLEX_FLOW_COLUMN);
    /* No gap between the screens, only one of which is ever shown: LVGL
     * takes a gap from a growing item for every sibling before it, hidden
     * ones included (see Notes). */
    lv_obj_set_style_pad_row(a->frame, 0, 0);
    lv_obj_clear_flag(a->frame, LV_OBJ_FLAG_SCROLLABLE);
    for (i = 0; i < SCREEN_COUNT; i++) {
        a->screen[i] = make_screen(a->frame);
    }
    build_browser(a);
    build_viewer(a);
    build_name(a);
    build_confirm(a);
    build_storage(a);
    show_screen(a, SCREEN_BROWSE);
    pocketos_shell_set_status_hint("");

    start_dir(start, sizeof(start));
    r = go(a, start, NULL);
    if (r != 0) {
        say(a, files_strerror(r), true);
    }
    a->timer = lv_timer_create(on_timer, FILES_TIMER_MS, a);
    /* Only now: building lays objects out as it goes, and the layout step
     * shapes objects that must all exist. */
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);
    return a;
}

static void files_destroy(void *priv)
{
    struct files_app *a = priv;

    if (!a) {
        return;
    }
    /* The frame outlives this by a moment, until the shell deletes the app's
     * objects; nothing may call back into a freed app in between. */
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    /* No thread may outlive the app. A copy stops at its next chunk and
     * removes what it made; a delete stops at its next entry. */
    if (files_job_busy(&a->job)) {
        static const char *const what[] = { "a copy", "a move", "a delete" };
        enum files_op op = a->job.op;

        files_job_abandon(&a->job);
        /* Said as it ended, not as it was asked to: a job can finish in the
         * moment between the stop and the join, and on unit A one did. */
        if (a->job.result == 0) {
            LOG_WARN("files: closed while %s was running; it had finished", what[op]);
        } else if (a->job.result == -FILES_ECANCELED) {
            LOG_WARN("files: closed while %s was running; it was stopped and nothing was left half done",
                     what[op]);
        } else {
            LOG_WARN("files: closed while %s was running; it failed: %s", what[op],
                     files_strerror(a->job.result));
        }
    }
    pocketos_shell_keyboard_hide();
    files_dir_free(&a->dir);
    lv_free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_files);

const struct pocketos_app app_files = {
    .id = "files",
    .name = "Files",
    .icon = LV_SYMBOL_DIRECTORY,
    .icon_mask = &pos_app_icon_files,
    .create = files_create,
    .tick = NULL,
    .destroy = files_destroy,
};
