#!/bin/bash
# The first boot of a freshly flashed card is Doors', and the vendor launcher
# is not in the image at all.
#
# Up to v0.3.0 the patched vendor launcher (S99zz_k230_phone_ui) was on and
# S90doors-shell off unless /etc/default said otherwise, and no image ships
# those files, so every fresh card booted into the LILYGO launcher until
# someone wrote them by hand (there is no first-boot flag: the LILYGO BSP
# removes the SDK's /first_boot_flag and S00resizemmc). After that the
# launcher shipped switched off; now it is not shipped. Checked here:
#   - the real [3/5] block of apply_to_sdk.sh, run on an SDK tree an earlier
#     apply left the launcher in, takes every piece of it out - the overlay,
#     Buildroot's synced copy, the target tree, the package and its menu
#     line - leaves the vendor's own files alone, and finds nothing the
#     second time;
#   - the [3b2/5] step that the same lifted block carries since v0.3.5 takes
#     the vendor leftovers (root/script/sensor.sh, libasan, libgfortran) out
#     of all three trees;
#   - S90doors-shell starts with no settings file at all, ignores a leftover
#     ENABLE=1 for a launcher that is not installed, and still yields to one
#     that is (a unit on an older image).
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)
APPLY=platforms/k230/scripts/apply_to_sdk.sh
S90_SRC=platforms/k230/rootfs_overlay/etc/init.d/S90doors-shell
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
contains() { case "$1" in *"$2"*) echo 1 ;; *) echo 0 ;; esac; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- the launcher removal, as apply_to_sdk.sh does it ---------------------

# The block itself, from its echo to the next step's, so a change to it is
# what gets tested rather than a copy of it.
sed -n '/^echo "\[3\/5\]/,/^echo "\[3c\/5\]/p' "$APPLY" | sed '$d' > "$TMP/block.sh"
check "the launcher-removal block is found in apply_to_sdk.sh" \
    "$(grep -q 'S99zz_k230_phone_ui' "$TMP/block.sh" && echo 1 || echo 0)"
check "and it no longer runs the vendor's installer" \
    "$(grep -v '^ *#' "$TMP/block.sh" | grep -q 'install_to_sdk' && echo 0 || echo 1)"

SDK="$TMP/sdk"
CONF=k230_pocketos_defconfig
BR="$SDK/output/buildroot-2025.02.1"
OVL=board/canaan/k230-soc/rootfs_overlay
# An SDK tree as the vendor's install_to_sdk.sh leaves it, with the vendor's
# own files beside it that must survive, and the vendor leftovers that
# [3b2/5] takes out (owner's decision, 2026-10-07).
for root in "$SDK/buildroot-overlay/$OVL" "$BR/$OVL" "$SDK/output/$CONF/target"; do
    mkdir -p "$root/etc/init.d" "$root/root/script" "$root/root/app/face_detect" "$root/lib"
    printf '#!/bin/sh\n' > "$root/etc/init.d/S99zz_k230_phone_ui"
    printf '#!/bin/sh\n' > "$root/etc/init.d/S40bluetoothd"
    printf 'x' > "$root/root/script/sensor.sh"
    printf 'x' > "$root/lib/libasan.so.8"
    printf 'x' > "$root/lib/libgfortran.so.5"
    printf 'x' > "$root/root/app/face_detect/face_detect"
    for d in music nes videos photos screenshots recordings lorawan meshtastic notification nrf52840 picoclaw; do
        mkdir -p "$root/root/$d"
    done
    printf 'x' > "$root/root/music/music01.mp3"
done
mkdir -p "$SDK/output/$CONF/target/root/app/k230_phone_ui"
printf 'x' > "$SDK/output/$CONF/target/root/app/k230_phone_ui/k230_phone_ui"
mkdir -p "$SDK/buildroot-overlay/package/k230_phone_ui" "$BR/package/k230_phone_ui" \
         "$SDK/output/$CONF/build/k230_phone_ui-custom" "$SDK/buildroot-overlay/configs"
for menu in "$SDK/buildroot-overlay/package/Config_canaan.in" "$BR/package/Config_canaan.in"; do
    printf 'source "package/vvcam/Config.in"\n\nsource "package/k230_phone_ui/Config.in"\n' > "$menu"
done
printf 'BR2_PACKAGE_VVCAM=y\n' > "$SDK/buildroot-overlay/configs/$CONF"

run_block() { ( SDK_DIR="$SDK"; set -e; . "$TMP/block.sh" ) > "$TMP/apply.log" 2>&1; }
if run_block; then
    check "the block runs on an SDK the launcher was installed into" 1
else
    check "the block runs on an SDK the launcher was installed into" 0
    sed 's/^/    /' "$TMP/apply.log"
fi
left=$(find "$SDK" \( -name 'S99zz_k230_phone_ui' -o -name 'k230_phone_ui*' \
        -o -path '*/root/music' -o -path '*/root/videos' -o -path '*/root/notification' \
        -o -path '*/root/picoclaw' -o -path '*/root/nrf52840' \) | sed "s#$SDK/##")
check "no piece of the launcher is left in the SDK${left:+ (left: $(echo $left))}" \
    "$([ -z "$left" ] && echo 1 || echo 0)"
check "the package menus no longer name it" \
    "$(grep -q k230_phone_ui "$SDK/buildroot-overlay/package/Config_canaan.in" "$BR/package/Config_canaan.in" && echo 0 || echo 1)"
check "and keep the vendor's other packages" \
    "$(grep -q 'package/vvcam/Config.in' "$SDK/buildroot-overlay/package/Config_canaan.in" \
       && grep -q 'package/vvcam/Config.in' "$BR/package/Config_canaan.in" && echo 1 || echo 0)"
