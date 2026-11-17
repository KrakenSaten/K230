/*
 * The Games Hub's geometry. See hub_layout.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hub_layout.h"

#include <stdio.h>
#include <string.h>

/* The side margins of the header row: the folder page's (home_layout.c), so
 * the back slab does not move between the two pages. */
#define HEADER_MARGIN_PORTRAIT 28
#define HEADER_MARGIN_LANDSCAPE 36
#define TITLE_GAP 16             /* between the back slab and the title */
#define KEEPOUT_CLEAR 16         /* between the header's text and the status cluster */
#define GAP 10                   /* between posters */

/* Portrait: the wall's box in the 568 x 1232 design. */
#define P_X 20
#define P_Y 116
#define P_W 528
/* Landscape: 30 px side margins keep the wall out of the bottom corners. */
#define L_X 30
#define L_Y 76
#define L_W 1172
#define L_H 468

#define CHIP_W 106
#define CHIP_H 26
#define LOBBY_CHIP_W 126

static const struct hub_game games[HUB_GAMES] = {
    [HUB_TILE_ROTORTERROR] = { "rotorterror", "rotorterror", true, false },
    [HUB_TILE_RELAY_NINE] = { "neon-outpost", "relay-nine", true, false },
    [HUB_TILE_FLEET] = { "fleet", "fleet", false, false },
    [HUB_TILE_RADAR] = { "radar", "radar", false, false },
    [HUB_TILE_TIMBER] = { "timber", "timber", false, true },
    [HUB_TILE_SOLITAIRE] = { "solitaire", "solitaire", false, false },
    [HUB_TILE_BLACKJACK] = { "blackjack", "blackjack", false, false },
    [HUB_TILE_POKER] = { "poker", "poker", false, false },
    [HUB_TILE_2048] = { "2048", "2048", false, false },
};

static struct hub_rect rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct hub_rect r = { x, y, w, h };

    return r;
}

const struct hub_game *hub_game(int tile)
{
    return (tile >= 0 && tile < HUB_GAMES) ? &games[tile] : NULL;
}

int hub_tile_find(const char *app_id)
{
    int t;

    for (t = 0; app_id && t < HUB_GAMES; t++) {
        if (strcmp(games[t].id, app_id) == 0) {
            return t;
        }
    }
    return -1;
}

bool hub_games_match(const char *const *ids, int n)
{
    bool seen[HUB_GAMES] = { false };
    int k;

    if (!ids || n != HUB_GAMES) {
        return false;
    }
    for (k = 0; k < n; k++) {
        int t = hub_tile_find(ids[k]);

        if (t < 0 || seen[t]) {
            return false;
        }
        seen[t] = true;
    }
    return true;
}

