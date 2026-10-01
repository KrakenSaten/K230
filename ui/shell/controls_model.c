/*
 * DOORS Controls, the decisions. See controls_model.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "controls_model.h"

#include <stdio.h>
#include <string.h>

/* ---- radio ----------------------------------------------------------------- */

static bool is(const char *s, const char *w)
{
    return s && strcmp(s, w) == 0;
}

const char *controls_radio_text(const char *state)
{
    if (!state) {
        return "Not answering";
    }
    if (is(state, "off")) {
        return "Off";
    }
    if (is(state, "rx")) {
        return "Receiving";
    }
    if (is(state, "tx")) {
        return "Transmitting";
    }
    if (is(state, "error")) {
        return "On, not receiving";
    }
    return "Idle";
}

bool controls_radio_dot(const char *state)
{
    return is(state, "rx") || is(state, "tx");
}

bool controls_radio_on(const char *state)
{
    return state && !is(state, "off");
}

enum controls_radio_tap controls_radio_tapped(struct controls_radio_flow *flow, const char *state)
{
    if (flow->confirm != CONTROLS_CONFIRM_NONE || !state) {
        return CONTROLS_TAP_NOTHING;
    }
    if (!controls_radio_on(state)) {
        flow->confirm = CONTROLS_CONFIRM_ANTENNA;
        return CONTROLS_TAP_ASK_ANTENNA;
    }
    return CONTROLS_TAP_SWITCH_OFF;
}

bool controls_radio_answer(struct controls_radio_flow *flow, bool enable)
{
    bool was_open = flow->confirm == CONTROLS_CONFIRM_ANTENNA;

    flow->confirm = CONTROLS_CONFIRM_NONE;
    return was_open && enable;
}

void controls_radio_dismiss(struct controls_radio_flow *flow)
{
    flow->confirm = CONTROLS_CONFIRM_NONE;
}

/* ---- sysd's answers --------------------------------------------------------- */

static const cJSON *obj(const cJSON *o, const char *key)
{
    return cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
}

static const char *word(const cJSON *o, const char *key)
{
    const cJSON *v = obj(o, key);

    return cJSON_IsString(v) ? v->valuestring : NULL;
}

void controls_bluetooth_text(const cJSON *status, char *out, int len)
{
    const cJSON *ctl = obj(obj(status, "bluetooth"), "controllers");

    if (!status) {
        snprintf(out, (size_t)len, "Not answering");
    } else if (!cJSON_IsArray(ctl) || cJSON_GetArraySize(ctl) == 0) {
        snprintf(out, (size_t)len, "Not available");
    } else {
        /* A controller the kernel knows about, which nothing in Doors owns
         * or switches yet: said as that, not as on or off. */
        snprintf(out, (size_t)len, "Controller present");
    }
}

bool controls_bluetooth_available(const cJSON *status)
{
    const cJSON *ctl = obj(obj(status, "bluetooth"), "controllers");

    return cJSON_IsArray(ctl) && cJSON_GetArraySize(ctl) > 0;
}

static const char *battery_word(const char *st)
{
    if (is(st, "charging")) {
        return "Charging";
    }
    if (is(st, "discharging")) {
        return "On battery";
    }
    if (is(st, "full")) {
        return "Full";
    }
    if (is(st, "not_charging")) {
        return "Not charging";
    }
    return NULL;
}

void controls_battery_text(const cJSON *status, char *out, int len)
{
    const cJSON *power = obj(status, "power");
    const cJSON *bat = obj(power, "battery");
    const cJSON *present;
    const cJSON *cap;
    const char *w;

    if (!status) {
        snprintf(out, (size_t)len, "Not answering");
        return;
    }
    if (!cJSON_IsObject(power)) {
        snprintf(out, (size_t)len, "Unknown");
        return;
    }
    if (!cJSON_IsObject(bat)) {
        /* No gauge: what sysd says about the source is all there is. */
        snprintf(out, (size_t)len, "%s",
                 is(word(power, "source"), "external") ? "External power" : "Unknown");
        return;
    }
    present = obj(bat, "present");
    if (cJSON_IsFalse(present)) {
        snprintf(out, (size_t)len, "No battery");
        return;
    }
    cap = obj(bat, "capacity_percent");
    w = battery_word(word(bat, "status"));
    /* The gauge's own figure, whole, or nothing: sysd has already refused
     * anything outside 0..100. */
    if (cJSON_IsNumber(cap) && cap->valuedouble >= 0 && cap->valuedouble <= 100) {
        if (w) {
            snprintf(out, (size_t)len, "%d %% \xc2\xb7 %s", (int)cap->valuedouble, w);
        } else {
            snprintf(out, (size_t)len, "%d %%", (int)cap->valuedouble);
        }
    } else {
        snprintf(out, (size_t)len, "%s", w ? w : "Battery");
    }
}

bool controls_battery_dot(const cJSON *status)
{
    const cJSON *power = obj(status, "power");
    const cJSON *bat = obj(power, "battery");

    if (!cJSON_IsObject(bat) || cJSON_IsFalse(obj(bat, "present"))) {
        return false;
    }
    return is(word(bat, "status"), "charging") || is(word(power, "source"), "external");
}

/* ---- volume ------------------------------------------------------------- */

