/*
 * PocketFleet tactical grid. See fleet_grid.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_grid.h"

#include "pocketui.h"

#include <stdlib.h>
#include <string.h>

#define CELL_RADIUS 2
#define HULL_OUTLINE 2
#define CURSOR_OUTLINE 2

/* Motion (DS §12): restrained, never blocking, and entirely absent when the
 * reduced-motion setting is on. The sweep is a rotating radius with a short
 * fading tail, one step per timer tick. 80 ms and 6 degrees give a little
 * under five seconds per revolution and about twelve redraws a second of the
 * grid area only. */
#define MOTION_PERIOD_MS 80
#define SWEEP_STEP_DEG 6
#define SWEEP_TAIL 4
#define SWEEP_TAIL_DEG 5
#define FLASH_MS 320
#define FLASH_GROWTH 5

struct fleet_grid {
    const struct fleet_board *board;
    uint8_t mode;
    int cell;
    int gutter;
    int8_t cursor_row;
    int8_t cursor_col;
    int8_t preview_row;
    int8_t preview_col;
    uint8_t preview_len;
    uint8_t preview_vertical;
    uint8_t preview_valid;
    void (*tap)(void *user, int row, int col);
    void *user;
    lv_timer_t *motion;
    uint16_t sweep_deg;
    int8_t flash_row;
    int8_t flash_col;
    uint32_t flash_start;
};

/* What a cell should look like. Every value has both a fill and a shape, so
 * the states stay apart without relying on hue (DS §2). */
enum cell_look {
    LOOK_WATER = 0,
    LOOK_MISS,
    LOOK_HIT,
    LOOK_SUNK,
    LOOK_SHIP,
    LOOK_SHIP_HIT,
    LOOK_SHIP_SUNK
};

static struct fleet_grid *state_of(lv_obj_t *grid)
{
    return grid ? lv_obj_get_user_data(grid) : NULL;
}

static int span(const struct fleet_grid *g)
{
    return g->gutter + FLEET_GRID * g->cell + (FLEET_GRID - 1) * FLEET_GRID_GAP;
}

static void cell_area(const struct fleet_grid *g, const lv_area_t *coords, int row, int col,
                      lv_area_t *out)
{
    int32_t x = coords->x1 + g->gutter + col * (g->cell + FLEET_GRID_GAP);
    int32_t y = coords->y1 + g->gutter + row * (g->cell + FLEET_GRID_GAP);

    out->x1 = x;
    out->y1 = y;
    out->x2 = x + g->cell - 1;
    out->y2 = y + g->cell - 1;
}

static enum cell_look look_of(const struct fleet_grid *g, int idx)
{
    const struct fleet_board *b = g->board;
    uint8_t ship = b->ship_at[idx];
    int shot = b->shot[idx];
    int sunk = ship != FLEET_NO_SHIP && fleet_board_ship_sunk(b, (enum fleet_ship)ship);

    if (g->mode == FLEET_GRID_TARGET) {
        if (!shot) {
            return LOOK_WATER;
        }
        if (ship == FLEET_NO_SHIP) {
            return LOOK_MISS;
        }
        return sunk ? LOOK_SUNK : LOOK_HIT;
    }
    if (ship == FLEET_NO_SHIP) {
        return shot ? LOOK_MISS : LOOK_WATER;
    }
    if (!shot) {
        return LOOK_SHIP;
    }
    return sunk ? LOOK_SHIP_SUNK : LOOK_SHIP_HIT;
}

/* Centred square of the given side length, used for the hit and miss marks. */
static void mark_area(const lv_area_t *cell, int size, lv_area_t *out)
{
    int32_t cx = (cell->x1 + cell->x2) / 2;
    int32_t cy = (cell->y1 + cell->y2) / 2;

    out->x1 = cx - size / 2;
    out->y1 = cy - size / 2;
    out->x2 = out->x1 + size - 1;
    out->y2 = out->y1 + size - 1;
}

