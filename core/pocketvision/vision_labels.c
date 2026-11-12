/*
 * COCO class names. See vision_labels.h. Multi-word names are joined with a
 * hyphen so a name is always one protocol word.
 *
 * SEGMENT's class names and colours (vision_seg.h) are here too, at the end:
 * the screen shows a key to them and links this file, not the pipeline's.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_labels.h"

#include "vision_seg.h"

static const char *const coco[VISION_COCO_CLASSES] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
    "traffic-light", "fire-hydrant", "stop-sign", "parking-meter", "bench", "bird", "cat", "dog",
    "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
    "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports-ball", "kite",
    "baseball-bat", "baseball-glove", "skateboard", "surfboard", "tennis-racket", "bottle",
    "wine-glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich",
    "orange", "broccoli", "carrot", "hot-dog", "pizza", "donut", "cake", "chair", "couch",
    "potted-plant", "bed", "dining-table", "toilet", "tv", "laptop", "mouse", "remote",
    "keyboard", "cell-phone", "microwave", "oven", "toaster", "sink", "refrigerator", "book",
    "clock", "vase", "scissors", "teddy-bear", "hair-drier", "toothbrush",
};

const char *vision_label(uint32_t cls)
{
    return cls < VISION_COCO_CLASSES ? coco[cls] : "?";
}

/* COCO 2017's supercategories, in index order. */
static const uint32_t group_first[VISION_COCO_GROUPS + 1] = { 0, 1, 9, 14, 24, 29, 39, 46, 56, 62, 68, 73, 80 };
static const char *const group_names[VISION_COCO_GROUPS] = {
    "PEOPLE", "VEHICLES", "OUTDOOR", "ANIMALS", "ACCESSORIES", "SPORTS",
    "KITCHEN", "FOOD", "FURNITURE", "ELECTRONICS", "APPLIANCES", "INDOOR",
};
static const uint32_t r0_classes[VISION_R0_CLASSES] = { 2, 7, 5, 3, 1, 0 };

uint32_t vision_label_group_first(int g)
{
    return g >= 0 && g <= VISION_COCO_GROUPS ? group_first[g] : VISION_COCO_CLASSES;
}

const char *vision_label_group_name(int g)
{
    return g >= 0 && g < VISION_COCO_GROUPS ? group_names[g] : "?";
}

uint32_t vision_label_r0_class(int i)
{
    return i >= 0 && i < VISION_R0_CLASSES ? r0_classes[i] : VISION_COCO_CLASSES;
}

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Cityscapes' nineteen training classes and their usual colours. */
static const char *const seg_names[VISION_SEG_CLASSES] = {
    "road", "sidewalk", "building", "wall", "fence", "pole", "light", "sign", "vegetation", "terrain",
    "sky", "person", "rider", "car", "truck", "bus", "train", "motorcycle", "bicycle",
};
static const uint16_t seg_colors[VISION_SEG_CLASSES] = {
    RGB565(128, 64, 128), RGB565(244, 35, 232), RGB565(70, 70, 70),   RGB565(102, 102, 156), RGB565(190, 153, 153),
    RGB565(153, 153, 153), RGB565(250, 170, 30), RGB565(220, 220, 0),  RGB565(107, 142, 35),  RGB565(152, 251, 152),
    RGB565(70, 130, 180), RGB565(220, 20, 60),  RGB565(255, 0, 0),    RGB565(0, 0, 142),     RGB565(0, 0, 70),
    RGB565(0, 60, 100),   RGB565(0, 80, 100),   RGB565(0, 0, 230),    RGB565(119, 11, 32),
};

const char *vision_seg_label(uint32_t cls)
{
    return cls < VISION_SEG_CLASSES ? seg_names[cls] : "?";
}

uint16_t vision_seg_color(uint32_t cls)
{
    return cls < VISION_SEG_CLASSES ? seg_colors[cls] : 0;
}
