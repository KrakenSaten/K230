/*
 * PocketRadar sensor scope. See radar_scope.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "radar_scope.h"

#include "pocketui.h"

#include <stdlib.h>
#include <string.h>

#define RIM_INSET 2
#define RING_COUNT 3
#define TICK_COUNT 12
#define TICK_LEN 8
#define TICK_LEN_CARDINAL 14
#define HUB_RADIUS 5

/* Marker geometry, in pixels. Large enough to read at arm's length on a
 * 330 ppi panel without crowding the scope when several are up at once. */
#define MARK_RADIUS 12
#define MARK_STROKE 2
#define LOCK_RING_RADIUS 19
#define LOCK_RING_WIDTH 3
#define BRACKET_REACH 24
#define BRACKET_ARM 8

/* The sweep is the same shape PocketFleet's proved cheap: a radius with a
 * short fading tail, drawn over the contacts rather than behind them, and
 * only ever invalidating this object. */
#define SWEEP_TAIL 7
#define SWEEP_TAIL_DEG 3

#define FLASH_MS 340
#define FLASH_GROWTH 16

struct radar_scope {
    const struct radar_run *run;
    int radius;
    int centre;                 /* cx and cy are equal: the object is square */
    uint8_t motion;
    void (*tap)(void *user, int bearing, int range);
    void *user;
    uint8_t flash_kind;
    uint16_t flash_bearing;
    uint16_t flash_range;
    uint32_t flash_start;
};

static struct radar_scope *state_of(const lv_obj_t *scope)
{
    return scope ? lv_obj_get_user_data((lv_obj_t *)scope) : NULL;
}

/* Bearing 0 is the top of the scope; LVGL's 0 degrees is the 3 o'clock
 * position and both run clockwise, so the two differ by a quarter turn. */
static int screen_angle(int bearing_dd)
{
    return (radar_bearing_wrap(bearing_dd) / 10 + 270) % 360;
}

/* Polar to a pixel offset from the centre. The one place the conversion is
 * written, so the markers, the effects and the public accessor cannot
 * disagree about where a contact is. */
static void offset_for(const struct radar_scope *s, int bearing, int range,
                       int *dx, int *dy)
{
    int angle;
    int32_t reach;

    if (range < 0) {
        range = 0;
    }
    if (range > RADAR_RANGE_MAX) {
        range = RADAR_RANGE_MAX;
    }
    angle = screen_angle(bearing);
    reach = (int32_t)s->radius * range / RADAR_RANGE_MAX;
    *dx = (int)((lv_trigo_cos((int16_t)angle) * reach) >> LV_TRIGO_SHIFT);
    *dy = (int)((lv_trigo_sin((int16_t)angle) * reach) >> LV_TRIGO_SHIFT);
}

void radar_scope_point(const lv_obj_t *scope, int bearing, int range, int *dx, int *dy)
{
    const struct radar_scope *s = state_of(scope);

    if (!s || !dx || !dy) {
        return;
    }
    offset_for(s, bearing, range, dx, dy);
}

int radar_scope_polar(const lv_obj_t *scope, int dx, int dy, int *bearing, int *range)
{
    const struct radar_scope *s = state_of(scope);
    int32_t reach;
    int angle;

    if (!s || !bearing || !range || s->radius <= 0) {
        return -1;
    }
    reach = lv_sqrt32((uint32_t)(dx * dx + dy * dy));
    if (reach > s->radius) {
        return -1;
    }
    *range = (int)(reach * RADAR_RANGE_MAX / s->radius);
    if (dx == 0 && dy == 0) {
        *bearing = 0;
        return 0;
    }
    /* lv_atan2 measures from the 3 o'clock position the same way lv_trigo
     * does, so undoing the quarter turn is the whole of the inverse. */
    angle = (int)lv_atan2(dx, dy);
    *bearing = radar_bearing_wrap((angle + 90) * 10);
    return 0;
}

/* ---- drawing ----------------------------------------------------------- */

static void ring(lv_layer_t *layer, lv_point_t centre, int radius, lv_color_t color,
                 int width, lv_opa_t opa)
{
    lv_draw_arc_dsc_t dsc;

    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.radius = (uint16_t)radius;
    dsc.center = centre;
    dsc.start_angle = 0;
    dsc.end_angle = 360;
    dsc.opa = opa;
    lv_draw_arc(layer, &dsc);
}

static void radial(lv_layer_t *layer, lv_point_t centre, int angle, int from, int to,
                   lv_color_t color, int width, lv_opa_t opa)
{
    lv_draw_line_dsc_t dsc;
    int32_t c = lv_trigo_cos((int16_t)angle);
    int32_t s = lv_trigo_sin((int16_t)angle);

    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa = opa;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1.x = centre.x + ((c * from) >> LV_TRIGO_SHIFT);
    dsc.p1.y = centre.y + ((s * from) >> LV_TRIGO_SHIFT);
    dsc.p2.x = centre.x + ((c * to) >> LV_TRIGO_SHIFT);
    dsc.p2.y = centre.y + ((s * to) >> LV_TRIGO_SHIFT);
    lv_draw_line(layer, &dsc);
}

