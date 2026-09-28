/*
 * The DOORS launcher's geometry. See home_layout.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "home_layout.h"

#include <string.h>

#define MARGIN_PORTRAIT 28
#define MARGIN_LANDSCAPE 36
/* The top of the header band: level with the top of the status cluster (DS
 * §36, POCKETOS_CHROME_CLUSTER_Y), which now shares the top edge with it. */
#define HEADER_Y 14
#define HEADER_H_PORTRAIT 116
#define HEADER_H_LANDSCAPE 100
/* The date's line under the time, from the top of the header band. */
#define DATE_Y_PORTRAIT 78
#define DATE_Y_LANDSCAPE 70
#define DATE_H 28
#define BUTTON_W_LANDSCAPE 240
#define HEADER_CLEAR 16         /* between the header band and the status cluster */
/* Under the footer: clear of a 30 px rounded corner with 12 px to spare, and
 * the same on a rectangular panel, so the launcher above it does not move
 * with the corners (a larger bench corner pushes it up, footer_pad()). */
#define FOOTER_PAD 42

static int32_t max32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

static struct home_rect rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct home_rect r = { x, y, w, h };

    return r;
}

static int32_t panel_height(int rows)
{
    return HOME_PANEL_HEAD + HOME_PANEL_PAD + rows * HOME_CELL_H + (rows - 1) * HOME_PANEL_PAD +
           HOME_PANEL_BOTTOM;
}

/* One panel at (x, y), its n cells in rows of cols; returns its height. */
static int32_t place_panel(struct home_layout *out, int g, uint16_t *next, int n, int cols, int32_t x,
                           int32_t y, int32_t w, int32_t cw)
{
    int rows = (n + cols - 1) / cols;
    int i;

    out->panel[g] = rect(x, y, w, panel_height(rows));
    for (i = 0; i < n; i++) {
        out->cell[(*next)++] = rect(x + HOME_PANEL_PAD + (i % cols) * cw,
                                    y + HOME_PANEL_HEAD + HOME_PANEL_PAD + (i / cols) * (HOME_CELL_H + HOME_PANEL_PAD),
                                    cw, HOME_CELL_H);
    }
    return out->panel[g].h;
}

static int32_t footer_pad(const struct home_layout_in *in)
{
    return max32(FOOTER_PAD, in->inset_bottom + 12);
}

/* The header band (the time and the date) at (x, y, w, h), narrowed on both
 * sides until it is clear of the status cluster (DS §36): the cluster sits in
 * the top-right corner, and narrowing only the right side would push the
 * time off the centre line the panels are laid out on. */
static struct home_rect header_band(const struct home_layout_in *in, int32_t x, int32_t y, int32_t w,
                                    int32_t h)
{
    const struct home_rect *k = &in->keepout;
    int32_t right = x + w;

    if (k->w > 0 && k->h > 0 && k->y < y + h && y < k->y + k->h && k->x - HEADER_CLEAR < right) {
        right = k->x - HEADER_CLEAR;
        x = max32(x, in->width - right);
        w = max32(right - x, 0);
    }
    return rect(x, y, w, h);
}

static void portrait(const struct home_layout_in *in, struct home_layout *out)
{
    int32_t m = MARGIN_PORTRAIT;
    int32_t pw = in->width - 2 * m;
    int32_t inner = pw - 2 * HOME_PANEL_PAD;
    int cols = HOME_PORTRAIT_COLUMNS;
    int32_t cw;
    int32_t y;
    int32_t end;
    int32_t by;
    int32_t bw;
    uint16_t next = 0;
    int g;

    while (cols > 1 && inner / cols < HOME_CELL_MIN_W) {
        cols--;
    }
    cw = inner / cols;
    if (cw > HOME_CELL_W_PORTRAIT) {
        cw = HOME_CELL_W_PORTRAIT;
    }
    out->cell_w = cw;
    out->small_labels = cw < 110;
    out->header = header_band(in, m, HEADER_Y, pw, HEADER_H_PORTRAIT);
    out->date = rect(m, HEADER_Y + DATE_Y_PORTRAIT, pw, DATE_H);
    y = HEADER_Y + HEADER_H_PORTRAIT + 12;
    end = y;
    for (g = 0; g < in->ngroups; g++) {
        if (!in->count[g]) {
            continue;
        }
        y += place_panel(out, g, &next, in->count[g], cols, m, y, pw, cw) + HOME_PANEL_GAP;
        end = y - HOME_PANEL_GAP;
    }
    out->napps = next;
    by = max32(in->height - footer_pad(in) - HOME_BUTTON_H, end + 2 * HOME_PANEL_GAP);
    bw = (pw - 16) / 2;
    out->lock_button = rect(m, by, bw, HOME_BUTTON_H);
    out->controls_button = rect(m + pw - bw, by, bw, HOME_BUTTON_H);
    out->content_h = max32(in->height, by + HOME_BUTTON_H + footer_pad(in));
}

