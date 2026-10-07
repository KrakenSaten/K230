/*
 * PG 2048 view model. See g2048_view.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "g2048_view.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- the controls row ------------------------------------------------------ */

int g2048_view_has_progress(const struct g2048_game *g)
{
    return g && g->moves > 0 && !g->over;
}

enum g2048_panel g2048_view_panel(const struct g2048_game *g, int confirming)
{
    switch (g2048_state_of(g)) {
    case G2048_OVER:
        return G2048_PANEL_OVER;
    case G2048_WON:
        return G2048_PANEL_WON;
    case G2048_PLAYING:
    default:
        return confirming ? G2048_PANEL_CONFIRM : G2048_PANEL_PLAY;
    }
}

enum g2048_cmd g2048_view_move_cmd(enum g2048_dir dir)
{
    switch (dir) {
    case G2048_UP:
        return G2048_CMD_MOVE_UP;
    case G2048_DOWN:
        return G2048_CMD_MOVE_DOWN;
    case G2048_LEFT:
        return G2048_CMD_MOVE_LEFT;
    case G2048_RIGHT:
        return G2048_CMD_MOVE_RIGHT;
    default:
        return G2048_CMD_NONE;
    }
}

int g2048_view_cmd_dir(enum g2048_cmd cmd, enum g2048_dir *dir)
{
    enum g2048_dir d;

    switch (cmd) {
    case G2048_CMD_MOVE_UP:
        d = G2048_UP;
        break;
    case G2048_CMD_MOVE_DOWN:
        d = G2048_DOWN;
        break;
    case G2048_CMD_MOVE_LEFT:
        d = G2048_LEFT;
        break;
    case G2048_CMD_MOVE_RIGHT:
        d = G2048_RIGHT;
        break;
    default:
        return 0;
    }
    if (dir) {
        *dir = d;
    }
    return 1;
}

static enum g2048_cmd move_for_key(uint32_t key)
{
    switch (key) {
    case G2048_KEY_UP:
    case 'w':
    case 'W':
        return G2048_CMD_MOVE_UP;
    case G2048_KEY_DOWN:
    case 's':
    case 'S':
        return G2048_CMD_MOVE_DOWN;
    case G2048_KEY_LEFT:
    case 'a':
    case 'A':
        return G2048_CMD_MOVE_LEFT;
    case G2048_KEY_RIGHT:
    case 'd':
    case 'D':
        return G2048_CMD_MOVE_RIGHT;
    default:
        return G2048_CMD_NONE;
    }
}

enum g2048_cmd g2048_view_command_for_key(enum g2048_panel panel, int has_progress, uint32_t key)
{
    int is_new = key == 'n' || key == 'N';

    switch (panel) {
    case G2048_PANEL_PLAY:
        if (is_new) {
            return has_progress ? G2048_CMD_ASK_NEW : G2048_CMD_NEW_GAME;
        }
        return move_for_key(key);
    case G2048_PANEL_CONFIRM:
        if (is_new) {
            return G2048_CMD_NEW_GAME;
        }
        if (key == G2048_KEY_ENTER || key == G2048_KEY_ESC || key == G2048_KEY_BACKSPACE) {
            return G2048_CMD_CANCEL;
        }
        return G2048_CMD_NONE;
    case G2048_PANEL_WON:
        if (is_new) {
            return G2048_CMD_NEW_GAME;
        }
        if (key == G2048_KEY_ENTER || key == G2048_KEY_ESC) {
            return G2048_CMD_KEEP_GOING;
        }
        return G2048_CMD_NONE;
    case G2048_PANEL_OVER:
        return (is_new || key == G2048_KEY_ENTER) ? G2048_CMD_NEW_GAME : G2048_CMD_NONE;
    default:
        return G2048_CMD_NONE;
    }
}

/* ---- swipes -------------------------------------------------------------- */

int g2048_view_swipe_threshold(int board_px)
{
    int t = board_px / 10;

    return t < 32 ? 32 : t;
}

