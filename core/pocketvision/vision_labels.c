/*
 * COCO class names. See vision_labels.h. Multi-word names are joined with a
 * hyphen so a name is always one protocol word.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_labels.h"

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
