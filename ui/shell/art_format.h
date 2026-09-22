/*
 * The DOORS runtime art files (ui/assets/doors, installed in
 * /usr/share/doors/ui): what one is allowed to look like. Pure C, no LVGL,
 * so the host suite can hold every rule without a display.
 *
 * A file is LVGL 9's own image layout: the 12-byte lv_image_header_t (little
 * endian: magic 0x19, colour format, flags, width, height, stride, reserved)
 * followed by exactly the pixel data that header describes. Only the three
 * formats tools/design/gen_doors_ui.py writes are accepted, and the size must
 * match to the byte: a truncated or padded file is refused rather than drawn
 * with garbage in its last rows.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef DOORS_ART_FORMAT_H
#define DOORS_ART_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define ART_HEADER_SIZE 12
#define ART_MAGIC 0x19
#define ART_CF_A8 0x0E
#define ART_CF_RGB565 0x12
#define ART_CF_RGB565A8 0x14
/* No art is larger than the panel; anything bigger is not ours. */
#define ART_MAX_SIDE 2048

struct art_header {
    uint8_t cf;
    uint16_t w;
    uint16_t h;
    uint16_t stride;
    size_t data_size; /* bytes after the header */
};

/* Parse and check the header at buf (at least ART_HEADER_SIZE bytes) of a
 * file file_size bytes long. Returns 0, or -1 with *why set to a static
 * reason. */
int art_header_parse(const uint8_t *buf, size_t file_size, struct art_header *out, const char **why);

/* "<dir>/<name>.bin" into out; -1 when it does not fit or the name is not a
 * plain file name ([a-z0-9-], 1..48 characters). */
int art_path(const char *dir, const char *name, char *out, size_t out_len);

#endif
