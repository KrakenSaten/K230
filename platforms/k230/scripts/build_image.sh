#!/usr/bin/env bash
# Build the Doors SD-card image for the T-Display K230 and export it.
#
# Usage: build_image.sh [/path/to/T-Display-K230 checkout] [make-target]
#   make-target defaults to "all"; use e.g. "pocketos-rebuild" for the package only.
#   Images are exported to $POCKETOS_OUT_DIR or <repo>/out/k230/: the vendor
#   build's sysimage-sdcard.img, and the same image as the release artefact
#   doors-<version>[-rcN]-tdisplay-k230-<build_id>.img.gz with a .sha256 beside it.
#   POCKETOS_RELEASE_RC=N (a positive whole number) adds -rcN to that name.
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

# meshcored is in every image (the package builds it with ENABLE_MESHCORED=1)
# since its third-party notices landed (docs/LICENSING.md item 9). Whether the
# notices still cover it is refused early by apply_to_sdk.sh and finally by the
# package's install step (the Makefile's meshcored-shipping-check); the image
# gate at the end of this script (verify_image.sh) then refuses an image whose
# meshcored arrived without its init script, or from another build.

# The toolchain the SDK expects. Overridable so the export path can be
# exercised without one (tests/build_provenance_test.sh); the default is the
# only location docs/BUILD_ENVIRONMENT.md describes.
POCKETOS_TOOLCHAIN_CC="${POCKETOS_TOOLCHAIN_CC:-/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/bin/riscv64-unknown-linux-gnu-gcc}"
[ -x "${POCKETOS_TOOLCHAIN_CC}" ] \
    || { echo "toolchain missing, see docs/BUILD_ENVIRONMENT.md" >&2; exit 1; }

[ -f "${SDK_DIR}/buildroot-overlay/configs/${CONF}" ] \
    || { echo "run apply_to_sdk.sh first" >&2; exit 1; }

# The release candidate number goes into the artefact's name, so a value that
# cannot be one is refused before an hour of building, not after it.
POCKETOS_RELEASE_RC="${POCKETOS_RELEASE_RC:-}"
case "${POCKETOS_RELEASE_RC}" in
    ""|[1-9]|[1-9][0-9]|[1-9][0-9][0-9]) ;;
    *)
        echo "ERROR: POCKETOS_RELEASE_RC must be a whole number from 1 to 999 (2 gives -rc2), not '${POCKETOS_RELEASE_RC}'." >&2
        exit 1 ;;
esac

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

# The third-party notices the image carries (docs/LICENSING.md): the file the
# package installed must be the packaged one and match the sha256 in
# pocketos.hash, and the LVGL that was built must compile in no bundled code the
# notices do not name. apply_to_sdk.sh checked the texts themselves before
# packaging; this is the part only a build can see.
PKG_SRC="${SDK_DIR}/buildroot-overlay/package/pocketos/src"
if [ -f "${PKG_SRC}/tools/legal/gen_notices.sh" ]; then
    TARGET_NOTICES="${SDK_DIR}/output/${CONF}/target/usr/share/doors/THIRD_PARTY_NOTICES.txt"
    if ! cmp -s "${TARGET_NOTICES}" "${PKG_SRC}/THIRD_PARTY_NOTICES.txt"; then
        echo "ERROR: ${TARGET_NOTICES#"${SDK_DIR}"/} is missing or is not the packaged THIRD_PARTY_NOTICES.txt." >&2
        exit 1
    fi
    # The path the notices had through v0.0.9 must be the link to them, not a
    # copy an older package left in the target tree, which would go stale.
    OLD_NOTICES="${SDK_DIR}/output/${CONF}/target/usr/share/pocketos/THIRD_PARTY_NOTICES.txt"
    if [ ! -L "${OLD_NOTICES}" ] || ! cmp -s "${OLD_NOTICES}" "${TARGET_NOTICES}"; then
        echo "ERROR: ${OLD_NOTICES#"${SDK_DIR}"/} is not the link to ${TARGET_NOTICES#"${SDK_DIR}"/}." >&2
        exit 1
    fi
    # Buildroot reads pocketos.hash only during legal-info, and accepts a hash
    # file with no line for the notices, so the build holds them to it here, in
    # both copies: the one applied and the one Buildroot synced and will read.
    for PKG_HASH in "${SDK_DIR}/buildroot-overlay/package/pocketos/pocketos.hash" \
                    "${SDK_DIR}/output/buildroot-2025.02.1/package/pocketos/pocketos.hash"; do
        WANT="$(awk '$1 == "sha256" && $3 == "THIRD_PARTY_NOTICES.txt" { print $2 }' "${PKG_HASH}" 2>/dev/null || true)"
        if [ -z "${WANT}" ] || [ "${WANT}" != "$(sha256sum < "${TARGET_NOTICES}" | cut -d' ' -f1)" ]; then
            echo "ERROR: ${PKG_HASH#"${SDK_DIR}"/} is missing or does not hold the sha256 of" >&2
            echo "       the installed THIRD_PARTY_NOTICES.txt, so legal-info would refuse it." >&2
            echo "       Run tools/legal/gen_notices.sh, commit, and apply again." >&2
            exit 1
        fi
    done
    LV_CONF="${SDK_DIR}/output/${CONF}/staging/usr/include/lvgl/lv_conf.h"
    if [ -f "${LV_CONF}" ]; then
        bash "${PKG_SRC}/tools/legal/gen_notices.sh" --verify-lvconf "${LV_CONF}"
    fi
    echo "Third-party notices: installed, matching pocketos.hash, and the built LVGL matches them."