static void draw_cell(lv_layer_t *layer, const struct fleet_grid *g, const lv_area_t *area,
                      enum cell_look look)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t mark;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = CELL_RADIUS;
    dsc.bg_opa = LV_OPA_COVER;
    switch (look) {
    case LOOK_MISS:
        dsc.bg_color = pos_theme_color(POS_COLOR_BG);
        dsc.border_color = pos_theme_color(POS_COLOR_LINE);
        dsc.border_width = 1;
        dsc.border_opa = LV_OPA_COVER;
        break;
    case LOOK_HIT:
    case LOOK_SUNK:
        dsc.bg_color = pos_theme_color(POS_COLOR_RADIO_TX);
        break;
    case LOOK_SHIP:
        /* A raised fill alone is one quantisation step from water in RGB565
         * (DS feasibility H1), so the hull is drawn with a border instead of
         * a bright fill, which also keeps lit area down on the AMOLED. */
        dsc.bg_color = pos_theme_color(POS_COLOR_SURFACE_RAISED);
        dsc.border_color = pos_theme_color(POS_COLOR_TEXT_SECONDARY);
        dsc.border_width = HULL_OUTLINE;
        dsc.border_opa = LV_OPA_COVER;
        break;
    case LOOK_SHIP_HIT:
    case LOOK_SHIP_SUNK:
        dsc.bg_color = pos_theme_color(POS_COLOR_RADIO_RX);
        break;
    case LOOK_WATER:
    default:
        dsc.bg_color = pos_theme_color(POS_COLOR_SURFACE);
        break;
    }
    if (look == LOOK_SUNK || look == LOOK_SHIP_SUNK) {
        /* On a filled accent everything is drawn in text_on_accent (DS §4).
         * text_primary would be bright on bright: 1.19 against slate's RX. */
        dsc.border_color = pos_theme_color(POS_COLOR_TEXT_ON_ACCENT);
        dsc.border_width = HULL_OUTLINE;
        dsc.border_opa = LV_OPA_COVER;
    }
    lv_draw_rect(layer, &dsc, area);

    if (look == LOOK_MISS) {
        /* text_secondary, not text_muted: DS §13 allows muted only for
         * non-essential hints, and where a shot went is not one. */
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius = LV_RADIUS_CIRCLE;
        dsc.bg_opa = LV_OPA_COVER;
        dsc.bg_color = pos_theme_color(POS_COLOR_TEXT_SECONDARY);
        mark_area(area, g->cell / 6 + 2, &mark);
        lv_draw_rect(layer, &dsc, &mark);
    } else if (look == LOOK_HIT || look == LOOK_SHIP_HIT) {
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius = CELL_RADIUS;
        dsc.bg_opa = LV_OPA_COVER;
        dsc.bg_color = pos_theme_color(POS_COLOR_TEXT_ON_ACCENT);
        mark_area(area, g->cell / 3, &mark);
        lv_draw_rect(layer, &dsc, &mark);
    } else if (look == LOOK_SUNK || look == LOOK_SHIP_SUNK) {
        /* A cross, not a filled square: the two states share a fill and a
         * mark colour, so the shape has to carry the difference on its own. */
        lv_draw_line_dsc_t line;

        mark_area(area, g->cell / 2, &mark);
        lv_draw_line_dsc_init(&line);
        line.color = pos_theme_color(POS_COLOR_TEXT_ON_ACCENT);
        line.width = g->cell >= 32 ? 3 : 2;
        line.opa = LV_OPA_COVER;
        line.round_start = 1;
        line.round_end = 1;
        line.p1.x = mark.x1;
        line.p1.y = mark.y1;
        line.p2.x = mark.x2;
        line.p2.y = mark.y2;
        lv_draw_line(layer, &line);
        line.p1.x = mark.x2;
        line.p1.y = mark.y1;
        line.p2.x = mark.x1;
        line.p2.y = mark.y2;
        lv_draw_line(layer, &line);
    }
}

static void draw_outline(lv_layer_t *layer, const lv_area_t *area, lv_color_t color, int width)
{
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = CELL_RADIUS;
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = color;
    dsc.border_width = width;
    dsc.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &dsc, area);
}

