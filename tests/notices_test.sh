#!/bin/bash
# Third-party notices: what Doors ships about other people's work, and what
# it must not claim about its own (docs/LICENSING.md).
#
#   - THIRD_PARTY_NOTICES.txt is exactly what third_party/notices produces, and
#     every text there is accounted for.
#   - Each copied licence text is byte-identical to its pinned upstream wherever
#     that source can be read here. apply_to_sdk.sh repeats this strictly,
#     against the SDK's own source archives, before anything is packaged.
#   - Third-party code cannot reach a Doors binary without an entry: the
#     vendored trees the Makefile and the shell build compile, the fonts the
#     shell embeds, the LVGL it loads and the packages it depends on are all
#     tied to entries or to a Buildroot package that carries its own licence.
#   - The notices are installed into the image, handed to legal-info, and sent
#     by the bench deploy; pocketos.hash holds their sha256 for legal-info, and
#     a notices file that no longer matches it is refused.
#   - Doors' own licence (PocketOS through v0.0.9) stays undecided: no licence
#     file, the package says so and is not redistributable, and the notices
#     say so first.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

GEN=tools/legal/gen_notices.sh
SOURCES=third_party/notices/SOURCES
MK=platforms/k230/package/pocketos/pocketos.mk
HASHF=platforms/k230/package/pocketos/pocketos.hash
NOTICES=THIRD_PARTY_NOTICES.txt
ids() { grep -v -E '^[[:space:]]*(#|$)' "$SOURCES" | awk -F ' [|] ' '{print $1}' | sed 's/[[:space:]]*$//'; }
has_id() { ids | grep -qx "$1" && echo 1 || echo 0; }

# ---- the file and its sources ------------------------------------------
out=$(bash "$GEN" --check 2>&1); rc=$?
check "THIRD_PARTY_NOTICES.txt is what third_party/notices produces, and pocketos.hash matches it" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
[ $rc -eq 0 ] || printf '%s\n' "$out" | sed 's/^/     /'
check "every SOURCES line has six fields" \
    "$(grep -v -E '^[[:space:]]*(#|$)' "$SOURCES" | awk -F ' [|] ' 'NF != 6 {bad=1} END {print bad ? 0 : 1}')"
