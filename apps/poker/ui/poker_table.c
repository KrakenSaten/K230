/* Texas Hold'em table: the seats, the board and the pot over the scene's
 * indigo cloth (cg_scene.h), and their motion (cg_motion.h).
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "poker_table.h"
#include "bj_card_draw.h"
#include "cg_deck.h"
#include "cg_motion.h"
#include "cg_table.h"
#include "pocketui.h"
#include "poker_view.h"
#include <stdio.h>

#define KEY_POT 0xffffff01u

struct poker_table_state {
    const struct poker_game *game;
    struct cg_motion motion;
    struct cg_spots before;
    bool before_playing;
};

static struct poker_table_state *state_of(lv_obj_t *obj)
{
    return obj ? lv_obj_get_user_data(obj) : NULL;
}

/* The table is a Lamplight scene with a palette of its own in every theme
 * (cg_table.h): the roles the view asks for, in its inks. */
static lv_color_t table_ink(enum pos_color_token color)
{
    switch (color) {
    case POS_COLOR_STATUS_OK:
    case POS_COLOR_ACCENT_PRIMARY:
        return cg_table_ink(CG_INK_GOLD);
    case POS_COLOR_TEXT_SECONDARY:
        return cg_table_ink(CG_INK_DIM);
    default:
        return cg_table_ink(CG_INK_IVORY);
    }
}

static lv_area_t area(const lv_area_t *origin, int x, int y, int w, int h)
{
    return (lv_area_t){origin->x1 + x, origin->y1 + y, origin->x1 + x + w - 1, origin->y1 + y + h - 1};
}
static lv_area_t rel(struct poker_rect r)
{
    return (lv_area_t){r.x, r.y, r.x + r.w - 1, r.y + r.h - 1};
}
static const lv_font_t *font(enum pos_style_role role)
{
    lv_style_value_t v;
    if (lv_style_get_prop(pos_style(role), LV_STYLE_TEXT_FONT, &v) == LV_STYLE_RES_FOUND)
        return v.ptr;
    return LV_FONT_DEFAULT;
}
/* A hand's cards are new cards in the next hand, whatever their bytes. */
static uint32_t card_key(const struct poker_game *g, cg_card_t card)
{
    return ((g->hand_number & 0xffffu) << 8) | card;
}
static bool face_up(const struct poker_game *g, int seat)
{
    return seat == 0 || (g->showdown && !poker_playing(g));
}
/* The seat a finished hand's chips go to: the first winner. */
static int first_winner(const struct poker_game *g)
{
    for (int i = 0; i < POKER_SEATS; ++i)
        if (g->winners & (1u << i))
            return i;
    return -1;
}
static void place_of(lv_obj_t *obj, struct poker_table_place *place)
{
    const struct poker_type type = {
        lv_font_get_line_height(font(POS_STYLE_BUTTON_LABEL)),
        lv_font_get_line_height(font(POS_STYLE_CAPTION)),
        lv_font_get_line_height(font(POS_STYLE_ROW_TITLE)),
    };
    poker_view_table(lv_obj_get_width(obj), lv_obj_get_height(obj), &type, place);
}
/* Where the pot's chips stand (their foot's centre, as a rect round it), and
 * where a finished hand's chips stand: on the winner's cards. */
/* A chip's width for the pot line's height, as before Lamplight. */
static int chip_d(const struct poker_table_place *place)
{
    return place->pot.h >= 30 ? 26 : 20;
}
/* A box round chips standing on (cx, base) that covers every stack they can
 * draw: what the motion moves and repaints. */
static lv_area_t chips_box(int d, int cx, int base)
{
    int half, high;
    cg_table_chips_reach(d, &half, &high);
    return (lv_area_t){cx - half, base - high, cx + half, base};
}
static lv_area_t pot_spot(const struct poker_table_place *place)
{
    int d = chip_d(place);
    return chips_box(d, place->pot.x + 2 * d, place->pot.y + place->pot.h / 2 + d / 2);
}
/* Beside the winner's cards, on the side with more cloth, so they cover
 * neither the cards nor the seat's words. */
static lv_area_t winner_spot(const struct poker_table_place *place, int width, int seat)
{
    const struct poker_rect *a = &place->seat[seat].card[0], *b = &place->seat[seat].card[1];
    int right = width - (b->x + b->w), left = a->x;
    int reach = (right > left ? right : left) / 2;
    int cx = right > left ? b->x + b->w + (reach < 70 ? reach : 70) : a->x - (reach < 70 ? reach : 70);
    return chips_box(chip_d(place), cx, a->y + a->h - 4);
}
static bool pot_room(const struct poker_table_place *place)
{
    return place->pot.w >= 12 * chip_d(place);
}

