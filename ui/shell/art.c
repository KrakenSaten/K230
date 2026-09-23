/*
 * DOORS runtime art loader. See art.h and art_format.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "art.h"

#include "art_format.h"
#include "pocketlog/pocketlog.h"
#include "src/misc/cache/instance/lv_image_cache.h" /* lv_image_cache_drop: not in lvgl.h */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#ifndef POCKETOS_ART_DIR_DEFAULT
#define POCKETOS_ART_DIR_DEFAULT "/usr/share/doors/ui"
#endif

static unsigned loads;
static size_t held;

/* The pixels live in a mapping of their own, not in the heap. A background
 * is 1.4 MB and is freed the moment the lock opens; from the heap, glibc
 * kept one such block after a few rapid lock/open rounds on unit A (RssAnon
 * stayed 1.3 MB up, flat, until the shell exited). munmap gives the pages
 * back to the system every time, which is what "only while shown" means. */
static void *pixels_alloc(size_t n)
{
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    return p == MAP_FAILED ? NULL : p;
}

static void pixels_free(void *p, size_t n)
{
    if (p) {
        munmap(p, n);
    }
}

const char *art_dir(void)
{
    const char *d = getenv("POCKETOS_ART_DIR");

    return (d && d[0]) ? d : POCKETOS_ART_DIR_DEFAULT;
}

static unsigned long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)ts.tv_nsec / 1000000UL;
}

lv_image_dsc_t *art_load(const char *name)
{
    char path[512];
    uint8_t head[ART_HEADER_SIZE];
    struct art_header h = { 0 };
    struct stat st;
    const char *why = NULL;
    lv_image_dsc_t *img = NULL;
    uint8_t *data = NULL;
    unsigned long t0 = now_ms();
    FILE *f;

    if (art_path(art_dir(), name, path, sizeof(path)) < 0) {
        LOG_WARN("art: '%s' is not an art name", name ? name : "(null)");
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f) {
        LOG_WARN("art: %s: %s; drawing the fallback", path, strerror(errno));
        return NULL;
    }
    if (fstat(fileno(f), &st) != 0 || st.st_size < ART_HEADER_SIZE ||
        fread(head, 1, sizeof(head), f) != sizeof(head)) {
        why = "unreadable";
    } else if (art_header_parse(head, (size_t)st.st_size, &h, &why) == 0) {
        data = pixels_alloc(h.data_size);
        img = calloc(1, sizeof(*img));
        if (!data || !img) {
            why = "out of memory";
        } else if (fread(data, 1, h.data_size, f) != h.data_size) {
            why = "short read";
        } else {
            why = NULL;
        }
    }
    fclose(f);
    if (why) {
        LOG_WARN("art: %s: %s; drawing the fallback", path, why);
        pixels_free(data, h.data_size);
        free(img);
        return NULL;
    }
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = h.cf;
    img->header.w = h.w;
    img->header.h = h.h;
    img->header.stride = h.stride;
    img->data_size = (uint32_t)h.data_size;
    img->data = data;
    loads++;
    held += h.data_size;
    LOG_INFO("art: %s %ux%u, %zu bytes in %lu ms", name, (unsigned)h.w, (unsigned)h.h, h.data_size,
             now_ms() - t0);
    return img;
}

void art_free(lv_image_dsc_t *img)
{
    if (!img) {
        return;
    }
    /* Nothing may still be drawing it; LVGL may still remember it. */
    lv_image_cache_drop(img);
    held -= img->data_size;
    pixels_free((void *)img->data, img->data_size);
    free(img);
}

lv_image_dsc_t *art_load_background(const char *screen, bool landscape)
{
    char name[48];

    snprintf(name, sizeof(name), "bg-%s-%s", screen, landscape ? "landscape" : "portrait");
    return art_load(name);
}

unsigned art_loads(void)
{
    return loads;
}

size_t art_bytes_held(void)
{
    return held;
}
