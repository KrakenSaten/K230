#!/bin/bash
# What an exported image says about itself.
#
# build_image.sh used to describe the build by asking the repository what HEAD
# was at the moment it ran. That is a different question from what was applied
# to the SDK: apply at A, check out B, build without re-applying, and the
# report named B while the package held A. Nothing in the image was wrong -
# only everything said about it, which is the part a bench report quotes.
#
# It also exported with `[ -f ... ] && cp` straight into the output directory,
# so a build that produced no image still wrote a report, left every artefact
# of every previous build in place, and checksummed those too. SHA256SUMS.txt
# then listed, under a current date, images that build had never seen.
#
# The export path is exercised here against a fake SDK: a stub make, a stub
# toolchain, and an images directory this test fills by hand. That is enough,
# because what is under test is the bookkeeping around the build, not the
# build. Nothing here runs Buildroot or needs a cross compiler.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)
BUILD=platforms/k230/scripts/build_image.sh
CONF=k230_pocketos_defconfig
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
has()   { grep -q -- "$2" "$1" 2>/dev/null && echo 1 || echo 0; }

# The export path now ends in a gate that reads partition 1 out of the image,
# so this suite has to be able to build a real one. Without the tools for that
# it can check nothing, and reporting "0 failure(s)" would be the failure mode
# these gates exist to prevent.
. tests/mkbootimg.sh
missing="$(mkbootimg_missing_tools)"
command -v debugfs >/dev/null 2>&1 || missing="${missing} debugfs"
if [ -n "${missing}" ]; then
    echo "NOT RUN build_provenance_test: missing${missing}, so the export path cannot be exercised."
    echo "        This is a release gate; not running it is not a pass."
    if [ "${POCKETOS_ALLOW_SKIPPED_GATES:-0}" = "1" ]; then
        echo "        POCKETOS_ALLOW_SKIPPED_GATES=1 - continuing, but this gate did NOT pass."
        exit 0
    fi
    exit 77
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- a fake SDK the export can be pointed at ----------------------------

VEND="$TMP/vendor"
SDK="$VEND/k230_linux_sdk"
IMAGES="$SDK/output/$CONF/images"
mkdir -p "$IMAGES" "$SDK/buildroot-overlay/configs" "$TMP/bin"
: > "$SDK/buildroot-overlay/configs/$CONF"

# An SDK whose make builds nothing. The real make runs - build_image.sh resets
# PATH for Buildroot's sake, so a stub on PATH would not be seen anyway - it
# simply has nothing to do here. The artefacts are placed by the test, so every
# case below controls exactly what the "build" produced.
cat > "$SDK/Makefile" <<'MK'
.PHONY: all k230_pocketos_defconfig
all:
k230_pocketos_defconfig:
MK
printf '#!/bin/sh\nexit 0\n' > "$TMP/bin/cc-stub"
chmod 0755 "$TMP/bin/cc-stub"

# Commit A is what gets applied; the repository stays wherever it really is,
# which is the whole point of the first case.
COMMIT_A=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
SHORT_A=aaaaaaa
write_manifest() { # <short> <state> <override>
    cat > "$SDK/.pocketos-applied" <<EOF
manifest_version=1
pocketos_commit=$COMMIT_A
pocketos_commit_short=$1
pocketos_version=9.9.9
pocketos_build_id=$1
source_tree_state=$2
dirty_override=$3
vendor_bsp_commit=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
sdk_commit=cccccccccccccccccccccccccccccccccccccccc
defconfig=$CONF
applied_utc=2026-01-01T00:00:00Z
applied_epoch=${APPLIED_EPOCH:-1}
EOF
}

