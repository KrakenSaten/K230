/*
 * The DOORS launcher. See home.h; the geometry is home_layout.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "home.h"

#include "art.h"
#include "home_layout.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pos_glyphs.h"
#include "pos_input.h"

#include <ctype.h>
#include <string.h>

_Static_assert((int)HOME_HUE_RADIO == (int)POS_HUE_RADIO && (int)HOME_HUE_MESH == (int)POS_HUE_MESH &&
                   (int)HOME_HUE_NETWORK == (int)POS_HUE_NETWORK && (int)HOME_HUE_TOOLS == (int)POS_HUE_TOOLS &&
                   (int)HOME_HUE_AI == (int)POS_HUE_AI && (int)HOME_HUE_GAMES == (int)POS_HUE_GAMES &&
                   (int)HOME_HUE_SETTINGS == (int)POS_HUE_SETTINGS &&
                   (int)HOME_HUE_FILES == (int)POS_HUE_FILES && (int)HOME_HUE_APPS == (int)POS_HUE_APPS,
               "home_layout.h hues are pos_styles.h's");

/* The portal frame inside the 96 px icon canvas (the package's frame, 22..106
 * x 9..119 of 128, at 0.75): where the press mark sits. */
#define FRAME_TOP 7
#define MARK_ARM 16
#define MARK_W 3 /* 2 until DS §46.7: the press and focus mark read at arm's length */
#define UNDERLINE_W 96
/* An app with no portal icon: its own 32 px mask centred in the empty
 * frame's inner panel. */
#define MASK_X 32
#define MASK_Y 31

/* A folder's cell with no portal icon file: a symbol on the empty frame, as
 * an app without a mask gets its text icon. */
static const char *const folder_symbol[HOME_FOLDER_COUNT] = {
    [HOME_FOLDER_GAMES] = LV_SYMBOL_PLAY,
    [HOME_FOLDER_UTILITIES] = LV_SYMBOL_SETTINGS,
    [HOME_FOLDER_APPS] = LV_SYMBOL_LIST,
};

/* An empty favorite's slot, and the picker's Clear: the empty portal and
 * the symbol, dimmed, so they read as places rather than apps. */
#define PLACEHOLDER_OPA LV_OPA_60

struct cell {
    const struct pocketos_app *app; /* NULL: a folder's cell, an empty favorite, the picker's Clear */
    enum home_folder folder;        /* the folder the cell opens, or NONE */
    int fav;                        /* the favorite slot this cell is, or -1 */
    bool pick;                      /* the picker's: choosing it gives the slot app (NULL: clears it) */
    enum pos_env_hue hue;
    lv_obj_t *obj;
    lv_obj_t *label;
    lv_image_dsc_t *icon; /* owned; NULL when drawn on the shared frame */
};

/* One page of cells: the launcher's own, or the open folder's (or the
 * favorite picker's, which is built the same way). */
struct page {
    lv_obj_t *root;
    struct cell cells[HOME_MAX_APPS];
    int ncells;
    int focus; /* the cell the keys act on, or -1 */
};

enum pending {
    PENDING_NONE = 0,
    PENDING_OPEN_FOLDER,
    PENDING_CLOSE_FOLDER,
    PENDING_OPEN_APP,
    PENDING_PICK,   /* open the picker for pending_slot */
    PENDING_ASSIGN, /* give pending_slot pending_app, or clear it */
};

static struct {
    lv_obj_t *container; /* the shell's handle: both pages */
    struct page root;    /* the launcher's own page */
    struct page folder;  /* the open folder's (or picker's) page; its root is NULL when none is open */
    lv_obj_t *back;      /* its way back */
    enum home_folder open;
    int picking;         /* the slot the page is the picker for, or -1 */
    /* The favorites: what each slot holds ("" = empty), and where its cell is. */
    char fav[HOME_FAVORITES][HOME_FAVORITE_ID_MAX];
    struct home_rect fav_rect[HOME_FAVORITES];
    bool small_labels;
    lv_obj_t *clock;
    lv_obj_t *date;
    lv_image_dsc_t *frame; /* the empty portal, loaded on first need */
    struct home_actions actions;
    struct home_info info;
    /* What building a folder's page needs later. */
    const struct pocketos_app *const *apps;
    size_t napps;
    bool landscape;
    struct home_rect keepout;
    int32_t inset_bottom;
    int32_t width; /* the content area the launcher was built for */
    int32_t height;
    /* Keys. */
    bool keys_shown; /* a key has been used since the last touch: the mark follows the focus */
    enum pending pending;
    enum home_folder pending_folder;
    const struct pocketos_app *pending_app;
    int pending_slot;
} home = { .picking = -1 };

static struct home_rect rect_of(const lv_area_t *a)
{
    struct home_rect r = { a->x1, a->y1, lv_area_get_width(a), lv_area_get_height(a) };

    return r;
}

