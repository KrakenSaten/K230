/*
 * Recording file names: made by the app, checked again by the helper before
 * it creates anything, and parsed by both. Pure C, no filesystem.
 *
 *   REC-<yyyymmdd>-<hhmmss>[-<n>][-recovered].wav   wall clock set
 *   REC-<nnnn>[-<n>][-recovered].wav                wall clock not set
 *
 * The board has no RTC, so after a cold boot without a network the wall
 * clock is usually not set, and a date made from it would be 1970 and false.
 * Then the name is a sequence number, one past the highest in the folder, at
 * least four digits. <n> (2..999) separates two recordings that would have
 * had the same name; "-recovered" marks one that was repaired after an
 * interruption. A recording is written as <name>.part and renamed to <name>
 * only once it is complete.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETREC_NAMES_H
#define POCKETREC_NAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define REC_NAME_MAX 64          /* terminator included; every recording name fits */
#define REC_FILE_NAME_MAX 256    /* any name the list shows */
#define REC_PART_SUFFIX ".part"
#define REC_WAV_SUFFIX ".wav"
#define REC_SEQ_MAX 999999u
#define REC_DUP_MAX 999u

struct rec_name {
    char stamp[16];   /* "20260926-101500" or "0042" */
    unsigned dup;     /* 0 or 2..REC_DUP_MAX */
    bool recovered;
};

/* Parse a recording name (with .wav, without .part). */
bool rec_name_parse(const char *name, struct rec_name *out);
/* Build one back. 0, or -1 when it does not fit or the parts are invalid. */
int rec_name_build(char *out, size_t n, const struct rec_name *parts);

/* The stamp for a wall-clock time (tm valid) or a sequence number (tm NULL). */
void rec_name_stamp(struct rec_name *out, const struct tm *tm, unsigned seq);

/* The sequence number of a name made without a clock, else 0. */
unsigned rec_name_seq(const char *name);

/* Whether a directory entry is shown in the list: a visible name ending in
 * .wav (any case) or a recording's .part, at most REC_FILE_NAME_MAX - 1
 * bytes, no '/' or control characters. */
bool rec_name_listable(const char *name);
/* Whether name is a recording's temporary file (<recording name>.part). */
bool rec_name_is_part(const char *name);

#endif
