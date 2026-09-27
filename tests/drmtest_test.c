/*
 * pos-drmtest's pure logic (tools/drmtest/drmtest_logic.c): the K230 DSI
 * clock quantisation against the pinned kernel's rule, EDID decoding, the
 * choice of a safe test mode, and the test pattern.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "drmtest_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void clock_tests(void)
{
    int actual = 0;
    int err = 0;

    /* The panel mode unit A runs (dmesg: req=49500 adjusted=49500 div=12). */
    check("49.5 MHz (the RM69A10 mode) is exact",
          dt_clock_quantize(49500, &actual, &err) == 0 && actual == 49500 && err == 0);
    check("148.5 MHz (1080p60) is exact", dt_clock_quantize(148500, &actual, &err) == 0 && actual == 148500 && err == 0);
    check("74.25 MHz (720p60) is exact", dt_clock_quantize(74250, &actual, &err) == 0 && actual == 74250 && err == 0);
    check("27 MHz (480p/576p) is exact", dt_clock_quantize(27000, &actual, &err) == 0 && actual == 27000 && err == 0);
    check("25.175 MHz (640x480@60) becomes 24.75 MHz, -1.69 %",
          dt_clock_quantize(25175, &actual, &err) == 0 && actual == 24750 && err == -16881);
    check("40 MHz (800x600@60) becomes 39.6 MHz", dt_clock_quantize(40000, &actual, &err) == 0 && actual == 39600);
    check("65 MHz (1024x768@60) becomes 66 MHz", dt_clock_quantize(65000, &actual, &err) == 0 && actual == 66000);
    check("108 MHz (1280x1024@60) becomes 99 MHz, -8.33 %",
          dt_clock_quantize(108000, &actual, &err) == 0 && actual == 99000 && err == -83333);
    check("0 kHz is refused", dt_clock_quantize(0, &actual, &err) < 0);
    check("above 1188 MHz is refused (the divider would be 0)", dt_clock_quantize(1200000, &actual, &err) < 0);
}

static void put_desc_name(unsigned char *d, const char *name)
{
    size_t n = strlen(name);

    memset(d, 0, 18);
    d[3] = 0xfc;
    memset(d + 5, ' ', 13);
    memcpy(d + 5, name, n);
    if (n < 13) {
        d[5 + n] = 0x0a;
    }
}

static void make_edid(unsigned char *e)
{
    unsigned sum = 0;
    int i;

    memset(e, 0, 128);
    e[1] = e[2] = e[3] = e[4] = e[5] = e[6] = 0xff;
    /* "DEL": D=4 E=5 L=12 -> 00100 00101 01100 */
    e[8] = (unsigned char)((4 << 2) | (5 >> 3));
    e[9] = (unsigned char)(((5 & 7) << 5) | 12);
    e[10] = 0x34;
    e[11] = 0x12;
    e[12] = 7;
    e[16] = 12;
    e[17] = 34; /* 2024 */
    e[18] = 1;
    e[19] = 3;
    /* First detailed timing: 1920x1080, 148.5 MHz. */
    e[54] = 14850 & 0xff;
    e[55] = 14850 >> 8;
    e[56] = 1920 & 0xff;
    e[58] = (unsigned char)((1920 >> 8) << 4);
    e[59] = 1080 & 0xff;
    e[61] = (unsigned char)((1080 >> 8) << 4);
    put_desc_name(e + 72, "DOORS TEST");
    memset(e + 90, 0, 18);
    e[90 + 3] = 0xfd;
    e[90 + 5] = 50;
    e[90 + 6] = 75;
    e[90 + 7] = 30;
    e[90 + 8] = 83;
    e[90 + 9] = 17;
    e[126] = 1;
    for (i = 0; i < 127; i++) {
        sum += e[i];
    }
    e[127] = (unsigned char)(256 - (sum & 0xff));
}

