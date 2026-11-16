/*
 * PacketShark's saved captures, as the app lists and deletes them (M8;
 * ADR-016 rules 9 and 10): the .pcap files the capture helper wrote to the
 * captures folder (/root/Captures on the unit, ppc_captures_dir()).
 *
 * The app never opens a capture: it reads the folder's names, sizes and
 * dates, and removes one file when the owner confirms. Only names the
 * helper could have made (ppc_save_name_ok()) are listed or deleted, only
 * regular files, and never through a link, so nothing outside the folder
 * can be named. No packet byte is read here: the shell stays free of the
 * decoder (docs/apps/PACKETSHARK.md §3).
 *
 * Pure C, no LVGL (tests/psk_session_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PSK_SAVES_H
#define PSK_SAVES_H

#include "pocketpcap/ppc_proto.h"

#include <stddef.h>
#include <stdint.h>

#define PSK_SAVES_SCAN 512 /* entries of the folder looked at, at most */

struct psk_saved {
    char name[PPC_SAVE_NAME];
    uint64_t bytes;
    int64_t mtime; /* seconds since 1970 */
};

/* The newest `max` saved captures, newest first. Returns how many were put
 * in out; *count and *bytes (either may be NULL) get the totals of every
 * capture in the folder. A folder that is not there is an empty one. */
int psk_saves_list(struct psk_saved *out, int max, uint32_t *count, uint64_t *bytes);

/* Delete one saved capture by name. 0, or -1 when the name is not one the
 * helper makes, is not a regular file in the folder, or cannot be removed. */
int psk_saves_delete(const char *name);

/* "JUST NOW", "12 MIN AGO", "3 H AGO", "5 D AGO"; "" when either clock
 * reading is from before the clock was set. */
const char *psk_saves_age(int64_t mtime, int64_t now, char *buf, size_t buflen);

#endif
