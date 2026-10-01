#!/usr/bin/env bash
# Verify that a built SD-card image carries the Doors boot splash.
#
# Usage: verify_splash.sh /path/to/sysimage-sdcard.img [expected.xrgb]
#   expected.xrgb defaults to platforms/k230/rootfs_overlay/logo.xrgb in this
#   checkout.
#
# U-Boot loads /logo.xrgb from partition 1 and shows it only when it is exactly
# 568 x 1232 x 4 = 2,799,104 bytes (vendor board/canaan/common/logo/k230_logo.c).
# Anything else - no file, a wrong size, the vendor's own splash left behind by
# an SDK tree applied from somewhere else - still boots, with the wrong splash
# or none, and nothing in the build says so. So this reads the file out of the
# image itself and compares it with the committed splash byte for byte.
#
# Not part of verify_image.sh: a missing splash does not stop the board
# booting, and that gate answers only "can this image boot". Run both before a
# hardware splash check (docs/hardware/DOORS_GRAPHICS_GATE.md).
#
# Read-only, no root: the partition is copied out with dd and read with debugfs.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMG="${1:-}"
EXPECTED="${2:-${SCRIPT_DIR}/../rootfs_overlay/logo.xrgb}"
SIZE=2799104

[ -n "${IMG}" ] || { echo "usage: $(basename "$0") <sysimage-sdcard.img> [expected.xrgb]" >&2; exit 2; }
[ -f "${IMG}" ] || { echo "ERROR: no such image: ${IMG}" >&2; exit 2; }
[ -f "${EXPECTED}" ] || { echo "ERROR: no expected splash: ${EXPECTED}" >&2; exit 2; }
command -v debugfs >/dev/null 2>&1 || {
    echo "ERROR: debugfs (e2fsprogs) is required to look inside the image." >&2
    exit 2
}

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

le32() { od -An -tu4 -j "$1" -N4 "${IMG}" | tr -d ' '; }
SIG="$(od -An -tx2 -j 510 -N2 "${IMG}" | tr -d ' ')"
[ "${SIG}" = "aa55" ] || { echo "ERROR: ${IMG} has no MBR signature." >&2; exit 1; }
P1_LBA="$(le32 $((446 + 8)))"
P1_CNT="$(le32 $((446 + 12)))"
[ "${P1_LBA:-0}" -gt 0 ] && [ "${P1_CNT:-0}" -gt 0 ] || {
    echo "ERROR: ${IMG} has no first partition in its MBR." >&2
    exit 1
}
dd if="${IMG}" of="${WORK}/p1.img" bs=4M iflag=skip_bytes,count_bytes \
   skip=$((P1_LBA * 512)) count=$((P1_CNT * 512)) status=none 2>/dev/null
debugfs -R "ls /" "${WORK}/p1.img" >/dev/null 2>&1 || {
    echo "ERROR: partition 1 of ${IMG} is not a readable ext filesystem." >&2
    exit 1
}

fail() {
    echo "  $*" >&2
    echo "SPLASH: FAIL - U-Boot would show the wrong splash or none." >&2
    exit 1
}

debugfs -R "dump /logo.xrgb ${WORK}/logo.xrgb" "${WORK}/p1.img" >/dev/null 2>&1
[ -f "${WORK}/logo.xrgb" ] || fail "MISSING: /logo.xrgb is not in the boot partition"
got="$(stat -c %s "${WORK}/logo.xrgb")"
[ "${got}" -eq "${SIZE}" ] || fail "WRONG SIZE: /logo.xrgb is ${got} bytes; U-Boot accepts exactly ${SIZE} (568 x 1232 x 4)"
cmp -s "${WORK}/logo.xrgb" "${EXPECTED}" || \
    fail "DIFFERS: /logo.xrgb is not ${EXPECTED} (sha256 $(sha256sum < "${WORK}/logo.xrgb" | cut -c1-16)...)"

echo "  ok  /logo.xrgb, ${got} bytes, sha256 $(sha256sum < "${WORK}/logo.xrgb" | cut -d' ' -f1)"
echo "SPLASH: PASS - the boot partition carries the committed splash."
exit 0