static void edid_tests(void)
{
    unsigned char e[128];
    struct dt_edid d;

    make_edid(e);
    check("a well-formed block decodes", dt_edid_parse(e, sizeof(e), &d) == 0 && d.header_ok && d.checksum_ok);
    check("vendor id", strcmp(d.vendor, "DEL") == 0);
    check("product and serial", d.product == 0x1234 && d.serial == 7);
    check("week, year, version", d.week == 12 && d.year == 2024 && d.version == 1 && d.revision == 3);
    check("monitor name, trailing padding dropped", strcmp(d.name, "DOORS TEST") == 0);
    check("range limits", d.has_range && d.vmin_hz == 50 && d.vmax_hz == 75 && d.hmin_khz == 30 &&
                              d.hmax_khz == 83 && d.max_clock_mhz == 170);
    check("preferred timing", d.has_preferred && d.pref_width == 1920 && d.pref_height == 1080 &&
                                  d.pref_clock_khz == 148500);
    check("extension count", d.extensions == 1);

    e[20] ^= 0x55;
    check("a corrupted byte is reported as a bad checksum", dt_edid_parse(e, sizeof(e), &d) == 0 && !d.checksum_ok);
    e[0] = 0x12;
    check("a broken header is reported", dt_edid_parse(e, sizeof(e), &d) == 0 && !d.header_ok);
    check("less than one block is refused", dt_edid_parse(e, 64, &d) < 0);
    check("no data is refused", dt_edid_parse(NULL, 128, &d) < 0);
}

#define M(w, h, r, clk) { (w), (h), (r), (clk), 0, 0 }

static void choice_tests(void)
{
    /* What a common 1080p monitor's EDID gives the kernel. */
    static const struct dt_mode tv[] = {
        { 1920, 1080, 60, 148500, 0, 1 }, M(1920, 1080, 50, 148500), M(1920, 1080, 30, 74250),
        { 1920, 1080, 60, 74250, 1, 0 }, M(1280, 720, 60, 74250), M(1280, 720, 50, 74250),
        M(1024, 768, 60, 65000), M(800, 600, 60, 40000), M(720, 576, 50, 27000),
        M(720, 480, 60, 27000), M(640, 480, 60, 25175), M(640, 480, 60, 25200),
    };
    /* No EDID: what drm_add_modes_noedid() adds up to 1024x768. */
    static const struct dt_mode noedid[] = {
        M(1024, 768, 60, 65000), M(800, 600, 60, 40000), M(800, 600, 56, 36000), M(640, 480, 60, 25175),
    };
    static const struct dt_mode too_big[] = { M(3840, 2160, 30, 297000), M(2560, 1440, 60, 241500) };
    static const struct dt_mode small[] = { M(320, 240, 60, 6000) };
    static const struct dt_mode small_portrait[] = { M(240, 320, 60, 6000) };
    /* The AMOLED's only mode as the kernel reports it on unit A (DSI-1). */
    static const struct dt_mode amoled[] = { { 568, 1232, 52, 49500, 0, 1 } };
    char why[200];
    int i;

    i = dt_choose_mode(tv, (int)(sizeof(tv) / sizeof(tv[0])), NULL, why, sizeof(why));
    check("auto: 720x480@60 (27 MHz, exact) before inexact 640x480", i == 9);
    check("auto: the reason says exact", strstr(why, "exact") && !strstr(why, "INEXACT"));

    i = dt_choose_mode(noedid, 4, NULL, why, sizeof(why));
    check("auto without EDID: smallest, 640x480", i == 3);
    check("auto without EDID: flagged inexact", strstr(why, "INEXACT") != NULL);

    i = dt_choose_mode(tv, (int)(sizeof(tv) / sizeof(tv[0])), "1280x720", why, sizeof(why));
    check("explicit 1280x720: the 60 Hz mode", i == 4);
    i = dt_choose_mode(tv, (int)(sizeof(tv) / sizeof(tv[0])), "1280x720@50", why, sizeof(why));
    check("explicit 1280x720@50", i == 5);
    i = dt_choose_mode(tv, (int)(sizeof(tv) / sizeof(tv[0])), "1920x1080@60", why, sizeof(why));
    check("explicit 1920x1080@60: progressive, not the interlaced one", i == 0);
    i = dt_choose_mode(tv, (int)(sizeof(tv) / sizeof(tv[0])), "640x480", why, sizeof(why));
    check("explicit 640x480: the 25.175 MHz variant, closer to the DSI's 24.75 MHz", i == 10);
    i = dt_choose_mode(tv, (int)(sizeof(tv) / sizeof(tv[0])), "1366x768", why, sizeof(why));
    check("explicit size not offered", i == -1 && strstr(why, "does not offer"));
    i = dt_choose_mode(tv, (int)(sizeof(tv) / sizeof(tv[0])), "big", why, sizeof(why));
    check("explicit garbage refused", i == -1 && strstr(why, "not WxH"));
    i = dt_choose_mode(tv, (int)(sizeof(tv) / sizeof(tv[0])), "1280x720@", why, sizeof(why));
    check("explicit with an empty refresh refused", i == -1);

    check("auto: nothing at or below 1080p60's clock", dt_choose_mode(too_big, 2, NULL, why, sizeof(why)) == -1);
    check("auto: nothing of at least 640x480", dt_choose_mode(small, 1, NULL, why, sizeof(why)) == -1);
    check("auto: a portrait 240x320 is still too small", dt_choose_mode(small_portrait, 1, NULL, why, sizeof(why)) == -1);
    i = dt_choose_mode(amoled, 1, NULL, why, sizeof(why));
    check("auto: the AMOLED's portrait 568x1232 qualifies (unit A)", i == 0);
    check("auto: the AMOLED's 49.5 MHz is exact", strstr(why, "exact") && !strstr(why, "INEXACT"));
    check("no modes at all", dt_choose_mode(NULL, 0, NULL, why, sizeof(why)) == -1);
}

