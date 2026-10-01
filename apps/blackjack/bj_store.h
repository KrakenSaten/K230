/*
 * PG Blackjack save file: the session in progress, exactly.
 *
 * This is the only part of PG Blackjack that touches the filesystem. The
 * rules and the view model under apps/blackjack/engine and ui do no I/O
 * (tests/bj_lint.sh).
 *
 * Location: $POCKETOS_STATE_DIR/blackjack/game.v1, default
 * /var/lib/pocketos/blackjack/game.v1. App-owned storage in the pattern
 * PocketFleet established (docs/apps/POCKETFLEET.md, D2) and PG 2048 follows;
 * recorded as a deviation pending the owner (docs/apps/PGBLACKJACK.md).
 *
 * What is kept is the whole game: the bankroll, the bet, the stake on the
 * table, the last result, the round counter, the phase and outcome, the
 * double and new-shoe flags, the shoe's generator state and shuffle count,
 * the shoe's cards in order, and both hands. A hand left half played comes
 * back half played, with the same cards still to come, so leaving and
 * reopening can neither change a round nor move a chip. What is not kept is
 * the interaction - the last refused command and its caption.
 *
 * 181 bytes, little-endian, always:
 *
 *    0  magic "PGBJ"                                   4
 *    4  version (1)                                    2
 *    6  bankroll                                       4
 *   10  bet                                            4
 *   14  stake                                          4
 *   18  last result, signed                            4
 *   22  rounds                                         4
 *   26  phase, outcome, doubled, new shoe              4
 *   30  generator state                                4
 *   34  shuffles                                       4
 *   38  shoe: count, then 104 card slots             105
 *  143  player: count, then 16 card slots             17
 *  160  dealer: count, then 16 card slots             17
 *  177  FNV-1a of bytes 0..176                         4
 *
 * Slots past a count are written 0xFF and must read back so, which makes the
 * encoding of a game unique.
 *
 * A file that is not exactly that - damaged, truncated, padded, another
 * version, or a position bj_game_valid() refuses (a card more often than the
 * shoe holds it, a hand that should have settled, a result that does not
 * match the stake) - is refused whole: nothing of it is restored, it is left
 * where it is, and the next save replaces it. Writes are atomic (temp file,
 * fsync, rename).
 *
 * Nothing in it is secret. The chips are not money.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PGBJ_STORE_H
#define PGBJ_STORE_H

#include "engine/bj_rules.h"

#include <stddef.h>

#define BJ_STORE_DEFAULT_DIR "/var/lib/pocketos"
#define BJ_STORE_SUBDIR "blackjack"
#define BJ_STORE_FILE "game.v1"

#define BJ_SAVE_VERSION 1
#define BJ_SAVE_SIZE 181

const char *bj_store_dir(void);
const char *bj_store_path(void);

/* Encode a game: BJ_SAVE_SIZE, or -1 when buf is too small or the game is
 * not a valid position. Pure, no I/O. */
int bj_save_encode(const struct bj_game *g, uint8_t *buf, size_t n);
/* Decode into g: 0, or -1 leaving g untouched. */
int bj_save_decode(struct bj_game *g, const uint8_t *buf, size_t n);

/* Write the game: 0, or -1 when it could not be stored. */
int bj_store_save(const struct bj_game *g);
/* Read it: 0 on success, 1 when there is no file, -1 when there is one that
 * cannot be used. g is untouched unless 0 is returned. */
int bj_store_load(struct bj_game *g);

#endif