static void line_between(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                         lv_color_t color, int width)
{
    lv_draw_line_dsc_t dsc;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

static void box(lv_layer_t *layer, int cx, int cy, int half, int radius,
                lv_color_t color, int border)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t area;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    if (border > 0) {
        dsc.bg_opa = LV_OPA_TRANSP;
        dsc.border_color = color;
        dsc.border_width = border;
        dsc.border_opa = LV_OPA_COVER;
    } else {
        dsc.bg_opa = LV_OPA_COVER;
        dsc.bg_color = color;
    }
    area.x1 = cx - half;
    area.y1 = cy - half;
    area.x2 = cx + half;
    area.y2 = cy + half;
    lv_draw_rect(layer, &dsc, &area);
}

static void draw_face(lv_layer_t *layer, const struct radar_scope *s, lv_point_t centre)
{
    lv_color_t line_color = pos_theme_color(POS_COLOR_LINE);
    int i;

    for (i = 1; i <= RING_COUNT; i++) {
        ring(layer, centre, s->radius * i / RING_COUNT, line_color, 1, LV_OPA_COVER);
    }
    for (i = 0; i < TICK_COUNT; i++) {
        int angle = i * (360 / TICK_COUNT);
        int cardinal = (i % 3) == 0;
        int len = cardinal ? TICK_LEN_CARDINAL : TICK_LEN;

        radial(layer, centre, angle, s->radius - len, s->radius, line_color,
               cardinal ? 2 : 1, LV_OPA_COVER);
    }
    /* The hub is where the operator is. It is the only thing on the face
     * drawn in the accent, so the eye has one fixed point. */
    box(layer, centre.x, centre.y, HUB_RADIUS, LV_RADIUS_CIRCLE,
        pos_theme_color(POS_COLOR_ACCENT_PRIMARY), 0);
    ring(layer, centre, HUB_RADIUS + 5, pos_theme_color(POS_COLOR_ACCENT_PRIMARY), 1,
         LV_OPA_50);
}

static void draw_sweep(lv_layer_t *layer, const struct radar_scope *s, lv_point_t centre)
{
    lv_color_t color = pos_theme_color(POS_COLOR_RADIO_RX);
    int head = screen_angle(s->run->sweep);
    int tail;

    if (!s->motion) {
        /* Reduced motion keeps a bearing reference without animating it. */
        radial(layer, centre, screen_angle(0), 0, s->radius, color, 2, LV_OPA_30);
        return;
    }
    for (tail = SWEEP_TAIL; tail >= 0; tail--) {
        int angle = (head + 360 - tail * SWEEP_TAIL_DEG) % 360;

        radial(layer, centre, angle, 0, s->radius, color, tail == 0 ? 3 : 2,
               (lv_opa_t)(LV_OPA_60 / (tail + 1)));
    }
}

/* Four corner brackets around a marker: the selection cue that carries no
 * colour of its own, so it reads in any theme and in Night mode. */
static void draw_brackets(lv_layer_t *layer, int cx, int cy, lv_color_t color)
{
    int r = BRACKET_REACH;
    int a = BRACKET_ARM;

    line_between(layer, cx - r, cy - r, cx - r + a, cy - r, color, 2);
    line_between(layer, cx - r, cy - r, cx - r, cy - r + a, color, 2);
    line_between(layer, cx + r, cy - r, cx + r - a, cy - r, color, 2);
    line_between(layer, cx + r, cy - r, cx + r, cy - r + a, color, 2);
    line_between(layer, cx - r, cy + r, cx - r + a, cy + r, color, 2);
    line_between(layer, cx - r, cy + r, cx - r, cy + r - a, color, 2);
    line_between(layer, cx + r, cy + r, cx + r - a, cy + r, color, 2);
    line_between(layer, cx + r, cy + r, cx + r, cy + r - a, color, 2);
}

/* The shape a contact is drawn with, and the token it is drawn in. Shape is
 * the primary channel: the four identified classes are a circle, a triangle,
 * a crossed diamond and a square in a ring, which stay apart with no colour
 * at all (DS section 2). */
