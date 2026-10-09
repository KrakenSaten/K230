#!/usr/bin/env bash
# Apply the LILYGO BSP and the Doors package
# (Buildroot package `pocketos`) to a pinned K230 Linux SDK checkout.
#
# Usage: apply_to_sdk.sh [/path/to/T-Display-K230 checkout]
#   Default vendor checkout: $POCKETOS_VENDOR_DIR or <repo>/vendor/T-Display-K230
#   The SDK is the k230_linux_sdk submodule inside that checkout.
#
# Every first-party input comes from one snapshot: the commit HEAD names when
# this script starts, resolved once and then archived from by object id. The
# image is therefore a function of a commit - only tracked files, with the
# modes git records - and no uncommitted work is built, including the
# defconfig, Config.in and pocketos.mk that used to be copied from the working
# tree while this comment claimed otherwise.
#
# Resolving the commit once also settles a smaller question: HEAD is a moving
# reference and the archives are separate commands, so a commit landing
# between them would assemble a package out of two different trees.
#
# The pinned vendor commits are enforced, not merely reported. Set
# POCKETOS_ALLOW_PIN_DRIFT=1 to build against a different vendor tree on
# purpose, and record that in the build report.
#
# A dirty working tree is enforced the same way, and for the same reason. The
# package is the snapshot, so uncommitted work is not in it - and the loop that
# breaks is edit, build, deploy, test on hardware, where the result looks like
# evidence about the edit and is evidence about the commit. Set
# POCKETOS_ALLOW_DIRTY_BUILD=1 to package the snapshot from a dirty tree on
# purpose. The override changes nothing about what is packaged: it is still the
# snapshot, never the working tree.
#
# RadioLib is the exception worth knowing about. It is copied rather than
# archived, because it is an ignored checkout rather than part of our history,
# so its uncommitted changes WOULD be compiled in - which is why a dirty
# RadioLib is refused outright below rather than merely reported.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
PLATFORM_DIR="${REPO_DIR}/platforms/k230"
VENDOR_DIR="${1:-${POCKETOS_VENDOR_DIR:-${REPO_DIR}/vendor/T-Display-K230}}"
SDK_DIR="${VENDOR_DIR}/k230_linux_sdk"
CONF="k230_pocketos_defconfig"
# The Doors defconfig is composed here, not kept in this repository: the
# vendor BSP's own board defconfig, read from its pinned commit, followed by
# the Doors fragment (docs/licensing/APACHE_2_READINESS.md, B3). The LILYGO
# file carries no licence, so Doors holds no copy of it.
VENDOR_DEFCONFIG="k230_bsp/overlay/buildroot-overlay/configs/k230_canmv_t_display_rm69a10_defconfig"
DOORS_FRAGMENT="platforms/k230/configs/k230_pocketos.fragment"

EXPECTED_BSP_COMMIT="$(cat "${PLATFORM_DIR}/vendor_bsp_commit.txt")"
EXPECTED_SDK_COMMIT="$(cat "${PLATFORM_DIR}/vendor_sdk_commit.txt")"

# meshcored is part of the package (pocketos.mk builds it with
# ENABLE_MESHCORED=1), and it compiles MeshCore, orlp's ed25519 and rweather's
# Crypto. It may be packaged because the third-party notices cover all three
# (docs/LICENSING.md item 9, resolved 2026-09-22). The package's install step
# refuses it without them (the Makefile's meshcored-shipping-check) and is the
# real chokepoint; this is the same refusal before anything is assembled,
# because finding out at the end of a package build is finding out too late.
missing_notices=""
for id in meshcore ed25519 arduinolibs-crypto; do
    grep -q "^${id} *|" "${REPO_DIR}/third_party/notices/SOURCES" 2>/dev/null \
        || missing_notices="${missing_notices} ${id}"
done
if [ -n "${missing_notices}" ]; then
    echo "ERROR: the package builds meshcored, which contains MeshCore, orlp's ed25519" >&2
    echo "       and rweather's Crypto, and third_party/notices/SOURCES has no entry" >&2
    echo "       for:${missing_notices}. See docs/LICENSING.md item 9." >&2
    echo "       There is no override: this is a licensing rule, not a build preference." >&2
    exit 1
fi

[ -d "${VENDOR_DIR}/k230_bsp" ] || { echo "not a T-Display-K230 checkout: ${VENDOR_DIR}" >&2; exit 1; }
git -C "${SDK_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1 || { echo "SDK is not a git checkout: ${SDK_DIR}" >&2; exit 1; }
git -C "${REPO_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1 || { echo "Doors repository is not a git checkout: ${REPO_DIR}" >&2; exit 1; }

BSP_COMMIT="$(git -C "${VENDOR_DIR}" rev-parse HEAD)"
SDK_COMMIT="$(git -C "${SDK_DIR}" rev-parse HEAD)"
REPO_COMMIT="$(git -C "${REPO_DIR}" rev-parse --short HEAD)"
# Everything first-party below comes from this one commit object, resolved
# once. HEAD is a moving reference: the archives below are separate commands,
# and a commit landing between them would assemble a package out of two
# different trees. Naming the object once removes the question.
SNAPSHOT_COMMIT="$(git -C "${REPO_DIR}" rev-parse HEAD)"
# The Doors checkout's own state, decided once and read-only from here on: it
# is what the manifest's source_tree_state and the provenance summary report.
# The dependency checks further down write to variables of their own
# (meshcore_tree_check: DEP_COMMIT, DEP_STATE). They once shared TREE_STATE
# with this, and the manifest then reported the Crypto checkout's state as the
# Doors tree's; a dirty Doors tree built with the override lost its note in
# BUILD_INFO. readonly turns any repeat of that into a failed apply.
# tests/provenance_state_test.sh runs these functions out of this file.
doors_tree_state() { # <repo> -> sets REPO_STATUS, DIRTY_TAG, REPO_DIRTY, SOURCE_TREE_STATE
    REPO_STATUS="$(git -C "$1" status --porcelain)"
    DIRTY_TAG=""
    REPO_DIRTY=""
    SOURCE_TREE_STATE="clean"
    if [ -n "${REPO_STATUS}" ]; then
        DIRTY_TAG="-dirty"
        REPO_DIRTY=" (working tree dirty)"
        SOURCE_TREE_STATE="dirty"
    fi
}
doors_tree_state "${REPO_DIR}"
readonly SOURCE_TREE_STATE
DIRTY_OVERRIDE="no"
BUILD_ID="${REPO_COMMIT}${DIRTY_TAG}"
echo "Doors apply"
echo "Repo   : ${REPO_DIR} (version $(cat "${REPO_DIR}/VERSION")) @ ${REPO_COMMIT}${REPO_DIRTY}"
echo "Vendor : ${VENDOR_DIR} @ ${BSP_COMMIT}"
echo "SDK    : ${SDK_DIR} @ ${SDK_COMMIT}"