fresh_images() { # the artefacts a real build would have just produced
    rm -f "$IMAGES"/*
    for f in Image sysimage-sdcard.img.gz k.dtb; do
        printf 'content of %s\n' "$f" > "$IMAGES/$f"
    done
    # The SD-card image has to be a real one. build_image.sh now reads
    # partition 1 out of it and refuses to export an image U-Boot could not
    # boot, so a placeholder here would only prove that the gate works - which
    # is tests/image_contents_test.sh's job - while making every provenance
    # case below fail for a reason that has nothing to do with provenance.
    mkbootimg_complete "$IMAGES/sysimage-sdcard.img"
}

# Deliberately does not clear $TMP/out: whether a previous export survives is
# exactly what case 6 is about, and clearing it here made that case vacuous -
# it passed against a build_image.sh that copied straight into the output
# directory. Each case sets the directory up as it needs it.
run_build() { # leaves output in $TMP/out, combined log in $TMP/log
    ( cd "$REPO" && \
        POCKETOS_TOOLCHAIN_CC="$TMP/bin/cc-stub" \
        POCKETOS_OUT_DIR="$TMP/out" \
        bash "$BUILD" "$VEND" all ) > "$TMP/log" 2>&1
    echo $?
}

# ---- 1. the A -> B regression -------------------------------------------
#
# Applied at A. The repository is at whatever this checkout is, which is not A.
# The report must say A.

rm -rf "$TMP/out"
write_manifest "$SHORT_A" clean no
fresh_images
rc=$(run_build)
check "a build with a manifest succeeds" $([ "$rc" = "0" ] && echo 1 || echo 0)
[ "$rc" != "0" ] && sed -n '1,12p' "$TMP/log"
INFO="$TMP/out/BUILD_INFO.txt"
check "BUILD_INFO names the applied source" $(has "$INFO" "$SHORT_A")
REAL_HEAD=$(git rev-parse --short HEAD 2>/dev/null || echo none)
check "and not the repository's current HEAD ($REAL_HEAD)" \
      $([ "$(has "$INFO" "$REAL_HEAD")" = "0" ] && echo 1 || echo 0)
check "BUILD_INFO says the build did not re-apply" $(has "$INFO" "did not re-apply")
check "BUILD_INFO carries the applied BUILD_ID" $(has "$INFO" "BUILD_ID  : $SHORT_A")
check "BUILD_INFO carries the applied vendor pins" $(has "$INFO" "bbbbbbbbbbbb")

# ---- 2. a dirty apply is still visible in the report --------------------

rm -rf "$TMP/out"
write_manifest "${SHORT_A}-dirty" dirty yes
fresh_images
rc=$(run_build)
check "a build applied from a dirty tree succeeds" $([ "$rc" = "0" ] && echo 1 || echo 0)
check "and the report says the apply was dirty" $(has "$INFO" "applied from a dirty tree")
check "and names the override" $(has "$INFO" "POCKETOS_ALLOW_DIRTY_BUILD=1 at apply time")

# ---- 3. no manifest at all ----------------------------------------------

rm -rf "$TMP/out"
write_manifest "$SHORT_A" clean no
fresh_images
rm -f "$SDK/.pocketos-applied"
rc=$(run_build)
check "a build with no manifest fails" $([ "$rc" != "0" ] && echo 1 || echo 0)
check "and says to run apply_to_sdk.sh" $(has "$TMP/log" "apply_to_sdk.sh first")
check "and exports nothing" $([ ! -d "$TMP/out" ] && echo 1 || echo 0)

# ---- 4. a missing required artefact -------------------------------------

rm -rf "$TMP/out"
write_manifest "$SHORT_A" clean no
fresh_images
rm -f "$IMAGES/sysimage-sdcard.img"
rc=$(run_build)
check "a build that produced no SD-card image fails" $([ "$rc" != "0" ] && echo 1 || echo 0)
check "and names what was missing" $(has "$TMP/log" "sysimage-sdcard.img was not produced")
check "and exports nothing" $([ ! -d "$TMP/out" ] && echo 1 || echo 0)

# ---- 5. a stale artefact from an earlier build --------------------------

rm -rf "$TMP/out"
APPLIED_EPOCH=$(date +%s)
write_manifest "$SHORT_A" clean no
fresh_images
touch -d '2020-01-01 00:00:00' "$IMAGES/sysimage-sdcard.img"
rc=$(run_build)
check "an SD-card image older than the applied source fails" $([ "$rc" != "0" ] && echo 1 || echo 0)
check "and says it predates the source" $(has "$TMP/log" "older than the source applied")
APPLIED_EPOCH=1

# ---- 6. nothing survives from a previous export -------------------------

write_manifest "$SHORT_A" clean no
fresh_images
rm -rf "$TMP/out"; mkdir -p "$TMP/out"
printf 'an image from a build long ago\n' > "$TMP/out/sysimage-sdcard.img.old"
printf 'stale\n' > "$TMP/out/k230-canmv-rm69a10-hdmi.dtb"
rc=$(run_build)
check "the export succeeds" $([ "$rc" = "0" ] && echo 1 || echo 0)
check "a stale artefact from a previous export is gone" \
      $([ ! -e "$TMP/out/sysimage-sdcard.img.old" ] && echo 1 || echo 0)
check "a stale artefact this build did not produce is gone" \
      $([ ! -e "$TMP/out/k230-canmv-rm69a10-hdmi.dtb" ] && echo 1 || echo 0)

# ---- 7. the checksums cover this build's artefacts, and only those -------

SUMS="$TMP/out/SHA256SUMS.txt"
check "SHA256SUMS.txt exists" $([ -f "$SUMS" ] && echo 1 || echo 0)
check "it covers the SD-card image" $(has "$SUMS" "sysimage-sdcard.img")
check "it covers BUILD_INFO.txt" $(has "$SUMS" "BUILD_INFO.txt")
check "it does not cover itself" \
      $([ "$(grep -c 'SHA256SUMS.txt' "$SUMS")" = "0" ] && echo 1 || echo 0)
listed=$(wc -l < "$SUMS")
present=$(find "$TMP/out" -maxdepth 1 -type f ! -name SHA256SUMS.txt | wc -l)
check "it lists every exported file and nothing else ($listed listed, $present present)" \
      $([ "$listed" = "$present" ] && echo 1 || echo 0)
check "the checksums are of the files actually exported" \
      $( (cd "$TMP/out" && sha256sum -c SHA256SUMS.txt >/dev/null 2>&1) && echo 1 || echo 0)

# ---- 8. the report lists what it exported -------------------------------

check "BUILD_INFO lists the exported artefacts" $(has "$INFO" "Exported  :")
check "and names the SD-card image among them" \
      $(grep '^Exported  :' "$INFO" | grep -q 'sysimage-sdcard.img' && echo 1 || echo 0)
check "and does not name one this build did not produce" \
      $(grep '^Exported  :' "$INFO" | grep -q 'rm69a10-hdmi' && echo 0 || echo 1)

# ---- 9. the case the v0.0.9 release build hit -------------------------
#
# Buildroot copies the kernel and the device trees into images/ only when
# their own package rebuilds. A release build that produces a new SD-card
# image from an unchanged kernel is the ordinary case, not a failure - the
# first genuine v0.0.9 build did exactly that and the export refused it,
# because Image was on the required list and checked for freshness.
#
# So: a fresh SD-card image beside a kernel from before the apply must export,
# and the report must say which artefacts were carried rather than built.

rm -rf "$TMP/out"
APPLIED_EPOCH=$(date +%s)
write_manifest "$SHORT_A" clean no
fresh_images
sleep 1
touch "$IMAGES/sysimage-sdcard.img" "$IMAGES/sysimage-sdcard.img.gz"
touch -d '2020-01-01 00:00:00' "$IMAGES/Image" "$IMAGES/k.dtb"
rc=$(run_build)
check "a new image beside an unchanged kernel exports" $([ "$rc" = "0" ] && echo 1 || echo 0)
[ "$rc" != "0" ] && sed -n '1,10p' "$TMP/log"
check "the kernel is still exported" $([ -f "$TMP/out/Image" ] && echo 1 || echo 0)
check "and the report names it as carried, not built" \
      $(grep '^Carried   :' "$INFO" 2>/dev/null | grep -q 'Image' && echo 1 || echo 0)
check "the SD-card image is not called carried" \
      $(grep '^Carried   :' "$INFO" 2>/dev/null | grep -q 'sysimage-sdcard.img ' && echo 0 || echo 1)
check "the checksums still cover everything exported" \
      $( (cd "$TMP/out" && sha256sum -c SHA256SUMS.txt >/dev/null 2>&1) && echo 1 || echo 0)
APPLIED_EPOCH=1

echo "build_provenance_test: $failed failure(s)"
exit $((failed > 0))