static void place(lv_obj_t *obj, const struct home_rect *r)
{
    lv_obj_set_pos(obj, r->x, r->y);
    lv_obj_set_size(obj, r->w, r->h);
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static struct page *showing(void)
{
    return home.folder.root ? &home.folder : &home.root;
}

/* ---- deferred navigation ------------------------------------------------ *
 *
 * A tap or a key arrives inside an LVGL event, and what it asks for - a page
 * built, a page deleted with the button that was tapped on it, an app opened
 * and the focus given to it - must not happen there: LVGL does not keep a
 * focus changed inside an event, and an object must not be deleted inside
 * its own. The request is recorded and carried out on the next timer pass.
 * One at a time: a second before the first is done is the same finger or key
 * again, and is dropped. */

static void page_close(void);
static void show_focus(struct page *p, int k);

static void run_pending(void *unused)
{
    enum pending p = home.pending;
    enum home_folder f = home.pending_folder;
    const struct pocketos_app *app = home.pending_app;
    int slot = home.pending_slot;

    (void)unused;
    home.pending = PENDING_NONE;
    home.pending_app = NULL;
    switch (p) {
    case PENDING_PICK:
        home_favorite_pick(slot);
        break;
    case PENDING_ASSIGN:
        /* The picker goes first, with the cell that was chosen on it; then
         * the slot's own cell is rebuilt, and the keys come back to it. */
        page_close();
        if (home_favorite_set(slot, app ? app->id : NULL) == 0) {
            home.root.focus = slot;
            if (home.keys_shown) {
                show_focus(&home.root, slot);
            }
        }
        break;
    case PENDING_OPEN_FOLDER: {
        const struct home_folder_def *def = home_folder_get(f);

        if (def) {
            home_folder_open(def->id);
        }
        break;
    }
    case PENDING_CLOSE_FOLDER:
        home_folder_close();
        break;
    case PENDING_OPEN_APP:
        if (app && home.actions.open) {
            home.actions.open(app);
        }
        break;
    default:
        break;
    }
}

static bool request_slot(enum pending p, enum home_folder f, const struct pocketos_app *app, int slot)
{
    if (home.pending != PENDING_NONE) {
        return false;
    }
    home.pending = p;
    home.pending_folder = f;
    home.pending_app = app;
    home.pending_slot = slot;
    lv_async_call(run_pending, NULL);
    return true;
}

static void request(enum pending p, enum home_folder f, const struct pocketos_app *app)
{
    request_slot(p, f, app, -1);
}

/* ---- cells ------------------------------------------------------------- */

static void activate(const struct cell *c)
{
    if (c->pick) {
        request_slot(PENDING_ASSIGN, HOME_FOLDER_NONE, c->app, home.picking);
    } else if (c->fav >= 0 && !c->app) {
        request_slot(PENDING_PICK, HOME_FOLDER_NONE, NULL, c->fav);
    } else if (c->folder != HOME_FOLDER_NONE) {
        request(PENDING_OPEN_FOLDER, c->folder, NULL);
    } else if (c->app) {
        request(PENDING_OPEN_APP, HOME_FOLDER_NONE, c->app);
    }
}

static int index_of(const struct page *p, const struct cell *c)
{
    return (c >= p->cells && c < p->cells + p->ncells) ? (int)(c - p->cells) : -1;
}

static void on_cell_pressed(lv_event_t *e)
{
    struct cell *c = lv_event_get_user_data(e);
    struct page *p = showing();
    int k = index_of(p, c);

    /* A finger takes over from the keys: the mark goes, and the focus is
     * where the finger was, so a key after it starts from there. */
    if (home.keys_shown && p->focus >= 0 && p->focus < p->ncells) {
        lv_obj_invalidate(p->cells[p->focus].obj);
    }
    home.keys_shown = false;
    if (k >= 0) {
        p->focus = k;
    }
}

static void on_cell_clicked(lv_event_t *e)
{
    activate(lv_event_get_user_data(e));
}

/* A finger held on a favorite's slot (LVGL's long press: never sent once
 * the page has started to scroll under it). The picker opens on the next
 * timer pass, and the finger is ignored until it lifts: its release must
 * neither open the slot's app (LVGL sends CLICKED after a long press) nor
 * land on the picker, which is now under it. */
static void on_cell_long_pressed(lv_event_t *e)
{
    struct cell *c = lv_event_get_user_data(e);

    if (c->fav < 0 || home.folder.root) {
        return;
    }
    if (request_slot(PENDING_PICK, HOME_FOLDER_NONE, NULL, c->fav)) {
        lv_indev_wait_release(lv_event_get_indev(e));
    }
}

/* The package's focus mark - a bracket over the frame's top corners and a
 * rule under the name, in the app's colour - shown while the cell is held,
 * and on the cell the keys are on once a key has been used. Drawn, not
 * built from objects, so a cell is three objects however it is marked, and
 * the mark can never catch a touch. */
static void on_cell_draw_post(lv_event_t *e)
{
    struct cell *c = lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    const struct page *p = showing();
    lv_draw_rect_dsc_t d;
    lv_area_t a;
    lv_area_t r;
    int32_t ix;
    int32_t rx;
    int32_t ly;
    bool focused = home.keys_shown && p->focus >= 0 && index_of(p, c) == p->focus;

    if (!lv_obj_has_state(obj, LV_STATE_PRESSED) && !focused) {
        return;
    }
    lv_obj_get_coords(obj, &a);
    /* Over the icon's box, but never outside the cell: a landscape cell is
     * narrower than the icon's transparent canvas, and a mark outside the
     * cell would be clipped to half a bracket. */
    ix = a.x1 + (lv_area_get_width(&a) - HOME_ICON) / 2;
    if (ix < a.x1) {
        ix = a.x1;
    }
    lv_draw_rect_dsc_init(&d);
    d.bg_color = pos_env_hue(c->hue);
    d.bg_opa = LV_OPA_COVER;
    d.radius = 0;
    /* top-left bracket */
    r = (lv_area_t){ ix, a.y1 + FRAME_TOP - 4, ix + MARK_ARM - 1, a.y1 + FRAME_TOP - 4 + MARK_W - 1 };
    lv_draw_rect(layer, &d, &r);
    r = (lv_area_t){ ix, a.y1 + FRAME_TOP - 4, ix + MARK_W - 1, a.y1 + FRAME_TOP - 4 + MARK_ARM - 1 };
    lv_draw_rect(layer, &d, &r);
    /* top-right bracket */
    rx = LV_MIN(ix + HOME_ICON - 1, a.x2);
    r = (lv_area_t){ rx - MARK_ARM + 1, a.y1 + FRAME_TOP - 4, rx, a.y1 + FRAME_TOP - 4 + MARK_W - 1 };
    lv_draw_rect(layer, &d, &r);
    r = (lv_area_t){ rx - MARK_W + 1, a.y1 + FRAME_TOP - 4, rx, a.y1 + FRAME_TOP - 4 + MARK_ARM - 1 };
    lv_draw_rect(layer, &d, &r);
    /* the rule under the name */
    ly = a.y2 - 1;
    r = (lv_area_t){ a.x1 + (lv_area_get_width(&a) - UNDERLINE_W) / 2, ly,
                     a.x1 + (lv_area_get_width(&a) + UNDERLINE_W) / 2 - 1, ly + 1 }; /* 2 px, DS §46.7 */
    lv_draw_rect(layer, &d, &r);
}

static const char *cell_id(const struct cell *c)
{
    const struct home_folder_def *def = home_folder_get(c->folder);

    return c->app ? c->app->id : def ? def->id : "";
}

/* What home_focus_id says for a cell: its app or folder, or the place. */
static const char *cell_name(const struct cell *c)
{
    static const char *const slots[HOME_FAVORITES] = { "favorite-1", "favorite-2", "favorite-3" };

    if (c->fav >= 0 && c->fav < HOME_FAVORITES) {
        return slots[c->fav];
    }
    return (c->pick && !c->app) ? "clear" : cell_id(c);
}

static void cell_icon(struct cell *c, int32_t ix, struct home_info *count)
{
    char name[48];
    lv_obj_t *img;
    const char *id = cell_id(c);
    bool place = !c->app && c->folder == HOME_FOLDER_NONE; /* an empty favorite, the picker's Clear */

    lv_snprintf(name, sizeof(name), "icon-%s", id);
    c->icon = (c->folder != HOME_FOLDER_NONE || (c->app && home_entry_find(id))) ? art_load(name) : NULL;
    if (c->icon) {
        img = lv_image_create(c->obj);
        pos_style_add(img, POS_STYLE_ENV_ICON, 0);
        lv_image_set_src(img, c->icon);
        lv_obj_set_pos(img, ix, 0);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        if (count) {
            count->icons_art++;
        }
        return;
    }
    /* No portal icon for this cell: the empty portal, with the app's own
     * mask - or its text icon, or the folder's symbol - where the glyph
     * would be. */
    if (count) {
        count->icons_fallback++;
    }
    if (!home.frame) {
        home.frame = art_load("icon-frame");
    }
    if (home.frame) {
        img = lv_image_create(c->obj);
        pos_style_add(img, POS_STYLE_ENV_ICON, 0);
        lv_image_set_src(img, home.frame);
        lv_obj_set_pos(img, ix, 0);
    } else {
        img = plain(c->obj);
        pos_style_add(img, POS_STYLE_ENV_PANEL, 0);
        lv_obj_set_pos(img, ix + 16, FRAME_TOP);
        lv_obj_set_size(img, 64, 83);
    }
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    if (place) {
        lv_obj_set_style_opa(img, PLACEHOLDER_OPA, 0);
    }
    if (c->app && c->app->icon_mask) {
        img = lv_image_create(c->obj);
        lv_obj_remove_style_all(img);
        pos_style_add(img, POS_STYLE_ENV_GLYPH, 0);
        lv_image_set_src(img, c->app->icon_mask);
        lv_obj_set_pos(img, ix + MASK_X, MASK_Y);
    } else {
        const char *text = c->app ? c->app->icon
                           : !place ? folder_symbol[c->folder]
                           : c->pick ? LV_SYMBOL_CLOSE
                                     : LV_SYMBOL_PLUS;

        img = lv_label_create(c->obj);
        lv_label_set_text(img, text ? text : "");
        /* The colour of the environment, the font of the symbols (an
         * LV_SYMBOL_* is not in the text fonts). */
        pos_style_add(img, POS_STYLE_ENV_TEXT, 0);
        pos_style_add(img, POS_STYLE_SYMBOL, 0);
        lv_obj_set_pos(img, ix + MASK_X + 4, MASK_Y + 5);
        if (place) {
            lv_obj_set_style_opa(img, PLACEHOLDER_OPA, 0);
        }
    }
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

static void cell_create(lv_obj_t *parent, struct cell *c, const struct home_rect *r, bool small,
                        struct home_info *count)
{
    int32_t ix = (r->w - HOME_ICON) / 2;
    const struct home_folder_def *def = home_folder_get(c->folder);

    c->obj = plain(parent);
    place(c->obj, r);
    lv_obj_add_flag(c->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c->obj, on_cell_pressed, LV_EVENT_PRESSED, c);
    lv_obj_add_event_cb(c->obj, on_cell_clicked, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(c->obj, on_cell_draw_post, LV_EVENT_DRAW_POST, c);
    if (c->fav >= 0) {
        lv_obj_add_event_cb(c->obj, on_cell_long_pressed, LV_EVENT_LONG_PRESSED, c);
    }
    /* The mark is drawn just outside the icon's box; let it be. */
    lv_obj_set_style_pad_all(c->obj, 0, 0);
    lv_obj_set_ext_click_area(c->obj, 0);
    cell_icon(c, ix, count);
    c->label = lv_label_create(c->obj);
    pos_style_add(c->label, small ? POS_STYLE_ENV_TEXT_SMALL : POS_STYLE_ENV_TEXT, 0);
    lv_label_set_text(c->label, c->app ? c->app->name
                                : def  ? def->name
                                : c->pick ? "Clear"
                                : c->fav >= 0 ? "Add"
                                              : "");
    if (!c->app && !def) {
        lv_obj_set_style_opa(c->label, PLACEHOLDER_OPA, 0);
    }
    lv_label_set_long_mode(c->label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(c->label, r->w);
    /* One line, so a name wider than its cell ends in "..." on that line
     * instead of wrapping below the cell, which clips it mid-word (the text
     * sizes make that possible, DS §46.4). The line the font draws: at Small
     * exactly the height the label had. */
    lv_obj_set_height(c->label, lv_font_get_line_height(lv_obj_get_style_text_font(c->label, LV_PART_MAIN)));
    lv_obj_set_style_text_align(c->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(c->label, 0, HOME_ICON + (small ? 6 : 2));
}

/* The art a page's cells hold. Called after the objects that drew it are
 * gone: nothing may point at an image that has been released. */
static void cells_release(struct page *p)
{
    int k;

    for (k = 0; k < p->ncells; k++) {
        art_free(p->cells[k].icon);
        p->cells[k].icon = NULL;
    }
    p->ncells = 0;
    p->focus = -1;
}

/* ---- panels, header, footer ---------------------------------------------- */

static void panel_create(lv_obj_t *parent, const struct home_rect *r, const char *name)
{
    lv_obj_t *p = plain(parent);
    lv_obj_t *caption;
    lv_obj_t *rule;

    pos_style_add(p, POS_STYLE_ENV_PANEL, 0);
    place(p, r);
    caption = lv_label_create(p);
    pos_style_add(caption, POS_STYLE_ENV_CAPTION, 0);
    lv_label_set_text(caption, name);
    lv_obj_set_pos(caption, 17, 11);
    rule = plain(p);
    pos_style_add(rule, POS_STYLE_ENV_DIVIDER, 0);
    lv_obj_set_pos(rule, 0, HOME_PANEL_HEAD);
    lv_obj_set_size(rule, r->w - 2, 1);
}

static void on_action(lv_event_t *e)
{
    void (*fn)(void) = (void (*)(void))lv_event_get_user_data(e);

    if (fn) {
        fn();
    }
}

static lv_obj_t *action_button(const struct home_rect *r, const lv_image_dsc_t *glyph, const char *text,
                               void (*fn)(void))
{
    lv_obj_t *b = plain(home.root.root);
    lv_obj_t *o;

    pos_style_add(b, POS_STYLE_ENV_PANEL, 0);
    pos_style_add(b, POS_STYLE_ENV_PANEL_PRESSED, LV_STATE_PRESSED);
    place(b, r);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 12, 0);
    lv_obj_add_event_cb(b, on_action, LV_EVENT_CLICKED, (void *)fn);
    o = lv_image_create(b);
    lv_obj_remove_style_all(o);
    pos_style_add(o, POS_STYLE_ENV_GLYPH, 0);
    lv_image_set_src(o, glyph);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    o = lv_label_create(b);
    pos_style_add(o, POS_STYLE_ENV_TEXT, 0);
    lv_label_set_text(o, text);
    return b;
}

/* The time in its band, which keeps clear of the status cluster (DS §36),
 * and the date in its own row under it, which is lower than the cluster and
 * so has the whole width. Both centred on the panels' centre line. */
static void header_create(const struct home_rect *band, const struct home_rect *date_row)
{
    lv_obj_t *h = plain(home.root.root);

    place(h, band);
    home.clock = lv_label_create(h);
    pos_style_add(home.clock, POS_STYLE_ENV_CLOCK, 0);
    lv_label_set_text(home.clock, "--:--");
    lv_obj_align(home.clock, LV_ALIGN_TOP_MID, 0, 0);
    home.date = lv_label_create(home.root.root);
    pos_style_add(home.date, POS_STYLE_ENV_TEXT_SECONDARY, 0);
    lv_label_set_text(home.date, "");
    lv_label_set_long_mode(home.date, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(home.date, LV_TEXT_ALIGN_CENTER, 0);
    place(home.date, date_row);
}

/* The one call that changes the label only when the text does: a clock that
 * rewrites itself every second invalidates the photograph under it every
 * second, and on the panel that is a redraw nobody can see. */
static void set_if_changed(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void home_set_time(const char *hm, const char *date)
{
    set_if_changed(home.clock, hm ? hm : "--:--");
    set_if_changed(home.date, (date && date[0]) ? date : "Time not set");
}

/* ---- keys ----------------------------------------------------------------- */

/* Move the keys' focus to cell k of page p and bring it into view. Moving
 * the mark is redrawing two cells; no LVGL focus changes hands. */
static void show_focus(struct page *p, int k)
{
    if (p->focus >= 0 && p->focus < p->ncells) {
        lv_obj_invalidate(p->cells[p->focus].obj);
    }
    p->focus = k;
    if (k >= 0 && k < p->ncells) {
        lv_obj_invalidate(p->cells[k].obj);
        /* In the first row, the whole top of the page - the time, or the
         * way back and the name - comes back into view with it. */
        if (lv_obj_get_y(p->cells[k].obj) <= lv_obj_get_y(p->cells[0].obj)) {
            lv_obj_scroll_to_y(p->root, 0, LV_ANIM_OFF);
        } else {
            lv_obj_scroll_to_view(p->cells[k].obj, LV_ANIM_OFF);
        }
    }
}

static void centre(const struct cell *c, int32_t *x, int32_t *y)
{
    lv_area_t a;

    lv_obj_get_coords(c->obj, &a);
    *x = a.x1 + lv_area_get_width(&a) / 2;
    *y = a.y1 + lv_area_get_height(&a) / 2;
}

/* The next cell from `from` for key: Left and Right step through the page's
 * order (the panels' order, row by row) and stop at its ends; Up and Down
 * go to the nearest row above or below and, in it, the cell nearest in x,
 * so they cross from panel to panel as the eye does. Nothing moves past an
 * edge: the focus stays where it is. */
static int neighbour(const struct page *p, int from, uint32_t key)
{
    int32_t fx;
    int32_t fy;
    int best = -1;
    int32_t best_dy = 0;
    int32_t best_dx = 0;
    int k;

    if (key == LV_KEY_LEFT) {
        return from > 0 ? from - 1 : from;
    }
    if (key == LV_KEY_RIGHT) {
        return from + 1 < p->ncells ? from + 1 : from;
    }
    centre(&p->cells[from], &fx, &fy);
    for (k = 0; k < p->ncells; k++) {
        int32_t x;
        int32_t y;
        int32_t dy;
        int32_t dx;

        centre(&p->cells[k], &x, &y);
        dy = key == LV_KEY_UP ? fy - y : y - fy;
        dx = x > fx ? x - fx : fx - x;
        if (dy <= HOME_CELL_H / 2) {
            continue; /* the same row, or the wrong way */
        }
        if (best < 0 || dy < best_dy - HOME_CELL_H / 2 || (dy <= best_dy + HOME_CELL_H / 2 && dx < best_dx)) {
            best = k;
            best_dy = dy;
            best_dx = dx;
        }
    }
    return best >= 0 ? best : from;
}

static void on_key(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    struct page *p = showing();

    if (home.pending != PENDING_NONE) {
        return;
    }
    switch (key) {
    case LV_KEY_LEFT:
    case LV_KEY_RIGHT:
    case LV_KEY_UP:
    case LV_KEY_DOWN:
        if (p->ncells == 0) {
            break;
        }
        if (!home.keys_shown || p->focus < 0 || p->focus >= p->ncells) {
            /* The first key shows where the focus is rather than moving it. */
            home.keys_shown = true;
            show_focus(p, p->focus >= 0 && p->focus < p->ncells ? p->focus : 0);
            break;
        }
        show_focus(p, neighbour(p, p->focus, key));
        break;
    case LV_KEY_ENTER:
    case ' ':
        if (p->focus >= 0 && p->focus < p->ncells) {
            home.keys_shown = true;
            activate(&p->cells[p->focus]);
        }
        break;
    case 'e':
    case 'E':
        /* The keys' long press: the picker for the focused favorite. */
        if (p == &home.root && p->focus >= 0 && p->focus < p->ncells && p->cells[p->focus].fav >= 0) {
            home.keys_shown = true;
            request_slot(PENDING_PICK, HOME_FOLDER_NONE, NULL, p->cells[p->focus].fav);
        }
        break;
    case LV_KEY_ESC:
    case LV_KEY_BACKSPACE:
        if (home.folder.root) {
            home.keys_shown = true;
            request(PENDING_CLOSE_FOLDER, HOME_FOLDER_NONE, NULL);
        }
        break;
    default:
        break;
    }
}

void home_keys_attach(void)
{
    lv_group_t *g = pos_input_group();

    if (!g || !home.container) {
        return;
    }
    /* Not re-added when it is in already: adding moves an object to the
     * group's end and takes the focus off it. */
    if (lv_obj_get_group(home.container) != g) {
        pos_input_add_obj(home.container);
    }
    pos_input_focus(home.container);
}

void home_keys_detach(void)
{
    if (home.container && lv_obj_get_group(home.container)) {
        lv_group_remove_obj(home.container);
    }
}

/* ---- folders ---------------------------------------------------------------- */

static void on_folder_back(lv_event_t *e)
{
    (void)e;
    request(PENDING_CLOSE_FOLDER, HOME_FOLDER_NONE, NULL);
}

static void ids_of(const char *ids[HOME_MAX_APPS])
{
    size_t k;

    for (k = 0; k < home.napps; k++) {
        ids[k] = home.apps[k]->id;
    }
}

int home_folder_size(const char *folder_id)
{
    const char *ids[HOME_MAX_APPS];
    uint8_t order[HOME_MAX_APPS];
    enum home_folder f = home_folder_find(folder_id);

    if (f == HOME_FOLDER_NONE) {
        return -1;
    }
    ids_of(ids);
    return home_folder_order(ids, (int)home.napps, f, order);
}

/* The folder page's top row: the way back, the app header's slab (DS §7) in
 * the launcher's glass, and the folder's name beside it. */
static void folder_header(const struct home_folder_layout *lay, const char *name)
{
    lv_obj_t *back = plain(home.folder.root);
    lv_obj_t *o;

    home.back = back;
    pos_style_add(back, POS_STYLE_ENV_PANEL, 0);
    pos_style_add(back, POS_STYLE_ENV_PANEL_PRESSED, LV_STATE_PRESSED);
    place(back, &lay->back);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, on_folder_back, LV_EVENT_CLICKED, NULL);
    o = lv_label_create(back);
    lv_label_set_text(o, LV_SYMBOL_LEFT);
    pos_style_add(o, POS_STYLE_ENV_TEXT, 0);
    pos_style_add(o, POS_STYLE_SYMBOL, 0);
    lv_obj_center(o);
    o = lv_label_create(home.folder.root);
    pos_style_add(o, POS_STYLE_ENV_TITLE, 0);
    lv_label_set_text(o, name);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(o, lay->title.w);
    lv_obj_set_pos(o, lay->title.x,
                   lay->title.y + (lay->title.h - lv_font_get_line_height(lv_obj_get_style_text_font(o, 0))) / 2);
}

static enum pos_env_hue hue_of(const char *id)
{
    const struct home_entry *en = home_entry_find(id);

    return en ? (enum pos_env_hue)en->hue : POS_HUE_APPS;
}

/* The second page - a folder's, or the favorite picker's - over the
 * launcher's: the way back, the name, one panel captioned `caption` holding
 * home.folder.cells[0..n-1], which the caller has filled in. Nothing may be
 * open (page_close first). False when it cannot be laid out. */
static bool page_build(const char *what, const char *name, const char *caption, int n)
{
    struct home_folder_layout_in in;
    static struct home_folder_layout lay;
    int k;

    memset(&in, 0, sizeof(in));
    in.width = home.width;
    in.height = home.height;
    in.landscape = home.landscape;
    in.inset_bottom = home.inset_bottom;
    in.keepout = home.keepout;
    in.n = n;
    if (home_folder_layout_compute(&in, &lay) < 0) {
        LOG_ERROR("launcher: no layout for %s (%d app(s)) in %dx%d", what, n, (int)in.width, (int)in.height);
        return false;
    }
    home.folder.root = plain(home.container);
    lv_obj_set_size(home.folder.root, LV_PCT(100), LV_PCT(100));
    if (lay.content_h > in.height) {
        lv_obj_add_flag(home.folder.root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(home.folder.root, LV_DIR_VER);
    }
    folder_header(&lay, name);
    panel_create(home.folder.root, &lay.panel, caption);
    home.folder.ncells = n;
    for (k = 0; k < n; k++) {
        cell_create(home.folder.root, &home.folder.cells[k], &lay.cell[k], lay.small_labels, NULL);
    }
    home.folder.focus = 0;
    lv_obj_add_flag(home.root.root, LV_OBJ_FLAG_HIDDEN);
    /* Placed now, not at the next refresh: whoever opened it - a key, a
     * tap, shell.folder, a restart - may ask where things are at once. This
     * never runs inside a layout pass (see "deferred navigation"). */
    lv_obj_update_layout(home.folder.root);
    if (home.keys_shown) {
        show_focus(&home.folder, 0);
    }
    LOG_INFO("launcher: %s open, %d app(s) in %d column(s)%s, art %u bytes held", what, n, lay.cols,
             lay.content_h > in.height ? ", scrolls" : "", (unsigned)art_bytes_held());
    return true;
}

/* Back to the launcher's page from whichever second page is up. */
static void page_close(void)
{
    if (!home.folder.root) {
        return;
    }
    lv_obj_delete(home.folder.root);
    home.folder.root = NULL;
    home.back = NULL;
    cells_release(&home.folder);
    home.open = HOME_FOLDER_NONE;
    home.picking = -1;
    lv_obj_remove_flag(home.root.root, LV_OBJ_FLAG_HIDDEN);
    if (home.keys_shown && home.root.focus >= 0 && home.root.focus < home.root.ncells) {
        show_focus(&home.root, home.root.focus);
    }
}

bool home_folder_open(const char *id)
{
    const char *ids[HOME_MAX_APPS];
    uint8_t order[HOME_MAX_APPS];
    enum home_folder f = home_folder_find(id);
    const struct home_folder_def *def = home_folder_get(f);
    char caption[32];
    char what[48];
    int n;
    int k;

    if (!def || !home.container) {
        return false;
    }
    if (home.open == f && home.folder.root) {
        return true;
    }
    ids_of(ids);
    n = home_folder_order(ids, (int)home.napps, f, order);
    if (n == 0) {
        LOG_WARN("launcher: folder %s has no app installed; not opened", def->id);
        return false;
    }
    home_folder_close();
    /* Coming back will be to the folder's cell. */
    for (k = 0; k < home.root.ncells; k++) {
        if (home.root.cells[k].folder == f) {
            home.root.focus = k;
        }
    }
    for (k = 0; k < n; k++) {
        struct cell *c = &home.folder.cells[k];

        memset(c, 0, sizeof(*c));
        c->app = home.apps[order[k]];
        c->folder = HOME_FOLDER_NONE;
        c->fav = -1;
        c->hue = hue_of(c->app->id);
    }
    /* The panel, captioned in the package's capitals as a group's is. */
    for (k = 0; def->name[k] && k < (int)sizeof(caption) - 1; k++) {
        caption[k] = (char)toupper((unsigned char)def->name[k]);
    }
    caption[k] = '\0';
    lv_snprintf(what, sizeof(what), "folder %s", def->id);
    if (!page_build(what, def->name, caption, n)) {
        cells_release(&home.folder);
        return false;
    }
    home.open = f;
    return true;
}

void home_folder_close(void)
{
    const struct home_folder_def *def = home_folder_get(home.open);
    int slot = home.picking;

    if (!home.folder.root) {
        return;
    }
    page_close();
    if (slot >= 0) {
        LOG_INFO("launcher: favorite %d picker closed, art %u bytes held", slot + 1, (unsigned)art_bytes_held());
    } else {
        LOG_INFO("launcher: folder %s closed, art %u bytes held", def ? def->id : "?", (unsigned)art_bytes_held());
    }
}

const char *home_folder_current(void)
{
    const struct home_folder_def *def = home.folder.root ? home_folder_get(home.open) : NULL;

    return def ? def->id : NULL;
}

/* ---- favorites ---------------------------------------------------------------- */

static void stored_of(const char *stored[HOME_FAVORITES])
{
    int s;

    for (s = 0; s < HOME_FAVORITES; s++) {
        stored[s] = home.fav[s][0] ? home.fav[s] : NULL;
    }
}

/* The installed app a slot holds, or NULL. */
static const struct pocketos_app *fav_app(int slot)
{
    const char *ids[HOME_MAX_APPS];
    int i;

    if (slot < 0 || slot >= HOME_FAVORITES || !home.fav[slot][0]) {
        return NULL;
    }
    ids_of(ids);
    i = home_favorite_resolve(ids, (int)home.napps, home.fav[slot]);
    return i >= 0 ? home.apps[i] : NULL;
}

/* How many slots hold an installed app now. */
static int favorites_set(void)
{
    int s;
    int n = 0;

    for (s = 0; s < HOME_FAVORITES; s++) {
        n += fav_app(s) != NULL;
    }
    return n;
}

/* The slot's cell on the launcher's page, (re)built from what it holds. */
static void fav_cell_build(int slot)
{
    struct cell *c = &home.root.cells[slot];

    if (c->obj) {
        lv_obj_delete(c->obj);
    }
    art_free(c->icon);
    memset(c, 0, sizeof(*c));
    c->fav = slot;
    c->folder = HOME_FOLDER_NONE;
    c->app = fav_app(slot);
    c->hue = c->app ? hue_of(c->app->id) : POS_HUE_APPS;
    cell_create(home.root.root, c, &home.fav_rect[slot], home.small_labels, NULL);
}

int home_favorite_set(int slot, const char *id)
{
    const char *ids[HOME_MAX_APPS];
    const char *stored[HOME_FAVORITES];
    bool clear = !id || !id[0];

    if (!home.container || slot < 0 || slot >= HOME_FAVORITES) {
        return -1;
    }
    if (!clear) {
        int r;

        ids_of(ids);
        stored_of(stored);
        r = home_favorite_check(ids, (int)home.napps, stored, slot, id);
        if (r < 0) {
            LOG_WARN("launcher: favorite %d not set to %s: %s", slot + 1, id,
                     r == -2 ? "another favorite holds it" : "no such app");
            return r;
        }
    }
    /* A picker that is open offers what the slots held before. */
    if (home.picking >= 0) {
        home_folder_close();
    }
    lv_snprintf(home.fav[slot], sizeof(home.fav[slot]), "%s", clear ? "" : id);
    if (home.actions.favorite_set && home.actions.favorite_set(slot, clear ? NULL : home.fav[slot]) < 0) {
        LOG_WARN("launcher: favorite %d set but not kept; it lasts until the shell stops", slot + 1);
    }
    fav_cell_build(slot);
    LOG_INFO("launcher: favorite %d %s%s", slot + 1, clear ? "cleared" : "is ", clear ? "" : home.fav[slot]);
    return 0;
}

const char *home_favorite_id(int slot)
{
    const struct pocketos_app *app = home.container ? fav_app(slot) : NULL;

    return app ? app->id : NULL;
}

const char *home_favorite_stored(int slot)
{
    return (home.container && slot >= 0 && slot < HOME_FAVORITES && home.fav[slot][0]) ? home.fav[slot] : NULL;
}

int home_favorite_picking(void)
{
    return home.folder.root ? home.picking : -1;
}

bool home_favorite_pick(int slot)
{
    const char *ids[HOME_MAX_APPS];
    const char *stored[HOME_FAVORITES];
    uint8_t order[HOME_MAX_APPS];
    const struct pocketos_app *current;
    char name[32];
    char what[40];
    int clear;
    int n;
    int k;
    int at = 0;

    if (!home.container || slot < 0 || slot >= HOME_FAVORITES) {
        return false;
    }
    if (home.picking == slot && home.folder.root) {
        return true;
    }
    ids_of(ids);
    stored_of(stored);
    n = home_favorite_candidates(ids, (int)home.napps, stored, slot, order);
    clear = home.fav[slot][0] ? 1 : 0;
    if (n + clear == 0) {
        LOG_WARN("launcher: favorite %d: no app to offer; picker not opened", slot + 1);
        return false;
    }
    if (n + clear > HOME_MAX_APPS) {
        n = HOME_MAX_APPS - clear;
    }
    home_folder_close();
    current = fav_app(slot);
    if (clear) {
        struct cell *c = &home.folder.cells[0];

        memset(c, 0, sizeof(*c));
        c->fav = -1;
        c->pick = true;
        c->hue = POS_HUE_APPS;
    }
    for (k = 0; k < n; k++) {
        struct cell *c = &home.folder.cells[clear + k];

        memset(c, 0, sizeof(*c));
        c->app = home.apps[order[k]];
        c->fav = -1;
        c->pick = true;
        c->hue = hue_of(c->app->id);
        if (c->app == current) {
            at = clear + k;
        }
    }
    /* Coming back will be to the slot. */
    home.root.focus = slot;
    lv_snprintf(name, sizeof(name), "Favorite %d", slot + 1);
    lv_snprintf(what, sizeof(what), "favorite %d picker", slot + 1);
    if (!page_build(what, name, "CHOOSE AN APP", clear + n)) {
        cells_release(&home.folder);
        return false;
    }
    home.picking = slot;
    /* The keys start on what the slot holds, so Enter changes nothing. */
    if (home.keys_shown) {
        show_focus(&home.folder, at);
    } else {
        home.folder.focus = at;
    }
    return true;
}

bool home_favorite_area(int slot, lv_area_t *out)
{
    if (!home.container || home.folder.root || slot < 0 || slot >= HOME_FAVORITES || slot >= home.root.ncells ||
        !home.root.cells[slot].obj) {
        return false;
    }
    lv_obj_get_coords(home.root.cells[slot].obj, out);
    return true;
}

/* ---- building ------------------------------------------------------------ */

lv_obj_t *home_create(lv_obj_t *parent, const struct pocketos_app *const *apps, size_t napps,
                      bool landscape, const lv_area_t *keepout, const struct home_actions *actions)
{
    const char *ids[HOME_MAX_APPS];
    struct home_item items[HOME_MAX_APPS];
    uint8_t count[HOME_GROUP_COUNT];
    struct home_layout_in in;
    static struct home_layout lay;
    const struct pos_display_geometry *g = pocketui_display_geometry();
    struct pocketui_layout_guard guard;
    struct pos_insets ins;
    int placed;
    int k;
    int grp;

    memset(&home.info, 0, sizeof(home.info));
    home.actions = *actions;
    if (napps > HOME_MAX_APPS) {
        napps = HOME_MAX_APPS;
    }
    home.apps = apps;
    home.napps = napps;
    home.landscape = landscape;
    home.open = HOME_FOLDER_NONE;
    home.picking = -1;
    home.pending = PENDING_NONE;
    home.keys_shown = false;
    home.root.focus = -1;
    home.folder.focus = -1;
    for (k = 0; k < (int)napps; k++) {
        ids[k] = apps[k]->id;
    }
    placed = home_root_order(ids, (int)napps, items, count);
    /* The favorites, as they were kept. Something that cannot be an app id
     * is dropped here (and stays in the file until the slot is set); an id
     * of an app this build does not have is kept, and shown empty. */
    for (k = 0; k < HOME_FAVORITES; k++) {
        const char *v = actions->favorite_get ? actions->favorite_get(k) : NULL;

        home.fav[k][0] = '\0';
        if (v && v[0] && !home_favorite_id_ok(v)) {
            LOG_WARN("launcher: favorite %d holds \"%.40s\", which is no app id; shown empty", k + 1, v);
        } else if (v && v[0]) {
            lv_snprintf(home.fav[k], sizeof(home.fav[k]), "%s", v);
            if (home_favorite_resolve(ids, (int)napps, v) < 0) {
                LOG_WARN("launcher: favorite %d holds %s, which is not installed; shown empty", k + 1, v);
            }
        }
    }

    memset(&in, 0, sizeof(in));
    /* The corner clearance of the content area, from the one platform rule
     * (pocketui.h); the launcher is built once, so the guard is only a way
     * to ask. */
    lv_obj_update_layout(parent);
    pocketui_layout_guard_reset(&guard);
    pocketui_layout_begin(&guard, parent, &ins);
    in.width = g->width;
    in.height = lv_obj_get_height(parent);
    in.landscape = landscape;
    in.inset_left = ins.left;
    in.inset_right = ins.right;
    in.inset_bottom = ins.bottom;
    if (keepout) {
        in.keepout = rect_of(keepout);
    }
    home.keepout = in.keepout;
    home.inset_bottom = in.inset_bottom;
    home.width = in.width;
    home.height = in.height;
    /* The favorites' panel first, then the groups (home_layout_groups). */
    home_layout_groups(&in, count);
    if (home_layout_compute(&in, &lay) < 0) {
        LOG_ERROR("launcher: no layout for %d place(s) in %dx%d", HOME_FAVORITES + placed, (int)in.width,
                  (int)in.height);
        return NULL;
    }

    /* The shell's handle, the object the keys arrive on, and the parent of
     * both pages: showing and hiding it shows and hides whichever page is
     * up. */
    home.container = plain(parent);
    lv_obj_set_size(home.container, LV_PCT(100), LV_PCT(100));
    lv_obj_add_event_cb(home.container, on_key, LV_EVENT_KEY, NULL);
    home.root.root = plain(home.container);
    lv_obj_set_size(home.root.root, LV_PCT(100), LV_PCT(100));
    if (lay.content_h > in.height) {
        lv_obj_add_flag(home.root.root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(home.root.root, LV_DIR_VER);
    }
    header_create(&lay.header, &lay.date);
    panel_create(home.root.root, &lay.panel[0], "FAVORITES");
    for (grp = 0; grp < HOME_GROUP_COUNT; grp++) {
        if (count[grp]) {
            panel_create(home.root.root, &lay.panel[grp + 1], home_group_name((enum home_group)grp));
            home.info.groups++;
        }
    }
    /* Cells 0 .. HOME_FAVORITES-1 are the favorites; the apps and folders
     * follow, as home_root_order placed them. */
    home.small_labels = lay.small_labels;
    for (k = 0; k < HOME_FAVORITES; k++) {
        home.fav_rect[k] = lay.cell[k];
        home.root.cells[k].obj = NULL;
        home.root.cells[k].icon = NULL;
        fav_cell_build(k);
    }
    home.root.ncells = HOME_FAVORITES + placed;
    for (k = 0; k < placed; k++) {
        struct cell *c = &home.root.cells[HOME_FAVORITES + k];

        memset(c, 0, sizeof(*c));
        c->fav = -1;
        if (items[k].folder != HOME_FOLDER_NONE) {
            c->folder = (enum home_folder)items[k].folder;
            c->hue = (enum pos_env_hue)home_folder_get(c->folder)->hue;
            home.info.folders++;
        } else {
            c->app = apps[items[k].index];
            c->hue = hue_of(c->app->id);
        }
        cell_create(home.root.root, c, &lay.cell[HOME_FAVORITES + k], lay.small_labels, &home.info);
    }
    action_button(&lay.lock_button, &pos_glyph_lock, "Lock", actions->lock);
    action_button(&lay.controls_button, &pos_glyph_settings, "Controls", actions->controls);
    home.info.apps = (int)napps;
    home.info.cells = placed;
    home.info.favorites = HOME_FAVORITES;
    home.info.scrolls = lay.content_h > in.height;
    home.info.wrapped = lay.wrapped;
    home.info.cell_w = lay.cell_w;
    LOG_INFO("launcher: %d group(s), %d app(s), %s, %d px cells%s, icons %d drawn + %d fallback%s; %d cell(s), "
             "%d of them folder(s); %d favorite(s), %d set",
             home.info.groups, (int)napps, landscape ? "landscape" : "portrait", (int)lay.cell_w,
             lay.wrapped ? " (wrapped)" : "", home.info.icons_art, home.info.icons_fallback,
             home.info.scrolls ? ", scrolls" : "", placed, home.info.folders, HOME_FAVORITES, favorites_set());
    return home.container;
}

void home_destroy(void)
{
    if (!home.container) {
        return;
    }
    if (home.pending != PENDING_NONE) {
        lv_async_call_cancel(run_pending, NULL);
        home.pending = PENDING_NONE;
    }
    lv_obj_delete(home.container);
    home.container = NULL;
    home.folder.root = NULL;
    home.back = NULL;
    home.root.root = NULL;
    cells_release(&home.folder);
    cells_release(&home.root);
    memset(home.root.cells, 0, sizeof(home.root.cells));
    art_free(home.frame);
    home.frame = NULL;
    home.clock = NULL;
    home.date = NULL;
    home.open = HOME_FOLDER_NONE;
    home.picking = -1;
}

void home_info(struct home_info *out)
{
    *out = home.info;
    out->favorites_set = home.container ? favorites_set() : 0;
}

bool home_header_area(lv_area_t *time, lv_area_t *date)
{
    if (!home.clock || !home.date) {
        return false;
    }
    lv_obj_get_coords(home.clock, time);
    lv_obj_get_coords(home.date, date);
    return true;
}

bool home_cell_area(const char *app_id, lv_area_t *out)
{
    const struct page *p = showing();
    int k;

    /* An app's own cell, not a favorite's: that one is home_favorite_area's. */
    for (k = 0; app_id && k < p->ncells; k++) {
        if (p->cells[k].app && p->cells[k].fav < 0 && strcmp(p->cells[k].app->id, app_id) == 0) {
            lv_obj_get_coords(p->cells[k].obj, out);
            return true;
        }
    }
    return false;
}

bool home_folder_area(const char *folder_id, lv_area_t *out)
{
    enum home_folder f = home_folder_find(folder_id);
    int k;

    for (k = 0; f != HOME_FOLDER_NONE && !home.folder.root && k < home.root.ncells; k++) {
        if (home.root.cells[k].folder == f) {
            lv_obj_get_coords(home.root.cells[k].obj, out);
            return true;
        }
    }
    return false;
}

bool home_folder_back_area(lv_area_t *out)
{
    if (!home.folder.root || !home.back) {
        return false;
    }
    lv_obj_get_coords(home.back, out);
    return true;
}

const char *home_focus_id(bool *shown)
{
    const struct page *p = showing();

    if (shown) {
        *shown = home.keys_shown;
    }
    if (!home.container || p->focus < 0 || p->focus >= p->ncells) {
        return NULL;
    }
    return cell_name(&p->cells[p->focus]);
}
