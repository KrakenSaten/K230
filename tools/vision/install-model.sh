#!/usr/bin/env bash
# Put the detector's model on a unit, for the Vision prototype
# (docs/apps/VISION.md, "The model").
#
# Usage: install-model.sh <unit ip> [/path/to/T-Display-K230 checkout]
#
# The model is the pinned vendor SDK's yolov8n.kmodel. It is not part of the
# Doors package or of this repository (docs/LICENSING.md, open item 10), so
# the bench copies it to where pos-vision reads it, records its hash for the
# gate sheet, and nothing else. Run it once per unit, after deploy.sh.
set -euo pipefail

UNIT="${1:?usage: install-model.sh <unit ip> [vendor checkout]}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VENDOR_DIR="${2:-${POCKETOS_VENDOR_DIR:-${REPO_DIR}/vendor/T-Display-K230}}"
MODEL="${VENDOR_DIR}/k230_linux_sdk/buildroot-overlay/package/yolo/utils/yolov8n.kmodel"
DEST=/usr/share/doors/vision

[ -f "${MODEL}" ] || { echo "no model at ${MODEL}" >&2; exit 1; }
sha=$(sha256sum "${MODEL}" | cut -c1-64)
echo "model ${MODEL}: $(stat -c %s "${MODEL}") bytes, sha256 ${sha}"
ssh "root@${UNIT}" "mkdir -p ${DEST}"
scp -q "${MODEL}" "root@${UNIT}:${DEST}/yolov8n.kmodel"
ssh "root@${UNIT}" "sha256sum ${DEST}/yolov8n.kmodel; df -h / | tail -1"
