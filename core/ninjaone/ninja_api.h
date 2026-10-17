/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef NINJA_API_H
#define NINJA_API_H
#include "ninja_model.h"
#include <stddef.h>
/* JSON decoding belongs exclusively to the helper. kind: o, d, a.
 * Paginated pages require strictly increasing IDs after the supplied cursor. */
int ninja_decode(struct ninja_snapshot *s, char kind, const char *body, size_t len,
                 int after, int *next, unsigned *rows);
typedef int (*ninja_get_fn)(void *ctx, const char *path, char **body, size_t *len, int *retry_s);
int ninja_collect(struct ninja_snapshot *s, ninja_get_fn get, void *ctx, unsigned page_size);
int ninja_base_valid(const char *base);
int ninja_token_read(char *token, size_t cap);
int ninja_token_write(const char *token);
#endif
