/* Relay Nine's scene scaler: the RN_W x RN_H RGB565 frame resized in the
 * app into a display-sized buffer, so LVGL draws it unscaled (a line copy)
 * instead of running its per-pixel image transform every frame.
 *
 * Sampling is pixel-centre aligned, so a same-size target is an exact copy.
 * Filtered mode is bilinear with 1/32 weights on packed RGB565, rounded;
 * nearest mode picks one source pixel per target pixel and copies repeated
 * rows. Pure C without LVGL, so the engine tests drive it directly.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef RELAY_SCALE_H
#define RELAY_SCALE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct rn_scaler {
    int sw,sh,w,h;      /* source and target size */
    uint16_t *pixels;   /* w*h target, row stride w */
    /* Per target column/row: first source index and the weight of the next
     * one (0..32) for filtered mode, and the nearest source index. */
    uint16_t *x0,*xn,*y0,*yn;uint8_t *fx,*fy;
    uint32_t *row[2];int row_y[2]; /* horizontally filtered source rows */
    uint32_t *spread;   /* one source row in packed form */
};
/* Bytes rn_scaler_resize allocates for a target size, for the memory budget. */
size_t rn_scaler_bytes(int sw,int w,int h);
/* Sets the source and target size; reallocates only when they change.
 * Returns false (and leaves the scaler empty) when allocation fails or a
 * size is out of range. */
bool rn_scaler_resize(struct rn_scaler *s,int sw,int sh,int w,int h);
void rn_scaler_free(struct rn_scaler *s);
/* Scales src (sw*sh, stride sw) into s->pixels. */
void rn_scale(struct rn_scaler *s,const uint16_t *src,bool filtered);
#endif