static void landscape(const struct home_layout_in *in, struct home_layout *out)
{
    int32_t m = MARGIN_LANDSCAPE;
    int32_t line = in->width - 2 * m;
    int groups = 0;
    int napps = 0;
    int32_t cw;
    int32_t y;
    int32_t end;
    int32_t by;
    uint16_t next = 0;
    int g;

    for (g = 0; g < in->ngroups; g++) {
        if (in->count[g]) {
            groups++;
            napps += in->count[g];
        }
    }
    out->header = header_band(in, m, HEADER_Y, line, HEADER_H_LANDSCAPE);
    out->date = rect(m, HEADER_Y + DATE_Y_LANDSCAPE, line, DATE_H);
    y = HEADER_Y + HEADER_H_LANDSCAPE + 4;
    end = y;
    cw = napps ? (line - (groups - 1) * HOME_PANEL_GAP - groups * 2 * HOME_PANEL_PAD) / napps : 0;
    if (cw > HOME_CELL_W_PORTRAIT) {
        cw = HOME_CELL_W_PORTRAIT;
    }
    if (napps && cw >= HOME_CELL_MIN_W) {
        /* One row, centred. */
        int32_t total = napps * cw + groups * 2 * HOME_PANEL_PAD + (groups - 1) * HOME_PANEL_GAP;
        int32_t x = (in->width - total) / 2;

        for (g = 0; g < in->ngroups; g++) {
            int32_t w;

            if (!in->count[g]) {
                continue;
            }
            w = in->count[g] * cw + 2 * HOME_PANEL_PAD;
            place_panel(out, g, &next, in->count[g], in->count[g], x, y, w, cw);
            x += w + HOME_PANEL_GAP;
        }
        end = y + panel_height(1);
    } else if (napps) {
        /* Wrapped: lines of panels, each line centred; a group wider than a
         * line wraps its own cells. */
        int32_t lx = 0;
        int32_t lh = 0;
        int line_first = 0;

        cw = HOME_CELL_WRAP_W;
        out->wrapped = true;
        for (g = 0; g <= in->ngroups; g++) {
            int cols;
            int32_t w;

            if (g < in->ngroups && !in->count[g]) {
                continue;
            }
            if (g < in->ngroups) {
                cols = in->count[g];
                if (cols * cw + 2 * HOME_PANEL_PAD > line) {
                    cols = (int)((line - 2 * HOME_PANEL_PAD) / cw);
                }
                w = cols * cw + 2 * HOME_PANEL_PAD;
            } else {
                cols = 0;
                w = 0;
            }
            /* Close the line when this panel does not fit on it (or at the end). */
            if (g == in->ngroups || (lx > 0 && lx + HOME_PANEL_GAP + w > line)) {
                int32_t shift = m + (line - lx) / 2 - m;
                int k;

                for (k = line_first; k < g; k++) {
                    int c;
                    int first = 0;

                    if (!in->count[k]) {
                        continue;
                    }
                    for (c = 0; c < k; c++) {
                        first += in->count[c];
                    }
                    out->panel[k].x += shift;
                    for (c = 0; c < in->count[k]; c++) {
                        out->cell[first + c].x += shift;
                    }
                }
                y += lh + HOME_PANEL_GAP;
                lx = 0;
                lh = 0;
                line_first = g;
                if (g == in->ngroups) {
                    break;
                }
            }
            lh = max32(lh, place_panel(out, g, &next, in->count[g], cols, m + (lx ? lx + HOME_PANEL_GAP : 0),
                                       y, w, cw));
            lx += (lx ? HOME_PANEL_GAP : 0) + w;
        }
        end = y - HOME_PANEL_GAP;
    }
    out->cell_w = cw;
    out->small_labels = cw < 110;
    out->napps = next;
    by = max32(in->height - footer_pad(in) - HOME_BUTTON_H, end + 2 * HOME_PANEL_GAP);
    out->lock_button = rect(max32(m, in->inset_left + 12), by, BUTTON_W_LANDSCAPE, HOME_BUTTON_H);
    out->controls_button = rect(in->width - max32(m, in->inset_right + 12) - BUTTON_W_LANDSCAPE, by,
                                BUTTON_W_LANDSCAPE, HOME_BUTTON_H);
    out->content_h = max32(in->height, by + HOME_BUTTON_H + footer_pad(in));
}

