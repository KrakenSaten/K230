/*
 * pocketcam's photo metadata: the few facts Doors knows about a photo when it
 * is taken, written into the file as standard EXIF (JPEG) or as comment lines
 * (PPM), and read back from any file the gallery shows.
 *
 * WHAT IS WRITTEN. Only what is actually known:
 *
 *   Orientation       always 1: the encoder turns every still upright itself
 *   Software          "Doors <version>"
 *   DateTime,         the wall clock at capture, local time - only when the
 *   DateTimeOriginal  clock is valid (POCKETCAM_WALL_VALID_FROM). This board
 *                     has no RTC; after a cold boot without a network the
 *                     clock is unset, and then no date is written at all
 *                     rather than a made-up 1970 one.
 *   ImageDescription  "Simulated picture" under the fake backend, so a test
 *                     pattern is never mistaken for a photo
 *
 * Nothing else: no make or model (the v4l2 backend does not identify the
 * sensor at run time), no exposure, no GPS - the hardware reports none of it.
 *
 * WHAT IS READ. Orientation, DateTimeOriginal (else DateTime), Software and
 * ImageDescription from any EXIF block, little- or big-endian, with every
 * offset and count checked against the block: a damaged or hostile block
 * yields fewer facts, never a read outside it.
 *
 * Pure C: no libjpeg, no I/O. Tested in tests/pocketcam_gallery_test.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_POCKETCAM_EXIF_H
#define POCKETOS_POCKETCAM_EXIF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* "YYYY:MM:DD HH:MM:SS" and its terminator: EXIF's own date format. */
#define POCKETCAM_EXIF_DATE_LEN 20
#define POCKETCAM_EXIF_TEXT_MAX 64
/* The APP1 payload the writer produces is never larger than this. */
#define POCKETCAM_EXIF_MAX 512

/* What a photo is known to be when it is taken. */
struct pocketcam_photo_meta {
    int64_t taken;      /* wall clock, epoch seconds; below the valid floor: unknown */
    bool simulated;     /* the fake backend's test pattern */
};

/* What a file says about itself. Empty strings and orientation 1 when it
 * says nothing. */
struct pocketcam_exif {
    int orientation;                          /* 1..8, EXIF's numbering */
    char taken[POCKETCAM_EXIF_DATE_LEN];      /* "YYYY:MM:DD HH:MM:SS" or "" */
    char software[POCKETCAM_EXIF_TEXT_MAX];
    char description[POCKETCAM_EXIF_TEXT_MAX];
};

void pocketcam_exif_clear(struct pocketcam_exif *e);

/* The date for meta->taken in EXIF's format, local time, into out (at least
 * POCKETCAM_EXIF_DATE_LEN). false, and out empty, when the clock was not set. */
bool pocketcam_exif_date(int64_t taken, char *out, size_t out_len);

/* An APP1 payload ("Exif\0\0" and a little-endian TIFF block) for meta into
 * buf. Returns its length, or 0 when buf is too small. */
size_t pocketcam_exif_build(const struct pocketcam_photo_meta *meta, uint8_t *buf, size_t len);

/* Parse an APP1 payload (starting "Exif\0\0"). Returns 0 with out filled
 * (fields it did not find left empty), or -1 when this is not EXIF at all. */
int pocketcam_exif_parse(const uint8_t *data, size_t len, struct pocketcam_exif *out);

/* The same facts as PPM comment lines ("# doors-taken ...\n" and so on) into
 * buf, terminated. Returns the length written (0 when nothing to say or no
 * room). */
size_t pocketcam_exif_ppm_comments(const struct pocketcam_photo_meta *meta, char *buf, size_t len);

/* Read one PPM comment line (without "#" and the newline) into out, when it
 * is one of ours. */
void pocketcam_exif_ppm_parse_comment(const char *line, struct pocketcam_exif *out);

/* Whether s is a well-formed EXIF date: "YYYY:MM:DD HH:MM:SS" with plausible
 * fields (a camera with no clock may write zeros or spaces there). */
bool pocketcam_exif_date_valid(const char *s);

#endif
