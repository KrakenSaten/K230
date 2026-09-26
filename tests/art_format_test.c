/*
 * The DOORS runtime art files (ui/shell/art_format.h): which headers the
 * shell accepts, which it refuses and why, and that every file committed
 * under the directory given on the command line (ui/assets/doors) is one it
 * accepts, at the size its name says.
 *
 * Pure C: built and run by the root Makefile (make test).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "art_format.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void header(uint8_t *b, uint8_t magic, uint8_t cf, uint16_t w, uint16_t h, uint16_t stride)
{
    memset(b, 0, ART_HEADER_SIZE);
    b[0] = magic;
    b[1] = cf;
    b[4] = (uint8_t)(w & 0xff);
    b[5] = (uint8_t)(w >> 8);
    b[6] = (uint8_t)(h & 0xff);
    b[7] = (uint8_t)(h >> 8);
    b[8] = (uint8_t)(stride & 0xff);
    b[9] = (uint8_t)(stride >> 8);
}

static void test_rules(void)
{
    uint8_t b[ART_HEADER_SIZE];
    struct art_header h;
    const char *why = NULL;
    char path[64];

    header(b, ART_MAGIC, ART_CF_RGB565, 568, 1232, 1136);
    check("a portrait RGB565 background is accepted",
          art_header_parse(b, ART_HEADER_SIZE + 568u * 1232 * 2, &h, &why) == 0 && h.data_size == 1399552 &&
              h.w == 568 && h.h == 1232);
    check("one byte short is refused as truncated",
          art_header_parse(b, ART_HEADER_SIZE + 568u * 1232 * 2 - 1, &h, &why) < 0 && strcmp(why, "truncated") == 0);
    check("one byte long is refused",
          art_header_parse(b, ART_HEADER_SIZE + 568u * 1232 * 2 + 1, &h, &why) < 0);
    header(b, ART_MAGIC, ART_CF_RGB565A8, 96, 96, 192);
    check("a 96 px RGB565A8 icon is accepted",
          art_header_parse(b, ART_HEADER_SIZE + 96u * 96 * 3, &h, &why) == 0 && h.data_size == 27648);
    header(b, ART_MAGIC, ART_CF_A8, 32, 32, 32);
    check("a 32 px A8 glyph is accepted", art_header_parse(b, ART_HEADER_SIZE + 1024, &h, &why) == 0);
    header(b, 0x18, ART_CF_RGB565, 8, 8, 16);
    check("a wrong magic is refused", art_header_parse(b, ART_HEADER_SIZE + 128, &h, &why) < 0);
    header(b, ART_MAGIC, 0x10 /* ARGB8888 */, 8, 8, 32);
    check("a format DOORS art does not use is refused", art_header_parse(b, ART_HEADER_SIZE + 256, &h, &why) < 0);
    header(b, ART_MAGIC, ART_CF_RGB565, 8, 8, 20);
    check("a padded stride is refused", art_header_parse(b, ART_HEADER_SIZE + 160, &h, &why) < 0);
    header(b, ART_MAGIC, ART_CF_RGB565, 0, 8, 0);
    check("a zero width is refused", art_header_parse(b, ART_HEADER_SIZE, &h, &why) < 0);
    header(b, ART_MAGIC, ART_CF_RGB565, 4096, 8, 8192);
    check("an oversized image is refused", art_header_parse(b, ART_HEADER_SIZE + 65536, &h, &why) < 0);
    header(b, ART_MAGIC, ART_CF_RGB565, 8, 8, 16);
    b[2] = 1;
    check("flags set are refused", art_header_parse(b, ART_HEADER_SIZE + 128, &h, &why) < 0);
    check("a file shorter than a header is refused", art_header_parse(b, 11, &h, &why) < 0);
    check("NULL is refused", art_header_parse(NULL, 100, &h, &why) < 0);

    check("a plain name makes a path", art_path("/usr/share/doors/ui", "bg-home-portrait", path, sizeof(path)) == 0 &&
                                           strcmp(path, "/usr/share/doors/ui/bg-home-portrait.bin") == 0);
    check("a name with a slash is refused", art_path("/d", "../etc/passwd", path, sizeof(path)) < 0);
    check("an upper-case name is refused", art_path("/d", "Icon", path, sizeof(path)) < 0);
    check("an empty name is refused", art_path("/d", "", path, sizeof(path)) < 0);
    check("a path that does not fit is refused", art_path("/a/very/long/directory", "icon-radio", path, 16) < 0);
}

static void test_committed(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int files = 0;
    int bad = 0;
    int bg = 0;
    int icons = 0;

    check("the art directory opens", d != NULL);
    if (!d) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char path[512];
        char name[64];
        uint8_t b[ART_HEADER_SIZE];
        struct art_header h;
        struct stat st;
        const char *why = "unreadable";
        size_t n = strlen(e->d_name);
        FILE *f;

        if (n < 5 || strcmp(e->d_name + n - 4, ".bin") != 0) {
            continue;
        }
        files++;
        snprintf(name, sizeof(name), "%.*s", (int)(n - 4), e->d_name);
        if (art_path(dir, name, path, sizeof(path)) < 0 || stat(path, &st) != 0 || !(f = fopen(path, "rb"))) {
            printf("FAIL %s: not a usable art name or unreadable\n", e->d_name);
            bad++;
            continue;
        }
        if (fread(b, 1, sizeof(b), f) != sizeof(b) || art_header_parse(b, (size_t)st.st_size, &h, &why) < 0) {
            printf("FAIL %s: %s\n", e->d_name, why);
            bad++;
        } else if (strncmp(name, "bg-", 3) == 0) {
            int land = strstr(name, "-landscape") != NULL;

            bg++;
            if (h.cf != ART_CF_RGB565 || h.w != (land ? 1232 : 568) || h.h != (land ? 568 : 1232)) {
                printf("FAIL %s: not an opaque full-screen %s background\n", e->d_name, land ? "landscape" : "portrait");
                bad++;
            }
        } else if (strncmp(name, "icon-", 5) == 0) {
            icons++;
            if (h.cf != ART_CF_RGB565A8 || h.w != 96 || h.h != 96) {
                printf("FAIL %s: not a 96 px RGB565A8 icon\n", e->d_name);
                bad++;
            }
        }
        fclose(f);
    }
    closedir(d);
    check("every committed art file is one the shell accepts, at its size", bad == 0);
    check("six backgrounds: lock, open, home in both orientations", bg == 6);
    check("seventeen icons: sixteen apps and the empty frame", icons == 17);
    check("nothing else is in the directory as art", files == bg + icons);
}

int main(int argc, char **argv)
{
    test_rules();
    if (argc > 1) {
        test_committed(argv[1]);
    }
    printf("art_format_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
