"""YOLOX-Tiny 416, DOORS Traffic classes, trained from scratch.

The architecture and augmentation are upstream's exps/default/yolox_tiny.py
(depth 0.33, width 0.375, 416 input, mosaic scale 0.5-1.5, no mixup) so a
DOORS-trained checkpoint is comparable with the upstream one the 2026-10-04
evaluation measured. What differs is the data (DOORS_TRAIN_DATA, built by
data/prepare_coco_traffic.py), the class count and a fixed seed.

Nothing is loaded from a pretrained checkpoint: YOLOX's own init_yolo and
initialize_biases run in get_model, and train.sh refuses -c/--ckpt.

Environment (all optional except DOORS_TRAIN_DATA):
  DOORS_TRAIN_DATA   dataset root: annotations/, train2017/, val2017/
  DOORS_CLASS_SET    traffic4 (default) | traffic6 | vehicle2, must match the
                     class set the dataset was prepared with (checked)
  DOORS_MAX_EPOCH    default 300 (upstream)
  DOORS_SEED         default 20261004
  DOORS_WORKERS      data loader workers, default 4
  DOORS_OUTPUT_DIR   where YOLOX writes the run, default ./YOLOX_outputs
"""
import json
import os

from yolox.exp import Exp as MyExp

# Kept in step with data/class_sets.py (no import: YOLOX loads this file by
# path, so the training tree is not on sys.path).
CLASS_SETS = {
    "traffic4": ["car", "truck", "bus", "motorcycle"],
    "traffic6": ["car", "truck", "bus", "motorcycle", "bicycle", "person"],
    "vehicle2": ["vehicle", "motorcycle"],
}


class Exp(MyExp):
    def __init__(self):
        super().__init__()
        # ---- model: upstream yolox_tiny
        self.depth = 0.33
        self.width = 0.375
        self.act = "silu"

        # ---- data
        self.class_set = os.environ.get("DOORS_CLASS_SET", "traffic4")
        if self.class_set not in CLASS_SETS:
            raise ValueError("DOORS_CLASS_SET must be one of %s" % sorted(CLASS_SETS))
        self.class_names = CLASS_SETS[self.class_set]
        self.num_classes = len(self.class_names)
        self.data_dir = os.environ.get("DOORS_TRAIN_DATA")
        self.train_ann = "traffic_train.json"
        self.val_ann = "traffic_val.json"
        self.data_num_workers = int(os.environ.get("DOORS_WORKERS", "4"))

        self.input_size = (416, 416)
        self.test_size = (416, 416)
        self.random_size = (10, 20)  # multiscale 320..640 during training

        # ---- augmentation: upstream yolox_tiny
        self.mosaic_prob = 1.0
        self.mosaic_scale = (0.5, 1.5)
        self.enable_mixup = False
        self.hsv_prob = 1.0
        self.flip_prob = 0.5
        self.degrees = 10.0
        self.translate = 0.1
        self.shear = 2.0

        # ---- schedule: upstream defaults (SGD, nesterov, warm cosine)
        self.max_epoch = int(os.environ.get("DOORS_MAX_EPOCH", "300"))
        self.warmup_epochs = 5
        self.no_aug_epochs = 15
        self.basic_lr_per_img = 0.01 / 64.0
        self.scheduler = "yoloxwarmcos"
        self.min_lr_ratio = 0.05
        self.weight_decay = 5e-4
        self.momentum = 0.9
        self.ema = True
        self.eval_interval = 10
        self.save_history_ckpt = False

        self.test_conf = 0.01
        self.nmsthre = 0.65

        self.seed = int(os.environ.get("DOORS_SEED", "20261004"))
        self.output_dir = os.environ.get("DOORS_OUTPUT_DIR", "./YOLOX_outputs")
        self.exp_name = "yolox_tiny_traffic_416_" + self.class_set

    def get_dataset(self, cache=False, cache_type="ram"):
        self._check_dataset()
        return super().get_dataset(cache=cache, cache_type=cache_type)

    def _check_dataset(self):
        """The dataset must have been prepared for this class set; a
        mismatch would silently train the wrong label order."""
        if not self.data_dir:
            raise RuntimeError("DOORS_TRAIN_DATA is not set")
        man = os.path.join(self.data_dir, "manifest.json")
        with open(man) as f:
            m = json.load(f)
        if m.get("class_set") != self.class_set or m.get("classes") != self.class_names:
            raise RuntimeError("dataset %s was prepared for %s %s, the exp wants %s %s"
                               % (self.data_dir, m.get("class_set"), m.get("classes"),
                                  self.class_set, self.class_names))
