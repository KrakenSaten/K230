#!/bin/bash
# Third-party notices: what PocketOS ships about other people's work, and what
# it must not claim about its own (docs/LICENSING.md).
#
#   - THIRD_PARTY_NOTICES.txt is exactly what third_party/notices produces, and
#     every text there is accounted for.
#   - Each copied licence text is byte-identical to its pinned upstream wherever
#     that source can be read here. apply_to_sdk.sh repeats this strictly,
#     against the SDK's own source archives, before anything is packaged.
#   - Third-party code cannot reach a PocketOS binary without an entry: the
#     vendored trees the Makefile and the shell build compile, the fonts the
#     shell embeds, the LVGL it loads and the packages it depends on are all
#     tied to entries or to a Buildroot package that carries its own licence.
#   - The notices are installed into the image, handed to legal-info, and sent
#     by the bench deploy.
#   - PocketOS's own licence stays undecided: no licence file, the package says
#     so and is not redistributable, and the notices say so first.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

GEN=tools/legal/gen_notices.sh
SOURCES=third_party/notices/SOURCES
MK=platforms/k230/package/pocketos/pocketos.mk
NOTICES=THIRD_PARTY_NOTICES.txt
ids() { grep -v -E '^[[:space:]]*(#|$)' "$SOURCES" | awk -F ' [|] ' '{print $1}' | sed 's/[[:space:]]*$//'; }
has_id() { ids | grep -qx "$1" && echo 1 || echo 0; }

# ---- the file and its sources ------------------------------------------
out=$(bash "$GEN" --check 2>&1); rc=$?
check "THIRD_PARTY_NOTICES.txt is what third_party/notices produces" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
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
unknown=""
for v in $vend; do
    case "$v" in
        '$(RADIOLIB_DIR)'|vendor/RadioLib|third_party/RadioLib) [ "$(has_id radiolib)" = 1 ] || unknown="$unknown $v" ;;
        '$(GGWAVE_DIR)'|vendor/ggwave|third_party/ggwave)
            for i in ggwave reed-solomon ooura-fft; do [ "$(has_id $i)" = 1 ] || unknown="$unknown $v($i)"; done ;;
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
        cjson|libgpiod2|libdrm|libevdev|alsa-lib|host-*) ;;
        lvgl) [ "$(has_id lvgl)" = 1 ] || unknown="$unknown lvgl" ;;
        *) unknown="$unknown $d" ;;
    esac
done
check "every package dependency is covered or carries its own licence metadata${unknown:+ (review:$unknown)}" \
    "$([ -z "$unknown" ] && echo 1 || echo 0)"

# ---- shipped ----------------------------------------------------------------
check "make install puts the notices at /usr/share/pocketos, mode 0644" \
    "$(sed -n '/^install:/,/^$/p' Makefile | grep -q 'install -D -m 0644 THIRD_PARTY_NOTICES.txt $(DESTDIR)$(PREFIX)/share/pocketos/THIRD_PARTY_NOTICES.txt' && echo 1 || echo 0)"
TMPD=$(mktemp -d); trap 'rm -rf "$TMPD"' EXIT
git archive --format=tar HEAD -- platforms/k230/scripts/apply_to_sdk.sh | tar -x -C "$TMPD" 2>/dev/null
PATHSPEC=$(sed -n 's/^POCKETOS_PKG_PATHSPEC="\(.*\)"$/\1/p' "$TMPD/platforms/k230/scripts/apply_to_sdk.sh" 2>/dev/null)
mkdir -p "$TMPD/pkg"
# shellcheck disable=SC2086
git archive --format=tar HEAD -- $PATHSPEC 2>/dev/null | tar -x -C "$TMPD/pkg" 2>/dev/null
check "the committed package source carries the notices, their sources and the tool" \
    "$([ -f "$TMPD/pkg/THIRD_PARTY_NOTICES.txt" ] && [ -f "$TMPD/pkg/$SOURCES" ] && [ -f "$TMPD/pkg/$GEN" ] && echo 1 || echo 0)"
check "legal-info collects the notices" "$(grep -q '^POCKETOS_LICENSE_FILES = THIRD_PARTY_NOTICES.txt$' "$MK" && echo 1 || echo 0)"
check "the bench deploy sends them like the image carries them" \
    "$([ "$(grep -c 'usr/share/pocketos/THIRD_PARTY_NOTICES.txt' platforms/k230/scripts/deploy.sh)" -ge 2 ] && echo 1 || echo 0)"

# ---- PocketOS's own licence: undecided ------------------------------------
check "no licence file claims a licence for PocketOS" \
    "$(for f in LICENSE LICENSE.txt LICENSE.md LICENCE COPYING; do [ -e "$f" ] && exit 1; done; echo 1)"
check "the package declares the licence not yet decided, with no licence granted" \
    "$(grep -q '^POCKETOS_LICENSE = Not yet decided (PocketOS; no licence granted)' "$MK" && echo 1 || echo 0)"
check "and not redistributable, so legal-info does not publish PocketOS's source" \
    "$(grep -q '^POCKETOS_REDISTRIBUTE = NO$' "$MK" && echo 1 || echo 0)"
check "the notices say so before anything else" \
    "$(head -8 "$NOTICES" | tr '\n' ' ' | grep -q 'no licence has been chosen for PocketOS.s own code.*not authorised' && echo 1 || echo 0)"

echo "notices_test: $failed failure(s)"
exit $((failed > 0))