# A dirty tree is refused rather than reported. This used to be a NOTE that
# printed here and carried on; a note that scrolls past a hundred lines of
# overlay output, and dies to a `| tail`, is not a guard - it was missed in
# exactly the way it was meant to prevent. Refusing turns it into a decision.
#
# Neither branch changes what is packaged. `git archive HEAD` is the whole
# mechanism and the override does not widen it: there is no path here that
# puts the working tree into the package.
if [ -n "${REPO_DIRTY}" ]; then
    if [ "${POCKETOS_ALLOW_DIRTY_BUILD:-0}" = "1" ]; then
        DIRTY_OVERRIDE="yes"
        echo "WARNING: the working tree is dirty and POCKETOS_ALLOW_DIRTY_BUILD=1 is set." >&2
        echo "         Packaging HEAD (${REPO_COMMIT}). Your uncommitted changes are NOT" >&2
        echo "         included - not in the package, not in the image, not in anything" >&2
        echo "         deployed from it. Say so in the build report." >&2
    else
        echo "ERROR: the working tree is dirty, and the package is assembled with" >&2
        echo "       \`git archive HEAD\`. Uncommitted changes would NOT be included -" >&2
        echo "       not in the package, not in the image, not in anything deployed" >&2
        echo "       from it. The build would be HEAD (${REPO_COMMIT}) while looking" >&2
        echo "       like it carried your edits." >&2
        echo "       Commit or stash them, or set POCKETOS_ALLOW_DIRTY_BUILD=1 to" >&2
        echo "       package HEAD on purpose and say so in the build report." >&2
        printf '%s\n' "${REPO_STATUS}" | awk 'NR<=10 {print "       " $0}
            END {if (NR>10) printf "       ... and %d more\n", NR-10}' >&2
        exit 1
    fi
fi

# A pinned commit that has drifted is refused rather than reported: the BSP
# overlay is a patch stack against these exact trees, and BUILD_INFO.txt would
# otherwise claim pins the image was not built from.
pin_check() { # <what> <actual> <expected>
    [ "$2" = "$3" ] && return 0
    if [ "${POCKETOS_ALLOW_PIN_DRIFT:-0}" = "1" ]; then
        echo "WARNING: $1 is $2, pinned $3; continuing (POCKETOS_ALLOW_PIN_DRIFT=1)" >&2
        return 0
    fi
    echo "ERROR: $1 is $2, pinned $3." >&2
    echo "       Check out the pinned commit, or set POCKETOS_ALLOW_PIN_DRIFT=1 to build" >&2
    echo "       against a different vendor tree on purpose and say so in the build report." >&2
    exit 1
}
pin_check "vendor BSP commit" "${BSP_COMMIT}" "${EXPECTED_BSP_COMMIT}"
pin_check "SDK commit" "${SDK_COMMIT}" "${EXPECTED_SDK_COMMIT}"

# RadioLib is the one vendored dependency that reaches the image as source:
# it is compiled into radiod. Everything else the package needs - cjson, lvgl,
# libgpiod, libdrm, libevdev - is a Buildroot package, so the SDK pin above
# already fixes their versions. RadioLib is an ignored working-tree checkout,
# so nothing fixed its version at all: the release build checked 034126e by
# hand and the check lived in a document (docs/KNOWN_ISSUES.md).
#
# It is treated like the other pins now, including refusing to guess: a
# RadioLib that is not a git checkout cannot be identified, and a build that
# cannot say which radio stack it contains is not one to ship.
RADIOLIB_DIR_SRC="${REPO_DIR}/vendor/RadioLib"
EXPECTED_RADIOLIB_COMMIT="$(cat "${PLATFORM_DIR}/vendor_radiolib_commit.txt")"
if git -C "${RADIOLIB_DIR_SRC}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    RADIOLIB_COMMIT="$(git -C "${RADIOLIB_DIR_SRC}" rev-parse HEAD)"
    RADIOLIB_STATE="clean"
    # -c core.autocrlf=true, or this answer depends on which git is asking.
    # This checkout lives on a Windows drive: git for Windows checked it out
    # through autocrlf, so the files hold CRLF, and a git whose config leaves
    # autocrlf unset - WSL's, on the same files - calls all 403 of them
    # modified. Normalising here makes the question "has anyone edited
    # RadioLib" instead of "which git is asking", and it can only ever hide a
    # difference that is CR alone.
    [ -n "$(git -c core.autocrlf=true -C "${RADIOLIB_DIR_SRC}" status --porcelain)" ] \
        && RADIOLIB_STATE="dirty"
else
    RADIOLIB_COMMIT="unknown"
    RADIOLIB_STATE="not-a-git-checkout"
fi
pin_check "RadioLib commit" "${RADIOLIB_COMMIT}" "${EXPECTED_RADIOLIB_COMMIT}"
if [ "${RADIOLIB_STATE}" = "dirty" ]; then
    # Unlike our own tree, this one is copied rather than archived, so a local
    # edit here really would be compiled into radiod.
    if [ "${POCKETOS_ALLOW_PIN_DRIFT:-0}" = "1" ]; then
        echo "WARNING: the RadioLib checkout is dirty; its uncommitted changes WILL be" >&2
        echo "         compiled into radiod (POCKETOS_ALLOW_PIN_DRIFT=1)." >&2
    else
        echo "ERROR: the RadioLib checkout at ${RADIOLIB_DIR_SRC} is dirty." >&2
        echo "       It is copied into the package, not archived from a commit, so" >&2
        echo "       those uncommitted changes would be compiled into radiod and" >&2
        echo "       nothing in the image would record them." >&2
        echo "       Commit or discard them, or set POCKETOS_ALLOW_PIN_DRIFT=1 and say" >&2
        echo "       so in the build report." >&2
        exit 1
    fi
