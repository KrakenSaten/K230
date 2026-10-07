/*
 * The DOORS launcher's geometry. See home_layout.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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
 * tests/doors_ui_assets_test.sh holds the two tables together). The last
 * column is the folder an app is shown in (home_layout.h). DS §47:
 * ESSENTIALS holds Terminal, RIFT, Browser and Settings, one tap from home;
 * every other app is in FOLDERS, inside a folder, and the folders' cells
 * come in the order of their first rows here - Apps, Utilities, Games.
 * APPS is alphabetical by name; UTILITIES keeps the order it had (DS §42.2:
 * Clock, Calendar, Calculator, Notes, Files, Recorder, Camera) and GAMES the
 * order it had (§39). Radio, which no group named after the change, is in
 * APPS. System is a page of Settings (shell.c app_pages): HOME_GROUP_NONE,
 * no place on the launcher; its row keeps its colour for a favorite that
 * still holds it. */
static const struct home_entry entries[] = {
    { "terminal", HOME_GROUP_ESSENTIALS, HOME_HUE_TOOLS, HOME_FOLDER_NONE },
    { "rift", HOME_GROUP_ESSENTIALS, HOME_HUE_MESH, HOME_FOLDER_NONE },
    { "browser", HOME_GROUP_ESSENTIALS, HOME_HUE_NETWORK, HOME_FOLDER_NONE },
    { "settings", HOME_GROUP_ESSENTIALS, HOME_HUE_SETTINGS, HOME_FOLDER_NONE },
    { "deskbuddy", HOME_GROUP_FOLDERS, HOME_HUE_AI, HOME_FOLDER_APPS },
    { "mp3", HOME_GROUP_FOLDERS, HOME_HUE_APPS, HOME_FOLDER_APPS },
    { "photo", HOME_GROUP_FOLDERS, HOME_HUE_FILES, HOME_FOLDER_APPS },
    { "radio", HOME_GROUP_FOLDERS, HOME_HUE_RADIO, HOME_FOLDER_APPS },
    { "video", HOME_GROUP_FOLDERS, HOME_HUE_TOOLS, HOME_FOLDER_APPS },
    { "vision", HOME_GROUP_FOLDERS, HOME_HUE_AI, HOME_FOLDER_APPS },
    { "wave", HOME_GROUP_FOLDERS, HOME_HUE_NETWORK, HOME_FOLDER_APPS },
    { "zabbix", HOME_GROUP_FOLDERS, HOME_HUE_TOOLS, HOME_FOLDER_APPS },
    { "clock", HOME_GROUP_FOLDERS, HOME_HUE_AI, HOME_FOLDER_UTILITIES },
    { "calendar", HOME_GROUP_FOLDERS, HOME_HUE_TOOLS, HOME_FOLDER_UTILITIES },
    { "calculator", HOME_GROUP_FOLDERS, HOME_HUE_APPS, HOME_FOLDER_UTILITIES },
    { "notes", HOME_GROUP_FOLDERS, HOME_HUE_FILES, HOME_FOLDER_UTILITIES },
    { "files", HOME_GROUP_FOLDERS, HOME_HUE_FILES, HOME_FOLDER_UTILITIES },
    { "recorder", HOME_GROUP_FOLDERS, HOME_HUE_TOOLS, HOME_FOLDER_UTILITIES },
    { "camera", HOME_GROUP_FOLDERS, HOME_HUE_TOOLS, HOME_FOLDER_UTILITIES },
    { "fleet", HOME_GROUP_FOLDERS, HOME_HUE_GAMES, HOME_FOLDER_GAMES },
    { "radar", HOME_GROUP_FOLDERS, HOME_HUE_RADIO, HOME_FOLDER_GAMES },
    { "timber", HOME_GROUP_FOLDERS, HOME_HUE_FILES, HOME_FOLDER_GAMES },
    { "solitaire", HOME_GROUP_FOLDERS, HOME_HUE_GAMES, HOME_FOLDER_GAMES },
    { "blackjack", HOME_GROUP_FOLDERS, HOME_HUE_GAMES, HOME_FOLDER_GAMES },
    { "2048", HOME_GROUP_FOLDERS, HOME_HUE_GAMES, HOME_FOLDER_GAMES },
    { "system", HOME_GROUP_NONE, HOME_HUE_APPS, HOME_FOLDER_NONE },
/* A test seam: tests/home_folder_test.c builds this file with more rows
 * (tests/home_folder_entries.h), to fill a folder past one screen. No shell
 * build defines it. */
#ifdef HOME_LAYOUT_TEST_ENTRIES_FILE
#include HOME_LAYOUT_TEST_ENTRIES_FILE
#endif
};

