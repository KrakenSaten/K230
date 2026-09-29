/*
 * Faces compared (core/pocketvision/vision_embed.c): the alignment puts
 * the template's own points on themselves, undoes a known scale, turn and
 * shift, and brings a face's points close to the template when they carry
 * noise; points that are not a face refused. Unit length, the vendor's
 * score (the same face 1000, opposite 0, unrelated 500), NaN refused. The
 * owner: a mean of views made unit, the model it belongs to, its text form
 * read back exactly, and every kind of damaged or foreign text refused.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketvision/vision_embed.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static const float T[5][2] = {
    { 38.2946f, 51.6963f }, { 73.5318f, 51.5014f }, { 56.0252f, 71.7366f },
    { 41.5493f, 92.3655f }, { 70.7299f, 92.2041f },
};

static void apply(const float m[6], float x, float y, float *ox, float *oy)
{
    *ox = m[0] * x + m[1] * y + m[2];
    *oy = m[3] * x + m[4] * y + m[5];
}

static float worst(const float m[6], const float p[5][2])
{
    float w = 0.0f;
    int i;

    for (i = 0; i < 5; i++) {
        float x;
        float y;
        float d;

        apply(m, p[i][0], p[i][1], &x, &y);
        d = hypotf(x - T[i][0], y - T[i][1]);
        w = d > w ? d : w;
    }
    return w;
}

static void test_align(void)
{
    float m[6];
    float p[5][2];
    float same[5][2];
    int i;

    check("the template onto itself: the identity", vision_embed_align(T, m) == 0 && fabsf(m[0] - 1) < 1e-4f &&
                                                        fabsf(m[1]) < 1e-4f && fabsf(m[2]) < 1e-3f &&
                                                        fabsf(m[3]) < 1e-4f && fabsf(m[4] - 1) < 1e-4f &&
                                                        fabsf(m[5]) < 1e-3f);
    /* The template scaled by 2.5, turned 30 degrees and moved: a face in a
     * picture; the alignment must bring it back exactly. */
    for (i = 0; i < 5; i++) {
        float c = cosf(0.5236f);
        float s = sinf(0.5236f);

        p[i][0] = 2.5f * (c * T[i][0] - s * T[i][1]) + 300.0f;
        p[i][1] = 2.5f * (s * T[i][0] + c * T[i][1]) + 80.0f;
    }
    check("a face scaled, turned and moved is brought back", vision_embed_align(p, m) == 0 && worst(m, p) < 0.01f);
    check("the matrix is a similarity (no mirror, no shear)", fabsf(m[0] - m[4]) < 1e-5f && fabsf(m[1] + m[3]) < 1e-5f &&
                                                                 m[0] * m[4] - m[1] * m[3] > 0);
    check("its scale undoes the face's", fabsf(hypotf(m[0], m[3]) - 0.4f) < 1e-4f);
    for (i = 0; i < 5; i++) {
        p[i][0] = 3.0f * T[i][0] + 100.0f + (i % 2 ? 2.0f : -2.0f);
        p[i][1] = 3.0f * T[i][1] + 50.0f + (i < 2 ? 1.5f : -1.5f);
    }
    check("points off by up to 2.5 px (0.83 px at the template's scale): every point within 1.2 px",
          vision_embed_align(p, m) == 0 && worst(m, p) < 1.2f);
    for (i = 0; i < 5; i++) {
        same[i][0] = 10.0f;
        same[i][1] = 10.0f;
    }
    check("five points in one place are no face", vision_embed_align(same, m) == -1);
    same[2][0] = NAN;
    check("nor a point that is not a number", vision_embed_align(same, m) == -1);
}