static void draw_labels(lv_layer_t *layer, lv_obj_t *obj, const struct fleet_grid *g,
                        const lv_area_t *coords)
{
    lv_draw_label_dsc_t dsc;
    char text[4];
    lv_area_t area;
    int i;

    lv_draw_label_dsc_init(&dsc);
    dsc.color = pos_theme_color(POS_COLOR_TEXT_SECONDARY);
    /* The caption role is on the object, so the font comes from the theme
     * rather than from a font symbol named here (style_lint). */
    dsc.font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    dsc.align = LV_TEXT_ALIGN_CENTER;
    dsc.text = text;
    if (!dsc.font) {
        return;
    }
    for (i = 0; i < FLEET_GRID; i++) {
        text[0] = (char)('A' + i);
        text[1] = '\0';
        cell_area(g, coords, 0, i, &area);
        area.y1 = coords->y1;
        area.y2 = coords->y1 + g->gutter - 4;
        lv_draw_label(layer, &dsc, &area);
    }
    for (i = 0; i < FLEET_GRID; i++) {
        text[0] = '\0';
        if (i == 9) {
            text[0] = '1';
            text[1] = '0';
            text[2] = '\0';
        } else {
            text[0] = (char)('1' + i);
            text[1] = '\0';
        }
        cell_area(g, coords, i, 0, &area);
        area.x1 = coords->x1;
        area.x2 = coords->x1 + g->gutter - 6;
        /* Nudge onto the row's optical centre for the caption line height. */
        area.y1 += (g->cell - 16) / 2;
        lv_draw_label(layer, &dsc, &area);
    }
}

/* A rotating radius with a short tail, drawn over the cells at low opacity so
 * it reads as a sweep without hiding anything. */
static void draw_sweep(lv_layer_t *layer, const struct fleet_grid *g,
                       const lv_area_t *coords)
{
    lv_draw_line_dsc_t dsc;
    int32_t play = FLEET_GRID * g->cell + (FLEET_GRID - 1) * FLEET_GRID_GAP;
    int32_t cx = coords->x1 + g->gutter + play / 2;
    int32_t cy = coords->y1 + g->gutter + play / 2;
    int32_t reach = play / 2;
    int tail;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = pos_theme_color(POS_COLOR_ACCENT_PRIMARY);
    dsc.width = 3;
    dsc.round_end = 1;
    for (tail = SWEEP_TAIL; tail >= 0; tail--) {
        int16_t angle = (int16_t)((g->sweep_deg + 360 - tail * SWEEP_TAIL_DEG) % 360);

        dsc.opa = (lv_opa_t)(LV_OPA_40 / (tail + 1));
        dsc.p1.x = cx;
        dsc.p1.y = cy;
        dsc.p2.x = cx + (lv_trigo_cos(angle) * reach >> LV_TRIGO_SHIFT);
        dsc.p2.y = cy + (lv_trigo_sin(angle) * reach >> LV_TRIGO_SHIFT);
        lv_draw_line(layer, &dsc);
    }
}

/* One widening, fading ring on the cell that has just resolved. */
static void draw_flash(lv_layer_t *layer, const struct fleet_grid *g,
                       const lv_area_t *coords)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t area;
    uint32_t elapsed = lv_tick_elaps(g->flash_start);
    int32_t grow;

    if (elapsed >= FLASH_MS) {
        return;
    }
    cell_area(g, coords, g->flash_row, g->flash_col, &area);
    grow = (int32_t)(FLASH_GROWTH * elapsed / FLASH_MS);
    area.x1 -= grow;
    area.y1 -= grow;
    area.x2 += grow;
    area.y2 += grow;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = CELL_RADIUS;
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = pos_theme_color(POS_COLOR_TEXT_PRIMARY);
    dsc.border_width = 2;
    dsc.border_opa = (lv_opa_t)(LV_OPA_COVER - LV_OPA_COVER * elapsed / FLASH_MS);
    lv_draw_rect(layer, &dsc, &area);
}