check "ids are unique" "$([ "$(ids | sort | uniq -d | wc -l)" = 0 ] && echo 1 || echo 0)"
orphans=""
for t in third_party/notices/texts/*; do
    b=$(basename "$t")
    grep -q -E "(^| )text:${b}\$|^${b%.txt} [|] .*[|] [a-z_]+:[^ ]+\$" "$SOURCES" || orphans="$orphans $b"
done
check "every file in third_party/notices/texts belongs to an entry${orphans:+ (orphans:$orphans)}" \
    "$([ -z "$orphans" ] && echo 1 || echo 0)"
check "each entry appears in the notices, in the index and with its text" \
    "$(n=$(ids | wc -l); [ "$(grep -c '^    In the image: ' "$NOTICES")" -eq "$n" ] &&
       [ "$(grep -c -x '================================================================================' "$NOTICES")" -eq $((2 * n)) ] &&
       echo 1 || echo 0)"

# ---- the texts against their pinned upstream ----------------------------
up=$(bash "$GEN" --verify-upstream 2>&1); rc=$?
printf '%s\n' "$up" | grep -E '^(ok|FAIL) ' | sed 's/^\(ok\|FAIL\) \+/     /'
check "no copied text differs from its pinned upstream where that source is readable here" \
    "$([ $rc -eq 0 ] && echo 1 || echo 0)"
unread=$(printf '%s\n' "$up" | grep -c '^note ')
[ "$unread" -gt 0 ] && echo "     $unread text(s) have no pinned source in this checkout; apply_to_sdk.sh verifies all of them strictly before packaging"
check "apply_to_sdk.sh verifies every text strictly, against the SDK's archives, before writing the package" \
    "$(grep -q 'gen_notices.sh" --verify-upstream --sdk "${SDK_DIR}" --strict' platforms/k230/scripts/apply_to_sdk.sh &&
       awk '/verify-upstream --sdk/{v=NR} /^POCKETOS_PKG_PATHSPEC=/{p=NR} END{exit !(v && p && v < p)}' platforms/k230/scripts/apply_to_sdk.sh &&
       echo 1 || echo 0)"
check "and refuses to package when the notices are wrong, with no override" \
    "$(grep -q 'an image must not ship wrong notices' platforms/k230/scripts/apply_to_sdk.sh && echo 1 || echo 0)"
check "the LVGL configuration is checked against the notices after apply and after a build" \
    "$(grep -q -- '--verify-lvconf' platforms/k230/scripts/apply_to_sdk.sh && grep -q -- '--verify-lvconf' platforms/k230/scripts/build_image.sh && echo 1 || echo 0)"

# ---- pins agree with the build -------------------------------------------
commit_of() { grep -E "^$1 [|]" "$SOURCES" | grep -o -E 'commit [0-9a-f]{40}' | head -1 | cut -d' ' -f2; }
check "RadioLib's entry names the pinned commit" \
    "$([ "$(commit_of radiolib)" = "$(cat platforms/k230/vendor_radiolib_commit.txt)" ] && echo 1 || echo 0)"
check "ggwave's entry names the pinned commit" \
    "$([ "$(commit_of ggwave)" = "$(cat platforms/k230/vendor_ggwave_commit.txt)" ] && echo 1 || echo 0)"
check "LVGL's entry names the commit the defconfig builds" \
    "$([ "$(commit_of lvgl)" = "$(tr -d '\r' < platforms/k230/configs/k230_pocketos_defconfig | sed -n 's/^BR2_PACKAGE_LVGL_CUSTOM_VERSION="\(.*\)"$/\1/p')" ] && echo 1 || echo 0)"

# ---- nothing third-party reaches a binary without an entry --------------
# Vendored source the GNU make tree compiles.
vend=$(grep -v -E '^[[:space:]]*#' Makefile |
       grep -o -E '\$\((RADIOLIB|GGWAVE)_DIR\)|vendor/[A-Za-z0-9_.-]+|third_party/[A-Za-z0-9_.-]+' | sort -u | tr '\n' ' ')
# meshcored compiles MeshCore, orlp's ed25519 and rweather's Crypto. The image
# package builds and installs it (pocketos.mk, ENABLE_MESHCORED=1) since the
# notices gained entries for all three (docs/LICENSING.md item 9, 2026-09-22);
# an ordinary host build still leaves it out, because it needs two extra
# checkouts. What must stay true is the rule, not the default: nothing installs
# meshcored while the notices say nothing about what it contains. So the gate
# is executed below with the notices as they are - where it must pass - and
# with an entry it cannot find - where it must still refuse. An earlier version
# of this file only read the Makefile's default text, and
# `ENABLE_MESHCORED=1 make install` shipped the binary while the test said it
# could not; a gate that is only read is not tested.
MESHCORE_IDS="meshcore ed25519 arduinolibs-crypto"

check "an ordinary host build leaves meshcored out (it needs vendor/RIFT and vendor/Crypto)" \
    "$(grep -q '^ENABLE_MESHCORED ?= 0$' Makefile &&
       awk '/^ifeq \(\$\(ENABLE_MESHCORED\),1\)/{c++} END{exit !(c >= 2)}' Makefile &&
       echo 1 || echo 0)"
# Wired to `install`, so the refusal happens before anything is written, and
# under `make -j` too: a prerequisite completes before the recipe starts.
check "installing is gated on the notices, not on a flag" \
    "$(grep -q '^install: all meshcored-shipping-check$' Makefile && echo 1 || echo 0)"
check "and the gate reads the notices themselves" \
    "$(awk '/^MESHCORE_NOTICES_OK :=/{f=1} f && /third_party\/notices\/SOURCES/{print "1"; exit}' Makefile |
       grep -qx 1 && echo 1 || echo 0)"

# Executed, not read. These need no build and no vendored checkout: the gate
# decides before any of that.
tmpd="$TMP/shipping"
mkdir -p "$tmpd"
make ENABLE_MESHCORED=1 meshcored-shipping-check > "$tmpd/on.log" 2>&1
check "with the notices as they are, an enabled build may be installed" \
    "$([ $? -eq 0 ] && echo 1 || echo 0)"
check "and the gate says the notices cover it" \
    "$(grep -q 'the notices cover what it contains' "$tmpd/on.log" && echo 1 || echo 0)"
# The negative control: one entry it cannot find, and it refuses again. Without
# this the pass above could be unconditional and nobody would notice.
make ENABLE_MESHCORED=1 MESHCORE_NOTICE_IDS="$MESHCORE_IDS no-such-notice" \
    meshcored-shipping-check > "$tmpd/miss.log" 2>&1
check "the gate still refuses when a notices entry is missing" \
    "$([ $? -ne 0 ] && echo 1 || echo 0)"
check "and names the missing entry" \
    "$(grep -q 'no-such-notice' "$tmpd/miss.log" && echo 1 || echo 0)"
check "and says that building and testing it is still allowed" \
    "$(grep -q 'make ENABLE_MESHCORED=1 meshcored' "$tmpd/miss.log" && echo 1 || echo 0)"

# Explicitly 0, not merely unset: this run may have been given a 1, and the
# question here is what the disabled configuration does.
ENABLE_MESHCORED=0 make ENABLE_MESHCORED=0 meshcored-shipping-check > "$tmpd/off.log" 2>&1
check "the default configuration passes it" "$([ $? -eq 0 ] && echo 1 || echo 0)"

# The positive control: the gate is driven by the notices, so an empty
# requirement satisfies it. Without this the refusal above could be
# unconditional and nobody would notice until the notices were written.
make ENABLE_MESHCORED=1 MESHCORE_NOTICE_IDS= meshcored-shipping-check > "$tmpd/sat.log" 2>&1
check "and it stops refusing once its requirement is met" \
    "$([ $? -eq 0 ] && echo 1 || echo 0)"

# The image package builds it, and installs it.
check "the image package builds meshcored" \
    "$(grep -Eq '^\s.*ENABLE_MESHCORED=1 -C \$\(@D\) all$' platforms/k230/package/pocketos/pocketos.mk && echo 1 || echo 0)"
check "and installs it through the gated install target" \
    "$(grep -Eq '^\s.*ENABLE_MESHCORED=1 -C \$\(@D\) DESTDIR=.* install$' platforms/k230/package/pocketos/pocketos.mk && echo 1 || echo 0)"

# The package path refuses early rather than at the end of a long build, and
# this is executed: apply_to_sdk.sh, copied into a scratch tree whose notices
# lack the three entries, must stop before it looks for an SDK.
fake="$tmpd/fakerepo"
mkdir -p "$fake/platforms/k230/scripts" "$fake/third_party/notices"
cp platforms/k230/scripts/apply_to_sdk.sh "$fake/platforms/k230/scripts/"
cp platforms/k230/vendor_bsp_commit.txt platforms/k230/vendor_sdk_commit.txt "$fake/platforms/k230/"
grep -v -E '^(meshcore|ed25519|arduinolibs-crypto) *[|]' third_party/notices/SOURCES \
    > "$fake/third_party/notices/SOURCES"
bash "$fake/platforms/k230/scripts/apply_to_sdk.sh" "$fake/no-sdk" > "$tmpd/app.log" 2>&1
check "apply_to_sdk.sh refuses a package whose notices lack the MeshCore entries" \
    "$([ $? -ne 0 ] && echo 1 || echo 0)"
check "before it assembles anything, and names them" \
    "$(grep -q 'has no entry' "$tmpd/app.log" && grep -q 'meshcore ed25519 arduinolibs-crypto' "$tmpd/app.log" &&
       ! grep -q 'not a T-Display-K230 checkout' "$tmpd/app.log" && echo 1 || echo 0)"

# The notices requirement itself, against the configuration this run was
# given, is asserted by the vendored-tree loop below - which is where every
# other compiled tree is checked, and where this one belongs.

unknown=""
for v in $vend; do
    case "$v" in
        '$(RADIOLIB_DIR)'|vendor/RadioLib|third_party/RadioLib) [ "$(has_id radiolib)" = 1 ] || unknown="$unknown $v" ;;
        '$(GGWAVE_DIR)'|vendor/ggwave|third_party/ggwave)
            for i in ggwave reed-solomon ooura-fft; do [ "$(has_id $i)" = 1 ] || unknown="$unknown $v($i)"; done ;;
        # Not a vendored tree: the notices' own source directory, which the
        # Makefile now names because the shipping gate reads it.
        third_party/notices) ;;
        vendor/RIFT|third_party/RIFT|vendor/Crypto|third_party/Crypto)
            # meshcored compiles these, and every image ships it.
            for i in $MESHCORE_IDS; do
                [ "$(has_id $i)" = 1 ] || unknown="$unknown $v($i)"
            done ;;
        *) unknown="$unknown $v" ;;
    esac
done
check "every vendored tree the Makefile compiles has entries${unknown:+ (not covered:$unknown)}" \
    "$([ -z "$unknown" ] && echo 1 || echo 0)"
check "ggwave's FFT and Reed-Solomon code are what the package copies" \
    "$(grep -q 'src/fft.h' platforms/k230/scripts/apply_to_sdk.sh && grep -q 'src/reed-solomon/rs.hpp' platforms/k230/scripts/apply_to_sdk.sh && echo 1 || echo 0)"
# What the shell build embeds and loads.
check "the shell embeds the IBM Plex bitmaps, which have an entry" \
    "$(grep -q 'ui/pocketui/fonts/pos_font_\*\.c' ui/shell/CMakeLists.txt && [ "$(has_id ibm-plex)" = 1 ] &&
       grep -q -l 'IBMPlex' ui/pocketui/fonts/pos_font_sans_16.c && echo 1 || echo 0)"
fontsrc=$(grep -h -o -E -- '--font [^ ]+' ui/pocketui/fonts/*.c | awk '{print $2}' | xargs -n1 basename | sort -u | grep -v -E '^IBMPlex(Sans|Mono)-' | tr '\n' ' ')
check "every embedded bitmap font was converted from IBM Plex${fontsrc:+ (other sources: $fontsrc)}" \
    "$([ -z "$fontsrc" ] && echo 1 || echo 0)"
check "the shell loads LVGL, which has an entry, and uses its Montserrat and Font Awesome glyphs, which have entries" \
    "$(grep -q 'target_link_libraries(pocketos-shell PRIVATE lvgl' ui/shell/CMakeLists.txt &&
       for i in lvgl montserrat font-awesome-5; do [ "$(has_id $i)" = 1 ] || exit 1; done && echo 1 || echo 0)"
# Packages the pocketos package builds against: each is either covered here
# or a Buildroot package that already carries its own licence metadata.
deps=$(sed -n 's/^POCKETOS_DEPENDENCIES = //p' "$MK")
unknown=""
for d in $deps; do
    case "$d" in
        # jpeg: libjpeg 9f for pos-camera, in the image and its legal manifest
        # already (docs/legal/licenses/libjpeg-9f), with its own IJG metadata.
        # libcurl: libcurl 8.12.1 (with OpenSSL 3.4.1) for pos-zabbix, in the
        # image already (the curl tool), with its own curl licence metadata;
        # linked dynamically, nothing of it is compiled into Doors. pos-browser
        # links it too, and jpeg.
        # libpng: libpng 1.6 for pos-browser's PNG pictures, in the image and
        # its sysroot already (for OpenCV), with its own libpng licence
        # metadata; linked dynamically, nothing of it is compiled into Doors.
        # libnncase, libmmz: the nncase 2.11 runtime and the shared-memory pool
        # for pos-vision's detector (docs/apps/VISION.md), in the image and its
        # sysroot already (ai2d_kpu, face_detect). Unlike the three above they
        # are linked STATICALLY into pos-vision, and the SDK ships them with no
        # licence metadata at all: reviewed as docs/LICENSING.md items 5 and 10
        # (the runtime is Apache-2.0 upstream; the model it runs is the open
        # question), on a prototype branch that is not released.
        # ffmpeg: FFmpeg 4.4.4 (libavformat, libavcodec, libswresample,
        # libavutil) for pos-mp3's decoder (docs/apps/MP3.md), in the image
        # and its sysroot already (OpenCV selects it), built without GPL or
        # nonfree parts, so LGPL-2.1-or-later with its own licence metadata
        # (docs/legal/manifest.csv, docs/legal/licenses/ffmpeg-4.4.4); linked
        # dynamically, nothing of it is compiled into Doors.
        cjson|libgpiod2|libdrm|libevdev|alsa-lib|jpeg|libcurl|libpng|libnncase|libmmz|ffmpeg|host-*) ;;
        lvgl) [ "$(has_id lvgl)" = 1 ] || unknown="$unknown lvgl" ;;
        *) unknown="$unknown $d" ;;
    esac
done
check "every package dependency is covered or carries its own licence metadata${unknown:+ (review:$unknown)}" \
    "$([ -z "$unknown" ] && echo 1 || echo 0)"

# ---- shipped ----------------------------------------------------------------
check "make install puts the notices at /usr/share/doors, mode 0644" \
    "$(sed -n '/^install:/,/^$/p' Makefile | grep -q 'install -D -m 0644 THIRD_PARTY_NOTICES.txt $(DESTDIR)$(PREFIX)/share/doors/THIRD_PARTY_NOTICES.txt' && echo 1 || echo 0)"
check "and links them from /usr/share/pocketos, where they were through v0.0.9" \
    "$(sed -n '/^install:/,/^$/p' Makefile | grep -q 'ln -sfn ../doors/THIRD_PARTY_NOTICES.txt $(DESTDIR)$(PREFIX)/share/pocketos/THIRD_PARTY_NOTICES.txt' && echo 1 || echo 0)"
TMPD=$(mktemp -d); trap 'rm -rf "$TMPD"' EXIT
git archive --format=tar HEAD -- platforms/k230/scripts/apply_to_sdk.sh | tar -x -C "$TMPD" 2>/dev/null
PATHSPEC=$(sed -n 's/^POCKETOS_PKG_PATHSPEC="\(.*\)"$/\1/p' "$TMPD/platforms/k230/scripts/apply_to_sdk.sh" 2>/dev/null)
mkdir -p "$TMPD/pkg"
# shellcheck disable=SC2086
git archive --format=tar HEAD -- $PATHSPEC 2>/dev/null | tar -x -C "$TMPD/pkg" 2>/dev/null
check "the committed package source carries the notices, their sources and the tool" \
    "$([ -f "$TMPD/pkg/THIRD_PARTY_NOTICES.txt" ] && [ -f "$TMPD/pkg/$SOURCES" ] && [ -f "$TMPD/pkg/$GEN" ] && echo 1 || echo 0)"
check "legal-info collects the notices" "$(grep -q '^POCKETOS_LICENSE_FILES = THIRD_PARTY_NOTICES.txt$' "$MK" && echo 1 || echo 0)"
check "the bench deploy sends them like the image carries them, link included" \
    "$([ "$(grep -c 'usr/share/doors/THIRD_PARTY_NOTICES.txt' platforms/k230/scripts/deploy.sh)" -ge 2 ] &&
       [ "$(grep -c 'usr/share/pocketos/THIRD_PARTY_NOTICES.txt' platforms/k230/scripts/deploy.sh)" -ge 2 ] && echo 1 || echo 0)"
check "build_image.sh checks the installed notices and the link at the old path" \
    "$(grep -q 'target/usr/share/doors/THIRD_PARTY_NOTICES.txt' platforms/k230/scripts/build_image.sh &&
       grep -q 'is not the link to' platforms/k230/scripts/build_image.sh && echo 1 || echo 0)"

# ---- the hash legal-info checks them against ------------------------------
# Buildroot's hash file for the package (manual, "The .hash file"). legal-info
# refuses a licence file that does not match it, but it reads the file only
# then, and it carries on when the file has no line for a licence file, so a
# missing line would check nothing. The build therefore holds the notices to it
# as well.
hash_of() { awk -v f="$2" '$1 == "sha256" && $3 == f { print $2 }' "$1" 2>/dev/null; }
check "pocketos.hash sits beside pocketos.mk, in Buildroot's format (type, hash and name two spaces apart, LF, final newline)" \
    "$([ -f "$HASHF" ] && [ -z "$(tail -c 1 "$HASHF")" ] && ! grep -q "$(printf '\r')" "$HASHF" &&
       [ "$(grep -v -E '^(#.*)?$' "$HASHF" | grep -c -v -E '^sha256  [0-9a-f]{64}  [^ ]+$')" = 0 ] &&
       echo 1 || echo 0)"
missing=""
for f in $(sed -n 's/^POCKETOS_LICENSE_FILES = //p' "$MK"); do
    [ -n "$(hash_of "$HASHF" "$f")" ] || missing="$missing $f"
done
check "every licence file legal-info collects has a sha256 there${missing:+ (missing:$missing)}" \
    "$([ -z "$missing" ] && echo 1 || echo 0)"
git show "HEAD:$HASHF" > "$TMPD/head.hash" 2>/dev/null
check "the committed pocketos.hash holds the sha256 of the committed notices" \
    "$(want=$(hash_of "$TMPD/head.hash" THIRD_PARTY_NOTICES.txt)
       [ -n "$want" ] && [ "$want" = "$(git show "HEAD:$NOTICES" | sha256sum | cut -d' ' -f1)" ] && echo 1 || echo 0)"
check "apply_to_sdk.sh requires it, checks it with the notices, and installs it from the snapshot" \
    "$(grep -q '^         platforms/k230/package/pocketos/pocketos.hash; do$' platforms/k230/scripts/apply_to_sdk.sh &&
       awk '/^NOTICES_DIR="\$\(mktemp -d\)"$/{a=NR} /^    platforms\/k230\/package\/pocketos\/pocketos\.hash \\$/{h=NR}
            /gen_notices\.sh" --check/{c=NR} END{exit !(a && h > a && c > h)}' platforms/k230/scripts/apply_to_sdk.sh &&
       grep -q 'install -m 0644 "${SNAPSHOT_DIR}/platforms/k230/package/pocketos/pocketos.hash" "${PKG_DIR}/pocketos.hash"' platforms/k230/scripts/apply_to_sdk.sh &&
       echo 1 || echo 0)"
check "build_image.sh fails a build whose installed notices do not match it" \
    "$(grep -q 'buildroot-overlay/package/pocketos/pocketos.hash' platforms/k230/scripts/build_image.sh &&
       grep -q 'so legal-info would refuse it' platforms/k230/scripts/build_image.sh && echo 1 || echo 0)"

# And the check fails when it should, on a scratch copy of what the tool reads.
G="$TMPD/hashguard"
mkdir -p "$G/tools" "$G/third_party" "$G/docs/legal" "$G/$(dirname "$HASHF")"
cp -r tools/legal "$G/tools/"; cp -r third_party/notices "$G/third_party/"
cp -r docs/legal/fonts docs/legal/third-party "$G/docs/legal/"
cp "$NOTICES" "$G/"; cp "$HASHF" "$G/$HASHF"
reset_guard() { cp "$NOTICES" "$G/"; cp "$HASHF" "$G/$HASHF"; cp third_party/notices/texts/unscii-8.txt "$G/third_party/notices/texts/"; }
guard() { bash "$G/$GEN" --check 2>&1; }
check "a scratch copy passes the check" "$(guard > /dev/null && echo 1 || echo 0)"
printf 'edited by hand\n' >> "$G/$NOTICES"
out=$(guard); rc=$?
check "a hand edit to the notices is refused, and the stale hash named" \
    "$([ $rc -ne 0 ] && printf '%s\n' "$out" | grep -q '^FAIL .*pocketos.hash' && echo 1 || echo 0)"
reset_guard; printf '\n' >> "$G/third_party/notices/texts/unscii-8.txt"
bash "$G/$GEN" > /dev/null; cp "$HASHF" "$G/$HASHF"
out=$(guard); rc=$?
check "regenerated notices without their new hash are refused" \
    "$([ $rc -ne 0 ] && printf '%s\n' "$out" | grep -q '^ok   THIRD_PARTY_NOTICES.txt is current' &&
       printf '%s\n' "$out" | grep -q '^FAIL .*pocketos.hash' && echo 1 || echo 0)"
bash "$G/$GEN" > /dev/null
check "the generator writes both, so the maintained path passes with a new hash" \
    "$(guard > /dev/null && ! cmp -s "$G/$HASHF" "$HASHF" && echo 1 || echo 0)"
reset_guard; sed -i "s/^sha256  [0-9a-f]*  /sha256  $(printf '' | sha256sum | cut -d' ' -f1)  /" "$G/$HASHF"
check "a wrong hash is refused" \
    "$([ "$(hash_of "$G/$HASHF" THIRD_PARTY_NOTICES.txt)" != "$(hash_of "$HASHF" THIRD_PARTY_NOTICES.txt)" ] &&
       ! guard > /dev/null && echo 1 || echo 0)"
reset_guard; rm -f "$G/$HASHF"
check "a missing hash file is refused" "$(guard > /dev/null && echo 0 || echo 1)"

# ---- the Vision model (docs/LICENSING.md item 10) ----------------------------
# The package installs the SDK's yolov8n.kmodel, AGPL-3.0, for internal images.
# Its notice is a statement written here followed by the FSF's licence text,
# which must stay unchanged, and the package must refuse the model without its
# notice or without the pinned hash.
MODEL_SHA=tools/vision/yolov8n.kmodel.sha256
MODEL_TEXT=third_party/notices/texts/yolov8n-kmodel.txt
pinned=$(cut -c1-64 "$MODEL_SHA" 2>/dev/null)
check "the Vision model's notice ends with the FSF's AGPL-3.0 text, unchanged" \
    "$([ "$(sed -n '/^-----BEGIN AGPL-3.0-----$/,$p' "$MODEL_TEXT" | tail -n +2 | sha256sum | cut -d' ' -f1)" = \
        0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0 ] && echo 1 || echo 0)"
check "its entry and its text name the model the package pins" \
    "$([ -n "$pinned" ] && grep -q "^yolov8n-kmodel [|] .*sha256 $pinned" "$SOURCES" &&
       grep -q "$pinned" "$MODEL_TEXT" && echo 1 || echo 0)"
check "the package installs the model only with its notice and its pinned hash" \
    "$(sed -n '/^define POCKETOS_INSTALL_TARGET_CMDS/,/^endef/p' "$MK" | tr '\n' ' ' |
       grep -q "grep -q '^yolov8n-kmodel \*|' .*SOURCES .*exit 1; } .*sha256sum -c \$(@D)/$MODEL_SHA .*\$(INSTALL) -D -m 0644 .*yolov8n.kmodel \$(TARGET_DIR)/usr/share/doors/vision/yolov8n.kmodel" &&
       echo 1 || echo 0)"
VMODEL=vendor/T-Display-K230/k230_linux_sdk/buildroot-overlay/package/yolo/utils/yolov8n.kmodel
if [ -f "$VMODEL" ]; then
    check "the pinned hash is the vendor SDK's yolov8n.kmodel" \
        "$([ "$(sha256sum < "$VMODEL" | cut -d' ' -f1)" = "$pinned" ] && echo 1 || echo 0)"
fi

# ---- Doors' own licence (PocketOS through v0.0.9): undecided -----------------
check "no licence file claims a licence for Doors" \
    "$(for f in LICENSE LICENSE.txt LICENSE.md LICENCE COPYING; do [ -e "$f" ] && exit 1; done; echo 1)"
check "the package declares the licence not yet decided, with no licence granted" \
    "$(grep -q '^POCKETOS_LICENSE = Not yet decided (Doors; no licence granted)' "$MK" && echo 1 || echo 0)"
check "and not redistributable, so legal-info does not publish Doors' source" \
    "$(grep -q '^POCKETOS_REDISTRIBUTE = NO$' "$MK" && echo 1 || echo 0)"
check "the notices say so before anything else" \
    "$(head -8 "$NOTICES" | tr '\n' ' ' | grep -q 'no licence has been chosen for the code of Doors .*No licence to it is granted.*not authorised' && echo 1 || echo 0)"
check "and tie the statement to the PocketOS name the copyright lines still carry" \
    "$(head -8 "$NOTICES" | tr '\n' ' ' | grep -q 'called PocketOS through v0.0.9; its source files still name "PocketOS authors"' && echo 1 || echo 0)"

echo "notices_test: $failed failure(s)"
exit $((failed > 0))
