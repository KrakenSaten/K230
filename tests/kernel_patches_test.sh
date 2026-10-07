#!/bin/bash
# Doors-owned kernel patches reach the SDK the way the apply script says.
#
# platforms/k230/patches/linux holds the patches Doors adds on top of the
# LILYGO BSP kernel stack (ADR-011). Two things can silently go wrong with
# them: a patch file that Buildroot will not apply (wrong prefix, CRLF, a
# number in the vendor's range so it sorts among the BSP patches), and an
# apply step that leaves a renamed patch behind so both versions apply. This
# checks the files as they are, then lifts install_kernel_patches() out of
# apply_to_sdk.sh and runs it against a scratch snapshot and a scratch SDK.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
APPLY=${APPLY:-platforms/k230/scripts/apply_to_sdk.sh}
PATCHES=platforms/k230/patches/linux
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- the patch files themselves --------------------------------------------
n=0
for p in "$PATCHES"/*.patch; do
    [ -f "$p" ] || continue
    n=$((n + 1))
    b=$(basename "$p")
    check "$b is numbered in the Doors range 0070-0099" \
        "$(echo "$b" | grep -Eq '^00[7-9][0-9]-[a-z0-9-]+\.patch$' && echo 1 || echo 0)"
    # Counted as bytes: a grep for a literal CR misreports under Git Bash.
    check "$b has LF line endings only" "$([ "$(tr -cd '\r' < "$p" | wc -c | tr -d ' ')" = 0 ] && echo 1 || echo 0)"
    check "$b is a -p1 patch (a/ and b/ prefixes)" \
        "$(grep -q '^--- a/' "$p" && grep -q '^+++ b/' "$p" && ! grep -q '^--- [^a]' "$p" && echo 1 || echo 0)"
    # The evidence a patch rests on, by what it touches: the display stack
    # cites HDMI_KERNEL_FIX.md, the board device tree K230_BUTTONS.md.
    # Nothing else may be touched - never the vendor's shared k230.dtsi.
    check "$b names ADR-011" "$(grep -q 'ADR-011' "$p" && echo 1 || echo 0)"
    check "$b touches only the display stack or the board device tree" \
        "$(grep '^+++ b/' "$p" | grep -vqE '^\+\+\+ b/(drivers/gpu/drm/|arch/riscv/boot/dts/canaan/k230-canmv-rm69a10\.dts$)' && echo 0 || echo 1)"
    if grep -q '^+++ b/drivers/gpu/drm/' "$p"; then
        check "$b names the display fix sheet" "$(grep -q 'HDMI_KERNEL_FIX.md' "$p" && echo 1 || echo 0)"
    fi
    if grep -q '^+++ b/arch/riscv/boot/dts/' "$p"; then
        check "$b names the button sheet" "$(grep -q 'K230_BUTTONS.md' "$p" && echo 1 || echo 0)"
    fi
done
check "the sheets the patches name exist" \
    "$([ -f docs/hardware/HDMI_KERNEL_FIX.md ] && [ -f docs/hardware/K230_BUTTONS.md ] && echo 1 || echo 0)"
check "there is at least one Doors kernel patch" "$([ "$n" -ge 1 ] && echo 1 || echo 0)"

# ---- the apply step, out of the script --------------------------------------
sed -n '/^install_kernel_patches() {/,/^}/p' "$APPLY" > "$TMP/fn.sh"
check "apply_to_sdk.sh defines install_kernel_patches" "$([ -s "$TMP/fn.sh" ] && echo 1 || echo 0)"
check "the snapshot archive carries platforms/k230/patches/linux" \
    "$(grep -q 'platforms/k230/patches/linux \\' "$APPLY" && echo 1 || echo 0)"
check "the manifest records the patches" \
    "$(grep -q '^doors_kernel_patches=${KERNEL_PATCHES:-none}$' "$APPLY" && echo 1 || echo 0)"

SNAP="$TMP/snap"; SDK="$TMP/sdk"
mkdir -p "$SNAP/platforms/k230/patches/linux" "$SDK/buildroot-overlay/linux"
printf 'vendor\n' > "$SDK/buildroot-overlay/linux/0064-input-k230-pmu-pwrkey.patch"
printf 'stale\n'  > "$SDK/buildroot-overlay/linux/0079-old-name.patch"
printf 'keep\n'   > "$SDK/buildroot-overlay/linux/0001-timeconst.patch.conditional"
printf 'one\n'    > "$SNAP/platforms/k230/patches/linux/0070-first.patch"
printf 'two\n'    > "$SNAP/platforms/k230/patches/linux/0071-second.patch"
printf 'no\n'     > "$SNAP/platforms/k230/patches/linux/0050-not-ours.patch"
( . "$TMP/fn.sh"; install_kernel_patches "$SNAP" "$SDK"; echo "$KERNEL_PATCHES" > "$TMP/list" )
check "both Doors patches are installed" \
    "$([ "$(cat "$SDK/buildroot-overlay/linux/0070-first.patch")" = one ] && [ "$(cat "$SDK/buildroot-overlay/linux/0071-second.patch")" = two ] && echo 1 || echo 0)"
check "installed with mode 0644" \
    "$([ "$(stat -c %a "$SDK/buildroot-overlay/linux/0070-first.patch")" = 644 ] && echo 1 || echo 0)"
check "a stale Doors patch is removed" "$([ ! -e "$SDK/buildroot-overlay/linux/0079-old-name.patch" ] && echo 1 || echo 0)"
check "vendor patches and .conditional files are left alone" \
    "$([ -e "$SDK/buildroot-overlay/linux/0064-input-k230-pmu-pwrkey.patch" ] && [ -e "$SDK/buildroot-overlay/linux/0001-timeconst.patch.conditional" ] && echo 1 || echo 0)"
check "a patch outside the Doors range is not installed" \
    "$([ ! -e "$SDK/buildroot-overlay/linux/0050-not-ours.patch" ] && echo 1 || echo 0)"
check "KERNEL_PATCHES lists them in order" \
    "$([ "$(cat "$TMP/list")" = "0070-first.patch 0071-second.patch" ] && echo 1 || echo 0)"

rm -f "$SNAP/platforms/k230/patches/linux/"*.patch
( . "$TMP/fn.sh"; install_kernel_patches "$SNAP" "$SDK"; echo "[$KERNEL_PATCHES]" > "$TMP/list" )
check "no patches: nothing installed, list empty" \
    "$([ "$(cat "$TMP/list")" = "[]" ] && [ ! -e "$SDK/buildroot-overlay/linux/0070-first.patch" ] && echo 1 || echo 0)"

echo "kernel_patches_test: $failed failure(s)"
[ "$failed" = 0 ]
