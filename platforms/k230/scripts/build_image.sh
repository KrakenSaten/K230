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

[ -f "${SDK_DIR}/buildroot-overlay/configs/${CONF}" ] \
    || { echo "run apply_to_sdk.sh first" >&2; exit 1; }

# The files the boot partition needs before U-Boot can start a kernel.
BOOT_ARTEFACTS="Image k230-canmv-rm69a10.dtb k230-canmv-rm69a10-hdmi.dtb"

# Buildroot copies these into images/ from the linux package's install-images
# step, and it runs that step exactly once: afterwards .stamp_images_installed
# says the job is done, whatever became of the files. Delete images/Image - as
# the v0.0.9 release procedure did, to prove no artefact was being reused - and
# nothing puts it back.
#
# Nothing downstream notices. post-image.sh does `cp Image boot/`, which fails;
# but that script turns errexit off partway through (`set +e` on line 90, never
# restored), so the failure is printed and ignored. genimage then packs a boot
# partition with no kernel into sysimage-sdcard.img, and the build exits 0.
# That image was built, exported, checksummed, flashed and verified before
# U-Boot said `Failed to load '/Image'`.
#
# So: if the artefacts are not in images/, drop the stamp that claims they are,
# and let Buildroot reinstall them. Cheap, idempotent, and it does not rebuild
# the kernel - only re-runs the copy that was skipped.
ensure_kernel_artefacts() {
    local images="${SDK_DIR}/output/${CONF}/images" name stamp missing=""
    [ -d "${images}" ] || return 0
    for name in ${BOOT_ARTEFACTS}; do
        [ -f "${images}/${name}" ] || missing="${missing} ${name}"
    done
    [ -n "${missing}" ] || return 0
    echo "note: images/ is missing${missing}"
    for stamp in "${SDK_DIR}/output/${CONF}/build/linux-"*/.stamp_images_installed; do
        [ -f "${stamp}" ] || continue
        echo "      dropping $(basename "$(dirname "${stamp}")")/.stamp_images_installed so Buildroot reinstalls them"
        rm -f "${stamp}"
    done
}
ensure_kernel_artefacts

echo "Build ${CONF} target=${TARGET} in ${SDK_DIR}"
make -C "${SDK_DIR}" CONF="${CONF}" "${CONF}"
make -C "${SDK_DIR}" CONF="${CONF}" "${TARGET}"

