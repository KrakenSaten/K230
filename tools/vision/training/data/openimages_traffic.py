"""Open Images V7 metadata for the DOORS Traffic dataset: class folding, per-image
provenance and the policy checks that decide whether an image may be used.

Only Open Images' own metadata files are read (download_v7.html):

  oidv7-class-descriptions-boxable.csv, bbox_labels_600_hierarchy.json
  {train,validation,test} images CSV   ImageID, Subset, OriginalURL, OriginalLandingURL,
                                       License, AuthorProfileURL, Author, Title,
                                       OriginalSize, OriginalMD5, Thumbnail300KURL, Rotation
  {train,validation,test} bbox CSV     ImageID, Source, LabelName, Confidence, XMin, XMax,
                                       YMin, YMax, IsOccluded, IsTruncated, IsGroupOf,
                                       IsDepiction, IsInside
  {train,validation,test} human image-level labels (boxable)

The rules are in docs/vision/DATASET_PROVENANCE.md section 4; every rejection
carries a reason code from REASONS.
"""
import csv
import math
from collections import defaultdict
from pathlib import Path

# Boxable classes folded into the six DOORS Traffic classes. The folding
# follows Open Images' own hierarchy (bbox_labels_600_hierarchy.json):
# Limousine and Van are children of Car; Man, Woman, Boy and Girl are
# children of Person. Taxi is a sibling of Car under Land vehicle; COCO
# labels taxis as car, so it folds into car.
OI_FOLD = {
    "/m/0k4j": "car", "/m/01lcw4": "car", "/m/0h2r6": "car", "/m/0pg52": "car",
    "/m/07r04": "truck",
    "/m/01bjv": "bus",
    "/m/04_sv": "motorcycle",
    "/m/0199g": "bicycle",
    "/m/01g317": "person", "/m/04yx4": "person", "/m/03bt1vf": "person",
    "/m/01bl7v": "person", "/m/05r655": "person",
}
OI_NAME = {
    "/m/0k4j": "Car", "/m/01lcw4": "Limousine", "/m/0h2r6": "Van", "/m/0pg52": "Taxi",
    "/m/07r04": "Truck", "/m/01bjv": "Bus", "/m/04_sv": "Motorcycle", "/m/0199g": "Bicycle",
    "/m/01g317": "Person", "/m/04yx4": "Man", "/m/03bt1vf": "Woman", "/m/01bl7v": "Boy",
    "/m/05r655": "Girl",
    "/m/07yv9": "Vehicle", "/m/01prls": "Land vehicle", "/m/012n7d": "Ambulance",
}
# The primary label per DOORS class, used to read human-verified absence.
OI_PRIMARY = {"car": "/m/0k4j", "truck": "/m/07r04", "bus": "/m/01bjv",
              "motorcycle": "/m/04_sv", "bicycle": "/m/0199g", "person": "/m/01g317"}
# A box with one of these labels may hide a target vehicle under a label
# that maps to no single DOORS class (Vehicle, Land vehicle: unspecific;
# Ambulance: car or truck). Such images are rejected, not relabelled.
OI_AMBIGUOUS = {"/m/07yv9", "/m/01prls", "/m/012n7d"}
# Selection rule: an Open Images image is a candidate when it has a box of
# one of these classes. Person-only images are outside the rule (volume;
# COCO already supplies persons).
SELECT_CLASSES = {"car", "truck", "bus", "motorcycle", "bicycle"}

CC_BY_20 = "https://creativecommons.org/licenses/by/2.0/"
SUBSETS = {"train": "train", "validation": "validation", "test": "test"}
FILES = {
    "classes": "oidv7-class-descriptions-boxable.csv",
    "hierarchy": "bbox_labels_600_hierarchy.json",
    "images": {"train": "train-images-boxable-with-rotation.csv",
               "validation": "validation-images-with-rotation.csv",
               "test": "test-images-with-rotation.csv"},
    "boxes": {"train": "oidv6-train-annotations-bbox.csv",
              "validation": "validation-annotations-bbox.csv",
              "test": "test-annotations-bbox.csv"},
    "labels": {"train": "train-annotations-human-imagelabels-boxable.csv",
               "validation": "validation-annotations-human-imagelabels-boxable.csv",
               "test": "test-annotations-human-imagelabels-boxable.csv"},
}

REASONS = {
    "licence_not_cc_by_2.0": "License column is not exactly " + CC_BY_20,
    "missing_author": "Author or AuthorProfileURL empty: CC BY attribution impossible",
    "missing_original_url": "OriginalURL or OriginalLandingURL empty: source not traceable",
    "not_in_image_list": "ImageID has boxes but no row in the images CSV",
    "rotation_not_zero": "Rotation is not 0 (or unknown): boxes may not match the stored pixels",
    "ambiguous_vehicle_box": "has a Vehicle, Land vehicle or Ambulance box",
    "depiction": "a target box is a depiction (drawing, toy, poster)",
    "inside_view": "a target box is a view from inside the object",
    "invalid_box": "a target box is outside [0,1] or has no area",
}


