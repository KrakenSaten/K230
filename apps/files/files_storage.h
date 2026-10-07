/*
 * Files: what the Storage screen says about the two places it lists -
 * Internal Storage and the USB drive. LVGL-free (tests/files_storage_test.c).
 *
 * The USB drive is sysd's: Files never mounts, unmounts or ejects anything
 * itself. It reads sysd's storage.status, asks for storage.eject, and shows
 * the answer (docs/api/system.md, "Storage"). This file turns that answer
 * into a struct and into the words on screen.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef FILES_STORAGE_H
#define FILES_STORAGE_H

#include "files_fs.h"

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum files_usb_state {
    FILES_USB_UNKNOWN = 0, /* sysd did not answer */
    FILES_USB_ABSENT,
    FILES_USB_MOUNTED,
    FILES_USB_EJECTING,
    FILES_USB_EJECTED,     /* safe to remove */
    FILES_USB_UNSUPPORTED, /* not FAT: exFAT, NTFS or not recognised */
    FILES_USB_ERROR
};

struct files_usb {
    enum files_usb_state state;
    char fs[16];                 /* "FAT32"; "" when not known */
    char label[48];              /* "" when the drive has none */
    char path[FILES_PATH_MAX];   /* where it is mounted, when it is */
    int64_t total;               /* -1 when not known */
    int64_t free;
    char error[160];
};

/* sysd's storage.status result (NULL when sysd did not answer). */
void files_usb_parse(const cJSON *result, struct files_usb *u);

/* The USB Drive row's second line: "SANDISK · FAT32 · 52 GB free of 57 GB",
 * "Not connected", "Ejecting…", "Safe to remove", "exFAT is not supported -
 * use FAT32"... */
void files_usb_caption(char *out, size_t out_len, const struct files_usb *u);

/* The Internal Storage row's second line: "312 MB free of 574 MB", or ""
 * when it could not be read (total < 0). */
void files_space_caption(char *out, size_t out_len, int64_t total, int64_t avail);

/* Eject can be offered: the drive is mounted. */
bool files_usb_can_eject(const struct files_usb *u);

#endif