const char *home_group_name(enum home_group g)
{
    static const char *const names[HOME_GROUP_COUNT] = { "ESSENTIALS", "FOLDERS", "MORE" };

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

/* ---- the folders ----------------------------------------------------------- */

/* One row per folder, in enum home_folder order; the art for a folder's cell
 * is icon-<id> (tools/design/gen_doors_ui.py, in the same colour). */
static const struct home_folder_def folders[HOME_FOLDER_COUNT] = {
    [HOME_FOLDER_NONE] = { NULL, NULL, HOME_HUE_APPS },
    [HOME_FOLDER_GAMES] = { "games", "Games", HOME_HUE_GAMES },
    [HOME_FOLDER_UTILITIES] = { "utilities", "Utilities", HOME_HUE_TOOLS },
    [HOME_FOLDER_APPS] = { "apps", "Apps", HOME_HUE_APPS },
};

const struct home_folder_def *home_folder_get(enum home_folder f)
{
    return (f > HOME_FOLDER_NONE && f < HOME_FOLDER_COUNT) ? &folders[f] : NULL;
}

enum home_folder home_folder_find(const char *id)
{
    int f;

    for (f = HOME_FOLDER_NONE + 1; id && f < HOME_FOLDER_COUNT; f++) {
        if (strcmp(folders[f].id, id) == 0) {
            return (enum home_folder)f;
        }
    }
    return HOME_FOLDER_NONE;
}

int home_root_order(const char *const *ids, int n, struct home_item items[HOME_MAX_APPS],
                    uint8_t count[HOME_GROUP_COUNT])
{
    uint8_t order[HOME_MAX_APPS];
    uint8_t per_group[HOME_GROUP_COUNT];
    bool placed_folder[HOME_FOLDER_COUNT] = { false };
    int total = home_group_order(ids, n, order, per_group);
    int placed = 0;
    int g;
    int k = 0;

    memset(count, 0, HOME_GROUP_COUNT);
    /* home_group_order lists the apps group by group; walk it in step. */
    for (g = 0; g < HOME_GROUP_COUNT; g++) {
        int end = k + per_group[g];

        for (; k < end && k < total; k++) {
            const struct home_entry *en = home_entry_find(ids[order[k]]);
            enum home_folder f = en ? en->folder : HOME_FOLDER_NONE;

            if (f > HOME_FOLDER_NONE && f < HOME_FOLDER_COUNT) {
                if (placed_folder[f]) {
                    continue;
                }
                placed_folder[f] = true;
                items[placed].folder = (uint8_t)f;
                items[placed].index = 0;
            } else {
                items[placed].folder = HOME_FOLDER_NONE;
                items[placed].index = order[k];
            }
            placed++;
            count[g]++;
        }
    }
    return placed;
}

int home_folder_order(const char *const *ids, int n, enum home_folder f, uint8_t order[HOME_MAX_APPS])
{
    uint8_t all[HOME_MAX_APPS];
    uint8_t per_group[HOME_GROUP_COUNT];
    int total;
    int placed = 0;
    int k;

    if (f <= HOME_FOLDER_NONE || f >= HOME_FOLDER_COUNT) {
        return 0;
    }
    total = home_group_order(ids, n, all, per_group);
    for (k = 0; k < total; k++) {
        const struct home_entry *en = home_entry_find(ids[all[k]]);

        if (en && en->folder == f) {
            order[placed++] = all[k];
        }
    }
    return placed;
}

/* ---- favorites -------------------------------------------------------------- */

const char *home_favorite_key(int slot)
{
    static const char *const keys[HOME_FAVORITES] = { "launcher_favorite_1", "launcher_favorite_2",
                                                      "launcher_favorite_3" };

    return (slot >= 0 && slot < HOME_FAVORITES) ? keys[slot] : NULL;
}

bool home_favorite_id_ok(const char *id)
{
    size_t k;

    if (!id || !id[0] || strlen(id) >= HOME_FAVORITE_ID_MAX) {
        return false;
    }
    for (k = 0; id[k]; k++) {
        char c = id[k];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

int home_favorite_resolve(const char *const *ids, int n, const char *id)
{
    int i;

    if (!home_favorite_id_ok(id)) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (ids[i] && strcmp(ids[i], id) == 0) {
            return i;
        }
    }
    return -1;
}

static bool held_elsewhere(const char *const stored[HOME_FAVORITES], int slot, const char *id)
{
    int s;

    for (s = 0; s < HOME_FAVORITES; s++) {
        if (s != slot && stored[s] && strcmp(stored[s], id) == 0) {
            return true;
        }
    }
    return false;
}

int home_favorite_check(const char *const *ids, int n, const char *const stored[HOME_FAVORITES], int slot,
                        const char *id)
{
    if (slot < 0 || slot >= HOME_FAVORITES || home_favorite_resolve(ids, n, id) < 0) {
        return -1;
    }
    return held_elsewhere(stored, slot, id) ? -2 : 0;
}

int home_favorite_candidates(const char *const *ids, int n, const char *const stored[HOME_FAVORITES], int slot,
                             uint8_t order[HOME_MAX_APPS])
{
    uint8_t all[HOME_MAX_APPS];
    uint8_t per_group[HOME_GROUP_COUNT];
    int total = home_group_order(ids, n, all, per_group);
    int placed = 0;
    int k;
    int j;

    for (k = 0; k < total; k++) {
        const char *id = ids[all[k]];
        bool seen = false;

        if (!home_favorite_id_ok(id) || held_elsewhere(stored, slot, id)) {
            continue;
        }
        for (j = 0; j < placed && !seen; j++) {
            seen = strcmp(ids[order[j]], id) == 0;
        }
        if (!seen) {
            order[placed++] = all[k];
        }
    }
    return placed;
}

void home_layout_groups(struct home_layout_in *in, const uint8_t count[HOME_GROUP_COUNT])
{
    int g;

    in->ngroups = HOME_GROUP_COUNT + 1;
    in->count[0] = HOME_FAVORITES;
    for (g = 0; g < HOME_GROUP_COUNT; g++) {
        in->count[g + 1] = count[g];
    }
}

/* ---- a folder's page --------------------------------------------------------- */

#define FOLDER_BACK_Y 8          /* the back slab's row, as the app header's */
#define FOLDER_PANEL_GAP 20      /* between the header row and the panel */
#define FOLDER_TITLE_GAP 16      /* between the back slab and the title */
#define FOLDER_MAX_COLUMNS 9

int home_folder_layout_compute(const struct home_folder_layout_in *in, struct home_folder_layout *out)
{
    int32_t m;
    int32_t line;
    int32_t cw;
    int32_t pw;
    int32_t px;
    int32_t py;
    int32_t title_right;
    int rows;
    int cols;
    int i;

    if (!in || !out || in->n < 0 || in->n > HOME_MAX_APPS || in->width < HOME_CELL_MIN_W + 2 * 48 ||
        in->height < HOME_CELL_H) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    m = in->landscape ? MARGIN_LANDSCAPE : MARGIN_PORTRAIT;
    line = in->width - 2 * m;
    out->back = rect(m, FOLDER_BACK_Y, HOME_BACK_W, HOME_BACK_H);
    /* The title runs from the slab to the status cluster, never under it. */
    title_right = in->width - m;
    if (in->keepout.w > 0 && in->keepout.y < FOLDER_BACK_Y + HOME_BACK_H &&
        in->keepout.y + in->keepout.h > FOLDER_BACK_Y && in->keepout.x - HEADER_CLEAR < title_right) {
        title_right = in->keepout.x - HEADER_CLEAR;
    }
    out->title = rect(m + HOME_BACK_W + FOLDER_TITLE_GAP, FOLDER_BACK_Y,
                      max32(title_right - (m + HOME_BACK_W + FOLDER_TITLE_GAP), 0), HOME_BACK_H);

    if (in->landscape) {
        /* A row of as many as fit, the panel exactly as wide as its cells. */
        cols = (int)((line - 2 * HOME_PANEL_PAD) / HOME_CELL_W_PORTRAIT);
        cols = cols > FOLDER_MAX_COLUMNS ? FOLDER_MAX_COLUMNS : cols;
        cols = in->n < cols ? (in->n > 0 ? in->n : 1) : cols;
        cw = HOME_CELL_W_PORTRAIT;
        pw = cols * cw + 2 * HOME_PANEL_PAD;
        px = (in->width - pw) / 2;
    } else {
        /* The launcher's portrait columns, the panel the whole width. */
        int32_t inner = line - 2 * HOME_PANEL_PAD;

        cols = HOME_PORTRAIT_COLUMNS;
        while (cols > 1 && inner / cols < HOME_CELL_MIN_W) {
            cols--;
        }
        cw = inner / cols;
        cw = cw > HOME_CELL_W_PORTRAIT ? HOME_CELL_W_PORTRAIT : cw;
        pw = line;
        px = m;
    }
    rows = in->n > 0 ? (in->n + cols - 1) / cols : 1;
    py = FOLDER_BACK_Y + HOME_BACK_H + FOLDER_PANEL_GAP;
    out->panel = rect(px, py, pw, panel_height(rows));
    for (i = 0; i < in->n; i++) {
        out->cell[i] = rect(px + HOME_PANEL_PAD + (i % cols) * cw,
                            py + HOME_PANEL_HEAD + HOME_PANEL_PAD + (i / cols) * (HOME_CELL_H + HOME_PANEL_PAD), cw,
                            HOME_CELL_H);
    }
    out->napps = (uint16_t)in->n;
    out->cols = cols;
    out->cell_w = cw;
    out->small_labels = cw < 110;
    out->content_h = max32(in->height, out->panel.y + out->panel.h + max32(FOOTER_PAD, in->inset_bottom + 12));
    return 0;
}