fi
echo "RadioLib: ${RADIOLIB_DIR_SRC} @ ${RADIOLIB_COMMIT} (${RADIOLIB_STATE})"

# ggwave is the second dependency that reaches the image as source: its modem
# is compiled into pos-wave (docs/apps/WAVE.md). Same rules as RadioLib - an
# ignored checkout, pinned, refused when it drifts or is dirty - because it is
# copied, not archived.
GGWAVE_DIR_SRC="${REPO_DIR}/vendor/ggwave"
EXPECTED_GGWAVE_COMMIT="$(cat "${PLATFORM_DIR}/vendor_ggwave_commit.txt")"
if git -C "${GGWAVE_DIR_SRC}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    GGWAVE_COMMIT="$(git -C "${GGWAVE_DIR_SRC}" rev-parse HEAD)"
    GGWAVE_STATE="clean"
    [ -n "$(git -c core.autocrlf=true -C "${GGWAVE_DIR_SRC}" status --porcelain)" ] \
        && GGWAVE_STATE="dirty"
else
    GGWAVE_COMMIT="unknown"
    GGWAVE_STATE="not-a-git-checkout"
fi
pin_check "ggwave commit" "${GGWAVE_COMMIT}" "${EXPECTED_GGWAVE_COMMIT}"
if [ "${GGWAVE_STATE}" = "dirty" ]; then
    if [ "${POCKETOS_ALLOW_PIN_DRIFT:-0}" = "1" ]; then
        echo "WARNING: the ggwave checkout is dirty; its uncommitted changes WILL be" >&2
        echo "         compiled into pos-wave (POCKETOS_ALLOW_PIN_DRIFT=1)." >&2
    else
        echo "ERROR: the ggwave checkout at ${GGWAVE_DIR_SRC} is dirty." >&2
        echo "       It is copied into the package, not archived from a commit, so" >&2
        echo "       those uncommitted changes would be compiled into pos-wave and" >&2
        echo "       nothing in the image would record them." >&2
        echo "       Commit or discard them, or set POCKETOS_ALLOW_PIN_DRIFT=1 and say" >&2
        echo "       so in the build report." >&2
        exit 1
    fi
fi
echo "ggwave  : ${GGWAVE_DIR_SRC} @ ${GGWAVE_COMMIT} (${GGWAVE_STATE})"

# MeshCore (vendor/RIFT) and rweather's Crypto (vendor/Crypto) are the third
# and fourth trees that reach the image as source: meshcored compiles them
# (protocols/meshcore). Same rules as RadioLib and ggwave - ignored checkouts,
# pinned by protocols/meshcore/vendor_*_commit.txt, refused when they drift or
# are dirty - because what goes on the air is decided by the protocol source,
# and a package built from an unpinned tree would say it was pinned.
meshcore_tree_check() { # <what> <dir> <pin file> -> sets DEP_COMMIT, DEP_STATE
    local what="$1" dir="$2" expected
    expected="$(tr -d ' \t\r\n' < "$3")"
    if git -C "${dir}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        DEP_COMMIT="$(git -C "${dir}" rev-parse HEAD)"
        DEP_STATE="clean"
        [ -n "$(git -c core.autocrlf=true -C "${dir}" status --porcelain)" ] && DEP_STATE="dirty"
    else
        DEP_COMMIT="unknown"
        DEP_STATE="not-a-git-checkout"
    fi
    pin_check "${what} commit" "${DEP_COMMIT}" "${expected}"
    if [ "${DEP_STATE}" = "dirty" ]; then
        if [ "${POCKETOS_ALLOW_PIN_DRIFT:-0}" = "1" ]; then
            echo "WARNING: the ${what} checkout is dirty; its uncommitted changes WILL be" >&2
            echo "         compiled into meshcored (POCKETOS_ALLOW_PIN_DRIFT=1). The export is" >&2
            echo "         recorded as <commit>-dirty, so protocols/meshcore will refuse it unless" >&2
            echo "         the package is built with MESHCORE_ALLOW_UNPINNED=1." >&2
        else
            echo "ERROR: the ${what} checkout at ${dir} is dirty; its uncommitted" >&2
            echo "       changes would be compiled into meshcored." >&2
            echo "       Commit or discard them, or set POCKETOS_ALLOW_PIN_DRIFT=1 and say" >&2
            echo "       so in the build report." >&2
            exit 1
        fi
    fi
}
RIFT_DIR_SRC="${REPO_DIR}/vendor/RIFT"
meshcore_tree_check "MeshCore (vendor/RIFT)" "${RIFT_DIR_SRC}" "${REPO_DIR}/protocols/meshcore/vendor_rift_commit.txt"
RIFT_COMMIT="${DEP_COMMIT}"; RIFT_STATE="${DEP_STATE}"
echo "MeshCore: ${RIFT_DIR_SRC} @ ${RIFT_COMMIT} (${RIFT_STATE})"
CRYPTO_DIR_SRC="${REPO_DIR}/vendor/Crypto"
meshcore_tree_check "Crypto (vendor/Crypto)" "${CRYPTO_DIR_SRC}" "${REPO_DIR}/protocols/meshcore/vendor_crypto_commit.txt"
CRYPTO_COMMIT="${DEP_COMMIT}"; CRYPTO_STATE="${DEP_STATE}"
echo "Crypto  : ${CRYPTO_DIR_SRC} @ ${CRYPTO_COMMIT} (${CRYPTO_STATE})"

# The defconfig, Config.in and pocketos.mk used to be installed straight from
# the working tree while everything else came from git. They decide what is in
# the image and how it is built, so an uncommitted edit to any of them changed
# the build while the script went on printing "the working tree is never
# packaged". They come out of the snapshot with the rest now.
SNAPSHOT_DIR="$(mktemp -d)"
cleanup_snapshot() { rm -rf "${SNAPSHOT_DIR}"; }
trap cleanup_snapshot EXIT
git -C "${REPO_DIR}" archive --format=tar "${SNAPSHOT_COMMIT}" \
    -- "${DOORS_FRAGMENT}" platforms/k230/vendor_lvgl_commit.txt platforms/k230/package/pocketos \
       platforms/k230/patches/linux \
    | tar -xp -C "${SNAPSHOT_DIR}"