if [ "${TARGET}" = "all" ]; then
    IMAGES="${SDK_DIR}/output/${CONF}/images"

    MANIFEST="${SDK_DIR}/.pocketos-applied"
    [ -f "${MANIFEST}" ] || {
        echo "ERROR: no applied-source manifest at ${MANIFEST}." >&2
        echo "       Run apply_to_sdk.sh first: without it this build cannot say" >&2
        echo "       which source it contains, and a report that guesses is worse" >&2
        echo "       than none." >&2
        exit 1
    }
    m() { sed -n "s/^$1=//p" "${MANIFEST}"; }

    # The release deliverable. It is what gets flashed, so it must exist and it
    # must have been built from the source currently applied to this SDK.
    #
    # Everything else here is a copy of something already inside that image -
    # the kernel, the device trees - and Buildroot only refreshes those when
    # their own package rebuilds. Requiring them to be new fails an ordinary
    # release build: the v0.0.9 build produced a new SD-card image from a
    # kernel that had not changed since September 4th, and the export refused
    # it. So they are exported when present and reported as carried over when
    # they predate the apply, which is the honest answer rather than either
    # refusing the build or passing them off as this build's work.
    REQUIRED="sysimage-sdcard.img"
    OPTIONAL="sysimage-sdcard.img.gz Image k230-canmv-rm69a10.dtb k230-canmv-rm69a10-hdmi.dtb k.dtb"

    # Freshness is measured against the apply, not against this script's start.
    # "Newer than the moment I began" is the wrong question: re-running the
    # export over an unchanged build would fail it, because Buildroot correctly
    # regenerates nothing. "Built after the source it claims to contain was
    # applied" is the property that actually matters, and it still catches an
    # image left over from before the current source went in.
    APPLIED_EPOCH="$(m applied_epoch)"
    # A manifest written before this field existed judges nothing rather than
    # refusing a build it cannot date.
    [ -n "${APPLIED_EPOCH}" ] || APPLIED_EPOCH=0

    for name in ${REQUIRED}; do
        [ -f "${IMAGES}/${name}" ] || {
            echo "ERROR: ${name} was not produced by this build (${IMAGES})." >&2
            echo "       Nothing has been exported; ${OUT_DIR} is untouched." >&2
            exit 1
        }
        if [ "$(stat -c %Y "${IMAGES}/${name}")" -lt "${APPLIED_EPOCH}" ]; then
            echo "ERROR: ${name} is older than the source applied to this SDK." >&2
            echo "       It was built before the current source went in, so it does" >&2
            echo "       not contain it. Nothing has been exported." >&2
            exit 1
        fi
    done

    # Everything above this point asks questions about the export directory:
    # which files were produced, how old they are, whether their checksums
    # match. None of that can tell whether the image can boot - v0.0.9 passed
    # all of it with no kernel inside. So before anything is exported, read
    # partition 1 out of the image itself and check the files U-Boot loads.
    "${SCRIPT_DIR}/verify_image.sh" "${IMAGES}/sysimage-sdcard.img" || {
        echo "ERROR: the image this build produced cannot boot." >&2
        echo "       Nothing has been exported; ${OUT_DIR} is untouched." >&2
        exit 1
    }

    # A fresh staging directory, swapped in at the end. Exporting straight into
    # ${OUT_DIR} left every artefact of every previous build in place, and the
    # checksum file then covered them too - so SHA256SUMS.txt listed, with a
    # current date beside it, images this build had never seen.
    STAGE="${OUT_DIR}.staging.$$"
    rm -rf "${STAGE}"
    mkdir -p "${STAGE}"
    trap 'rm -rf "${STAGE}"' EXIT

    exported=""
    carried=""
    for name in ${REQUIRED} ${OPTIONAL}; do
        if [ -f "${IMAGES}/${name}" ]; then
            cp -a "${IMAGES}/${name}" "${STAGE}/"
            exported="${exported} ${name}"
            # Present, exported, but not made by this build. Named separately
            # so nobody has to compare timestamps to find that out.
            if [ "$(stat -c %Y "${IMAGES}/${name}")" -lt "${APPLIED_EPOCH}" ]; then
                carried="${carried} ${name}"
            fi
        fi
    done

    # The report describes the source that was applied to this SDK, read from
    # the manifest apply_to_sdk.sh left behind - not whatever the repository
    # happens to be checked out at now. Apply at A, check out B, build without
    # re-applying, and this still says A, because A is what is in the package.
    {
        echo "PocketOS $(m pocketos_version) image for LILYGO T-Display K230"
        echo "Build UTC : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "Defconfig : ${CONF}"
        echo "Vendor BSP: $(m vendor_bsp_commit)"
        echo "SDK       : $(m sdk_commit)"
        echo "RadioLib  : $(m radiolib_commit)$([ "$(m radiolib_state)" != "clean" ] && echo " ($(m radiolib_state))")"
        echo "ggwave    : $(m ggwave_commit)$([ "$(m ggwave_state)" != "clean" ] && echo " ($(m ggwave_state))")"
        echo "PocketOS  : $(m pocketos_commit_short)$([ "$(m source_tree_state)" = "dirty" ] && echo " (applied from a dirty tree)")"
        echo "BUILD_ID  : $(m pocketos_build_id)"
        echo "Applied   : $(m applied_utc) (source of the above; this build did not re-apply)"
        [ "$(m dirty_override)" = "yes" ] && \
            echo "Override  : POCKETOS_ALLOW_DIRTY_BUILD=1 at apply time -- uncommitted changes are NOT in this image"
        echo "Exported  :${exported}"
        [ -n "${carried}" ] && \
            echo "Carried   :${carried} (unchanged since before this source was applied; Buildroot rebuilds these only when their own package changes)"
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
