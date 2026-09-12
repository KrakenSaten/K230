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

# The toolchain the SDK expects. Overridable so the export path can be
# exercised without one (tests/build_provenance_test.sh); the default is the
# only location docs/BUILD_ENVIRONMENT.md describes.
POCKETOS_TOOLCHAIN_CC="${POCKETOS_TOOLCHAIN_CC:-/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/bin/riscv64-unknown-linux-gnu-gcc}"
[ -x "${POCKETOS_TOOLCHAIN_CC}" ] \
    || { echo "toolchain missing, see docs/BUILD_ENVIRONMENT.md" >&2; exit 1; }

# Everything the export treats as "produced by this build" is newer than this.
BUILD_START_EPOCH="$(date +%s)"
[ -f "${SDK_DIR}/buildroot-overlay/configs/${CONF}" ] \
    || { echo "run apply_to_sdk.sh first" >&2; exit 1; }

echo "Build ${CONF} target=${TARGET} in ${SDK_DIR}"
make -C "${SDK_DIR}" CONF="${CONF}" "${CONF}"
make -C "${SDK_DIR}" CONF="${CONF}" "${TARGET}"

if [ "${TARGET}" = "all" ]; then
    IMAGES="${SDK_DIR}/output/${CONF}/images"

    # Artefacts the export exists to produce. A missing one used to be skipped
    # by `[ -f ... ] && cp`, so a build that never produced an image still
    # exported a directory, wrote a report about it and checksummed whatever
    # happened to be lying there from last time.
    REQUIRED="sysimage-sdcard.img Image"
    OPTIONAL="sysimage-sdcard.img.gz k230-canmv-rm69a10.dtb k230-canmv-rm69a10-hdmi.dtb k.dtb"

    for name in ${REQUIRED}; do
        [ -f "${IMAGES}/${name}" ] || {
            echo "ERROR: ${name} was not produced by this build (${IMAGES})." >&2
            echo "       Nothing has been exported; ${OUT_DIR} is untouched." >&2
            exit 1
        }
        # And produced by *this* build. An image left from an earlier run is
        # the one thing an export must never pass off as the current one.
        if [ "$(stat -c %Y "${IMAGES}/${name}")" -lt "${BUILD_START_EPOCH}" ]; then
            echo "ERROR: ${name} is older than this build started." >&2
            echo "       It is left over from an earlier run, so this build did not" >&2
            echo "       produce it. Nothing has been exported." >&2
            exit 1
        fi
    done

    # A fresh staging directory, swapped in at the end. Exporting straight into
    # ${OUT_DIR} left every artefact of every previous build in place, and the
    # checksum file then covered them too - so SHA256SUMS.txt listed, with a
    # current date beside it, images this build had never seen.
    STAGE="${OUT_DIR}.staging.$$"
    rm -rf "${STAGE}"
    mkdir -p "${STAGE}"
    trap 'rm -rf "${STAGE}"' EXIT

    exported=""
    for name in ${REQUIRED} ${OPTIONAL}; do
        if [ -f "${IMAGES}/${name}" ]; then
            cp -a "${IMAGES}/${name}" "${STAGE}/"
            exported="${exported} ${name}"
        fi
    done

    # The report describes the source that was applied to this SDK, read from
    # the manifest apply_to_sdk.sh left behind - not whatever the repository
    # happens to be checked out at now. Apply at A, check out B, build without
    # re-applying, and this still says A, because A is what is in the package.
    MANIFEST="${SDK_DIR}/.pocketos-applied"
    [ -f "${MANIFEST}" ] || {
        echo "ERROR: no applied-source manifest at ${MANIFEST}." >&2
        echo "       Run apply_to_sdk.sh first: without it this build cannot say" >&2
        echo "       which source it contains, and a report that guesses is worse" >&2
        echo "       than none." >&2
        exit 1
    }
    m() { sed -n "s/^$1=//p" "${MANIFEST}"; }

    {
        echo "PocketOS $(m pocketos_version) image for LILYGO T-Display K230"
        echo "Build UTC : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "Defconfig : ${CONF}"
        echo "Vendor BSP: $(m vendor_bsp_commit)"
        echo "SDK       : $(m sdk_commit)"
        echo "PocketOS  : $(m pocketos_commit_short)$([ "$(m source_tree_state)" = "dirty" ] && echo " (applied from a dirty tree)")"
        echo "BUILD_ID  : $(m pocketos_build_id)"
        echo "Applied   : $(m applied_utc) (source of the above; this build did not re-apply)"
        [ "$(m dirty_override)" = "yes" ] && \
            echo "Override  : POCKETOS_ALLOW_DIRTY_BUILD=1 at apply time -- uncommitted changes are NOT in this image"
        echo "Exported  :${exported}"
    } > "${STAGE}/BUILD_INFO.txt"

    # Only what this build exported, named explicitly rather than by listing
    # the directory.
    (cd "${STAGE}" && sha256sum BUILD_INFO.txt ${exported} > SHA256SUMS.txt)

    rm -rf "${OUT_DIR}"
    mkdir -p "$(dirname "${OUT_DIR}")"
    mv "${STAGE}" "${OUT_DIR}"
    trap - EXIT
    echo "Exported to ${OUT_DIR}"
    cat "${OUT_DIR}/BUILD_INFO.txt"
fi