for f in "${DOORS_FRAGMENT}" \
         platforms/k230/vendor_lvgl_commit.txt \
         platforms/k230/package/pocketos/Config.in \
         platforms/k230/package/pocketos/pocketos.mk \
         platforms/k230/package/pocketos/pocketos.hash; do
    [ -f "${SNAPSHOT_DIR}/${f}" ] || {
        echo "ERROR: ${f} is missing from the ${REPO_COMMIT} snapshot." >&2
        echo "       Every first-party build input has to be committed." >&2
        exit 1
    }
done

# The defconfig: the vendor board defconfig exactly as the pinned BSP commit
# holds it (line ends made LF), then the Doors fragment's settings, comments
# dropped. A fragment setting may not restate one the vendor file already
# makes. The one other thing the fragment can do is turn off a package the
# vendor file switches on: a line that is exactly "# BR2_<X> is not set"
# drops the vendor's "BR2_<X>=y" and takes its place, and is refused for a
# package the vendor file does not set to y. Any other comment is dropped.
compose_defconfig() { # <output file>
    local out="$1" line key
    git -C "${VENDOR_DIR}" show "${BSP_COMMIT}:${VENDOR_DEFCONFIG}" | tr -d '\r' > "${out}"
    [ -s "${out}" ] || { echo "ERROR: no ${VENDOR_DEFCONFIG} at the vendor BSP commit ${BSP_COMMIT}." >&2; return 1; }
    grep -v -E '^[[:space:]]*(#|$)' "${SNAPSHOT_DIR}/${DOORS_FRAGMENT}" > "${out}.doors" || true
    grep -E '^# BR2_[A-Z0-9_]+ is not set$' "${SNAPSHOT_DIR}/${DOORS_FRAGMENT}" > "${out}.off" || true
    while IFS= read -r line; do
        key="${line%%=*}"
        if grep -q -E "^(# )?${key}([= ]|\$)" "${out}"; then
            echo "ERROR: ${DOORS_FRAGMENT} sets ${key}, which the vendor defconfig already sets." >&2
            return 1
        fi
    done < "${out}.doors"
    while IFS= read -r line; do
        key="${line#\# }"
        key="${key% is not set}"
        if ! grep -q -x "${key}=y" "${out}"; then
            echo "ERROR: ${DOORS_FRAGMENT} turns off ${key}, which the vendor defconfig does not switch on." >&2
            return 1
        fi
        grep -v -x "${key}=y" "${out}" > "${out}.tmp"
        mv "${out}.tmp" "${out}"
    done < "${out}.off"
    cat "${out}.off" "${out}.doors" >> "${out}"
    rm -f "${out}.doors" "${out}.off"
}

# Vision's R0 detector (from 0.3.6, an experimental beta; docs/apps/VISION.md
# "The model", MODEL_LICENSES.md). The kmodel is a build input kept outside
# git, like every model (no model file is committed), so it is named by path
# and accepted only with the sha256 the snapshot pins in
# tools/vision/r0-model.sha256. Checked here, before the SDK is touched; it is
# copied into the package with the rest of the source below, and the package
# checks it again when it installs it. POCKETOS_VISION_R0_KMODEL=none leaves
# it out on purpose, for a userspace-only rebuild to deploy; the image gate
# (verify_image.sh) refuses an image without it.
R0_PIN="$(git -C "${REPO_DIR}" show "${SNAPSHOT_COMMIT}:tools/vision/r0-model.sha256")"
R0_NAME="$(printf '%s\n' "${R0_PIN}" | awk 'NR == 1 { print $2 }')"
R0_SHA="$(printf '%s\n' "${R0_PIN}" | awk 'NR == 1 { print $1 }')"
R0_SRC="${POCKETOS_VISION_R0_KMODEL:-}"
if [ "${R0_SRC}" = "none" ]; then
    VISION_R0_STATE="none"
    echo "NOTE: POCKETOS_VISION_R0_KMODEL=none: the package carries no R0 detector,"
    echo "      and an image built from this apply will fail the image gate."
elif [ -z "${R0_SRC}" ] || [ ! -f "${R0_SRC}" ]; then
    echo "ERROR: Vision's R0 detector is a build input kept outside git." >&2
    echo "       Set POCKETOS_VISION_R0_KMODEL to the file ${R0_NAME}" >&2
    echo "       (sha256 ${R0_SHA}, tools/vision/r0-model.sha256)," >&2
    echo "       or to 'none' for a userspace-only rebuild without it.${R0_SRC:+ Not a file: ${R0_SRC}}" >&2
    echo "       Nothing has been applied." >&2
    exit 1
else
    R0_GOT="$(sha256sum < "${R0_SRC}" | cut -d' ' -f1)"
    if [ "${R0_GOT}" != "${R0_SHA}" ]; then
        echo "ERROR: ${R0_SRC} is not the pinned R0 detector:" >&2
        echo "       sha256 ${R0_GOT}, tools/vision/r0-model.sha256 says ${R0_SHA}." >&2
        echo "       There is no override: the image carries that model or none. Nothing has been applied." >&2
        exit 1
    fi
    VISION_R0_STATE="${R0_SHA}"
    echo "Vision R0 detector: ${R0_SRC} (sha256 ${R0_SHA}, as pinned)"
fi

echo "[1/5] Vendor BSP overlay"
"${VENDOR_DIR}/k230_bsp/scripts/apply.sh" "${SDK_DIR}"

