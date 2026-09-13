/*
 * What counts as a sendable Wave message, in one place for the app and its
 * helper: well-formed UTF-8 (shortest form, no surrogates, nothing above
 * U+10FFFF) with no control character (C0, DEL, C1).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETWAVE_TEXT_H
#define POCKETWAVE_TEXT_H

#include <stddef.h>

/* 1 when bytes[0..len) is clean text by the rule above; 0 otherwise. An
 * empty buffer is clean; whether empty is allowed is the caller's rule. */
int wave_text_clean(const char *bytes, size_t len);

#endif
