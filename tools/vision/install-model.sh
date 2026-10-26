#!/usr/bin/env bash
# Put a detector model on a unit by hand, for the Vision app
# (docs/apps/VISION.md, "The model" and "GENERAL 80").
#
# Usage: install-model.sh <unit ip> [/path/to/T-Display-K230 checkout]
#        POCKETOS_VISION_MODEL_FILE=<file> install-model.sh <unit ip>
#        install-model.sh --general <file> <unit ip>
#
# Without --general: the helper's own default, yolov8n.kmodel, from the pinned
# SDK (or another file to try, POCKETOS_VISION_MODEL_FILE), copied to where
# pos-vision reads it; it prints its hash for the gate sheet and says whether
# that is the pinned file (yolov8n.kmodel.sha256). No image has carried it since
# 0.3.5. The model is AGPL-3.0 and may be used on internal units only
# (docs/LICENSING.md, item 10).
#
# With --general: GENERAL 80, the upstream YOLOX-Tiny 416 file that Vision's
# MODEL button offers beside R0 (80 COCO classes). Only the file whose sha256
# is in general-80-model.sha256 is copied - the one the unit A A/B gate passed
# with - under the name Vision looks for. Its weights carry no licence statement
# (MODEL_LICENSES.md): it is for bench units, by hand, and is refused in an
# image. How to obtain it is in docs/apps/VISION.md, "GENERAL 80".
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DEST=/usr/share/doors/vision

if [ "${1:-}" = "--general" ]; then
    MODEL="${2:?usage: install-model.sh --general <file> <unit ip>}"
    UNIT="${3:?usage: install-model.sh --general <file> <unit ip>}"
    NAME=det-upstream-yolox-tiny-416.kmodel
    [ -f "${MODEL}" ] || { echo "no model at ${MODEL}" >&2; exit 1; }
    sha=$(sha256sum "${MODEL}" | cut -c1-64)
    pin=$(cut -c1-64 "${SCRIPT_DIR}/general-80-model.sha256")
    echo "model ${MODEL}: $(stat -c %s "${MODEL}") bytes, sha256 ${sha}"
    if [ "${sha}" != "${pin}" ]; then
        echo "refused: not the GENERAL 80 file the A/B gate passed with (sha256 ${pin})" >&2
        exit 2
    fi
else
    UNIT="${1:?usage: install-model.sh <unit ip> [vendor checkout]}"
    VENDOR_DIR="${2:-${POCKETOS_VENDOR_DIR:-${REPO_DIR}/vendor/T-Display-K230}}"
    MODEL="${POCKETOS_VISION_MODEL_FILE:-${VENDOR_DIR}/k230_linux_sdk/buildroot-overlay/package/yolo/utils/yolov8n.kmodel}"
    NAME=yolov8n.kmodel
    [ -f "${MODEL}" ] || { echo "no model at ${MODEL}" >&2; exit 1; }
    sha=$(sha256sum "${MODEL}" | cut -c1-64)
    echo "model ${MODEL}: $(stat -c %s "${MODEL}") bytes, sha256 ${sha}"
    if [ "${sha}" = "$(cut -c1-64 "${SCRIPT_DIR}/yolov8n.kmodel.sha256")" ]; then
        echo "this is the pinned model"
    else
        echo "NOTE: this is not the pinned model (${SCRIPT_DIR}/yolov8n.kmodel.sha256)"
    fi
fi
ssh "root@${UNIT}" "mkdir -p ${DEST}"
scp -q "${MODEL}" "root@${UNIT}:${DEST}/${NAME}"
ssh "root@${UNIT}" "sha256sum ${DEST}/${NAME}; df -h / | tail -1"