kept=0
for root in "$SDK/buildroot-overlay/$OVL" "$BR/$OVL" "$SDK/output/$CONF/target"; do
    [ -f "$root/etc/init.d/S40bluetoothd" ] \
        && [ -f "$root/root/app/face_detect/face_detect" ] && kept=$((kept + 1))
done
check "the vendor's own files beside it are kept" "$([ "$kept" = 3 ] && echo 1 || echo 0)"
# sensor.sh used to be counted above; since v0.3.5 the [3b2/5] step in the
# same block removes it on purpose, together with libasan and libgfortran.
leftovers=$(find "$SDK" \( -path '*/root/script/sensor.sh' -o -name 'libasan.so*' \
        -o -name 'libgfortran.so*' \) | sed "s#$SDK/##")
check "the vendor leftovers are taken out by [3b2/5]${leftovers:+ (left: $(echo $leftovers))}" \
    "$([ -z "$leftovers" ] && echo 1 || echo 0)"
check "and it says so" "$(contains "$(cat "$TMP/apply.log")" "removed buildroot-overlay/$OVL/root/script/sensor.sh")"
run_block
check "a second apply finds nothing to remove" \
    "$(contains "$(cat "$TMP/apply.log")" "no vendor launcher in the SDK")"
check "and no vendor leftovers either" "$(contains "$(cat "$TMP/apply.log")" "none in the SDK")"
printf 'BR2_PACKAGE_K230_PHONE_UI=y\n' >> "$SDK/buildroot-overlay/configs/$CONF"
check "NEGATIVE CONTROL: a Doors defconfig that names the launcher is refused" \
    "$(run_block && echo 0 || echo 1)"

# ---- S90doors-shell with no settings file ---------------------------------

ROOT="$TMP/s90"
mkdir -p "$ROOT/etc/default" "$ROOT/etc/init.d" "$ROOT/usr/bin" "$ROOT/run/pocketos" \
         "$ROOT/var/run" "$ROOT/var/lib/pocketos/log" "$ROOT/dev/dri"
: > "$ROOT/dev/dri/card0"
# A supervisor stand-in that records it was started, then waits to be stopped.
printf '#!/bin/sh\nenv > "%s/shell.env"\nexec sleep 30\n' "$ROOT" > "$ROOT/usr/bin/pos-supervise"
printf '#!/bin/sh\nexit 0\n' > "$ROOT/usr/bin/doors-shell"
chmod 0755 "$ROOT/usr/bin/pos-supervise" "$ROOT/usr/bin/doors-shell"
sed -e "s#/etc/default/#$ROOT/etc/default/#g" -e "s#/etc/init.d/#$ROOT/etc/init.d/#g" \
    -e "s#/usr/bin/#$ROOT/usr/bin/#g" -e "s#/var/run/#$ROOT/var/run/#g" \
    -e "s#/run/pocketos#$ROOT/run/pocketos#g" -e "s#/var/lib/pocketos#$ROOT/var/lib/pocketos#g" \
    -e "s#/dev/dri/card0#$ROOT/dev/dri/card0#g" \
    "$S90_SRC" > "$ROOT/S90"
check "the S90 copy has every path moved into the test root" \
    "$(grep -v '^\s*#' "$ROOT/S90" | grep -E '(^|[ "=])/(etc|usr|var|run|dev)/' | grep -vq "$ROOT" && echo 0 || echo 1)"

s90() { sh "$ROOT/S90" "$@" 2>&1; }
started() { local n=0; while [ ! -s "$ROOT/shell.env" ] && [ "$n" -lt 30 ]; do sleep 0.1; n=$((n + 1)); done; [ -s "$ROOT/shell.env" ]; }

out=$(s90 start)
check "with no settings file at all, S90 starts the shell (a fresh card boots Doors)" \
    "$([ "$(contains "$out" "OK")" = 1 ] && started && echo 1 || echo 0)"
s90 stop >/dev/null; rm -f "$ROOT/shell.env"

# A unit flashed with this image keeps its /etc/default files, and one that
# had switched the launcher back on still says ENABLE=1 for a launcher that
# is gone. Yielding to it would leave the panel dark.
printf 'ENABLE=1\n' > "$ROOT/etc/default/k230_phone_ui"
out=$(s90 start)
check "a leftover ENABLE=1 with no launcher installed does not stop the shell" \
    "$([ "$(contains "$out" "OK")" = 1 ] && started && echo 1 || echo 0)"
check "and S90 says it ignored the switch" "$(contains "$out" "vendor launcher not installed")"
s90 stop >/dev/null; rm -f "$ROOT/shell.env"

# A unit on an older image, which still has the launcher, keeps its choice.
printf '#!/bin/sh\nexit 0\n' > "$ROOT/etc/init.d/S99zz_k230_phone_ui"
chmod 0755 "$ROOT/etc/init.d/S99zz_k230_phone_ui"
out=$(s90 start)
check "a unit whose installed launcher is switched back on keeps it: S90 yields" \
    "$(contains "$out" "owns the panel")"
check "and starts nothing" "$([ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)"
rm -f "$ROOT/etc/default/k230_phone_ui" "$ROOT/etc/init.d/S99zz_k230_phone_ui"

printf 'ENABLE=0\n' > "$ROOT/etc/default/doors-shell"
out=$(s90 start)
check "a unit that switches the shell off keeps it off" \
    "$([ "$(contains "$out" "disabled")" = 1 ] && [ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)"
rm -f "$ROOT/etc/default/doors-shell"

echo "first_boot_default_test: $failed failure(s)"
exit $((failed > 0))
