/*
 * PG Solitaire save file: the deal in progress.
 *
 * This is the only part of PG Solitaire that touches the filesystem. The
 * rules and the view model under apps/solitaire/engine and ui do no I/O
 * (tests/sol_lint.sh).
 *
 * Location: $POCKETOS_STATE_DIR/solitaire/game.v1, default
 * /var/lib/pocketos/solitaire/game.v1. App-owned storage in the pattern
 * PocketFleet established (docs/apps/POCKETFLEET.md, D2) and PG 2048 follows;
 * recorded as a deviation pending the owner (docs/apps/PGSOLITAIRE.md).
 *
 * What is kept is the game, exactly: every pile with its cards in order and
 * how many of them lie face down, the seed, the move and pass counters and
 * whether it is won. The waste and the stock are piles like any other, so
 * where the next draw comes from is kept by construction. What is not kept
 * is the interaction - the cursor, a selection, a caption - which a resumed
 * game starts fresh.
 *
 * Every card is in exactly one pile, so the piles always hold 52 cards and
 * the file is always 101 bytes, little-endian:
 *
 *   0  magic "PGSL"                         4
 *   4  version (1)                           2
 *   6  seed                                  4
 *  10  moves                                 4
 *  14  passes                                4
 *  18  won                                   1
 *  19  thirteen piles, stock first: n, down 26
 *  45  the 52 cards, pile by pile, bottom first 52
 *  97  FNV-1a of bytes 0..96                 4
 *
 * A file that is not exactly that - damaged, truncated, padded, another
 * version, or a position sol_game_valid() refuses (a card missing or twice,
 * a foundation out of order, a face-down card on top of a column) - is
 * refused whole: nothing of it is restored, it is left where it is, and the
 * next save replaces it. Writes are atomic (temp file, fsync, rename).
 *
 * Nothing in it is secret.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGSOL_STORE_H
#define PGSOL_STORE_H

#include "engine/sol_rules.h"

#include <stddef.h>

#define SOL_STORE_DEFAULT_DIR "/var/lib/pocketos"
#define SOL_STORE_SUBDIR "solitaire"
#define SOL_STORE_FILE "game.v1"

#define SOL_SAVE_VERSION 1
#define SOL_SAVE_SIZE 101

const char *sol_store_dir(void);
const char *sol_store_path(void);

/* Encode a game: SOL_SAVE_SIZE, or -1 when buf is too small or the game is
 * not a valid position. Pure, no I/O. */
int sol_save_encode(const struct sol_game *g, uint8_t *buf, size_t n);
/* Decode into g: 0, or -1 leaving g untouched. */
int sol_save_decode(struct sol_game *g, const uint8_t *buf, size_t n);

/* Write the game: 0, or -1 when it could not be stored. */
int sol_store_save(const struct sol_game *g);
/* Read it: 0 on success, 1 when there is no file, -1 when there is one that
 * cannot be used. g is untouched unless 0 is returned. */
int sol_store_load(struct sol_game *g);

#endif
