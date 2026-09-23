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
#define MARK_W 2
#define UNDERLINE_W 96
/* An app with no portal icon: its own 32 px mask centred in the empty
 * frame's inner panel. */
#define MASK_X 32
#define MASK_Y 31

struct cell {
    const struct pocketos_app *app;
    enum pos_env_hue hue;
    lv_obj_t *obj;
    lv_obj_t *label;
    lv_image_dsc_t *icon; /* owned; NULL when drawn on the shared frame */
};

static struct {
    lv_obj_t *root;
    lv_obj_t *clock;
    lv_obj_t *date;
    struct cell cells[HOME_MAX_APPS];
    int ncells;
    lv_image_dsc_t *frame; /* the empty portal, loaded on first need */
    struct home_actions actions;
    struct home_info info;
} home;

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

/* ---- cells ------------------------------------------------------------- */

static void on_cell_clicked(lv_event_t *e)
{
    struct cell *c = lv_event_get_user_data(e);

    if (home.actions.open) {
        home.actions.open(c->app);
    }
}

/* The package's focus mark - a bracket over the frame's top corners and a
 * rule under the name, in the app's colour - shown while the cell is held.
 * Drawn, not built from objects, so a cell is three objects however it is
 * marked, and the mark can never catch a touch. */
static void on_cell_draw_post(lv_event_t *e)
{
    struct cell *c = lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_rect_dsc_t d;
    lv_area_t a;
    lv_area_t r;
    int32_t ix;
    int32_t rx;
    int32_t ly;

    if (!lv_obj_has_state(obj, LV_STATE_PRESSED)) {
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
                     a.x1 + (lv_area_get_width(&a) + UNDERLINE_W) / 2 - 1, ly };
    lv_draw_rect(layer, &d, &r);
}