static void pattern_tests(void)
{
    const int w = 640;
    const int h = 480;
    const int stride = 656; /* pitch wider than the picture, as dumb buffers can be */
    uint32_t *px = calloc((size_t)stride * h, sizeof(*px));
    int b;
    int x;
    int y;
    int bw;
    int bh;
    int i;
    int beyond_ok = 1;

    if (!px) {
        check("allocation", 0);
        return;
    }
    for (i = 0; i < stride * h; i++) {
        px[i] = 0xdeadbeefu;
    }
    dt_draw_pattern(px, w, h, stride, "DOORS K230 HDMI TEST", "640x480@60 HDMI-A-1", "DSI 24.750 MHZ -1.69%");
    b = dt_pattern_border(w, h);
    check("border is white at every edge", px[0] == DT_RGB_WHITE && px[w - 1] == DT_RGB_WHITE &&
                                                px[(size_t)(h - 1) * stride] == DT_RGB_WHITE &&
                                                px[(size_t)(h - 1) * stride + w - 1] == DT_RGB_WHITE);
    check("inside the border is black", px[(size_t)(h - 3 * b) * stride + w / 2] == DT_RGB_BLACK);
    dt_pattern_block(w, h, 0, &x, &y, &bw, &bh);
    check("red block", px[(size_t)(y + bh / 2) * stride + x + bw / 2] == DT_RGB_RED);
    dt_pattern_block(w, h, 1, &x, &y, &bw, &bh);
    check("green block", px[(size_t)(y + bh / 2) * stride + x + bw / 2] == DT_RGB_GREEN);
    dt_pattern_block(w, h, 2, &x, &y, &bw, &bh);
    check("blue block", px[(size_t)(y + bh / 2) * stride + x + bw / 2] == DT_RGB_BLUE);
    check("blocks stay inside the picture", x + bw <= w - b && y + bh <= h - b);
    {
        int text_pixels = 0;

        for (y = 4 * b; y < h / 3; y++) {
            for (x = 4 * b; x < w - 4 * b; x++) {
                text_pixels += px[(size_t)y * stride + x] == DT_RGB_WHITE;
            }
        }
        check("text is drawn in the top third", text_pixels > 500);
    }
    for (y = 0; y < h; y++) {
        for (x = w; x < stride; x++) {
            beyond_ok &= px[(size_t)y * stride + x] == 0xdeadbeefu;
        }
    }
    check("nothing is written past the picture width", beyond_ok);

    /* The panel's own shape: portrait, narrow. Text must not run off. */
    free(px);
    px = calloc((size_t)568 * 1232, sizeof(*px));
    if (px) {
        dt_draw_pattern(px, 568, 1232, 568, "DOORS K230 HDMI TEST", "568x1232@52 DSI-1", NULL);
        check("portrait panel: right border intact", px[(size_t)600 * 568 + 567] == DT_RGB_WHITE);
    }
    free(px);
}

int main(void)
{
    clock_tests();
    edid_tests();
    choice_tests();
    pattern_tests();
    printf("drmtest_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
