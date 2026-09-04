#!/usr/bin/env bash
# Apply the LILYGO BSP, the vendor launcher (temporary) and the PocketOS
# package to a pinned K230 Linux SDK checkout.
#
# Usage: apply_to_sdk.sh [/path/to/T-Display-K230 checkout]
#   Default vendor checkout: $POCKETOS_VENDOR_DIR or <repo>/vendor/T-Display-K230
#   The SDK is the k230_linux_sdk submodule inside that checkout.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
PLATFORM_DIR="${REPO_DIR}/platforms/k230"
VENDOR_DIR="${1:-${POCKETOS_VENDOR_DIR:-${REPO_DIR}/vendor/T-Display-K230}}"
SDK_DIR="${VENDOR_DIR}/k230_linux_sdk"
CONF="k230_pocketos_defconfig"

EXPECTED_BSP_COMMIT="$(cat "${PLATFORM_DIR}/vendor_bsp_commit.txt")"
EXPECTED_SDK_COMMIT="$(cat "${PLATFORM_DIR}/vendor_sdk_commit.txt")"

[ -d "${VENDOR_DIR}/k230_bsp" ] || { echo "not a T-Display-K230 checkout: ${VENDOR_DIR}" >&2; exit 1; }
git -C "${SDK_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1 || { echo "SDK is not a git checkout: ${SDK_DIR}" >&2; exit 1; }

BSP_COMMIT="$(git -C "${VENDOR_DIR}" rev-parse HEAD)"
SDK_COMMIT="$(git -C "${SDK_DIR}" rev-parse HEAD)"
echo "PocketOS apply"
echo "Repo   : ${REPO_DIR} (version $(cat "${REPO_DIR}/VERSION"))"
echo "Vendor : ${VENDOR_DIR} @ ${BSP_COMMIT}"
echo "SDK    : ${SDK_DIR} @ ${SDK_COMMIT}"
[ "${BSP_COMMIT}" = "${EXPECTED_BSP_COMMIT}" ] || echo "WARNING: vendor BSP commit differs from pinned ${EXPECTED_BSP_COMMIT}" >&2
[ "${SDK_COMMIT}" = "${EXPECTED_SDK_COMMIT}" ] || echo "WARNING: SDK commit differs from pinned ${EXPECTED_SDK_COMMIT}" >&2

echo "[1/5] Vendor BSP overlay"
"${VENDOR_DIR}/k230_bsp/scripts/apply.sh" "${SDK_DIR}"

echo "[2/5] PocketOS defconfig"
install -m 0644 "${PLATFORM_DIR}/configs/${CONF}" "${SDK_DIR}/buildroot-overlay/configs/${CONF}"

echo "[3/5] Vendor launcher (temporary until the PocketOS shell exists)"
"${VENDOR_DIR}/k230_launcher/scripts/install_to_sdk.sh" "${SDK_DIR}" "${CONF}"

echo "[4/5] PocketOS rootfs overlay"
rsync -a "${PLATFORM_DIR}/rootfs_overlay/" "${SDK_DIR}/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/"

echo "[5/5] PocketOS package"
PKG_DIR="${SDK_DIR}/buildroot-overlay/package/pocketos"
mkdir -p "${PKG_DIR}/src"
install -m 0644 "${PLATFORM_DIR}/package/pocketos/Config.in" "${PKG_DIR}/Config.in"
install -m 0644 "${PLATFORM_DIR}/package/pocketos/pocketos.mk" "${PKG_DIR}/pocketos.mk"
rsync -a --delete \
    --exclude '/vendor' --exclude '/docs' --exclude '/platforms' --exclude '/out' \
    --exclude '/.git' --exclude '*.o' --exclude '/tools/pos/pos' \
    --exclude '/services/radiod/radiod' --exclude '/tests/airtime_test' \
    "${REPO_DIR}/" "${PKG_DIR}/src/"
# RadioLib (MIT) is compiled into radiod; sync its sources beside ours.
mkdir -p "${PKG_DIR}/src/third_party/RadioLib"
rsync -a --delete --exclude '/.git' --exclude '/examples' --exclude '/extras'     "${REPO_DIR}/vendor/RadioLib/" "${PKG_DIR}/src/third_party/RadioLib/"
CONFIG_IN="${SDK_DIR}/buildroot-overlay/package/Config_canaan.in"
if ! grep -q 'source "package/pocketos/Config.in"' "${CONFIG_IN}"; then
    printf '\nsource "package/pocketos/Config.in"\n' >> "${CONFIG_IN}"
fi
# Force Buildroot to re-sync the overlay and rebuild the package next time.
rm -rf "${SDK_DIR}/output/buildroot-2025.02.1/package/pocketos" \
       "${SDK_DIR}/output/${CONF}/build/pocketos-"*
for stamp in "${SDK_DIR}/.overlay_sync" "${SDK_DIR}"/output/*/.overlay_sync; do
    [ -f "${stamp}" ] && mv "${stamp}" "${stamp}.stale.$(date -u +%Y%m%d%H%M%S)"
done

echo "Done. Build with: ${PLATFORM_DIR}/scripts/build_image.sh ${VENDOR_DIR}"