static void cell_icon(struct cell *c, int32_t ix)
{
    char name[48];
    lv_obj_t *img;

    lv_snprintf(name, sizeof(name), "icon-%s", c->app->id);
    c->icon = home_entry_find(c->app->id) ? art_load(name) : NULL;
    if (c->icon) {
        img = lv_image_create(c->obj);
        pos_style_add(img, POS_STYLE_ENV_ICON, 0);
        lv_image_set_src(img, c->icon);
        lv_obj_set_pos(img, ix, 0);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        home.info.icons_art++;
        return;
    }
    /* No portal icon for this app: the empty portal, with the app's own
     * mask (or its text icon) where the glyph would be. */
    home.info.icons_fallback++;
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
    if (c->app->icon_mask) {
        img = lv_image_create(c->obj);
        lv_obj_remove_style_all(img);
        pos_style_add(img, POS_STYLE_ENV_GLYPH, 0);
        lv_image_set_src(img, c->app->icon_mask);
        lv_obj_set_pos(img, ix + MASK_X, MASK_Y);
    } else {
        img = lv_label_create(c->obj);
        lv_label_set_text(img, c->app->icon ? c->app->icon : "");
        /* The colour of the environment, the font of the symbols (an
         * LV_SYMBOL_* is not in the text fonts). */
        pos_style_add(img, POS_STYLE_ENV_TEXT, 0);
        pos_style_add(img, POS_STYLE_SYMBOL, 0);
        lv_obj_set_pos(img, ix + MASK_X + 4, MASK_Y + 5);
    }
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

static void cell_create(struct cell *c, const struct home_rect *r, bool small)
{
    int32_t ix = (r->w - HOME_ICON) / 2;

    c->obj = plain(home.root);
    place(c->obj, r);
    lv_obj_add_flag(c->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c->obj, on_cell_clicked, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(c->obj, on_cell_draw_post, LV_EVENT_DRAW_POST, c);
    /* The mark is drawn just outside the icon's box; let it be. */
    lv_obj_set_style_pad_all(c->obj, 0, 0);
    lv_obj_set_ext_click_area(c->obj, 0);
    cell_icon(c, ix);
    c->label = lv_label_create(c->obj);
    pos_style_add(c->label, small ? POS_STYLE_ENV_TEXT_SMALL : POS_STYLE_ENV_TEXT, 0);
    lv_label_set_text(c->label, c->app->name);
    lv_label_set_long_mode(c->label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(c->label, r->w);
    lv_obj_set_style_text_align(c->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(c->label, 0, HOME_ICON + (small ? 6 : 2));
}

/* ---- panels, header, footer ---------------------------------------------- */

static void panel_create(const struct home_rect *r, const char *name)
{
    lv_obj_t *p = plain(home.root);
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
    lv_obj_t *b = plain(home.root);
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

static void header_create(const struct home_rect *r, bool landscape)
{
    lv_obj_t *h = plain(home.root);

    place(h, r);
    home.clock = lv_label_create(h);
    pos_style_add(home.clock, POS_STYLE_ENV_CLOCK, 0);
    lv_label_set_text(home.clock, "--:--");
    lv_obj_align(home.clock, LV_ALIGN_TOP_MID, 0, 0);
    home.date = lv_label_create(h);
    pos_style_add(home.date, POS_STYLE_ENV_TEXT_SECONDARY, 0);
    lv_label_set_text(home.date, "");
    lv_obj_align(home.date, LV_ALIGN_TOP_MID, 0, landscape ? 70 : 78);
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

lv_obj_t *home_create(lv_obj_t *parent, const struct pocketos_app *const *apps, size_t napps,
                      bool landscape, const struct home_actions *actions)
{
    const char *ids[HOME_MAX_APPS];
    uint8_t order[HOME_MAX_APPS];
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
    for (k = 0; k < (int)napps; k++) {
        ids[k] = apps[k]->id;
    }
    placed = home_group_order(ids, (int)napps, order, count);

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
    in.ngroups = HOME_GROUP_COUNT;
    memcpy(in.count, count, sizeof(count));
    if (home_layout_compute(&in, &lay) < 0) {
        LOG_ERROR("launcher: no layout for %d app(s) in %dx%d", placed, (int)in.width, (int)in.height);
        return NULL;
    }

    home.root = plain(parent);
    lv_obj_set_size(home.root, LV_PCT(100), LV_PCT(100));
    if (lay.content_h > in.height) {
        lv_obj_add_flag(home.root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(home.root, LV_DIR_VER);
    }
    header_create(&lay.header, landscape);
    for (grp = 0; grp < HOME_GROUP_COUNT; grp++) {
        if (count[grp]) {
            panel_create(&lay.panel[grp], home_group_name((enum home_group)grp));
            home.info.groups++;
        }
    }
    home.ncells = placed;
    for (k = 0; k < placed; k++) {
        const struct home_entry *en = home_entry_find(apps[order[k]]->id);
        struct cell *c = &home.cells[k];

        c->app = apps[order[k]];
        c->hue = en ? (enum pos_env_hue)en->hue : POS_HUE_APPS;
        cell_create(c, &lay.cell[k], lay.small_labels);
    }
    action_button(&lay.lock_button, &pos_glyph_lock, "Lock", actions->lock);
    action_button(&lay.controls_button, &pos_glyph_settings, "Controls", actions->controls);
    home.info.apps = placed;
    home.info.scrolls = lay.content_h > in.height;
    home.info.wrapped = lay.wrapped;
    home.info.cell_w = lay.cell_w;
    LOG_INFO("launcher: %d group(s), %d app(s), %s, %d px cells%s, icons %d drawn + %d fallback%s",
             home.info.groups, placed, landscape ? "landscape" : "portrait", (int)lay.cell_w,
             lay.wrapped ? " (wrapped)" : "", home.info.icons_art, home.info.icons_fallback,
             home.info.scrolls ? ", scrolls" : "");
    return home.root;
}

void home_info(struct home_info *out)
{
    *out = home.info;
}

bool home_cell_area(const char *app_id, lv_area_t *out)
{
    int k;

    for (k = 0; k < home.ncells; k++) {
        if (strcmp(home.cells[k].app->id, app_id) == 0) {
            lv_obj_get_coords(home.cells[k].obj, out);
            return true;
        }
    }
    return false;
}