# Doors-owned kernel patches (ADR-011). They sit in the same Buildroot package
# directory as the vendor stack and apply after it: the vendor numbers its
# patches up to 0064, Doors uses 0070-0099. Like the vendor's own patches
# they only take effect on a fresh kernel extract (`make linux-dirclean`
# first); Buildroot does not re-patch a tree it has already built. Stale
# Doors patches are removed here, the way the SDK's sync removes patches that
# left the overlay, so a renamed patch is not applied twice.
install_kernel_patches() { # <snapshot dir> <sdk dir> -> sets KERNEL_PATCHES
    local src="$1/platforms/k230/patches/linux" dst="$2/buildroot-overlay/linux" f
    KERNEL_PATCHES=""
    rm -f "${dst}"/00[7-9][0-9]-*.patch
    for f in "${src}"/00[7-9][0-9]-*.patch; do
        [ -f "${f}" ] || continue
        install -m 0644 "${f}" "${dst}/$(basename "${f}")"
        KERNEL_PATCHES="${KERNEL_PATCHES}${KERNEL_PATCHES:+ }$(basename "${f}")"
    done
}
install_kernel_patches "${SNAPSHOT_DIR}" "${SDK_DIR}"
echo "      Doors kernel patches: ${KERNEL_PATCHES:-none}"

echo "[2/5] Doors defconfig (${CONF}): vendor board defconfig + ${DOORS_FRAGMENT}"
COMPOSED_DEFCONFIG="${SNAPSHOT_DIR}/composed/${CONF}"
mkdir -p "${SNAPSHOT_DIR}/composed"
compose_defconfig "${COMPOSED_DEFCONFIG}"
# The notices name the LVGL that vendor_lvgl_commit.txt pins; the defconfig
# must build that one.
LVGL_PIN="$(tr -d '\r\n' < "${SNAPSHOT_DIR}/platforms/k230/vendor_lvgl_commit.txt")"
grep -qx "BR2_PACKAGE_LVGL_CUSTOM_VERSION=\"${LVGL_PIN}\"" "${COMPOSED_DEFCONFIG}" || {
    echo "ERROR: the composed defconfig does not build LVGL ${LVGL_PIN} (platforms/k230/vendor_lvgl_commit.txt)," >&2
    echo "       which the third-party notices name. Update the pin and the notices together." >&2
    exit 1
}
install -m 0644 "${COMPOSED_DEFCONFIG}" "${SDK_DIR}/buildroot-overlay/configs/${CONF}"
echo "      sha256 $(sha256sum < "${COMPOSED_DEFCONFIG}" | cut -d' ' -f1)"

echo "[3/5] Vendor launcher: not in the image"
# Up to v0.3.x the LILYGO launcher (k230_phone_ui) was installed here with the
# vendor's own install_to_sdk.sh and kept, switched off, as a recovery path.
# It is no longer built or shipped: nothing in Doors runs it, and nothing it
# did at runtime ran on Doors either, since it was disabled (the charger and
# gauge set-up, audio routing and low-battery shutdown it does live inside its
# own process; docs/KNOWN_ISSUES.md "Vendor launcher removed").
#
# The SDK tree is persistent, so a launcher installed by an earlier apply is
# still sitting in it, and Buildroot never deletes from an existing target
# tree. Everything install_to_sdk.sh put there is taken out again, in the
# three places the stale PocketOS-era shell below is cleared from: the
# overlay this script writes, Buildroot's synced copy of it (the copy the
# rootfs is really built from) and the target tree - plus the package itself
# and its line in the vendor package menu. The vendor's own SD card stays the
# way back to the launcher (docs/hardware/FIRST_BOOT.md, "Recovery").
LAUNCHER_ROOT_DIRS="music nes videos photos screenshots recordings lorawan meshtastic notification nrf52840 picoclaw maps"
_gone=0
_rm_launcher() { # <path>
    [ -e "$1" ] || [ -L "$1" ] || return 0
    rm -rf "$1" || { echo "cannot remove the vendor launcher's ${1#"${SDK_DIR}"/}" >&2; exit 1; }
    echo "      removed ${1#"${SDK_DIR}"/}"
    _gone=$((_gone + 1))
}
for root in "${SDK_DIR}/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay" \
            "${SDK_DIR}"/output/buildroot-*/board/canaan/k230-soc/rootfs_overlay \
            "${SDK_DIR}/output/${CONF}/target"; do
    [ -d "${root}" ] || continue
    _rm_launcher "${root}/etc/init.d/S99zz_k230_phone_ui"
    _rm_launcher "${root}/root/app/k230_phone_ui"
    for d in ${LAUNCHER_ROOT_DIRS}; do
        _rm_launcher "${root}/root/${d}"
    done
done
for p in "${SDK_DIR}/buildroot-overlay/package/k230_phone_ui" \
         "${SDK_DIR}"/output/buildroot-*/package/k230_phone_ui \
         "${SDK_DIR}"/output/"${CONF}"/build/k230_phone_ui*; do
    _rm_launcher "${p}"
done
for menu in "${SDK_DIR}/buildroot-overlay/package/Config_canaan.in" \
            "${SDK_DIR}"/output/buildroot-*/package/Config_canaan.in; do
    [ -f "${menu}" ] || continue
    if grep -q 'package/k230_phone_ui/Config.in' "${menu}"; then
        sed -i '\#^source "package/k230_phone_ui/Config.in"$#d' "${menu}"
        echo "      removed the launcher from ${menu#"${SDK_DIR}"/}"
        _gone=$((_gone + 1))
    fi
    if grep -q 'k230_phone_ui' "${menu}"; then
        echo "the vendor launcher is still in ${menu}" >&2
        exit 1
    fi
done
if grep -q 'K230_PHONE_UI' "${SDK_DIR}/buildroot-overlay/configs/${CONF}"; then
    echo "the Doors defconfig ${CONF} still names the vendor launcher" >&2
    exit 1
fi
if [ "${_gone}" -eq 0 ]; then
    echo "      no vendor launcher in the SDK"
fi