fi

# ADR-005 Phase 3: exactly one shell service, checked in the target tree before
# the rootfs is assembled from it. verify_image.sh asks the same question of the
# finished image; this one fails while the answer is still cheap to fix, and it
# names what to remove. Buildroot never deletes from a target tree, so a tree
# that has ever built a PocketOS-era shell keeps it until something says so.
TGT="${SDK_DIR}/output/${CONF}/target"
SHELL_TROUBLE=0
if [ ! -d "${TGT}" ]; then
    # No tree to look at (a stub SDK in a test); verify_image.sh still asks the
    # finished image, which is the check that actually ships.
    echo "Shell service: no target tree at output/${CONF}/target; checked on the image instead."
else
    for stale in usr/bin/pocketos-shell etc/init.d/S90pocketos-shell; do
        if [ -e "${TGT}/${stale}" ]; then
            echo "ERROR: the PocketOS-era shell is still in the target tree: ${stale}" >&2
            echo "       Re-run apply_to_sdk.sh (it removes it), or delete ${TGT}/${stale}." >&2
            SHELL_TROUBLE=1
        fi
    done
    # And in the overlay Buildroot actually builds the rootfs from: it keeps a
    # synced copy of board/..., so an old service left there is copied back
    # into every image however clean the target tree looks.
    for stale in "${SDK_DIR}"/output/buildroot-*/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S90pocketos-shell \
                 "${SDK_DIR}/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S90pocketos-shell"; do
        if [ -e "${stale}" ]; then
            echo "ERROR: the PocketOS-era shell is still in a rootfs overlay: ${stale#"${SDK_DIR}"/}" >&2
            echo "       Re-run apply_to_sdk.sh (it removes it)." >&2
            SHELL_TROUBLE=1
        fi
    done
    for want in usr/bin/doors-shell etc/init.d/S90doors-shell; do
        if [ ! -e "${TGT}/${want}" ]; then
            echo "ERROR: the Doors shell service is missing from the target tree: ${want}" >&2
            SHELL_TROUBLE=1
        fi
    done
    # Panel ownership is a per-unit decision, so no settings file is packaged.
    for never in etc/default/doors-shell etc/default/pocketos-shell etc/default/k230_phone_ui; do
        if [ -e "${TGT}/${never}" ]; then
            echo "ERROR: ${never} must not be shipped in the image (it is a unit's own setting)." >&2
            SHELL_TROUBLE=1
        fi
    done
    # Without those files the defaults decide, and the first boot of a fresh
    # card has to be Doors': no vendor launcher at all (apply_to_sdk.sh [3/5]
    # removes it), shell on.
    for gone in etc/init.d/S99zz_k230_phone_ui root/app/k230_phone_ui \
                root/music root/videos root/notification; do
        if [ -e "${TGT}/${gone}" ]; then
            echo "ERROR: the vendor launcher is still in the target tree: ${gone}" >&2
            echo "       Re-run apply_to_sdk.sh (it removes it)." >&2
            SHELL_TROUBLE=1
        fi
    done
    if ! grep -q '^ENABLE=1$' "${TGT}/etc/init.d/S90doors-shell" 2>/dev/null; then
        echo "ERROR: S90doors-shell in the target tree is not on by default." >&2
        SHELL_TROUBLE=1
    fi
    [ "${SHELL_TROUBLE}" -eq 0 ] || exit 1
    echo "Shell service: one identity in the target tree (doors-shell), no PocketOS-era leftovers."