int g2048_view_swipe(int dx, int dy, int threshold, enum g2048_dir *dir)
{
    int ax = abs(dx);
    int ay = abs(dy);
    enum g2048_dir d;

    if (ax >= ay) {
        if (ax < threshold || ax * 2 < ay * 3) {
            return 0;
        }
        d = dx > 0 ? G2048_RIGHT : G2048_LEFT;
    } else {
        if (ay < threshold || ay * 2 < ax * 3) {
            return 0;
        }
        d = dy > 0 ? G2048_DOWN : G2048_UP;
    }
    if (dir) {
        *dir = d;
    }
    return 1;
}

/* ---- layout ---------------------------------------------------------------- */

static int max_i(int a, int b)
{
    return a > b ? a : b;
}

static int min_i(int a, int b)
{
    return a < b ? a : b;
}

static struct g2048_rect rect(int x, int y, int w, int h)
{
    struct g2048_rect r = { x, y, w, h };

    return r;
}

/* Keep a rect inside a w x h area, with a size of at least zero. */
static struct g2048_rect clamp(struct g2048_rect r, int w, int h)
{
    r.x = min_i(max_i(r.x, 0), max_i(w, 0));
    r.y = min_i(max_i(r.y, 0), max_i(h, 0));
    r.w = max_i(min_i(r.w, w - r.x), 0);
    r.h = max_i(min_i(r.h, h - r.y), 0);
    return r;
}

void g2048_view_layout(int w, int h, struct g2048_layout *out)
{
    int size;

    if (!out) {
        return;
    }
    w = max_i(w, 1);
    h = max_i(h, 1);
    if (w > h) {
        /* Wide: the board takes the full height on the left, the side column
         * keeps at least its minimum width. */
        int side_x;

        size = max_i(min_i(h, w - G2048_SIDE_MIN_W - G2048_GAP), 1);
        side_x = size + G2048_GAP;
        out->arrangement = G2048_SIDE_BY_SIDE;
        out->board = rect(0, (h - size) / 2, size, size);
        out->hud = rect(side_x, 0, w - side_x, G2048_HUD_H);
        out->controls = rect(side_x, h - G2048_CONTROLS_H, w - side_x, G2048_CONTROLS_H);
    } else {
        /* Tall: the HUD on top, the controls at the foot where a thumb is,
         * and the board centred in what lies between. */
        int top = G2048_HUD_H + G2048_GAP;
        int room = h - G2048_HUD_H - G2048_CONTROLS_H - 2 * G2048_GAP;

        size = max_i(min_i(w, room), 1);
        out->arrangement = G2048_STACKED;
        out->hud = rect(0, 0, w, G2048_HUD_H);
        out->board = rect((w - size) / 2, top + max_i(room - size, 0) / 2, size, size);
        out->controls = rect(0, h - G2048_CONTROLS_H, w, G2048_CONTROLS_H);
    }
    out->hud = clamp(out->hud, w, h);
    out->board = clamp(out->board, w, h);
    out->controls = clamp(out->controls, w, h);
}

int g2048_view_cell_gap(int board_px)
{
    return max_i(board_px / 44, 6);
}

struct g2048_rect g2048_view_cell(int board_px, int index)
{
    int gap = g2048_view_cell_gap(board_px);
    int tile = max_i((board_px - (G2048_SIDE + 1) * gap) / G2048_SIDE, 1);
    int margin = (board_px - G2048_SIDE * tile - (G2048_SIDE - 1) * gap) / 2;
    int col;
    int row;

    if (index < 0 || index >= G2048_CELLS) {
        return rect(0, 0, 0, 0);
    }
    col = index % G2048_SIDE;
    row = index / G2048_SIDE;
    return rect(margin + col * (tile + gap), margin + row * (tile + gap), tile, tile);
}

/* ---- how a tile looks ------------------------------------------------------ */

/* Mix toward accent per exponent, 3..10; 11 and up take G2048_MIX_MAX. */
static const uint8_t ramp[] = { 0, 0, 0, 10, 20, 30, 40, 50, 60, 70, 80 };