echo "[3b2/5] Vendor leftovers: not in the image"
# Three things the vendor tree puts into the image that nothing on it uses
# (owner's decision, 2026-10-07; docs/licensing/APACHE_2_READINESS.md §14.2):
#  - root/script/sensor.sh, a vendor developer's helper from the SDK's board
#    overlay that fetches files from one engineer's build host for another
#    board, with credentials in it;
#  - /lib/libasan.so.8, which the vendor's post-build.sh copies from the
#    toolchain's sysroot (no package owns it, so legal-info never sees it);
#  - the toolchain's libgfortran, which the fragment stops (Fortran off).
# Overlays and post-build scripts run after every package hook, so they are
# removed at their source - the overlay, the post-build line - in the SDK
# overlay this script writes and in Buildroot's synced copy, and stale copies
# from the persistent target tree.
_left=0
for root in "${SDK_DIR}/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay" \
            "${SDK_DIR}"/output/buildroot-*/board/canaan/k230-soc/rootfs_overlay \
            "${SDK_DIR}/output/${CONF}/target"; do
    [ -d "${root}" ] || continue
    for f in root/script/sensor.sh lib/libasan.so.8 lib/libgfortran.so.5 lib/libgfortran.so.5.0.0 lib/libgfortran.so; do
        if [ -e "${root}/${f}" ] || [ -L "${root}/${f}" ]; then
            rm -f "${root}/${f}" || { echo "cannot remove ${root#"${SDK_DIR}"/}/${f}" >&2; exit 1; }
            echo "      removed ${root#"${SDK_DIR}"/}/${f}"
            _left=$((_left + 1))
        fi
    done
done
for pb in "${SDK_DIR}/buildroot-overlay/board/canaan/k230-soc/post-build.sh" \
          "${SDK_DIR}"/output/buildroot-*/board/canaan/k230-soc/post-build.sh; do
    [ -f "${pb}" ] || continue
    if grep -q 'libasan\.so' "${pb}"; then
        sed -i '/libasan\.so/d' "${pb}"
        echo "      removed the libasan copy from ${pb#"${SDK_DIR}"/}"
        _left=$((_left + 1))
    fi
    if grep -q 'libasan' "${pb}"; then
        echo "${pb} still copies libasan" >&2
        exit 1
    fi
done
[ "${_left}" -gt 0 ] || echo "      none in the SDK"

echo "[3c/5] sshd: no empty-password logins"
# Vendor sshd_config allows root with an empty password over the network
# (PermitRootLogin yes, PasswordAuthentication yes, PermitEmptyPasswords yes)
# and the root account ships with no password. Patch the vendor file in place
# at apply time, rather than copied into this repository (the LILYGO tree
# carries no licence): SSH then refuses the empty
# password until the operator sets one on the serial console (`passwd`), or
# installs a key in /root/.ssh/authorized_keys; local serial login is
# untouched and no password is embedded in the image.
SSHD="${SDK_DIR}/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/ssh/sshd_config"
[ -f "${SSHD}" ] || { echo "vendor sshd_config missing: ${SSHD}" >&2; exit 1; }
sed -i -e 's/^PermitEmptyPasswords yes$/PermitEmptyPasswords no/' "${SSHD}"
grep -q '^PermitEmptyPasswords no$' "${SSHD}" \
    || { echo "failed to set PermitEmptyPasswords no in ${SSHD}" >&2; exit 1; }
grep -q '^PermitEmptyPasswords yes' "${SSHD}" && { echo "PermitEmptyPasswords yes still present in ${SSHD}" >&2; exit 1; }

echo "[4/5] Doors rootfs overlay"
# Also from git, and for the same reason as the package below, but here the
# reason is sharper: Buildroot copies this overlay into the rootfs with
# rsync -a and BusyBox rcS runs `$i start`, so the mode on S60radiod and
# S90doors-shell decides whether the services start at all. Taken from the
# working tree it would be whatever the build host's filesystem reports, which
# on a WSL /mnt/c checkout is 0777 for every file. Merged onto the vendor's
# overlay, never deleting from it.
git -C "${REPO_DIR}" archive --format=tar "${SNAPSHOT_COMMIT}" -- platforms/k230/rootfs_overlay \
    | tar -x --strip-components=3 \
          -C "${SDK_DIR}/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/"

# ADR-005 Phase 3: the shell service is doors-shell, and there is never more
# than one of them. The overlay above is merged and never deletes, and
# Buildroot never deletes from an existing target tree either, so every place a
# PocketOS-era shell can still be sitting in this SDK is cleared here - before
# the image is assembled, rather than leaving the rootfs gate in build_image.sh
# to refuse a build that is otherwise fine.
# Three places, and the third is the one that bites: Buildroot syncs the
# overlay into its own tree (output/buildroot-<version>/board/...) and builds
# the rootfs from that copy, so cleaning only the overlay this script writes
# leaves the old service to be copied back into the image.
_stale=0
for f in "${SDK_DIR}/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S90pocketos-shell" \
         "${SDK_DIR}"/output/buildroot-*/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S90pocketos-shell \
         "${SDK_DIR}/output/${CONF}/target/etc/init.d/S90pocketos-shell" \
         "${SDK_DIR}/output/${CONF}/target/usr/bin/pocketos-shell"; do
    [ -e "${f}" ] || continue
    rm -f "${f}" || { echo "cannot remove the PocketOS-era shell at ${f}" >&2; exit 1; }
    echo "      removed the PocketOS-era shell: ${f#"${SDK_DIR}"/}"
    _stale=$((_stale + 1))
done
[ "${_stale}" -eq 0 ] && echo "      no PocketOS-era shell in the SDK"

echo "[5/5] Doors package (pocketos)"
# Third-party notices (docs/LICENSING.md). The package installs
# THIRD_PARTY_NOTICES.txt into the image and hands it to legal-info, so notices
# that no longer describe what is built are refused before the package is
# written: the file must be what third_party/notices produces, pocketos.hash
# (which legal-info checks it against) must hold its sha256, every copied
# licence text must be byte-identical to its pinned upstream (the RadioLib and
# ggwave checkouts, and the LVGL and lv_port_linux archives this SDK builds
# from), and the vendor LVGL configuration must not compile in bundled code the
# notices do not name. Checked from the snapshot, like everything packaged.
NOTICES_DIR="$(mktemp -d)"
git -C "${REPO_DIR}" archive --format=tar "${SNAPSHOT_COMMIT}" -- \
    LICENSE NOTICE THIRD_PARTY_NOTICES.txt third_party/notices tools/legal docs/legal/fonts docs/legal/third-party \
    platforms/k230/vendor_radiolib_commit.txt platforms/k230/vendor_ggwave_commit.txt \
    protocols/meshcore/vendor_rift_commit.txt protocols/meshcore/vendor_crypto_commit.txt \
    platforms/k230/package/pocketos/pocketos.hash \
    platforms/k230/vendor_lvgl_commit.txt | tar -x -C "${NOTICES_DIR}"