fi

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

    # The release artefact (ADR-005 Phase 2): the SD-card image that was just
    # verified, under the product's name, compressed, with a checksum file of
    # its own so it can travel without this directory. It is the same bytes as
    # sysimage-sdcard.img, which keeps the vendor build's name - not a second
    # build. The version and build id come from the applied manifest, like
    # everything the report says. gzip -n leaves the name and time out of the
    # header, and the result is read back before it is exported.
    REL_VERSION="$(m pocketos_version)"
    REL_BUILD_ID="$(m pocketos_build_id)"
    if [ -z "${REL_VERSION}" ] || [ -z "${REL_BUILD_ID}" ]; then
        echo "ERROR: the applied-source manifest has no pocketos_version or pocketos_build_id," >&2
        echo "       so the release artefact cannot be named. Run apply_to_sdk.sh again." >&2
        echo "       Nothing has been exported; ${OUT_DIR} is untouched." >&2
        exit 1
    fi
    RELEASE="doors-${REL_VERSION}${POCKETOS_RELEASE_RC:+-rc${POCKETOS_RELEASE_RC}}-tdisplay-k230-${REL_BUILD_ID}.img.gz"
    case "${RELEASE}" in
        *[!A-Za-z0-9._+-]*)
            echo "ERROR: '${RELEASE}' is not a usable file name (version or build id from the manifest)." >&2
            echo "       Nothing has been exported; ${OUT_DIR} is untouched." >&2
            exit 1 ;;
    esac
    IMAGE_SHA256="$(sha256sum < "${STAGE}/sysimage-sdcard.img" | cut -d' ' -f1)"
    gzip -n -c "${STAGE}/sysimage-sdcard.img" > "${STAGE}/${RELEASE}"
    if [ "$(gzip -dc "${STAGE}/${RELEASE}" | sha256sum | cut -d' ' -f1)" != "${IMAGE_SHA256}" ]; then
        echo "ERROR: ${RELEASE} does not decompress to sysimage-sdcard.img." >&2
        echo "       Nothing has been exported; ${OUT_DIR} is untouched." >&2
        exit 1
    fi
    (cd "${STAGE}" && sha256sum "${RELEASE}" > "${RELEASE}.sha256")
    exported="${exported} ${RELEASE} ${RELEASE}.sha256"

    # The report describes the source that was applied to this SDK, read from
    # the manifest apply_to_sdk.sh left behind - not whatever the repository
    # happens to be checked out at now. Apply at A, check out B, build without
    # re-applying, and this still says A, because A is what is in the package.
    {
        echo "Doors $(m pocketos_version) image for LILYGO T-Display K230"
        echo "Build UTC : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "Defconfig : ${CONF}"
        echo "Vendor BSP: $(m vendor_bsp_commit)"
        echo "SDK       : $(m sdk_commit)"
        echo "RadioLib  : $(m radiolib_commit)$([ "$(m radiolib_state)" != "clean" ] && echo " ($(m radiolib_state))")"
        echo "ggwave    : $(m ggwave_commit)$([ "$(m ggwave_state)" != "clean" ] && echo " ($(m ggwave_state))")"
        echo "MeshCore  : $(m meshcore_commit)$([ "$(m meshcore_state)" != "clean" ] && echo " ($(m meshcore_state))")"
        echo "Crypto    : $(m crypto_commit)$([ "$(m crypto_state)" != "clean" ] && echo " ($(m crypto_state))")"
        echo "Doors     : $(m pocketos_commit_short)$([ "$(m source_tree_state)" = "dirty" ] && echo " (applied from a dirty tree)")"
        echo "BUILD_ID  : $(m pocketos_build_id)"
        echo "Applied   : $(m applied_utc) (source of the above; this build did not re-apply)"
        [ "$(m dirty_override)" = "yes" ] && \
            echo "Override  : POCKETOS_ALLOW_DIRTY_BUILD=1 at apply time -- uncommitted changes are NOT in this image"
        echo "Image     : sysimage-sdcard.img sha256 ${IMAGE_SHA256}"
        echo "Release   : ${RELEASE} (the image above, gzip -n; checksum in ${RELEASE}.sha256)"
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
