/*
 * COCO class names. See vision_labels.h. Multi-word names are joined with a
 * hyphen so a name is always one protocol word.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
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
