/*
 * pocketcam's photo metadata. See pocketcam_exif.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketcam_exif.h"
#include "pocketcam_store.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef POCKETOS_VERSION
#define POCKETOS_VERSION "unknown"
#endif

#define TAG_DESCRIPTION 0x010E
#define TAG_ORIENTATION 0x0112
#define TAG_SOFTWARE 0x0131
#define TAG_DATETIME 0x0132
#define TAG_EXIF_IFD 0x8769
#define TAG_DATETIME_ORIGINAL 0x9003
#define TYPE_ASCII 2
#define TYPE_SHORT 3
#define TYPE_LONG 4
/* More entries than any real IFD0 has: a count past this is damage. */
#define IFD_ENTRIES_MAX 128

static const char exif_magic[6] = { 'E', 'x', 'i', 'f', 0, 0 };
static const char simulated_text[] = "Simulated picture";

void pocketcam_exif_clear(struct pocketcam_exif *e)
{
    memset(e, 0, sizeof(*e));
    e->orientation = 1;
}

bool pocketcam_exif_date(int64_t taken, char *out, size_t out_len)
{
    time_t t = (time_t)taken;
    struct tm tm;

    if (out_len) {
        out[0] = '\0';
    }
    if (taken < POCKETCAM_WALL_VALID_FROM || out_len < POCKETCAM_EXIF_DATE_LEN ||
        !localtime_r(&t, &tm)) {
        return false;
    }
    snprintf(out, out_len, "%04d:%02d:%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return true;
}

static bool in_range(const char *s, int from, int n, int lo, int hi)
{
    int v = 0;
    int i;

    for (i = from; i < from + n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (s[i] - '0');
    }
    return v >= lo && v <= hi;
}

bool pocketcam_exif_date_valid(const char *s)
{
    return s && strlen(s) == POCKETCAM_EXIF_DATE_LEN - 1 && s[4] == ':' && s[7] == ':' &&
           s[10] == ' ' && s[13] == ':' && s[16] == ':' && in_range(s, 0, 4, 1970, 9999) &&
           in_range(s, 5, 2, 1, 12) && in_range(s, 8, 2, 1, 31) && in_range(s, 11, 2, 0, 23) &&
           in_range(s, 14, 2, 0, 59) && in_range(s, 17, 2, 0, 60);
}

/* ---- writing ------------------------------------------------------------------ */

struct out {
    uint8_t *buf;
    size_t len;
    size_t tiff;  /* where the TIFF block starts: offsets count from here */
    bool full;
};

static void put16(struct out *o, size_t at, uint16_t v)
{
    if (at + 2 > o->len) {
        o->full = true;
        return;
    }
    o->buf[at] = (uint8_t)v;
    o->buf[at + 1] = (uint8_t)(v >> 8);
}

static void put32(struct out *o, size_t at, uint32_t v)
{
    put16(o, at, (uint16_t)v);
    put16(o, at + 2, (uint16_t)(v >> 16));
}

struct entry {
    uint16_t tag;
    uint16_t type;
    const char *text;  /* ASCII, or NULL */
    uint32_t value;    /* SHORT or LONG */
};

/* An IFD of n entries at *at; ASCII values that do not fit in four bytes go
 * to *data. Both advance. The next-IFD link is 0. */
static void put_ifd(struct out *o, size_t *at, size_t *data, const struct entry *e, int n)
{
    size_t p = *at;
    int i;

    put16(o, p, (uint16_t)n);
    p += 2;
    for (i = 0; i < n; i++, p += 12) {
        put16(o, p, e[i].tag);
        put16(o, p + 2, e[i].type);
        if (e[i].type == TYPE_ASCII) {
            size_t count = strlen(e[i].text) + 1;

            put32(o, p + 4, (uint32_t)count);
            if (count <= 4) {
                put32(o, p + 8, 0);
                if (p + 8 + count <= o->len) {
                    memcpy(o->buf + p + 8, e[i].text, count);
                }
            } else {
                put32(o, p + 8, (uint32_t)(*data - o->tiff));
                if (*data + count > o->len) {
                    o->full = true;
                } else {
                    memcpy(o->buf + *data, e[i].text, count);
                }
                *data += count + (count & 1); /* word-aligned, as TIFF asks */
            }
        } else {
            put32(o, p + 4, 1);
            if (e[i].type == TYPE_SHORT) {
                put16(o, p + 8, (uint16_t)e[i].value);
                put16(o, p + 10, 0);
            } else {
                put32(o, p + 8, e[i].value);
            }
        }
    }
    put32(o, p, 0);
    *at = p + 4;
}

static size_t ifd_data_bytes(const struct entry *e, int n)
{
    size_t total = 0;
    int i;

    for (i = 0; i < n; i++) {
        if (e[i].type == TYPE_ASCII && strlen(e[i].text) + 1 > 4) {
            size_t c = strlen(e[i].text) + 1;

            total += c + (c & 1);
        }
    }
    return total;
}

size_t pocketcam_exif_build(const struct pocketcam_photo_meta *meta, uint8_t *buf, size_t len)
{
    char date[POCKETCAM_EXIF_DATE_LEN];
    char software[48];
    struct entry ifd0[5];
    struct entry exif[1];
    struct out o = { buf, len, sizeof(exif_magic), false };
    bool dated = meta && pocketcam_exif_date(meta->taken, date, sizeof(date));
    int n0 = 0;
    size_t at;
    size_t data;
    size_t ifd0_end;

    if (!buf || len < sizeof(exif_magic) + 8) {
        return 0;
    }
    snprintf(software, sizeof(software), "Doors %s", POCKETOS_VERSION);
    /* Tags in ascending order, as TIFF requires. */
    if (meta && meta->simulated) {
        ifd0[n0++] = (struct entry){ TAG_DESCRIPTION, TYPE_ASCII, simulated_text, 0 };
    }
    ifd0[n0++] = (struct entry){ TAG_ORIENTATION, TYPE_SHORT, NULL, 1 };
    ifd0[n0++] = (struct entry){ TAG_SOFTWARE, TYPE_ASCII, software, 0 };
    if (dated) {
        ifd0[n0++] = (struct entry){ TAG_DATETIME, TYPE_ASCII, date, 0 };
        ifd0[n0++] = (struct entry){ TAG_EXIF_IFD, TYPE_LONG, NULL, 0 };
        exif[0] = (struct entry){ TAG_DATETIME_ORIGINAL, TYPE_ASCII, date, 0 };
    }

    memcpy(buf, exif_magic, sizeof(exif_magic));
    /* The TIFF header: little-endian, 42, IFD0 at 8. */
    buf[o.tiff] = 'I';
    buf[o.tiff + 1] = 'I';
    put16(&o, o.tiff + 2, 42);
    put32(&o, o.tiff + 4, 8);
    at = o.tiff + 8;
    ifd0_end = at + 2 + 12 * (size_t)n0 + 4;
    data = ifd0_end;
    if (dated) {
        /* The Exif IFD goes after IFD0's own data. */
        ifd0[n0 - 1].value = (uint32_t)(ifd0_end + ifd_data_bytes(ifd0, n0) - o.tiff);
    }
    put_ifd(&o, &at, &data, ifd0, n0);
    if (dated) {
        at = data;
        data = at + 2 + 12 + 4;
        put_ifd(&o, &at, &data, exif, 1);
    }
    return o.full ? 0 : data;
}

/* ---- reading ------------------------------------------------------------------ */

struct in {
    const uint8_t *t;  /* the TIFF block */
    size_t len;
    bool big;
};

static bool get16(const struct in *in, size_t at, uint16_t *v)
{
    if (at + 2 > in->len || at + 2 < at) {
        return false;
    }
    *v = in->big ? (uint16_t)(in->t[at] << 8 | in->t[at + 1])
                 : (uint16_t)(in->t[at + 1] << 8 | in->t[at]);
    return true;
}

static bool get32(const struct in *in, size_t at, uint32_t *v)
{
    uint16_t a;
    uint16_t b;

    if (!get16(in, at, &a) || !get16(in, at + 2, &b)) {
        return false;
    }
    *v = in->big ? (uint32_t)a << 16 | b : (uint32_t)b << 16 | a;
    return true;
}

/* An ASCII value, printable characters only, into out. */
static void get_text(const struct in *in, size_t entry, char *out, size_t out_len)
{
    uint32_t count;
    uint32_t off;
    size_t at;
    size_t i;
    size_t n = 0;

    out[0] = '\0';
    if (!get32(in, entry + 4, &count) || count == 0) {
        return;
    }
    if (count <= 4) {
        at = entry + 8;
    } else if (get32(in, entry + 8, &off)) {
        at = off;
    } else {
        return;
    }
    if (at >= in->len || count > in->len - at) {
        return;
    }
    for (i = 0; i < count && n + 1 < out_len; i++) {
        uint8_t c = in->t[at + i];

        if (c == 0) {
            break;
        }
        out[n++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    out[n] = '\0';
    /* Trailing spaces, which some writers pad with. */
    while (n > 0 && out[n - 1] == ' ') {
        out[--n] = '\0';
    }
}

/* Walk one IFD. Returns the Exif IFD's offset when IFD0 names one, else 0. */
static uint32_t walk_ifd(const struct in *in, uint32_t off, struct pocketcam_exif *out,
                         char *datetime, size_t datetime_len)
{
    uint16_t n;
    uint16_t i;
    uint32_t sub = 0;

    if (!get16(in, off, &n) || n > IFD_ENTRIES_MAX) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        size_t e = (size_t)off + 2 + (size_t)i * 12;
        uint16_t tag;
        uint16_t type;

        if (!get16(in, e, &tag) || !get16(in, e + 2, &type) || e + 12 > in->len) {
            break;
        }
        if (tag == TAG_ORIENTATION && type == TYPE_SHORT) {
            uint16_t v;

            if (get16(in, e + 8, &v) && v >= 1 && v <= 8) {
                out->orientation = v;
            }
        } else if (tag == TAG_SOFTWARE && type == TYPE_ASCII) {
            get_text(in, e, out->software, sizeof(out->software));
        } else if (tag == TAG_DESCRIPTION && type == TYPE_ASCII) {
            get_text(in, e, out->description, sizeof(out->description));
        } else if (tag == TAG_DATETIME && type == TYPE_ASCII) {
            get_text(in, e, datetime, datetime_len);
        } else if (tag == TAG_DATETIME_ORIGINAL && type == TYPE_ASCII) {
            char d[POCKETCAM_EXIF_DATE_LEN + 8];

            get_text(in, e, d, sizeof(d));
            if (pocketcam_exif_date_valid(d)) {
                memcpy(out->taken, d, sizeof(out->taken)); /* 19 characters and the NUL */
            }
        } else if (tag == TAG_EXIF_IFD && (type == TYPE_LONG || type == TYPE_SHORT)) {
            uint32_t v = 0;
            uint16_t s = 0;

            if (type == TYPE_LONG ? get32(in, e + 8, &v) : get16(in, e + 8, &s)) {
                sub = type == TYPE_LONG ? v : s;
            }
        }
    }
    return sub;
}

int pocketcam_exif_parse(const uint8_t *data, size_t len, struct pocketcam_exif *out)
{
    struct in in;
    uint16_t magic;
    uint32_t ifd0;
    uint32_t sub;
    char datetime[POCKETCAM_EXIF_DATE_LEN + 8] = "";

    pocketcam_exif_clear(out);
    if (!data || len < sizeof(exif_magic) + 8 || memcmp(data, exif_magic, sizeof(exif_magic)) != 0) {
        return -1;
    }
    in.t = data + sizeof(exif_magic);
    in.len = len - sizeof(exif_magic);
    if (in.t[0] == 'I' && in.t[1] == 'I') {
        in.big = false;
    } else if (in.t[0] == 'M' && in.t[1] == 'M') {
        in.big = true;
    } else {
        return -1;
    }
    if (!get16(&in, 2, &magic) || magic != 42 || !get32(&in, 4, &ifd0)) {
        return -1;
    }
    sub = walk_ifd(&in, ifd0, out, datetime, sizeof(datetime));
    /* The Exif IFD may not point back into IFD0 (a loop) - it is only ever
     * walked once, so the worst a bad pointer does is find nothing. */
    if (sub && sub != ifd0) {
        char unused[POCKETCAM_EXIF_DATE_LEN + 8];

        walk_ifd(&in, sub, out, unused, sizeof(unused));
    }
    if (!out->taken[0] && pocketcam_exif_date_valid(datetime)) {
        memcpy(out->taken, datetime, sizeof(out->taken));
    }
    return 0;
}

/* ---- PPM ---------------------------------------------------------------------- */

size_t pocketcam_exif_ppm_comments(const struct pocketcam_photo_meta *meta, char *buf, size_t len)
{
    char date[POCKETCAM_EXIF_DATE_LEN];
    int n;

    if (!buf || len == 0) {
        return 0;
    }
    buf[0] = '\0';
    n = snprintf(buf, len, "# doors-software Doors %s\n", POCKETOS_VERSION);
    if (meta && pocketcam_exif_date(meta->taken, date, sizeof(date)) && n >= 0 &&
        (size_t)n < len) {
        n += snprintf(buf + n, len - (size_t)n, "# doors-taken %s\n", date);
    }
    if (meta && meta->simulated && n >= 0 && (size_t)n < len) {
        n += snprintf(buf + n, len - (size_t)n, "# doors-description %s\n", simulated_text);
    }
    if (n < 0 || (size_t)n >= len) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

static const char *ppm_key(const char *line, const char *key)
{
    size_t n = strlen(key);

    while (*line == ' ') {
        line++;
    }
    return strncmp(line, key, n) == 0 && line[n] == ' ' ? line + n + 1 : NULL;
}

static void printable(char *out, size_t len, const char *s)
{
    size_t i;

    snprintf(out, len, "%s", s);
    for (i = 0; out[i]; i++) {
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] >= 0x7f) {
            out[i] = '?';
        }
    }
}

void pocketcam_exif_ppm_parse_comment(const char *line, struct pocketcam_exif *out)
{
    const char *v;

    if ((v = ppm_key(line, "doors-taken")) != NULL) {
        if (pocketcam_exif_date_valid(v)) {
            snprintf(out->taken, sizeof(out->taken), "%s", v);
        }
    } else if ((v = ppm_key(line, "doors-software")) != NULL) {
        printable(out->software, sizeof(out->software), v);
    } else if ((v = ppm_key(line, "doors-description")) != NULL) {
        printable(out->description, sizeof(out->description), v);
    }
}