/* Text in its place (poker_view_table), which leaves room for the lines it
 * may wrap to: two for hand names and payouts. */
static void text(lv_layer_t *layer, const lv_area_t *origin, struct poker_rect place, const char *value,
                 enum pos_style_role role, enum pos_color_token color)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = font(role);
    d.text = value;
    d.text_local = 1;
    d.align = LV_TEXT_ALIGN_CENTER;
    d.color = table_ink(color);
    lv_area_t r = area(origin, place.x, place.y, place.w, place.h);
    lv_draw_label(layer, &d, &r);
}
static void seat(lv_layer_t *layer, const lv_area_t *origin, const struct poker_table_state *st, int i,
                 const struct poker_seat_place *place)
{
    const struct poker_game *g = st->game;
    const struct poker_player *p = &g->player[i];
    char line[80];
    bool won = !poker_playing(g) && (g->winners & (1u << i));
    enum pos_color_token color = g->actor == i || won ? POS_COLOR_ACCENT_PRIMARY : POS_COLOR_TEXT_PRIMARY;
    const char *badge = g->button == i        ? (g->small_blind == i ? " D/SB" : " D")
                        : g->small_blind == i ? " SB"
                        : g->big_blind == i   ? " BB"
                                              : "";
    if (g->actor == i || won) {
        /* The seat whose turn it is, or a finished hand's winner, is ringed
         * in gold. */
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = cg_table_ink(CG_INK_GOLD);
        d.border_width = 2;
        d.radius = 8;
        lv_area_t r = area(origin, place->ring.x, place->ring.y, place->ring.w, place->ring.h);
        lv_draw_rect(layer, &d, &r);
    }
    snprintf(line, sizeof(line), "%s%s%s %u", g->actor == i ? ">" : "", poker_seat_name(i), badge, p->stack);
    text(layer, origin, place->name, line, POS_STYLE_BUTTON_LABEL, color);
    for (int k = 0; k < 2 && p->in_hand && !p->folded; ++k) {
        const struct poker_rect *c = &place->card[k];
        lv_area_t r = area(origin, c->x, c->y, c->w, c->h);
        if (cg_motion_pose(&st->motion, card_key(g, p->hole[k]), NULL) < 0)
            bj_draw_shadow(layer, &r);
    }
    for (int k = 0; k < 2; ++k) {
        const struct poker_rect *c = &place->card[k];
        lv_area_t r = area(origin, c->x, c->y, c->w, c->h);
        if (!p->in_hand || p->folded)
            bj_draw_slot(layer, &r, BJ_SLOT_PLAIN, CG_SPADES);
        else if (cg_motion_pose(&st->motion, card_key(g, p->hole[k]), NULL) >= 0)
            continue;
        else if (face_up(g, i))
            bj_draw_face(layer, &r, p->hole[k], BJ_INDEX_BELOW);
        else
            bj_draw_back(layer, &r);
    }
    if (!p->in_hand)
        snprintf(line, sizeof(line), "OUT");
    else if (p->folded)
        snprintf(line, sizeof(line), "FOLDED");
    else if (!poker_playing(g) && g->showdown)
        snprintf(line, sizeof(line), "%s | +%u", poker_rank_name(g->rank[i]), p->payout - p->refunded);
    else if (!poker_playing(g))
        snprintf(line, sizeof(line), p->payout ? "+%u chips" : "Hand complete", p->payout - p->refunded);
    else if (!p->stack)
        snprintf(line, sizeof(line), "ALL IN | %u", p->bet);
    else if (p->bet)
        snprintf(line, sizeof(line), "BET %u", p->bet);
    else
        snprintf(line, sizeof(line), p->acted ? "CHECK" : "In hand");
    text(layer, origin, place->status, line, POS_STYLE_CAPTION, won ? POS_COLOR_STATUS_OK : POS_COLOR_TEXT_SECONDARY);
}
static void draw(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    struct poker_table_state *st = state_of(obj);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t origin;
    lv_obj_get_coords(obj, &origin);
    if (!st || !st->game || !layer)
        return;
    const struct poker_game *g = st->game;
    /* Night dims the world - cloth, cards, chips; the words are interface. */
    cg_table_set_night(pos_theme_current_mode() == POS_MODE_NIGHT);
    /* Where everything goes: the tall layout, or the short one on a table
     * shorter than the panel's (poker_view.h). */
    struct poker_table_place place;
    place_of(obj, &place);
    char line[80];
    if (place.compact) {
        snprintf(line, sizeof(line), "HAND %u", g->hand_number);
        text(layer, &origin, place.hand, line, POS_STYLE_CAPTION, POS_COLOR_TEXT_SECONDARY);
        text(layer, &origin, place.blinds, "BLINDS\n10 / 20", POS_STYLE_CAPTION, POS_COLOR_TEXT_SECONDARY);
    } else {
        snprintf(line, sizeof(line), "HAND %u | BLINDS 10 / 20", g->hand_number);
        text(layer, &origin, place.hand, line, POS_STYLE_CAPTION, POS_COLOR_TEXT_SECONDARY);
    }
    seat(layer, &origin, st, 2, &place.seat[2]);
    seat(layer, &origin, st, 1, &place.seat[1]);
    seat(layer, &origin, st, 3, &place.seat[3]);
    const lv_font_t *head = cg_deck_font(CG_TYPE_HEADLINE_SMALL);
    int winners = 0;
    for (int i = 0; i < POKER_SEATS; ++i)
        winners += (g->winners >> i) & 1;
    if (!poker_playing(g) && winners == 1 && cg_motion_pose(&st->motion, KEY_POT, NULL) < 0 &&
        lv_font_get_line_height(head) <= place.pot.h + 16) {
        /* One winner: one sentence, in the one serif (decision 5). */
        int w = first_winner(g);
        lv_draw_label_dsc_t d;
        int lh = lv_font_get_line_height(head);
        lv_area_t r = area(&origin, place.pot.x, place.pot.y + (place.pot.h - lh) / 2, place.pot.w, lh);
        if (w == 0)
            snprintf(line, sizeof(line), "You take it.");
        else
            snprintf(line, sizeof(line), "%c%s takes it.", poker_seat_name(w)[0], poker_seat_name(w) + 1);
        for (char *c = line + 1; *c && *c != ' '; ++c)
            *c = (char)(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c);
        lv_draw_label_dsc_init(&d);
        d.font = head;
        d.text = line;
        d.text_local = 1;
        d.align = LV_TEXT_ALIGN_CENTER;
        d.color = cg_table_ink(CG_INK_GOLD);
        lv_draw_label(layer, &d, &r);
    } else {
        snprintf(line, sizeof(line), "%s | POT %u", poker_phase_name(g->phase),
                 poker_playing(g) ? poker_pot_total(g) : g->settled_pot);
        text(layer, &origin, place.pot, line, POS_STYLE_ROW_TITLE, POS_COLOR_TEXT_PRIMARY);
    }
    for (int i = 0; i < g->board_count; ++i) {
        const struct poker_rect *c = &place.board[i];
        lv_area_t r = area(&origin, c->x, c->y, c->w, c->h);
        if (cg_motion_pose(&st->motion, card_key(g, g->board[i]), NULL) < 0)
            bj_draw_shadow(layer, &r);
    }
    for (int i = 0; i < 5; ++i) {
        const struct poker_rect *c = &place.board[i];
        lv_area_t r = area(&origin, c->x, c->y, c->w, c->h);
        if (i >= g->board_count)
            bj_draw_slot(layer, &r, BJ_SLOT_PLAIN, CG_SPADES);
        else if (cg_motion_pose(&st->motion, card_key(g, g->board[i]), NULL) < 0)
            bj_draw_face(layer, &r, g->board[i], BJ_INDEX_BELOW);
    }
    seat(layer, &origin, st, 0, &place.seat[0]);
    if (pot_room(&place)) {
        /* The pot as chips at the head of its line; a finished hand's on the
         * winner's cards, once they have slid there. */
        struct cg_pose p;
        int w = first_winner(g);
        if (cg_motion_pose(&st->motion, KEY_POT, &p) == 1) {
            cg_table_chips(layer, origin.x1 + (p.rect.x1 + p.rect.x2) / 2, origin.y1 + p.rect.y2, g->settled_pot,
                           chip_d(&place));
        } else if (poker_playing(g) || w < 0) {
            lv_area_t a = pot_spot(&place);
            cg_table_chips(layer, origin.x1 + (a.x1 + a.x2) / 2, origin.y1 + a.y2,
                           poker_playing(g) ? poker_pot_total(g) : g->settled_pot, chip_d(&place));
        } else if (cg_motion_pose(&st->motion, KEY_POT, NULL) < 0) {
            lv_area_t a = winner_spot(&place, lv_obj_get_width(obj), w);
            cg_table_chips(layer, origin.x1 + (a.x1 + a.x2) / 2, origin.y1 + a.y2, g->settled_pot, chip_d(&place));
        }
    }
    /* Cards in motion, over everything. */
    for (int i = 0; i < cg_motion_count(&st->motion); ++i) {
        uint32_t key = cg_motion_key(&st->motion, i);
        struct cg_pose p;
        if (key == KEY_POT || cg_motion_pose(&st->motion, key, &p) != 1)
            continue;
        lv_area_t a = {origin.x1 + p.rect.x1, origin.y1 + p.rect.y1, origin.x1 + p.rect.x2, origin.y1 + p.rect.y2};
        if (p.scale_x >= 256)
            bj_draw_shadow(layer, &a);
        bj_draw_card_scaled(layer, &a, (cg_card_t)(key & 0xff), p.face, p.scale_x);
    }
}
static void table_delete(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct poker_table_state *st = state_of(obj);
    if (st)
        cg_motion_stop(&st->motion);
    lv_free(st);
    lv_obj_set_user_data(obj, NULL);
}
static void table_resized(lv_event_t *e)
{
    struct poker_table_state *st = state_of(lv_event_get_target_obj(e));
    if (st)
        cg_motion_stop(&st->motion);
}
lv_obj_t *poker_table_create(lv_obj_t *parent, const struct poker_game *game)
{
    lv_obj_t *table = lv_obj_create(parent);
    struct poker_table_state *st = lv_malloc_zeroed(sizeof(*st));
    if (!st) {
        lv_obj_delete(table);
        return NULL;
    }
    lv_obj_remove_style_all(table);
    lv_obj_remove_flag(table, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    st->game = game;
    lv_obj_set_user_data(table, st);
    cg_motion_init(&st->motion, table, 14);
    lv_obj_add_event_cb(table, draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(table, table_resized, LV_EVENT_SIZE_CHANGED, NULL);
    lv_obj_add_event_cb(table, table_delete, LV_EVENT_DELETE, NULL);
    return table;
}

/* ---- motion ------------------------------------------------------------------------ */

static void picture(lv_obj_t *obj, const struct poker_table_state *st, struct cg_spots *out)
{
    const struct poker_game *g = st->game;
    struct poker_table_place place;
    place_of(obj, &place);
    out->n = 0;
    /* Dealt round the table from the player's left, as the engine deals. */
    for (int k = 0; k < 2; ++k)
        for (int s = 1; s <= POKER_SEATS; ++s) {
            int i = s % POKER_SEATS;
            const struct poker_player *p = &g->player[i];
            if (!p->in_hand || p->folded)
                continue;
            out->s[out->n++] = (struct cg_spot){card_key(g, p->hole[k]), rel(place.seat[i].card[k]), face_up(g, i)};
        }
    for (int i = 0; i < g->board_count; ++i)
        out->s[out->n++] = (struct cg_spot){card_key(g, g->board[i]), rel(place.board[i]), 1};
}
void poker_table_before(lv_obj_t *table)
{
    struct poker_table_state *st = state_of(table);
    if (!st || !st->game)
        return;
    cg_motion_stop(&st->motion);
    picture(table, st, &st->before);
    st->before_playing = poker_playing(st->game);
}
void poker_table_after(lv_obj_t *table)
{
    static const struct cg_plan plan = {0, 110, 220, 240};
    struct poker_table_state *st = state_of(table);
    struct cg_spots after;
    struct poker_table_place place;
    uint32_t end = 0;
    int w;
    if (!st || !st->game)
        return;
    picture(table, st, &after);
    place_of(table, &place);
    /* The deck lies where the board's middle card goes. */
    lv_area_t deck = rel(place.board[2]);
    /* What a moving card repaints: itself and its shadow; the player's
     * cards are the largest. */
    st->motion.margin = cg_deck_shadow_reach(place.seat[0].card[0].w);
    cg_motion_diff(&st->motion, &st->before, &after, &deck, &plan);
    end = cg_motion_end(&st->motion);
    /* A hand that just finished: the pot slides to the winner. */
    w = first_winner(st->game);
    if (st->before_playing && !poker_playing(st->game) && w >= 0 && pot_room(&place)) {
        lv_area_t from = pot_spot(&place), to = winner_spot(&place, lv_obj_get_width(table), w);
        cg_motion_add(&st->motion, KEY_POT, CG_MOVE_TRAVEL, &from, &to, 1, 1, end + 120, 480);
    }
}
int poker_table_moving(lv_obj_t *table)
{
    struct poker_table_state *st = state_of(table);
    return st && cg_motion_busy(&st->motion);
}
void poker_table_finish(lv_obj_t *table)
{
    struct poker_table_state *st = state_of(table);
    if (st)
        cg_motion_stop(&st->motion);
}