static void draw_marker(lv_layer_t *layer, const struct radar_contact *c, int cx, int cy)
{
    lv_color_t color;
    int r = MARK_RADIUS;

    if (!c->classified) {
        /* An unidentified return: hollow, in the sensor colour. */
        box(layer, cx, cy, r, LV_RADIUS_CIRCLE, pos_theme_color(POS_COLOR_RADIO_RX),
            MARK_STROKE);
        return;
    }
    switch ((enum radar_class)c->cls) {
    case RADAR_CLASS_FAST:
        /* A triangle pointing at the hub: it is the one closing fastest. */
        color = pos_theme_color(POS_COLOR_RADIO_RX);
        line_between(layer, cx, cy + r, cx - r, cy - r, color, MARK_STROKE + 1);
        line_between(layer, cx, cy + r, cx + r, cy - r, color, MARK_STROKE + 1);
        line_between(layer, cx - r, cy - r, cx + r, cy - r, color, MARK_STROKE + 1);
        break;
    case RADAR_CLASS_DECOY:
        /* A crossed diamond. The cross is the part that says "leave it". */
        color = pos_theme_color(POS_COLOR_STATUS_WARN);
        line_between(layer, cx, cy - r, cx + r, cy, color, MARK_STROKE);
        line_between(layer, cx + r, cy, cx, cy + r, color, MARK_STROKE);
        line_between(layer, cx, cy + r, cx - r, cy, color, MARK_STROKE);
        line_between(layer, cx - r, cy, cx, cy - r, color, MARK_STROKE);
        line_between(layer, cx - r * 2 / 3, cy - r * 2 / 3, cx + r * 2 / 3,
                     cy + r * 2 / 3, color, MARK_STROKE);
        line_between(layer, cx + r * 2 / 3, cy - r * 2 / 3, cx - r * 2 / 3,
                     cy + r * 2 / 3, color, MARK_STROKE);
        break;
    case RADAR_CLASS_HIGH_VALUE:
        /* A square inside a ring: the only marker with two enclosures. */
        color = pos_theme_color(POS_COLOR_RADIO_TX);
        box(layer, cx, cy, r - 4, 2, color, 0);
        ring(layer, (lv_point_t){ cx, cy }, r + 2, color, MARK_STROKE, LV_OPA_COVER);
        break;
    case RADAR_CLASS_NORMAL:
    default:
        color = pos_theme_color(POS_COLOR_RADIO_RX);
        box(layer, cx, cy, r - 3, LV_RADIUS_CIRCLE, color, 0);
        break;
    }
}

static void draw_contact(lv_layer_t *layer, const struct radar_scope *s,
                         lv_point_t centre, const struct radar_contact *c)
{
    int dx;
    int dy;
    int cx;
    int cy;

    offset_for(s, c->bearing, c->range, &dx, &dy);
    cx = centre.x + dx;
    cy = centre.y + dy;

    draw_marker(layer, c, cx, cy);

    if (c->state == RADAR_CONTACT_NEW) {
        return;
    }
    draw_brackets(layer, cx, cy, pos_theme_color(POS_COLOR_ACCENT_PRIMARY));
    if (c->state == RADAR_CONTACT_ACQUIRED) {
        /* A completed lock is drawn in the transmit token rather than the
         * accent. Acquiring is something the sensor is doing and takes the
         * accent; being acquired means a shot is armed, which is something
         * about to go out. It also keeps the two apart by colour as well as
         * by a partial ring against a whole one, and it holds the "orange
         * means engage" reading in themes where the accent is itself the
         * sensor colour. */
        ring(layer, (lv_point_t){ cx, cy }, LOCK_RING_RADIUS,
             pos_theme_color(POS_COLOR_RADIO_TX), LOCK_RING_WIDTH, LV_OPA_COVER);
        return;
    }
    /* Acquisition in progress: the ring fills clockwise from the top, so how
     * far round it has gone is how close the lock is. */
    {
        lv_draw_arc_dsc_t dsc;
        int done = radar_contact_lock_permille(c) * 360 / 1000;

        lv_draw_arc_dsc_init(&dsc);
        dsc.color = pos_theme_color(POS_COLOR_ACCENT_PRIMARY);
        dsc.width = LOCK_RING_WIDTH;
        dsc.radius = LOCK_RING_RADIUS;
        dsc.center.x = cx;
        dsc.center.y = cy;
        dsc.start_angle = 270;
        dsc.end_angle = 270 + done;
        dsc.opa = LV_OPA_COVER;
        dsc.rounded = 1;
        if (done > 0) {
            lv_draw_arc(layer, &dsc);
        }
    }
}

