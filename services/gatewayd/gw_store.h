/*
 * gatewayd: the paired clients (ADR-019 rule 18). One small text file,
 * <state>/gatewayd/paired.v1, 0600 in a 0700 directory. It holds no secret:
 * a fingerprint, a name, the scopes and when, per client.
 *
 *   doors-gatewayd-paired 1
 *   client <64 hex> <scopes, hex> <paired wall seconds> <name, hex>
 *
 * Rules:
 *   - written by temp file, fsync, rename, fsync of the directory: a reader
 *     sees the old list or the new one, and a revocation that was
 *     acknowledged survives a power cut;
 *   - a file that does not parse to the last byte is a FAULT, not an empty
 *     list. Reading a damaged list as "nobody is paired" would be harmless;
 *     reading it as anything else would not, and telling the two apart is
 *     not this code's to guess. The service stays closed (state_fault);
 *   - an absent file is an empty list.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_STORE_H
#define GATEWAYD_GW_STORE_H

#include "gw.h"

#include <stdbool.h>
#include <stdint.h>

#define GW_STORE_PATH_MAX 512
#define GW_STORE_FILE "paired.v1"

struct gw_client {
    uint8_t fp[GW_FP_LEN];
    unsigned scopes;
    int64_t paired_wall_s;      /* 0: the clock was not set */
    char name[GW_NAME_MAX + 1];
};

struct gw_store {
    char dir[GW_STORE_PATH_MAX];
    struct gw_client c[GW_MAX_CLIENTS];
    int n;
};

/* Makes dir (0700) if needed. 0, or -1. */
int gw_store_dir(const char *dir);
/* True when the paired file exists (whatever it holds). */
bool gw_store_exists(const char *dir);

/* 0: loaded, or absent and empty. -1: unreadable or damaged; st is empty. */
int gw_store_load(struct gw_store *st, const char *dir);
/* Durable when it returns 0. -1: the file on storage is still the old one. */
int gw_store_save(const struct gw_store *st);

/* The index of the client with this key, or -1. Every entry is compared,
 * in constant time each. */
int gw_store_find(const struct gw_store *st, const uint8_t fp[GW_FP_LEN]);
/* In memory only; save afterwards. -1: full, already there, or a bad name
 * or scope set. */
int gw_store_add(struct gw_store *st, const uint8_t fp[GW_FP_LEN], const char *name, unsigned scopes,
                 int64_t wall_s);
void gw_store_remove(struct gw_store *st, int idx);

#endif
