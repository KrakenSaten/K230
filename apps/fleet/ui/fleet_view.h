/*
 * PocketFleet view model: match state turned into the short strings the
 * screens display. No LVGL here, so the wording stays testable and the
 * screen modules contain layout rather than formatting.
 *
 * Every function writes at most n bytes including the terminator and returns
 * 0, or -1 when the arguments are unusable (buf then holds "" when it can).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_VIEW_H
#define POCKETFLEET_VIEW_H

#include "../engine/fleet_rules.h"

/* "ADMIRAL · TURN 14", for the app header's right caption. */
int fleet_view_status(const struct fleet_game *game, char *buf, size_t n);
/* "D7 HIT", "D7 SUNK CRUISER", "D7 MISS". */
int fleet_view_shot(int row, int col, enum fleet_shot_result result, int sunk_ship,
                    char *buf, size_t n);
/* "YOU D7 HIT · ENEMY B3 MISS", the battle log line. */
int fleet_view_exchange(const char *own, const char *enemy, char *buf, size_t n);
/* "47 % · 8 OF 17". */
int fleet_view_accuracy(const struct fleet_stats *stats, char *buf, size_t n);
/* "3 OF 5 AFLOAT". */
int fleet_view_afloat(const struct fleet_board *board, char *buf, size_t n);
/* One line describing how a difficulty plays, for the Command screen. */
const char *fleet_view_difficulty_brief(enum fleet_difficulty difficulty);
/* Upper-case difficulty name, for captions (DS §3: captions are uppercase). */
int fleet_view_difficulty_caption(enum fleet_difficulty difficulty, char *buf, size_t n);
/* "CARRIER · 5" style roster line. */
int fleet_view_ship_line(enum fleet_ship ship, char *buf, size_t n);
/* Outcome heading for the Result screen. */
const char *fleet_view_outcome(const struct fleet_game *game);

#endif