def check_class_ids(meta_dir):
    """The label ids above must name the expected classes in Open Images' file."""
    with open(Path(meta_dir) / FILES["classes"], encoding="utf-8") as f:
        names = {r[0]: r[1] for r in csv.reader(f)}
    bad = {k: (v, names.get(k)) for k, v in OI_NAME.items() if names.get(k) != v}
    if bad:
        raise SystemExit("Open Images class ids do not match: %s" % bad)


def read_boxes(meta_dir, subset):
    """ImageID -> list of box dicts for target and ambiguous labels only."""
    wanted = set(OI_FOLD) | OI_AMBIGUOUS
    out = defaultdict(list)
    with open(Path(meta_dir) / FILES["boxes"][subset], encoding="utf-8") as f:
        header = f.readline().rstrip("\n").split(",")
        assert header[:13] == ["ImageID", "Source", "LabelName", "Confidence", "XMin", "XMax", "YMin",
                               "YMax", "IsOccluded", "IsTruncated", "IsGroupOf", "IsDepiction",
                               "IsInside"], header
        for line in f:
            p = line.rstrip("\n").split(",")
            if p[2] not in wanted:
                continue
            out[p[0]].append({"label": p[2], "source": p[1], "x0": float(p[4]), "x1": float(p[5]),
                              "y0": float(p[6]), "y1": float(p[7]), "occluded": int(p[8]),
                              "truncated": int(p[9]), "group": int(p[10]), "depiction": int(p[11]),
                              "inside": int(p[12])})
    return out


def read_human_labels(meta_dir, subset, ids):
    """ImageID -> {label: confidence} for the target primaries, human-verified."""
    wanted = set(OI_FOLD)
    out = defaultdict(dict)
    with open(Path(meta_dir) / FILES["labels"][subset], encoding="utf-8") as f:
        f.readline()
        for line in f:
            p = line.rstrip("\n").split(",")
            if p[2] in wanted and p[0] in ids:
                out[p[0]][p[2]] = int(p[3])
    return out


def read_images(meta_dir, subset, ids):
    """ImageID -> image metadata row, for the given ids only."""
    out = {}
    with open(Path(meta_dir) / FILES["images"][subset], encoding="utf-8", newline="") as f:
        r = csv.reader(f)
        header = next(r)
        for row in r:
            if row[0] in ids:
                out[row[0]] = dict(zip(header, row))
    return out


def rotation_ok(value):
    try:
        v = float(value)
    except ValueError:
        return False
    return not math.isnan(v) and v == 0.0


def decide(image_row, boxes):
    """Return the list of rejection reasons (empty = accepted by metadata)."""
    reasons = []
    if image_row is None:
        return ["not_in_image_list"]
    if image_row.get("License", "") != CC_BY_20:
        reasons.append("licence_not_cc_by_2.0")
    if not image_row.get("Author", "").strip() or not image_row.get("AuthorProfileURL", "").strip():
        reasons.append("missing_author")
    if not image_row.get("OriginalURL", "").strip() or not image_row.get("OriginalLandingURL", "").strip():
        reasons.append("missing_original_url")
    if not rotation_ok(image_row.get("Rotation", "")):
        reasons.append("rotation_not_zero")
    if any(b["label"] in OI_AMBIGUOUS for b in boxes):
        reasons.append("ambiguous_vehicle_box")
    tgt = [b for b in boxes if b["label"] in OI_FOLD]
    if any(b["depiction"] == 1 for b in tgt):
        reasons.append("depiction")
    if any(b["inside"] == 1 for b in tgt):
        reasons.append("inside_view")
    eps = 1e-6
    if any(not (-eps <= b["x0"] < b["x1"] <= 1 + eps and -eps <= b["y0"] < b["y1"] <= 1 + eps) for b in tgt):
        reasons.append("invalid_box")
    return reasons


PERSON_LABELS = {k for k, v in OI_FOLD.items() if v == "person"}


def person_verified(human_labels):
    """True when a human verified Person (or a child) as present or absent.

    Open Images draws boxes only for classes verified in the image. In a
    street photo where Person was never verified, people can be present
    without boxes, and training would teach the model to suppress them."""
    return any(k in human_labels for k in PERSON_LABELS)


def candidates(boxes_by_image):
    """Images inside the selection rule: at least one non-person target box."""
    return sorted(i for i, bs in boxes_by_image.items()
                  if any(OI_FOLD.get(b["label"]) in SELECT_CLASSES for b in bs))


def image_url(subset, image_id):
    """CVDF mirror named on download_v7.html (anonymous S3)."""
    return "https://open-images-dataset.s3.amazonaws.com/%s/%s.jpg" % (SUBSETS[subset], image_id)
