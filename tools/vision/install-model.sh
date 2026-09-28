#!/usr/bin/env bash
# Put the detector's model on a unit, for the Vision app
# (docs/apps/VISION.md, "The model").
#
# Usage: install-model.sh <unit ip> [/path/to/T-Display-K230 checkout]
#        POCKETOS_VISION_MODEL_FILE=<file> install-model.sh <unit ip>
#
# Images built from the pocketos package already carry the model at
# /usr/share/doors/vision/yolov8n.kmodel (pocketos.mk installs the pinned SDK's
# copy, checked against yolov8n.kmodel.sha256 next to this script). This is for
# the other cases: a unit whose userspace was deployed by hand onto an older
# image, a model file that was removed or damaged, or another model to try
# (POCKETOS_VISION_MODEL_FILE). It copies the file to where pos-vision reads it
# and prints its hash for the gate sheet. It also says whether that is the
# pinned model. The model is AGPL-3.0 and may be used on internal units only
# (docs/LICENSING.md, item 10).
set -euo pipefail

UNIT="${1:?usage: install-model.sh <unit ip> [vendor checkout]}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VENDOR_DIR="${2:-${POCKETOS_VENDOR_DIR:-${REPO_DIR}/vendor/T-Display-K230}}"
MODEL="${POCKETOS_VISION_MODEL_FILE:-${VENDOR_DIR}/k230_linux_sdk/buildroot-overlay/package/yolo/utils/yolov8n.kmodel}"
DEST=/usr/share/doors/vision

[ -f "${MODEL}" ] || { echo "no model at ${MODEL}" >&2; exit 1; }
sha=$(sha256sum "${MODEL}" | cut -c1-64)
echo "model ${MODEL}: $(stat -c %s "${MODEL}") bytes, sha256 ${sha}"
if [ "${sha}" = "$(cut -c1-64 "${SCRIPT_DIR}/yolov8n.kmodel.sha256")" ]; then
    echo "this is the pinned model, the one images carry"
else
    echo "NOTE: this is not the pinned model (${SCRIPT_DIR}/yolov8n.kmodel.sha256)"
fi
ssh "root@${UNIT}" "mkdir -p ${DEST}"
scp -q "${MODEL}" "root@${UNIT}:${DEST}/yolov8n.kmodel"
ssh "root@${UNIT}" "sha256sum ${DEST}/yolov8n.kmodel; df -h / | tail -1"