ln -s "${REPO_DIR}/vendor" "${NOTICES_DIR}/vendor"
NOTICES_OK=1
bash "${NOTICES_DIR}/tools/legal/gen_notices.sh" --check || NOTICES_OK=0
bash "${NOTICES_DIR}/tools/legal/gen_notices.sh" --verify-upstream --sdk "${SDK_DIR}" --strict || NOTICES_OK=0
LV_CONF="${SDK_DIR}/output/${CONF}/staging/usr/include/lvgl/lv_conf.h"
if [ -f "${LV_CONF}" ]; then
    bash "${NOTICES_DIR}/tools/legal/gen_notices.sh" --verify-lvconf "${LV_CONF}" || NOTICES_OK=0
else
    echo "NOTE: LVGL has not been built in this SDK output yet, so its configuration"
    echo "      could not be compared with the notices; build_image.sh checks it after"
    echo "      the build."
fi
rm -rf "${NOTICES_DIR}"
if [ "${NOTICES_OK}" != "1" ]; then
    echo "ERROR: the third-party notices do not match what this build contains (above)." >&2
    echo "       Update third_party/notices and run tools/legal/gen_notices.sh, commit," >&2
    echo "       and apply again. There is no override: an image must not ship wrong notices." >&2
    exit 1
fi
PKG_DIR="${SDK_DIR}/buildroot-overlay/package/pocketos"
mkdir -p "${PKG_DIR}/src"
install -m 0644 "${SNAPSHOT_DIR}/platforms/k230/package/pocketos/Config.in" "${PKG_DIR}/Config.in"
install -m 0644 "${SNAPSHOT_DIR}/platforms/k230/package/pocketos/pocketos.mk" "${PKG_DIR}/pocketos.mk"
install -m 0644 "${SNAPSHOT_DIR}/platforms/k230/package/pocketos/pocketos.hash" "${PKG_DIR}/pocketos.hash"
# What the package is built from. Kept on one line and in this form so
# tests/package_sync_test.sh can read it and stay in step with this script.
POCKETOS_PKG_PATHSPEC=". :(exclude)docs :(exclude)platforms"
# git archive rather than rsync: only tracked files at HEAD, with the modes
# git records, and no exclude-list to keep in step with the build outputs.
rm -rf "${PKG_DIR}/src"
mkdir -p "${PKG_DIR}/src"
# shellcheck disable=SC2086  # the pathspec is three words on purpose
git -C "${REPO_DIR}" archive --format=tar "${SNAPSHOT_COMMIT}" -- ${POCKETOS_PKG_PATHSPEC} \
    | tar -x -C "${PKG_DIR}/src/"
# PocketTimber's proof sprites and their converter live under docs/, which the
# package deliberately leaves out. They are the one part of docs/ a build
# consumes: ui/shell/CMakeLists.txt converts the renders to LVGL image arrays
# at configure time (host python3, which Buildroot provides; pocketos.mk
# depends on it). Without them the shell silently builds the placeholder
# blocks, which is not the app D3 validates. Only the renders and the
# converter travel; the Blender source and the studies stay in the
# repository. Same rule as above: one line, read by tests/package_sync_test.sh.
POCKETOS_PKG_ART_PATHSPEC="docs/design/timber-art/rendered docs/design/timber-art/tools"
# shellcheck disable=SC2086
git -C "${REPO_DIR}" archive --format=tar "${SNAPSHOT_COMMIT}" -- ${POCKETOS_PKG_ART_PATHSPEC} \
    | tar -x -C "${PKG_DIR}/src/"
# The exported tree has no git history, so the commit it came from travels
# beside VERSION. The Makefile and ui/shell/CMakeLists.txt compile both into
# every binary; the logs, the crash reports and <service>.info then name the
# build the device is actually running.
printf '%s\n' "${BUILD_ID}" > "${PKG_DIR}/src/BUILD_ID"
# RadioLib (MIT) is compiled into radiod; sync its sources beside ours. It is
# an ignored working-tree checkout rather than part of our history, so it is
# copied rather than archived; build products from a host-side compile of the
# sx1262 backend must not travel with it.
mkdir -p "${PKG_DIR}/src/third_party/RadioLib"
rsync -a --delete --exclude '/.git' --exclude '/examples' --exclude '/extras' \
    --exclude '*.o' --exclude '*.d' --exclude '*.a' --exclude '*.so' \
    "${REPO_DIR}/vendor/RadioLib/" "${PKG_DIR}/src/third_party/RadioLib/"
# ggwave (MIT; its Reed-Solomon code carries its own MIT licence): only the
# library and its licences travel - the header, the one source file, the FFT
# and Reed-Solomon headers it includes - not the examples, bindings or the
# web and Arduino ports.
rm -rf "${PKG_DIR}/src/third_party/ggwave"
mkdir -p "${PKG_DIR}/src/third_party/ggwave/include/ggwave" \
         "${PKG_DIR}/src/third_party/ggwave/src/reed-solomon"
install -m 0644 "${GGWAVE_DIR_SRC}/LICENSE" "${PKG_DIR}/src/third_party/ggwave/LICENSE"
install -m 0644 "${GGWAVE_DIR_SRC}/include/ggwave/ggwave.h" \
    "${PKG_DIR}/src/third_party/ggwave/include/ggwave/ggwave.h"
install -m 0644 "${GGWAVE_DIR_SRC}/src/ggwave.cpp" "${GGWAVE_DIR_SRC}/src/fft.h" \
    "${PKG_DIR}/src/third_party/ggwave/src/"
install -m 0644 "${GGWAVE_DIR_SRC}/src/reed-solomon/rs.hpp" "${GGWAVE_DIR_SRC}/src/reed-solomon/gf.hpp" \
    "${GGWAVE_DIR_SRC}/src/reed-solomon/poly.hpp" "${GGWAVE_DIR_SRC}/src/reed-solomon/LICENSE" \
    "${PKG_DIR}/src/third_party/ggwave/src/reed-solomon/"