static void grid_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct fleet_grid *g = state_of(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_area_t area;
    int row;
    int col;

    if (!g || !g->board || !layer) {
        return;
    }
    lv_obj_get_coords(obj, &coords);
    if (g->gutter) {
        draw_labels(layer, obj, g, &coords);
    }
    for (row = 0; row < FLEET_GRID; row++) {
        for (col = 0; col < FLEET_GRID; col++) {
            cell_area(g, &coords, row, col, &area);
            draw_cell(layer, g, &area, look_of(g, fleet_index(row, col)));
        }
    }
    if (g->preview_len) {
        lv_color_t color = g->preview_valid ? pos_theme_color(POS_COLOR_ACCENT_PRIMARY)
                                            : pos_theme_color(POS_COLOR_STATUS_WARN);
        int n;

        for (n = 0; n < g->preview_len; n++) {
            int r = g->preview_row + (g->preview_vertical ? n : 0);
            int c = g->preview_col + (g->preview_vertical ? 0 : n);

            if (!fleet_in_bounds(r, c)) {
                continue;
            }
            cell_area(g, &coords, r, c, &area);
            draw_outline(layer, &area, color, HULL_OUTLINE);
        }
    }
    if (g->motion && g->mode == FLEET_GRID_TARGET) {
        draw_sweep(layer, g, &coords);
    }
    if (g->cursor_row >= 0) {
        cell_area(g, &coords, g->cursor_row, g->cursor_col, &area);
        draw_outline(layer, &area, pos_theme_color(POS_COLOR_FOCUS), CURSOR_OUTLINE);
    }
    if (g->flash_row >= 0) {
        draw_flash(layer, g, &coords);
    }
}

static void grid_motion_tick(lv_timer_t *timer)
{
    lv_obj_t *obj = lv_timer_get_user_data(timer);
    struct fleet_grid *g = state_of(obj);

    if (!g) {
        return;
    }
    g->sweep_deg = (uint16_t)((g->sweep_deg + SWEEP_STEP_DEG) % 360);
    if (g->flash_row >= 0 && lv_tick_elaps(g->flash_start) >= FLASH_MS) {
        g->flash_row = -1;
    }
    /* Only the grid is invalidated, never the screen. */
    lv_obj_invalidate(obj);
}

static void grid_click(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct fleet_grid *g = state_of(obj);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;
    lv_area_t coords;
    int32_t x;
    int32_t y;
    int row;
    int col;

    if (!g || !g->tap || !indev) {
        return;
    }
    lv_indev_get_point(indev, &point);
    lv_obj_get_coords(obj, &coords);
    x = point.x - coords.x1 - g->gutter;
    y = point.y - coords.y1 - g->gutter;
    if (x < 0 || y < 0) {
        return;
    }
    col = (int)(x / (g->cell + FLEET_GRID_GAP));
    row = (int)(y / (g->cell + FLEET_GRID_GAP));
    if (!fleet_in_bounds(row, col)) {
        return;
    }
    g->tap(g->user, row, col);
}

static void grid_delete(lv_event_t *e)
{
    struct fleet_grid *g = state_of(lv_event_get_target_obj(e));

    if (g && g->motion) {
        lv_timer_delete(g->motion);
    }
    free(g);
}

/* Fallback for when the engine's watch table is full: the shared styles are
 * rewritten in place on a theme change, which does not invalidate a
 * custom-drawn object by itself. */
static void grid_theme_changed(lv_event_t *e)
{
    lv_obj_invalidate(lv_event_get_target_obj(e));
}

