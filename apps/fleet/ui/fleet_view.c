/*
 * PocketFleet view model. See fleet_view.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_view.h"

#include <stdio.h>
#include <string.h>

/* U+00B7 MIDDLE DOT, the separator the Design System uses in captions. */
#define DOT "\xc2\xb7"

static int fail(char *buf, size_t n)
{
    if (buf && n > 0) {
        buf[0] = '\0';
    }
    return -1;
}

static void upper(char *s)
{
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
    }
}

int fleet_view_status(const struct fleet_game *game, char *buf, size_t n)
{
    char level[24];

    if (!game || !buf || n == 0) {
        return fail(buf, n);
    }
    fleet_view_difficulty_caption((enum fleet_difficulty)game->difficulty, level, sizeof(level));
    switch (game->phase) {
    case FLEET_PHASE_DEPLOY:
        snprintf(buf, n, "%s " DOT " DEPLOY", level);
        break;
    case FLEET_PHASE_OVER:
        snprintf(buf, n, "%s " DOT " COMPLETE", level);
        break;
    default:
        snprintf(buf, n, "%s " DOT " TURN %u", level, (unsigned)game->turn);
        break;
    }
    return 0;
}

int fleet_view_shot(int row, int col, enum fleet_shot_result result, int sunk_ship,
                    char *buf, size_t n)
{
    char cell[FLEET_CELL_NAME_MAX];
    char ship[24];

    if (!buf || n == 0 || fleet_cell_name(row, col, cell, sizeof(cell)) != 0) {
        return fail(buf, n);
    }
    if (result == FLEET_SHOT_SUNK && sunk_ship >= 0 && sunk_ship < FLEET_SHIP_COUNT) {
        snprintf(ship, sizeof(ship), "%s", fleet_ship_name((enum fleet_ship)sunk_ship));
        upper(ship);
        snprintf(buf, n, "%s SUNK %s", cell, ship);
        return 0;
    }
    snprintf(buf, n, "%s %s", cell, fleet_shot_result_name(result));
    return 0;
}

int fleet_view_exchange(const char *own, const char *enemy, char *buf, size_t n)
{
    if (!buf || n == 0) {
        return -1;
    }
    if (own && enemy) {
        snprintf(buf, n, "YOU %s " DOT " ENEMY %s", own, enemy);
    } else if (own) {
        snprintf(buf, n, "YOU %s", own);
    } else if (enemy) {
        snprintf(buf, n, "ENEMY %s", enemy);
    } else {
        buf[0] = '\0';
    }
    return 0;
}

int fleet_view_accuracy(const struct fleet_stats *stats, char *buf, size_t n)
{
    unsigned percent;

    if (!stats || !buf || n == 0) {
        return fail(buf, n);
    }
    percent = stats->shots ? (unsigned)((stats->hits * 100u + stats->shots / 2u) / stats->shots)
                           : 0u;
    snprintf(buf, n, "%u %% " DOT " %u OF %u", percent, (unsigned)stats->hits,
             (unsigned)stats->shots);
    return 0;
}

int fleet_view_afloat(const struct fleet_board *board, char *buf, size_t n)
{
    if (!board || !buf || n == 0) {
        return fail(buf, n);
    }
    snprintf(buf, n, "%u OF %u AFLOAT", (unsigned)board->ships_afloat, FLEET_SHIP_COUNT);
    return 0;
}

const char *fleet_view_difficulty_brief(enum fleet_difficulty difficulty)
{
    switch (difficulty) {
    case FLEET_RECRUIT:
        return "Fires at random and never follows up a hit.";
    case FLEET_OFFICER:
        return "Searches at random, then works outwards from every hit.";
    case FLEET_COMMANDER:
        return "Searches a lattice no ship can hide in and locks onto a hull.";
    case FLEET_ADMIRAL:
        return "Weighs every position your fleet could still occupy.";
    default:
        return "";
    }
}

int fleet_view_difficulty_caption(enum fleet_difficulty difficulty, char *buf, size_t n)
{
    if (!buf || n == 0) {
        return -1;
    }
    snprintf(buf, n, "%s", fleet_difficulty_name(difficulty));
    upper(buf);
    return 0;
}

int fleet_view_ship_line(enum fleet_ship ship, char *buf, size_t n)
{
    uint8_t length = fleet_ship_length(ship);

    if (!buf || n == 0 || length == 0) {
        return fail(buf, n);
    }
    snprintf(buf, n, "%s", fleet_ship_name(ship));
    return 0;
}

const char *fleet_view_outcome(const struct fleet_game *game)
{
    if (!game || game->phase != FLEET_PHASE_OVER) {
        return "";
    }
    return game->winner == FLEET_SIDE_PLAYER ? "Enemy fleet destroyed" : "Fleet lost";
}