int hub_art_name(int tile, bool landscape, char *out, size_t out_len)
{
    const char *art = tile == HUB_TILE_LOBBY ? "lobby" : hub_game(tile) ? games[tile].art : NULL;
    int n;

    if (!art || !out) {
        return -1;
    }
    n = snprintf(out, out_len, "hub-%s-%s", art, landscape ? "landscape" : "portrait");
    return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

bool hub_rects_overlap(const struct hub_rect *a, const struct hub_rect *b)
{
    return a->w > 0 && a->h > 0 && b->w > 0 && b->h > 0 && a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

bool hub_rect_inside(const struct hub_rect *inner, const struct hub_rect *outer)
{
    return inner->x >= outer->x && inner->y >= outer->y && inner->x + inner->w <= outer->x + outer->w &&
           inner->y + inner->h <= outer->y + outer->h;
}

/* The header row: the back slab at the folder page's place, the title
 * beside it, and the count against the right margin or the status cluster,
 * whichever comes first. Portrait puts the count on a line of its own under
 * the row, where the cluster is not. */
static void header(const struct hub_frame *f, struct hub_layout *out, int32_t ox)
{
    int32_t m = f->landscape ? HEADER_MARGIN_LANDSCAPE : HEADER_MARGIN_PORTRAIT;
    int32_t tx = m + HUB_BACK_W + TITLE_GAP;
    int32_t right = f->landscape ? ox + L_X + L_W : ox + P_X + P_W;
    const struct hub_rect *k = &f->keepout;

    out->back = rect(m, HUB_BACK_Y, HUB_BACK_W, HUB_BACK_H);
    if (k->w > 0 && k->h > 0 && k->y < HUB_BACK_Y + HUB_BACK_H && k->y + k->h > HUB_BACK_Y &&
        k->x - KEEPOUT_CLEAR < right) {
        right = k->x - KEEPOUT_CLEAR;
    }
    if (f->landscape) {
        int32_t split = tx + 200; /* "Games" at 28 px, with room to spare */

        out->title = rect(tx, HUB_BACK_Y, split - tx, HUB_BACK_H);
        out->meta = rect(split, HUB_BACK_Y, right - split, HUB_BACK_H);
    } else {
        out->title = rect(tx, HUB_BACK_Y, right - tx, HUB_BACK_H);
        out->meta = rect(ox + P_X, 84, P_W, 20);
    }
}

static void portrait(struct hub_layout *out, int32_t ox)
{
    int32_t x = ox + P_X;
    int32_t y = P_Y;
    struct hub_rect *t = out->tile;
    const struct hub_rect *lobby;

    t[HUB_TILE_ROTORTERROR] = rect(x, y, 528, 300);
    y += 300 + GAP;
    t[HUB_TILE_RELAY_NINE] = rect(x, y, 258, 310);
    t[HUB_TILE_FLEET] = rect(x + 268, y, 125, 150);
    t[HUB_TILE_RADAR] = rect(x + 403, y, 125, 150);
    t[HUB_TILE_TIMBER] = rect(x + 268, y + 160, 125, 150);
    t[HUB_TILE_2048] = rect(x + 403, y + 160, 125, 150);
    y += 310 + GAP;
    t[HUB_TILE_SOLITAIRE] = rect(x, y, 169, 160);
    t[HUB_TILE_BLACKJACK] = rect(x + 179, y, 169, 160);
    t[HUB_TILE_POKER] = rect(x + 359, y, 169, 160);
    y += 160 + GAP;
    t[HUB_TILE_LOBBY] = rect(x, y, 528, 200);
    y += 200;
    out->wall = rect(x, P_Y, P_W, y - P_Y);
    out->hint = rect(x, y + 26, P_W, 20);

    out->chip[0] = rect(t[0].x + 14, t[0].y + 14, CHIP_W, CHIP_H);
    out->play[0] = rect(t[0].x + 378, t[0].y + 232, 134, 52);
    out->chip[1] = rect(t[1].x + 14, t[1].y + 14, CHIP_W, CHIP_H);
    out->play[1] = rect(t[1].x + 16, t[1].y + 248, 226, 46);

    lobby = &t[HUB_TILE_LOBBY];
    out->lobby_chip = rect(lobby->x + lobby->w - 22 - LOBBY_CHIP_W, lobby->y + 20, LOBBY_CHIP_W, CHIP_H);
    out->lobby_text = rect(lobby->x + 22, lobby->y + 104, 350, 22);
    out->lobby_host = rect(lobby->x + 22, lobby->y + lobby->h - 60, 150, 44);
    out->lobby_join = rect(lobby->x + 184, lobby->y + lobby->h - 60, 150, 44);
}

static void landscape(struct hub_layout *out, int32_t ox)
{
    int32_t x = ox + L_X;
    int32_t y = L_Y;
    int32_t col[3];
    struct hub_rect *t = out->tile;
    const struct hub_rect *lobby;
    int k;

    t[HUB_TILE_ROTORTERROR] = rect(x, y, 378, L_H);
    t[HUB_TILE_RELAY_NINE] = rect(x + 388, y, 290, L_H);
    col[0] = x + 688;
    col[1] = col[0] + 155 + GAP;
    col[2] = x + L_W - 155;
    for (k = 0; k < 3; k++) {
        t[HUB_TILE_FLEET + k] = rect(col[k], y, 155, 146);
        t[HUB_TILE_SOLITAIRE + k] = rect(col[k], y + 156, 155, 146);
    }
    t[HUB_TILE_2048] = rect(col[0], y + 312, 155, 156);
    t[HUB_TILE_LOBBY] = rect(col[1], y + 312, x + L_W - col[1], 156);
    out->wall = rect(x, y, L_W, L_H);

    for (k = 0; k < HUB_FEATURED; k++) {
        out->chip[k] = rect(t[k].x + 16, t[k].y + 16, CHIP_W, CHIP_H);
        out->play[k] = rect(t[k].x + 16, t[k].y + t[k].h - 62, 180, 46);
    }
    out->badge = rect(t[HUB_TILE_TIMBER].x + 8, t[HUB_TILE_TIMBER].y + 8, 88, 22);

    lobby = &t[HUB_TILE_LOBBY];
    out->lobby_chip = rect(lobby->x + 16, lobby->y + 52, LOBBY_CHIP_W, CHIP_H);
    out->lobby_host = rect(lobby->x + 16, lobby->y + lobby->h - 54, 138, 40);
    out->lobby_join = rect(lobby->x + 164, lobby->y + lobby->h - 54, 138, 40);
}

int hub_layout(const struct hub_frame *f, struct hub_layout *out)
{
    int32_t dw;
    int32_t dh;
    int32_t ox;
    struct hub_rect corner[4];
    struct hub_rect area;
    int t;
    int c;

    if (!f || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    dw = f->landscape ? HUB_LANDSCAPE_W : HUB_PORTRAIT_W;
    dh = f->landscape ? HUB_LANDSCAPE_H : HUB_PORTRAIT_H;
    if (f->width < dw || f->height < dh) {
        return -1;
    }
    out->landscape = f->landscape;
    ox = (f->width - dw) / 2;
    if (f->landscape) {
        landscape(out, ox);
    } else {
        portrait(out, ox);
    }
    header(f, out, ox);
    if (out->title.w < 120 || out->meta.w < 200) {
        return -1; /* the status cluster leaves the header no room */
    }

    /* Nothing of the wall under the status cluster, in a corner square, or
     * off the content area. The back slab is the one thing a corner square
     * may hold (DS §48); the header's text is clear of them by its place. */
    area = rect(0, 0, f->width, f->height);
    corner[0] = rect(0, 0, f->corner_top_left, f->corner_top_left);
    corner[1] = rect(f->width - f->corner_top_right, 0, f->corner_top_right, f->corner_top_right);
    corner[2] = rect(0, f->height - f->corner_bottom_left, f->corner_bottom_left, f->corner_bottom_left);
    corner[3] = rect(f->width - f->corner_bottom_right, f->height - f->corner_bottom_right,
                     f->corner_bottom_right, f->corner_bottom_right);
    for (t = 0; t < HUB_TILE_COUNT; t++) {
        if (!hub_rect_inside(&out->tile[t], &area) || hub_rects_overlap(&out->tile[t], &f->keepout)) {
            return -1;
        }
        for (c = 0; c < 4; c++) {
            if (hub_rects_overlap(&out->tile[t], &corner[c])) {
                return -1;
            }
        }
    }
    if (hub_rects_overlap(&out->title, &f->keepout) || hub_rects_overlap(&out->meta, &f->keepout)) {
        return -1;
    }
    if (out->hint.w > 0) {
        if (!hub_rect_inside(&out->hint, &area)) {
            return -1;
        }
        for (c = 2; c < 4; c++) {
            /* Centred text narrower than the row: the row's ends may lie
             * level with a bottom corner only if the row is above it. */
            if (hub_rects_overlap(&out->hint, &corner[c])) {
                return -1;
            }
        }
    }
    return 0;
}

/* ---- keys ------------------------------------------------------------------ */

static int32_t abs32(int32_t v)
{
    return v < 0 ? -v : v;
}

int hub_neighbour(const struct hub_layout *l, int from, enum hub_dir dir)
{
    const struct hub_rect *a;
    int best = -1;
    int32_t best_gap = 0;
    int32_t best_edge = 0;
    int t;

    if (!l || from < 0 || from >= HUB_GAMES) {
        return from;
    }
    a = &l->tile[from];
    for (t = 0; t < HUB_GAMES; t++) {
        const struct hub_rect *b = &l->tile[t];
        bool horizontal = dir == HUB_DIR_LEFT || dir == HUB_DIR_RIGHT;
        int32_t gap;
        int32_t edge;
        bool beside;

        if (t == from) {
            continue;
        }
        if (horizontal) {
            gap = dir == HUB_DIR_RIGHT ? b->x - (a->x + a->w) : a->x - (b->x + b->w);
            beside = a->y < b->y + b->h && b->y < a->y + a->h;
            edge = abs32(b->y - a->y);
        } else {
            gap = dir == HUB_DIR_DOWN ? b->y - (a->y + a->h) : a->y - (b->y + b->h);
            beside = a->x < b->x + b->w && b->x < a->x + a->w;
            edge = abs32(b->x - a->x);
        }
        if (gap < 0 || !beside) {
            continue;
        }
        if (best < 0 || gap < best_gap || (gap == best_gap && edge < best_edge)) {
            best = t;
            best_gap = gap;
            best_edge = edge;
        }
    }
    return best >= 0 ? best : from;
}