int home_layout_compute(const struct home_layout_in *in, struct home_layout *out)
{
    int total = 0;
    int g;

    if (!in || !out || in->ngroups > HOME_MAX_GROUPS || in->width < HOME_CELL_MIN_W + 2 * 48 ||
        in->height < HOME_CELL_H) {
        return -1;
    }
    for (g = 0; g < in->ngroups; g++) {
        total += in->count[g];
    }
    if (total > HOME_MAX_APPS) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (in->landscape) {
        landscape(in, out);
    } else {
        portrait(in, out);
    }
    return 0;
}

/* ---- the groups ------------------------------------------------------------ */

/* Launcher order within a group is this table's order. Colours are the
 * package's (tools/design/gen_doors_ui.py draws each icon in the same one;
 * tests/doors_ui_assets_test.sh holds the two tables together). */
static const struct home_entry entries[] = {
    { "rift", HOME_GROUP_CONNECT, HOME_HUE_MESH },
    { "radio", HOME_GROUP_CONNECT, HOME_HUE_RADIO },
    { "wave", HOME_GROUP_CONNECT, HOME_HUE_NETWORK },
    { "zabbix", HOME_GROUP_CONNECT, HOME_HUE_TOOLS },
    { "browser", HOME_GROUP_CONNECT, HOME_HUE_NETWORK },
    { "notes", HOME_GROUP_WORK, HOME_HUE_FILES },
    { "calendar", HOME_GROUP_WORK, HOME_HUE_TOOLS },
    { "clock", HOME_GROUP_WORK, HOME_HUE_AI },
    { "calculator", HOME_GROUP_WORK, HOME_HUE_APPS },
    { "fleet", HOME_GROUP_PLAY, HOME_HUE_GAMES },
    { "radar", HOME_GROUP_PLAY, HOME_HUE_RADIO },
    { "timber", HOME_GROUP_PLAY, HOME_HUE_FILES },
    { "settings", HOME_GROUP_DEVICE, HOME_HUE_SETTINGS },
    { "system", HOME_GROUP_DEVICE, HOME_HUE_APPS },
    { "files", HOME_GROUP_DEVICE, HOME_HUE_FILES },
    { "camera", HOME_GROUP_DEVICE, HOME_HUE_TOOLS },
    { "recorder", HOME_GROUP_DEVICE, HOME_HUE_TOOLS },
    { "vision", HOME_GROUP_DEVICE, HOME_HUE_AI },
};

const char *home_group_name(enum home_group g)
{
    static const char *const names[HOME_GROUP_COUNT] = { "CONNECTIONS", "WORKSPACE", "PLAY", "DEVICE",
                                                         "MORE" };

    return (g >= 0 && g < HOME_GROUP_COUNT) ? names[g] : "MORE";
}

const struct home_entry *home_entry_find(const char *id)
{
    size_t k;

    for (k = 0; id && k < sizeof(entries) / sizeof(entries[0]); k++) {
        if (strcmp(entries[k].id, id) == 0) {
            return &entries[k];
        }
    }
    return NULL;
}

int home_group_order(const char *const *ids, int n, uint8_t order[HOME_MAX_APPS],
                     uint8_t count[HOME_GROUP_COUNT])
{
    int placed = 0;
    int g;
    size_t k;
    int i;

    memset(count, 0, HOME_GROUP_COUNT);
    for (g = 0; g < HOME_GROUP_MORE; g++) {
        for (k = 0; k < sizeof(entries) / sizeof(entries[0]); k++) {
            if ((int)entries[k].group != g) {
                continue;
            }
            for (i = 0; i < n && placed < HOME_MAX_APPS; i++) {
                if (ids[i] && strcmp(ids[i], entries[k].id) == 0) {
                    order[placed++] = (uint8_t)i;
                    count[g]++;
                    break;
                }
            }
        }
    }
    for (i = 0; i < n && placed < HOME_MAX_APPS; i++) {
        if (!home_entry_find(ids[i])) {
            order[placed++] = (uint8_t)i;
            count[HOME_GROUP_MORE]++;
        }
    }
    return placed;
}
