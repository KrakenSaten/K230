/*
 * Files: what an entry says on screen - its type, size and time, a path
 * shortened to fit, a name made safe to draw. LVGL-free (tests/files_view_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef FILES_VIEW_H
#define FILES_VIEW_H

#include "files_fs.h"

#include <stddef.h>
#include <stdint.h>

/* "0 B", "999 B", "1.5 KB", "12 KB", "3.2 MB", "1.0 GB" (1024-based); ""
 * when unknown (bytes < 0). */
void files_view_size(char *out, size_t out_len, int64_t bytes);

/* "Folder", "Link to folder", "Link", "Broken link", "TXT file", "File",
 * "Device or pipe". */
void files_view_type(char *out, size_t out_len, const struct files_entry *e);

/* "YYYY-MM-DD HH:MM" local time; "" when unknown (t <= 0). */
void files_view_when(char *out, size_t out_len, int64_t t);

/* A row's second line: "Folder · 2026-09-23 10:00", "12 KB · 2026-09-23 10:00". */
void files_view_caption(char *out, size_t out_len, const struct files_entry *e);

/* The name as it can be drawn: bytes that are not UTF-8 become '?'. */
void files_view_name(char *out, size_t out_len, const char *name);

/* path shortened from the front to at most max_chars characters, keeping
 * the end, which is where the owner is: "/a/b/c/d" at 6 -> "…/c/d". The
 * last component is kept whole even when it alone is longer (the label
 * shortens that end itself). */
void files_view_path(char *out, size_t out_len, const char *path, int max_chars);

/* "Name", "Type", "Size", "Date". */
const char *files_view_sort_name(enum files_sort key);

#endif