# MeshCore and Crypto (meshcored): what protocols/meshcore compiles and the
# licences the notices were checked against, nothing else - MeshCore's src/
# and its bundled ed25519, and the Crypto library. The Makefile prefers
# third_party/ over vendor/ when it is there, which is how the package build
# finds them. The tree has no .git here, so the commit verified above travels
# beside it (.doors-pinned-commit), and protocols/meshcore's pin check reads
# that in place of `git rev-parse` - it is only ever written by this script,
# after the pin check passed.
rm -rf "${PKG_DIR}/src/third_party/RIFT" "${PKG_DIR}/src/third_party/Crypto"
mkdir -p "${PKG_DIR}/src/third_party/RIFT/lib" "${PKG_DIR}/src/third_party/Crypto/libraries"
rsync -a --exclude '*.o' --exclude '*.d' --exclude '*.a' \
    "${RIFT_DIR_SRC}/src" "${PKG_DIR}/src/third_party/RIFT/"
rsync -a --exclude '*.o' --exclude '*.d' --exclude '*.a' \
    "${RIFT_DIR_SRC}/lib/ed25519" "${PKG_DIR}/src/third_party/RIFT/lib/"
install -m 0644 "${RIFT_DIR_SRC}/license.txt" "${PKG_DIR}/src/third_party/RIFT/license.txt"
# A tree exported with uncommitted changes is recorded as <commit>-dirty, never
# as the commit: protocols/meshcore then refuses to call it pinned, which is
# the truth about it.
pin_record() { [ "$2" = "dirty" ] && printf '%s-dirty\n' "$1" || printf '%s\n' "$1"; }
pin_record "${RIFT_COMMIT}" "${RIFT_STATE}" > "${PKG_DIR}/src/third_party/RIFT/.doors-pinned-commit"
rsync -a --exclude '*.o' --exclude '*.d' --exclude '*.a' \
    "${CRYPTO_DIR_SRC}/libraries/Crypto" "${PKG_DIR}/src/third_party/Crypto/libraries/"
install -m 0644 "${CRYPTO_DIR_SRC}/libraries/LICENSE.txt" \
    "${PKG_DIR}/src/third_party/Crypto/libraries/LICENSE.txt"
pin_record "${CRYPTO_COMMIT}" "${CRYPTO_STATE}" > "${PKG_DIR}/src/third_party/Crypto/.doors-pinned-commit"
# Vision's R0 detector, checked against its pin before anything was applied
# (above), copied beside the source and checked again as copied.
mkdir -p "${PKG_DIR}/src/models/vision"
if [ "${VISION_R0_STATE}" != "none" ]; then
    install -m 0644 "${R0_SRC}" "${PKG_DIR}/src/models/vision/${R0_NAME}"
    if [ "$(sha256sum < "${PKG_DIR}/src/models/vision/${R0_NAME}" | cut -d' ' -f1)" != "${R0_SHA}" ]; then
        echo "ERROR: ${R0_SRC} changed while it was being packaged; apply again." >&2
        exit 1
    fi
fi
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

# The applied-source manifest. build_image.sh used to describe the build by
# asking the repository what HEAD was at the moment it ran, which is a
# different question from what was applied: apply at A, check out B, build, and
# the report named B while the package held A. Nothing in the image was wrong,
# only everything said about it.
#
# So what was applied is recorded here, next to what it was applied to, and
# build_image.sh reports from this file rather than from a repository it never
# read. One fact per line, no spaces in values, the shape pos-supervise's state
# file already established.
MANIFEST="${SDK_DIR}/.pocketos-applied"
write_manifest() { # <file>
    cat > "$1" <<EOF
manifest_version=1
pocketos_commit=${SNAPSHOT_COMMIT}
pocketos_commit_short=${REPO_COMMIT}
pocketos_version=$(cat "${REPO_DIR}/VERSION")
pocketos_build_id=${BUILD_ID}
source_tree_state=${SOURCE_TREE_STATE}
dirty_override=${DIRTY_OVERRIDE}
vendor_bsp_commit=${BSP_COMMIT}
sdk_commit=${SDK_COMMIT}
doors_kernel_patches=${KERNEL_PATCHES:-none}
radiolib_commit=${RADIOLIB_COMMIT}
radiolib_state=${RADIOLIB_STATE}
ggwave_commit=${GGWAVE_COMMIT}
ggwave_state=${GGWAVE_STATE}
meshcore_commit=${RIFT_COMMIT}
meshcore_state=${RIFT_STATE}
crypto_commit=${CRYPTO_COMMIT}
crypto_state=${CRYPTO_STATE}
vision_r0_kmodel=${VISION_R0_STATE}
defconfig=${CONF}
applied_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
applied_epoch=$(date +%s)
EOF
}
write_manifest "${MANIFEST}"

# Provenance, repeated where it cannot be missed. The warning above is printed
# before everything this script does, so it is the first thing to scroll away
# and the first thing a `| tail` discards. The single question worth answering
# after a bench build - what is actually in this package - is therefore
# answered again as the last thing the run says.
echo
echo "Provenance"
echo "  Packaged source : ${REPO_COMMIT} (${SNAPSHOT_COMMIT}) via git archive"
echo "  Source worktree : ${SOURCE_TREE_STATE}"
if [ "${DIRTY_OVERRIDE}" = "yes" ]; then
    echo "  Dirty override  : POCKETOS_ALLOW_DIRTY_BUILD=1 -- uncommitted changes are NOT included"
fi
echo "  RadioLib        : ${RADIOLIB_COMMIT} (${RADIOLIB_STATE})"
echo "  ggwave          : ${GGWAVE_COMMIT} (${GGWAVE_STATE})"
echo "  MeshCore        : ${RIFT_COMMIT} (${RIFT_STATE})"
echo "  Crypto          : ${CRYPTO_COMMIT} (${CRYPTO_STATE})"
echo "  Vision R0       : ${VISION_R0_STATE} (sha256 of the packaged kmodel, or none)"
echo "  Kernel patches  : ${KERNEL_PATCHES:-none}"
echo "  BUILD_ID        : ${BUILD_ID}"
echo "  The working tree is never packaged, with or without the override."
echo "Done. Build with: ${PLATFORM_DIR}/scripts/build_image.sh ${VENDOR_DIR}"
