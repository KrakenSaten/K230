#!/usr/bin/env bash
# Build the PocketOS SD-card image for the T-Display K230 and export it.
#
# Usage: build_image.sh [/path/to/T-Display-K230 checkout] [make-target]
#   make-target defaults to "all"; use e.g. "pocketos-rebuild" for the package only.
#   Images are exported to $POCKETOS_OUT_DIR or <repo>/out/k230/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
VENDOR_DIR="${1:-${POCKETOS_VENDOR_DIR:-${REPO_DIR}/vendor/T-Display-K230}}"
TARGET="${2:-all}"
SDK_DIR="${VENDOR_DIR}/k230_linux_sdk"
CONF="k230_pocketos_defconfig"
OUT_DIR="${POCKETOS_OUT_DIR:-${REPO_DIR}/out/k230}"

# WSL appends Windows PATH entries containing spaces; Buildroot refuses them.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

[ -x /opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/bin/riscv64-unknown-linux-gnu-gcc ] \
    || { echo "toolchain missing, see docs/BUILD_ENVIRONMENT.md" >&2; exit 1; }
[ -f "${SDK_DIR}/buildroot-overlay/configs/${CONF}" ] \
    || { echo "run apply_to_sdk.sh first" >&2; exit 1; }

echo "Build ${CONF} target=${TARGET} in ${SDK_DIR}"
make -C "${SDK_DIR}" CONF="${CONF}" "${CONF}"
make -C "${SDK_DIR}" CONF="${CONF}" "${TARGET}"

if [ "${TARGET}" = "all" ]; then
    IMAGES="${SDK_DIR}/output/${CONF}/images"
    mkdir -p "${OUT_DIR}"
    for name in sysimage-sdcard.img sysimage-sdcard.img.gz Image \
                k230-canmv-rm69a10.dtb k230-canmv-rm69a10-hdmi.dtb k.dtb; do
        [ -f "${IMAGES}/${name}" ] && cp -a "${IMAGES}/${name}" "${OUT_DIR}/"
    done
    cat > "${OUT_DIR}/BUILD_INFO.txt" <<EOF
PocketOS $(cat "${REPO_DIR}/VERSION") image for LILYGO T-Display K230
Build UTC : $(date -u +%Y-%m-%dT%H:%M:%SZ)
Defconfig : ${CONF}
Vendor BSP: $(git -C "${VENDOR_DIR}" rev-parse HEAD)
SDK       : $(git -C "${SDK_DIR}" rev-parse HEAD)
PocketOS  : $(git -C "${REPO_DIR}" rev-parse --short HEAD 2>/dev/null || echo "no commit yet")$(git -C "${REPO_DIR}" diff --quiet 2>/dev/null || echo " (dirty)")
EOF
    (cd "${OUT_DIR}" && ls -1 | grep -v SHA256SUMS.txt | xargs sha256sum > SHA256SUMS.txt)
    echo "Exported to ${OUT_DIR}"
    cat "${OUT_DIR}/BUILD_INFO.txt"
fi
