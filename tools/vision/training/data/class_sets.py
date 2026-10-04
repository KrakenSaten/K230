"""Class sets for the DOORS Traffic detector.

Each class set lists the training classes in label order (label 0 first) and,
per class, the source COCO category names folded into it and the index in
the 80-class COCO order the DOORS decoder and label table use
(core/pocketvision/vision_labels.c). The export widens the class convs to
80 outputs with label i at DOORS index doors_index[i] and every other class
scoring about 1e-13 (export_onnx.py: widen_cls_preds), so the kmodel keeps
the [1, 84, rows] layout and today's vision_decode, vision_labels and
vision_traffic read it unchanged.

Keep CLASS_SETS in exps/yolox_tiny_traffic_416.py in step with this file.
"""

# COCO 2017 category_id (1-based, with gaps) by name, from
# instances_*2017.json; checked against the file by prepare_coco_traffic.py.
COCO_CATEGORY_ID = {"person": 1, "bicycle": 2, "car": 3, "motorcycle": 4, "bus": 6, "truck": 8}

# Index in the contiguous 80-class COCO order (vision_labels.c).
DOORS_INDEX = {"person": 0, "bicycle": 1, "car": 2, "motorcycle": 3, "bus": 5, "truck": 7}

CLASS_SETS = {
    # The brief's set: the four motor-vehicle classes.
    "traffic4": [
        ("car", ["car"], DOORS_INDEX["car"]),
        ("truck", ["truck"], DOORS_INDEX["truck"]),
        ("bus", ["bus"], DOORS_INDEX["bus"]),
        ("motorcycle", ["motorcycle"], DOORS_INDEX["motorcycle"]),
    ],
    # Everything vision_traffic.c counts today (car, truck, bus, moto, bike,
    # person). Use this if Traffic must keep its bike/person counters.
    "traffic6": [
        ("car", ["car"], DOORS_INDEX["car"]),
        ("truck", ["truck"], DOORS_INDEX["truck"]),
        ("bus", ["bus"], DOORS_INDEX["bus"]),
        ("motorcycle", ["motorcycle"], DOORS_INDEX["motorcycle"]),
        ("bicycle", ["bicycle"], DOORS_INDEX["bicycle"]),
        ("person", ["person"], DOORS_INDEX["person"]),
    ],
    # Ablation: car/truck/bus folded into one class (COCO's car/truck
    # boundary is inconsistent for pickups and vans). Exported as "car", so
    # Traffic would count every four-wheeler as a car.
    "vehicle2": [
        ("vehicle", ["car", "truck", "bus"], DOORS_INDEX["car"]),
        ("motorcycle", ["motorcycle"], DOORS_INDEX["motorcycle"]),
    ],
    # DeskBuddy follow-up (YOLOX-Nano 320, not trained yet): person only.
    "person1": [
        ("person", ["person"], DOORS_INDEX["person"]),
    ],
}


def names(class_set):
    return [c[0] for c in CLASS_SETS[class_set]]


def doors_indices(class_set):
    return [c[2] for c in CLASS_SETS[class_set]]


def coco_fold(class_set):
    """COCO category name -> training label."""
    out = {}
    for label, (_, sources, _) in enumerate(CLASS_SETS[class_set]):
        for s in sources:
            out[s] = label
    return out