lv_obj_t *fleet_grid_create(lv_obj_t *parent, enum fleet_grid_mode mode, int cell, int labels)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct fleet_grid *g = calloc(1, sizeof(*g));

    if (!g) {
        lv_obj_delete(obj);
        return NULL;
    }
    g->mode = (uint8_t)mode;
    g->cell = cell;
    g->gutter = labels ? FLEET_GRID_GUTTER : 0;
    g->cursor_row = -1;
    g->cursor_col = -1;
    g->flash_row = -1;
    g->flash_col = -1;

    lv_obj_remove_style_all(obj);
    /* Only for the caption font the labels are drawn with. */
    pos_style_add(obj, POS_STYLE_CAPTION, 0);
    lv_obj_set_user_data(obj, g);
    lv_obj_set_size(obj, span(g), span(g));
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, grid_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, grid_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(obj, grid_delete, LV_EVENT_DELETE, NULL);
    /* The theme engine repaints registered custom-drawn objects itself; it
     * only asks the caller to subscribe when its table is full. */
    if (pos_theme_watch(obj) != 0) {
        lv_obj_add_event_cb(obj, grid_theme_changed,
                            (lv_event_code_t)pos_event_theme_changed(), NULL);
    }
    return obj;
}

void fleet_grid_bind(lv_obj_t *grid, const struct fleet_board *board)
{
    struct fleet_grid *g = state_of(grid);

    if (!g) {
        return;
    }
    g->board = board;
    lv_obj_invalidate(grid);
}

void fleet_grid_set_cursor(lv_obj_t *grid, int row, int col)
{
    struct fleet_grid *g = state_of(grid);

    if (!g) {
        return;
    }
    if (!fleet_in_bounds(row, col)) {
        g->cursor_row = -1;
        g->cursor_col = -1;
    } else {
        g->cursor_row = (int8_t)row;
        g->cursor_col = (int8_t)col;
    }
    lv_obj_invalidate(grid);
}

int fleet_grid_get_cursor(lv_obj_t *grid, int *row, int *col)
{
    struct fleet_grid *g = state_of(grid);

    if (!g || g->cursor_row < 0) {
        return -1;
    }
    if (row) {
        *row = g->cursor_row;
    }
    if (col) {
        *col = g->cursor_col;
    }
    return 0;
}

void fleet_grid_set_preview(lv_obj_t *grid, int row, int col, int length, int vertical,
                            int valid)
{
    struct fleet_grid *g = state_of(grid);

    if (!g) {
        return;
    }
    g->preview_row = (int8_t)row;
    g->preview_col = (int8_t)col;
    g->preview_len = (uint8_t)(length > 0 ? length : 0);
    g->preview_vertical = (uint8_t)(vertical != 0);
    g->preview_valid = (uint8_t)(valid != 0);
    lv_obj_invalidate(grid);
}

void fleet_grid_clear_preview(lv_obj_t *grid)
{
    struct fleet_grid *g = state_of(grid);

    if (g) {
        g->preview_len = 0;
        lv_obj_invalidate(grid);
    }
}

void fleet_grid_set_tap_cb(lv_obj_t *grid, void (*cb)(void *user, int row, int col),
                           void *user)
{
    struct fleet_grid *g = state_of(grid);

    if (g) {
        g->tap = cb;
        g->user = user;
    }
}

void fleet_grid_refresh(lv_obj_t *grid)
{
    if (grid) {
        lv_obj_invalidate(grid);
    }
}

void fleet_grid_set_motion(lv_obj_t *grid, int enabled)
{
    struct fleet_grid *g = state_of(grid);

    if (!g) {
        return;
    }
    if (enabled && !g->motion) {
        g->motion = lv_timer_create(grid_motion_tick, MOTION_PERIOD_MS, grid);
    } else if (!enabled && g->motion) {
        lv_timer_delete(g->motion);
        g->motion = NULL;
        g->flash_row = -1;
        lv_obj_invalidate(grid);
    }
}

void fleet_grid_flash(lv_obj_t *grid, int row, int col)
{
    struct fleet_grid *g = state_of(grid);

    /* Without motion the cell has already changed colour and shape, which is
     * the whole message; adding an instant ring would only be noise. */
    if (!g || !g->motion || !fleet_in_bounds(row, col)) {
        return;
    }
    g->flash_row = (int8_t)row;
    g->flash_col = (int8_t)col;
    g->flash_start = lv_tick_get();
    lv_obj_invalidate(grid);
}