void controls_volume_text(bool available, bool muted, int percent, char *out, int len)
{
    if (!available) {
        snprintf(out, (size_t)len, "Not available");
    } else if (muted) {
        snprintf(out, (size_t)len, "Muted");
    } else {
        snprintf(out, (size_t)len, "%d %%", percent);
    }
}

/* ---- layout ------------------------------------------------------------- */

#define GAP 16
#define MARGIN 36
#define TOP_ROW_Y 8

static struct controls_rect rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct controls_rect r = { x, y, w, h };

    return r;
}

bool controls_rects_overlap(const struct controls_rect *a, const struct controls_rect *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static int32_t max32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

int controls_layout(const struct controls_frame *f, struct controls_layout *out)
{
    const bool landscape = f->landscape;
    const int32_t width = f->width;
    const int32_t height = f->height;
    /* The side margin, raised to the top corners where they reach further
     * (50 px at the top in landscape on the T-Display K230, DS §21.1): the
     * header row starts at the top edge, and the columns below keep in line
     * with it. */
    const int32_t m = max32(MARGIN, max32(f->inset_top_left, f->inset_top_right));
    /* The header row ends short of the status cluster (DS §36), or of the
     * right margin where there is none. */
    int32_t row_end = width - m;
    int32_t col;
    int32_t tw;
    int32_t th;
    int32_t y0;
    int32_t panel_h;
    int32_t i;
    int rc = 0;

    memset(out, 0, sizeof(*out));
    out->margin = m;
    /* The header row's buttons centred on row 36, as the app header's back
     * slab is (8 + 56 / 2), so they line up with the status cluster beside
     * them (DS §36.1). */
    out->back = rect(m, TOP_ROW_Y, 72, 56);
    if (f->keepout.w > 0 && f->keepout.y < 12 + 92 && f->keepout.x - GAP < row_end) {
        row_end = f->keepout.x - GAP;
    }
    if (landscape) {
        /* Three rows of tiles on the left and three panels on the right
         * leave no room at the foot for Lock and Power, so in landscape they
         * sit at the right of the header row, where the width is - left of
         * the status cluster. */
        int32_t rx;
        int32_t rw;

        col = 520;
        th = 112;
        y0 = 104;
        panel_h = 88;
        out->list_row_h = 56;
        tw = (col - GAP) / 2;
        out->power = rect(row_end - 160, TOP_ROW_Y, 160, 56);
        out->lock = rect(out->power.x - GAP - 160, TOP_ROW_Y, 160, 56);
        out->header = rect(m + 92, 12, out->lock.x - GAP - (m + 92), 80);
        rx = m + col + 32;
        rw = width - m - rx;
        out->brightness = rect(rx, y0, rw, panel_h);
        out->volume = rect(rx, y0 + panel_h + GAP, rw, panel_h);
        out->list = rect(rx, out->volume.y + panel_h + GAP, rw, 3 * out->list_row_h);
    } else {
        int32_t py;
        int32_t foot;

        col = width - 2 * m;
        th = 120;
        y0 = 120;
        panel_h = 104;
        out->list_row_h = 64;
        tw = (col - GAP) / 2;
        out->header = rect(m + 92, 12, row_end - (m + 92), 92);
        py = y0 + 3 * (th + GAP);
        out->brightness = rect(m, py, col, panel_h);
        out->volume = rect(m, py + panel_h + GAP, col, panel_h);
        out->list = rect(m, out->volume.y + panel_h + GAP, col, 3 * out->list_row_h);
        foot = height - 48 - 64;
        out->lock = rect(m, foot, tw, 64);
        out->power = rect(m + tw + GAP, foot, tw, 64);
        if (out->list.y + out->list.h + GAP > foot) {
            rc = -1;
        }
    }
    for (i = 0; i < CONTROLS_TILE_COUNT; i++) {
        out->tile[i] = rect(m + (i % 2) * (tw + GAP), y0 + (i / 2) * (th + GAP), tw, th);
    }
    {
        int32_t dw = width - 2 * m < 520 ? width - 2 * m : 520;
        int32_t dh = 300;

        out->dialog = rect((width - dw) / 2, (height - dh) / 2, dw, dh);
    }
    /* Everything inside the content area, with the foot clear of the rounded
     * corners (30 px, platform.h) and a margin under the lowest panel. */
    {
        const struct controls_rect *all[] = { &out->back, &out->header, &out->brightness,
                                              &out->volume, &out->list, &out->lock, &out->power,
                                              &out->tile[0], &out->tile[1], &out->tile[2],
                                              &out->tile[3], &out->tile[4], &out->tile[5] };
        size_t a;
        size_t b;

        for (a = 0; a < sizeof(all) / sizeof(all[0]); a++) {
            if (all[a]->x < 0 || all[a]->y < 0 || all[a]->x + all[a]->w > width ||
                all[a]->y + all[a]->h > height - 24 || all[a]->w <= 0) {
                rc = -1;
            }
            if (f->keepout.w > 0 && controls_rects_overlap(all[a], &f->keepout)) {
                rc = -1;
            }
            for (b = a + 1; b < sizeof(all) / sizeof(all[0]); b++) {
                if (controls_rects_overlap(all[a], all[b])) {
                    rc = -1;
                }
            }
        }
    }
    return rc;
}