static uint32_t mix_rgb(uint32_t toward, uint32_t base, uint8_t mix)
{
    uint32_t out = 0;
    int shift;

    /* lv_color_mix: LV_UDIV255(c1 * mix + c2 * (255 - mix)), per channel. */
    for (shift = 0; shift <= 16; shift += 8) {
        uint32_t a = (toward >> shift) & 0xFFu;
        uint32_t b = (base >> shift) & 0xFFu;
        uint32_t c = ((a * mix + b * (255u - mix)) * 0x8081u) >> 0x17;

        out |= (c & 0xFFu) << shift;
    }
    return out;
}

uint32_t g2048_view_fill_rgb(const struct g2048_tile_look *look, const struct pos_theme_tokens *tokens)
{
    if (!look || !tokens) {
        return 0;
    }
    return mix_rgb(tokens->color[look->fill_toward], tokens->color[look->fill_base], look->fill_mix);
}

void g2048_view_tile_look(uint8_t exp, const struct pos_theme_tokens *tokens,
                          struct g2048_tile_look *out)
{
    uint32_t fill;

    if (!out) {
        return;
    }
    out->fill_base = POS_COLOR_SURFACE_RAISED;
    out->fill_toward = POS_COLOR_ACCENT_PRIMARY;
    out->fill_mix = exp < sizeof(ramp) ? ramp[exp] : G2048_MIX_MAX;
    out->border = POS_COLOR_ACCENT_PRIMARY;
    out->border_px = exp >= G2048_GOAL_EXP ? 2 : 0;
    out->text = POS_COLOR_TEXT_PRIMARY;
    if (exp <= 1) {
        out->text = POS_COLOR_TEXT_SECONDARY;
        return;
    }
    if (!tokens) {
        return;
    }
    fill = g2048_view_fill_rgb(out, tokens);
    if (pos_contrast(tokens->color[POS_COLOR_TEXT_ON_ACCENT], fill) >
        pos_contrast(tokens->color[POS_COLOR_TEXT_PRIMARY], fill)) {
        out->text = POS_COLOR_TEXT_ON_ACCENT;
    }
}

void g2048_view_label(uint8_t exp, char *out, int out_len)
{
    if (!out || out_len <= 0) {
        return;
    }
    snprintf(out, (size_t)out_len, "%u", (unsigned)g2048_value(exp));
}

/* ---- motion -------------------------------------------------------------- */

int g2048_view_slide_q8(int elapsed_ms)
{
    int r;

    if (elapsed_ms <= 0) {
        return 0;
    }
    if (elapsed_ms >= G2048_SLIDE_MS) {
        return 256;
    }
    r = G2048_SLIDE_MS - elapsed_ms;
    return 256 - (256 * r * r) / (G2048_SLIDE_MS * G2048_SLIDE_MS);
}

struct g2048_rect g2048_view_lerp(struct g2048_rect a, struct g2048_rect b, int q8)
{
    struct g2048_rect r;

    q8 = min_i(max_i(q8, 0), 256);
    r.x = a.x + (b.x - a.x) * q8 / 256;
    r.y = a.y + (b.y - a.y) * q8 / 256;
    r.w = a.w + (b.w - a.w) * q8 / 256;
    r.h = a.h + (b.h - a.h) * q8 / 256;
    return r;
}

int g2048_view_pop_px(int elapsed_ms)
{
    int s = elapsed_ms - G2048_SLIDE_MS;
    int half = G2048_SETTLE_MS / 2;

    if (s <= 0 || s >= G2048_SETTLE_MS) {
        return 0;
    }
    return G2048_POP_PX * (half - abs(s - half)) / half;
}

int g2048_view_spawn_q8(int elapsed_ms)
{
    int s = elapsed_ms - G2048_SLIDE_MS;

    if (s < 0) {
        return 0;
    }
    if (s >= G2048_SETTLE_MS) {
        return 256;
    }
    return 128 + 128 * s / G2048_SETTLE_MS;
}
