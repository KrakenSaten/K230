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

/* Objects365 v2's 365 classes, in the order of PaddleDetection's
 * objects365_detection_label_list.txt (the order the GENERAL 365 kmodel's
 * scores come in), made protocol-safe: lower case, words joined with a
 * hyphen, a slash pair cut to its first word, and the list's misspellings
 * ("Moniter/TV", "Bakset", "Mushroon"...) put right. The names COCO also
 * has are spelt as COCO spells them, so TRAFFIC finds car, bus, truck,
 * motorcycle, bicycle and person by name in either list. */
static const char *const o365[VISION_O365_CLASSES] = {
    "person", "sneakers", "chair", "other-shoes", "hat", "car", "lamp", "glasses", "bottle", "desk", "cup",
    "street-lights", "cabinet", "handbag", "bracelet", "plate", "picture-frame", "helmet", "book", "gloves",
    "storage-box", "boat", "leather-shoes", "flower", "bench", "potted-plant", "bowl", "flag", "pillow",
    "boots", "vase", "microphone", "necklace", "ring", "suv", "wine-glass", "belt", "monitor", "backpack",
    "umbrella", "traffic-light", "speaker", "watch", "tie", "trash-can", "slippers", "bicycle", "stool",
    "bucket", "van", "couch", "sandals", "basket", "drum", "pen", "bus", "wild-bird", "high-heels",
    "motorcycle", "guitar", "carpet", "cell-phone", "bread", "camera", "canned", "truck", "traffic-cone",
    "cymbal", "lifesaver", "towel", "stuffed-toy", "candle", "sailboat", "laptop", "awning", "bed", "faucet",
    "tent", "horse", "mirror", "power-outlet", "sink", "apple", "air-conditioner", "knife", "hockey-stick",
    "paddle", "pickup-truck", "fork", "traffic-sign", "balloon", "tripod", "dog", "spoon", "clock", "pot",
    "cow", "cake", "dining-table", "sheep", "hanger", "whiteboard", "napkin", "other-fish", "orange",
    "toiletry", "keyboard", "tomato", "lantern", "machinery-vehicle", "fan", "green-vegetables", "banana",
    "baseball-glove", "airplane", "mouse", "train", "pumpkin", "soccer", "skis", "luggage", "nightstand",
    "tea-pot", "telephone", "trolley", "headphones", "sports-car", "stop-sign", "dessert", "scooter",
    "stroller", "crane", "remote", "refrigerator", "oven", "lemon", "duck", "baseball-bat",
    "surveillance-camera", "cat", "jug", "broccoli", "piano", "pizza", "elephant", "skateboard", "surfboard",
    "gun", "skates", "gas-stove", "donut", "bow-tie", "carrot", "toilet", "kite", "strawberry", "other-balls",
    "shovel", "pepper", "computer-box", "toilet-paper", "cleaning-products", "chopsticks", "microwave",
    "pigeon", "baseball", "cutting-board", "coffee-table", "side-table", "scissors", "marker", "pie",
    "ladder", "snowboard", "cookies", "radiator", "fire-hydrant", "basketball", "zebra", "grape", "giraffe",
    "potato", "sausage", "tricycle", "violin", "egg", "fire-extinguisher", "candy", "fire-truck", "billiards",
    "converter", "bathtub", "wheelchair", "golf-club", "briefcase", "cucumber", "cigarette", "paint-brush",
    "pear", "heavy-truck", "hamburger", "extractor", "extension-cord", "tong", "tennis-racket", "folder",
    "american-football", "earphone", "mask", "kettle", "tennis", "ship", "swing", "coffee-machine", "slide",
    "carriage", "onion", "green-beans", "projector", "frisbee", "washing-machine", "chicken", "printer",
    "watermelon", "saxophone", "tissue", "toothbrush", "ice-cream", "hot-air-balloon", "cello",
    "french-fries", "scale", "trophy", "cabbage", "hot-dog", "blender", "peach", "rice", "wallet",
    "volleyball", "deer", "goose", "tape", "tablet", "cosmetics", "trumpet", "pineapple", "golf-ball",
    "ambulance", "parking-meter", "mango", "key", "hurdle", "fishing-rod", "medal", "flute", "brush",
    "penguin", "megaphone", "corn", "lettuce", "garlic", "swan", "helicopter", "green-onion", "sandwich",
    "nuts", "speed-limit-sign", "induction-cooker", "broom", "trombone", "plum", "rickshaw", "goldfish",
    "kiwi-fruit", "router", "poker-card", "toaster", "shrimp", "sushi", "cheese", "notepaper", "cherry",
    "pliers", "cd", "pasta", "hammer", "cue", "avocado", "hami-melon", "flask", "mushroom", "screwdriver",
    "soap", "recorder", "bear", "eggplant", "board-eraser", "coconut", "tape-measure", "pig", "showerhead",
    "globe", "chips", "steak", "crosswalk-sign", "stapler", "camel", "formula-1", "pomegranate", "dishwasher",
    "crab", "hoverboard", "meat-ball", "rice-cooker", "tuba", "calculator", "papaya", "antelope", "parrot",
    "seal", "butterfly", "dumbbell", "donkey", "lion", "urinal", "dolphin", "electric-drill", "hair-dryer",
    "egg-tart", "jellyfish", "treadmill", "lighter", "grapefruit", "game-board", "mop", "radish", "baozi",
    "target", "french", "spring-rolls", "monkey", "rabbit", "pencil-case", "yak", "red-cabbage", "binoculars",
    "asparagus", "barbell", "scallop", "noodles", "comb", "dumpling", "oyster", "table-tennis-paddle",
    "makeup-brush", "chainsaw", "eraser", "lobster", "durian", "okra", "lipstick", "cosmetics-mirror",
    "curling", "table-tennis",
};

const char *vision_label_o365(uint32_t cls)
{
    return cls < VISION_O365_CLASSES ? o365[cls] : "?";
}

const char *vision_label_for(uint32_t classes, uint32_t cls)
{
    return classes == VISION_O365_CLASSES ? vision_label_o365(cls) : vision_label(cls);
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