static void draw_flash(lv_layer_t *layer, const struct radar_scope *s, lv_point_t centre)
{
    uint32_t elapsed = lv_tick_elaps(s->flash_start);
    lv_color_t color;
    int32_t grow;
    int dx;
    int dy;
    int cx;
    int cy;

    if (elapsed >= FLASH_MS) {
        return;
    }
    offset_for(s, s->flash_bearing, s->flash_range, &dx, &dy);
    cx = centre.x + dx;
    cy = centre.y + dy;

    switch ((enum radar_scope_flash)s->flash_kind) {
    case RADAR_FLASH_FOUL:
        color = pos_theme_color(POS_COLOR_STATUS_WARN);
        break;
    case RADAR_FLASH_FADED:
        color = pos_theme_color(POS_COLOR_TEXT_SECONDARY);
        break;
    case RADAR_FLASH_HIT:
    default:
        color = pos_theme_color(POS_COLOR_RADIO_TX);
        break;
    }
    /* A hit or a foul bursts outwards; a lost track closes inwards, so the
     * two endings of a contact are told apart by direction as well as hue. */
    if (s->flash_kind == RADAR_FLASH_FADED) {
        grow = (int32_t)(FLASH_GROWTH - FLASH_GROWTH * (int32_t)elapsed / FLASH_MS);
    } else {
        grow = (int32_t)(FLASH_GROWTH * (int32_t)elapsed / FLASH_MS);
    }
    ring(layer, (lv_point_t){ cx, cy }, MARK_RADIUS + 4 + (int)grow, color, 2,
         (lv_opa_t)(LV_OPA_COVER - LV_OPA_COVER * elapsed / FLASH_MS));
}

static void scope_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct radar_scope *s = state_of(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_point_t centre;
    int i;

    if (!s || !layer) {
        return;
    }
    lv_obj_get_coords(obj, &coords);
    centre.x = coords.x1 + s->centre;
    centre.y = coords.y1 + s->centre;

    draw_face(layer, s, centre);
    if (s->run) {
        if (s->run->state == RADAR_RUN_ACTIVE) {
            draw_sweep(layer, s, centre);
        }
        for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
            const struct radar_contact *c = radar_run_slot(s->run, i);

            if (c && c->active) {
                draw_contact(layer, s, centre, c);
            }
        }
    }
    if (s->flash_kind != RADAR_FLASH_NONE && s->motion) {
        draw_flash(layer, s, centre);
    }
}

static void scope_click(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct radar_scope *s = state_of(obj);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;
    lv_area_t coords;
    int bearing;
    int range;

    if (!s || !s->tap || !indev) {
        return;
    }
    lv_indev_get_point(indev, &point);
    lv_obj_get_coords(obj, &coords);
    if (radar_scope_polar(obj, point.x - coords.x1 - s->centre,
                          point.y - coords.y1 - s->centre, &bearing, &range) != 0) {
        return;
    }
    s->tap(s->user, bearing, range);
}

static void scope_delete(lv_event_t *e)
{
    free(state_of(lv_event_get_target_obj(e)));
}

/* Fallback for when the theme engine's watch table is full. */
static void scope_theme_changed(lv_event_t *e)
{
    lv_obj_invalidate(lv_event_get_target_obj(e));
}

lv_obj_t *radar_scope_create(lv_obj_t *parent, int size)
{
    lv_obj_t *obj = lv_obj_create(parent);
    struct radar_scope *s = calloc(1, sizeof(*s));

    if (!s) {
        lv_obj_delete(obj);
        return NULL;
    }
    s->centre = size / 2;
    s->radius = size / 2 - RIM_INSET;
    s->motion = 1;

    lv_obj_remove_style_all(obj);
    /* Only so a caption font is available should the scope ever label
     * anything; no colour or font is named here (tests/style_lint.sh). */
    pos_style_add(obj, POS_STYLE_CAPTION, 0);
    lv_obj_set_user_data(obj, s);
    lv_obj_set_size(obj, size, size);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, scope_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, scope_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(obj, scope_delete, LV_EVENT_DELETE, NULL);
    if (pos_theme_watch(obj) != 0) {
        lv_obj_add_event_cb(obj, scope_theme_changed,
                            (lv_event_code_t)pos_event_theme_changed(), NULL);
    }
    return obj;
}

void radar_scope_bind(lv_obj_t *scope, const struct radar_run *run)
{
    struct radar_scope *s = state_of(scope);

    if (s) {
        s->run = run;
        lv_obj_invalidate(scope);
    }
}

void radar_scope_set_motion(lv_obj_t *scope, int enabled)
{
    struct radar_scope *s = state_of(scope);

    if (s) {
        s->motion = (uint8_t)(enabled != 0);
        lv_obj_invalidate(scope);
    }
}

void radar_scope_set_tap(lv_obj_t *scope, void (*cb)(void *user, int bearing, int range),
                         void *user)
{
    struct radar_scope *s = state_of(scope);

    if (s) {
        s->tap = cb;
        s->user = user;
    }
}

void radar_scope_flash(lv_obj_t *scope, enum radar_scope_flash kind, int bearing,
                       int range)
{
    struct radar_scope *s = state_of(scope);

    if (!s) {
        return;
    }
    s->flash_kind = (uint8_t)kind;
    s->flash_bearing = (uint16_t)radar_bearing_wrap(bearing);
    s->flash_range = (uint16_t)range;
    s->flash_start = lv_tick_get();
}
