#!/usr/bin/env bash
# Apply the LILYGO BSP, the vendor launcher (temporary) and the Doors package
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
git -C "${REPO_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1 || { echo "Doors repository is not a git checkout: ${REPO_DIR}" >&2; exit 1; }

BSP_COMMIT="$(git -C "${VENDOR_DIR}" rev-parse HEAD)"
SDK_COMMIT="$(git -C "${SDK_DIR}" rev-parse HEAD)"
REPO_COMMIT="$(git -C "${REPO_DIR}" rev-parse --short HEAD)"
# Everything first-party below comes from this one commit object, resolved
# once. HEAD is a moving reference: the archives below are separate commands,
# and a commit landing between them would assemble a package out of two
# different trees. Naming the object once removes the question.
SNAPSHOT_COMMIT="$(git -C "${REPO_DIR}" rev-parse HEAD)"
REPO_STATUS="$(git -C "${REPO_DIR}" status --porcelain)"
DIRTY_TAG=""
REPO_DIRTY=""
TREE_STATE="clean"
DIRTY_OVERRIDE="no"
if [ -n "${REPO_STATUS}" ]; then
    DIRTY_TAG="-dirty"
    REPO_DIRTY=" (working tree dirty)"
    TREE_STATE="dirty"
fi
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

# The defconfig, Config.in and pocketos.mk used to be installed straight from
# the working tree while everything else came from git. They decide what is in
# the image and how it is built, so an uncommitted edit to any of them changed
# the build while the script went on printing "the working tree is never
# packaged". They come out of the snapshot with the rest now.
SNAPSHOT_DIR="$(mktemp -d)"
cleanup_snapshot() { rm -rf "${SNAPSHOT_DIR}"; }
trap cleanup_snapshot EXIT
git -C "${REPO_DIR}" archive --format=tar "${SNAPSHOT_COMMIT}" \
    -- "platforms/k230/configs/${CONF}" platforms/k230/package/pocketos \
    | tar -xp -C "${SNAPSHOT_DIR}"
for f in "platforms/k230/configs/${CONF}" \
         platforms/k230/package/pocketos/Config.in \
         platforms/k230/package/pocketos/pocketos.mk \
         platforms/k230/package/pocketos/pocketos.hash; do
    [ -f "${SNAPSHOT_DIR}/${f}" ] || {
        echo "ERROR: ${f} is missing from the ${REPO_COMMIT} snapshot." >&2
        echo "       Every first-party build input has to be committed." >&2
        exit 1
    }
done

echo "[1/5] Vendor BSP overlay"
"${VENDOR_DIR}/k230_bsp/scripts/apply.sh" "${SDK_DIR}"

echo "[2/5] Doors defconfig (${CONF})"
install -m 0644 "${SNAPSHOT_DIR}/platforms/k230/configs/${CONF}" "${SDK_DIR}/buildroot-overlay/configs/${CONF}"

echo "[3/5] Vendor launcher (kept in the image; the panel switch below hands the panel to the Doors shell)"
"${VENDOR_DIR}/k230_launcher/scripts/install_to_sdk.sh" "${SDK_DIR}" "${CONF}"

echo "[3b/5] Panel switch for the vendor launcher"
# The vendor init script is patched in place at apply time rather than
# copied into this repository (the LILYGO tree carries no licence): an ENABLE
# switch in /etc/default/k230_phone_ui lets pocketos-shell own the panel
# across reboots. S90pocketos-shell reads the same file and refuses to start
# while the launcher is enabled. The launcher itself stays in the image.
S99="${SDK_DIR}/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S99zz_k230_phone_ui"
[ -f "${S99}" ] || { echo "vendor launcher init script missing: ${S99}" >&2; exit 1; }
if ! grep -q '/etc/default/k230_phone_ui' "${S99}"; then
    sed -i \
        -e '/^DRM_NODE=/a\
# PocketOS: ENABLE=0 in /etc/default/k230_phone_ui hands the panel to pocketos-shell.\
ENABLE=1\
[ -r /etc/default/k230_phone_ui ] && . /etc/default/k230_phone_ui' \
        -e '/printf "Starting k230_phone_ui: "/a\
\	[ "$ENABLE" = "1" ] || { echo "disabled (/etc/default/k230_phone_ui)"; return 0; }' \
        "${S99}"
fi
grep -q 'disabled (/etc/default/k230_phone_ui)' "${S99}" && grep -q '^ENABLE=1$' "${S99}" \
    || { echo "failed to add the panel switch to ${S99}" >&2; exit 1; }

echo "[3c/5] sshd: no empty-password logins"
# Vendor sshd_config allows root with an empty password over the network
# (PermitRootLogin yes, PasswordAuthentication yes, PermitEmptyPasswords yes)
# and the root account ships with no password. Patch the vendor file in place
# at apply time, like the launcher switch above: SSH then refuses the empty
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
_stale=0
for rel in "buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S90pocketos-shell" \
           "output/${CONF}/target/etc/init.d/S90pocketos-shell" \
           "output/${CONF}/target/usr/bin/pocketos-shell"; do
    f="${SDK_DIR}/${rel}"
    [ -e "${f}" ] || continue
    rm -f "${f}" || { echo "cannot remove the PocketOS-era shell at ${f}" >&2; exit 1; }
    echo "      removed the PocketOS-era shell: ${rel}"
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
    THIRD_PARTY_NOTICES.txt third_party/notices tools/legal docs/legal/fonts docs/legal/third-party \
    platforms/k230/vendor_radiolib_commit.txt platforms/k230/vendor_ggwave_commit.txt \
    platforms/k230/package/pocketos/pocketos.hash \
    "platforms/k230/configs/${CONF}" | tar -x -C "${NOTICES_DIR}"
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
cat > "${MANIFEST}" <<EOF
manifest_version=1
pocketos_commit=${SNAPSHOT_COMMIT}
pocketos_commit_short=${REPO_COMMIT}
pocketos_version=$(cat "${REPO_DIR}/VERSION")
pocketos_build_id=${BUILD_ID}
source_tree_state=${TREE_STATE}
dirty_override=${DIRTY_OVERRIDE}
vendor_bsp_commit=${BSP_COMMIT}
sdk_commit=${SDK_COMMIT}
radiolib_commit=${RADIOLIB_COMMIT}
radiolib_state=${RADIOLIB_STATE}
ggwave_commit=${GGWAVE_COMMIT}
ggwave_state=${GGWAVE_STATE}
defconfig=${CONF}
applied_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
applied_epoch=$(date +%s)
EOF

# Provenance, repeated where it cannot be missed. The warning above is printed
# before everything this script does, so it is the first thing to scroll away
# and the first thing a `| tail` discards. The single question worth answering
# after a bench build - what is actually in this package - is therefore
# answered again as the last thing the run says.
echo
echo "Provenance"
echo "  Packaged source : ${REPO_COMMIT} (${SNAPSHOT_COMMIT}) via git archive"
echo "  Source worktree : ${TREE_STATE}"
if [ "${DIRTY_OVERRIDE}" = "yes" ]; then
    echo "  Dirty override  : POCKETOS_ALLOW_DIRTY_BUILD=1 -- uncommitted changes are NOT included"
fi
echo "  RadioLib        : ${RADIOLIB_COMMIT} (${RADIOLIB_STATE})"
echo "  ggwave          : ${GGWAVE_COMMIT} (${GGWAVE_STATE})"
echo "  BUILD_ID        : ${BUILD_ID}"
echo "  The working tree is never packaged, with or without the override."
echo "Done. Build with: ${PLATFORM_DIR}/scripts/build_image.sh ${VENDOR_DIR}"
