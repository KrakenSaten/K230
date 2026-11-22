/*
 * gatewayd: the bounded frame reader of the Companion protocol
 * (docs/api/gateway.md §1): a 4-byte big-endian length, then that many
 * bytes. One fixed buffer per connection, allocated once; a peer cannot
 * make it grow, and a length of zero or above the limit ends the connection
 * before a byte of the body is kept.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_FRAME_H
#define GATEWAYD_GW_FRAME_H

#include <stddef.h>
#include <stdint.h>

struct gw_frame {
    uint8_t *buf;   /* max + 4 bytes */
    size_t len;     /* bytes held */
    size_t max;     /* the largest body accepted */
};

/* 0, or -1 when there is no memory. */
int gw_frame_init(struct gw_frame *f, size_t max);
void gw_frame_free(struct gw_frame *f);

/* Where to read into, and how much fits. *n is 0 when a whole frame is
 * waiting to be taken. */
uint8_t *gw_frame_space(struct gw_frame *f, size_t *n);
void gw_frame_added(struct gw_frame *f, size_t n);

/* 1: a frame, body at *p of *n bytes, valid until gw_frame_consume().
 * 0: more bytes are needed. -1: the length is zero or over the limit. */
int gw_frame_next(const struct gw_frame *f, const uint8_t **p, size_t *n);
/* Drops the frame gw_frame_next() returned. */
void gw_frame_consume(struct gw_frame *f);

#endif
