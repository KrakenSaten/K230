/*
 * Feed a kmodel's output tensor to DOORS' own decoder, unchanged.
 *
 * Links core/pocketvision/vision_decode.c, vision_nms.c, vision_labels.c and
 * vision_traffic.c from the tree as they are and runs the same steps
 * pos-vision runs on a KPU output: vision_decode, vision_nms, then the label
 * and Traffic class of each box. Proves a trained kmodel is accepted (no
 * -EPROTO, no bad rows) and that its classes land on the names Traffic
 * counts.
 *
 * Usage: decoder_check TENSOR.f32 IN_SIZE FRAME_W FRAME_H [CONF_PERMILLE]
 *   TENSOR.f32: raw little-endian float32 [1][84][rows] (sim output)
 * Exit 0 when the decoder accepted the tensor with no bad rows.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include <stdio.h>
#include <stdlib.h>

#include "vision_decode.h"
#include "vision_labels.h"
#include "vision_nms.h"
#include "vision_traffic.h"

#define MAX_CAND 256
#define MAX_DET 32

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s TENSOR.f32 IN_SIZE FRAME_W FRAME_H [CONF_PERMILLE]\n", argv[0]);
        return 2;
    }
    uint32_t in = (uint32_t)atoi(argv[2]);
    uint32_t rows = vision_decode_rows(in, in);
    size_t count = (size_t)84 * rows;
    float *t = malloc(count * sizeof(float));
    FILE *f = fopen(argv[1], "rb");
    if (!t || !f || fread(t, sizeof(float), count, f) != count || fgetc(f) != EOF) {
        fprintf(stderr, "cannot read %zu floats from %s\n", count, argv[1]);
        return 2;
    }
    fclose(f);

    struct vision_decode_params p = {
        .in_w = in, .in_h = in, .classes = 80,
        .frame_w = (uint32_t)atoi(argv[3]), .frame_h = (uint32_t)atoi(argv[4]),
        .conf_min = (uint16_t)(argc > 5 ? atoi(argv[5]) : 350),
    };
    uint32_t dims[3] = {1, 84, rows};
    struct vision_det cand[MAX_CAND], det[MAX_DET];
    uint32_t bad = 0;
    int n = vision_decode(t, count, dims, &p, cand, MAX_CAND, &bad);
    if (n < 0) {
        printf("{\"decode\": %d, \"accepted\": false}\n", n);
        return 1;
    }
    int k = vision_nms(cand, n, 650, det, MAX_DET);
    printf("{\"accepted\": true, \"rows\": %u, \"bad_rows\": %u, \"candidates\": %d, \"dets\": [", rows, bad, n);
    for (int i = 0; i < k; i++) {
        const char *name = vision_label(det[i].cls);
        printf("%s{\"cls\": %u, \"label\": \"%s\", \"traffic\": \"%s\", \"conf\": %u, \"box\": [%d, %d, %d, %d]}",
               i ? ", " : "", det[i].cls, name, vision_traffic_name(vision_traffic_class_of(name)),
               det[i].conf, det[i].box.x, det[i].box.y, det[i].box.w, det[i].box.h);
    }
    printf("]}\n");
    free(t);
    return bad == 0 ? 0 : 1;
}