static void test_compare(void)
{
    float a[4] = { 3, 0, 4, 0 };
    float b[4] = { 0, 2, 0, 0 };
    float c[4];
    float z[4] = { 0, 0, 0, 0 };

    check("unit length", vision_embed_unit(a, 4, c) == 0 && fabsf(c[0] - 0.6f) < 1e-6f && fabsf(c[2] - 0.8f) < 1e-6f);
    check("in place", vision_embed_unit(a, 4, a) == 0 && fabsf(a[2] - 0.8f) < 1e-6f);
    check("zero has no direction", vision_embed_unit(z, 4, c) == -1);
    z[1] = NAN;
    check("nor a NaN", vision_embed_unit(z, 4, c) == -1);
    vision_embed_unit(b, 4, b);
    check("the same face scores 1000", vision_embed_score(a, a, 4) == 1000);
    check("an unrelated one 500", vision_embed_score(a, b, 4) == 500);
    c[0] = -a[0];
    c[1] = -a[1];
    c[2] = -a[2];
    c[3] = -a[3];
    check("the opposite 0", vision_embed_score(a, c, 4) == 0);
}

static void test_owner(void)
{
    static struct vision_owner o;
    static struct vision_owner r;
    static char text[VISION_OWNER_TEXT_MAX];
    float v[8];
    float w[8];
    int n;
    int i;

    check("an owner for a model of 8 values", vision_owner_begin(&o, "face_embed.kmodel", 46333280, 8) == 0);
    check("a model name with a space is refused", vision_owner_begin(&r, "face embed", 1, 8) == -1);
    check("more values than an embedding has are refused", vision_owner_begin(&r, "m", 1, VISION_EMBED_MAX + 1) == -1);
    check("no views, no owner", vision_owner_finish(&o) == -1 && vision_owner_format(&o, text, sizeof(text)) == -1);
    for (i = 0; i < 8; i++) {
        v[i] = i == 0 ? 1.0f : 0.0f;
        w[i] = i == 1 ? 1.0f : 0.0f;
    }
    vision_owner_add(&o, v);
    vision_owner_add(&o, w);
    check("the views' mean made unit", vision_owner_finish(&o) == 0 && o.samples == 2 &&
                                            fabsf(o.v[0] - 0.70710678f) < 1e-5f && fabsf(o.v[1] - o.v[0]) < 1e-6f);
    check("it fits its model", vision_owner_fits(&o, "face_embed.kmodel", 46333280, 8));
    check("not another file, size or length", !vision_owner_fits(&o, "other.kmodel", 46333280, 8) &&
                                                  !vision_owner_fits(&o, "face_embed.kmodel", 1, 8) &&
                                                  !vision_owner_fits(&o, "face_embed.kmodel", 46333280, 512));
    n = vision_owner_format(&o, text, sizeof(text));
    {
        static const char head[] = "doors-vision-owner 1\nmodel face_embed.kmodel 46333280\ndim 8\nsamples 2\nv ";

        check("its text form", n > 0 && strncmp(text, head, sizeof(head) - 1) == 0 && text[n - 1] == '\n');
    }
    check("read back, the same", vision_owner_parse(text, &r) == 0 && r.dim == 8 && r.samples == 2 &&
                                     r.model_bytes == 46333280 && strcmp(r.model, "face_embed.kmodel") == 0 &&
                                     vision_embed_score(r.v, o.v, 8) == 1000);
    check("too small a buffer says so", vision_owner_format(&o, text, 40) == -1);

    n = vision_owner_format(&o, text, sizeof(text));
    text[n - 3] = 'x';
    check("a damaged value is refused", vision_owner_parse(text, &r) == -1);
    vision_owner_format(&o, text, sizeof(text));
    text[n - 1] = '\0';
    check("so is a text cut short", vision_owner_parse(text, &r) == -1);
    vision_owner_format(&o, text, sizeof(text));
    snprintf(text + n - 1, sizeof(text) - (size_t)n + 1, " 0.1\n");
    check("and one with a value too many", vision_owner_parse(text, &r) == -1);
    vision_owner_format(&o, text, sizeof(text));
    text[strlen("doors-vision-owner ")] = '2';
    check("and another version", vision_owner_parse(text, &r) == -1);
    check("and nonsense", vision_owner_parse("hello\n", &r) == -1 && vision_owner_parse("", &r) == -1);
    o.v[0] = 0.9f;
    vision_owner_format(&o, text, sizeof(text));
    check("and a vector that is not of unit length", vision_owner_parse(text, &r) == -1);
}

int main(void)
{
    test_align();
    test_compare();
    test_owner();
    printf("vision_embed_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
