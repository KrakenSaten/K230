#!/usr/bin/env bash
# Verify that a PocketOS SD-card image can actually boot.
#
# Usage: verify_image.sh /path/to/sysimage-sdcard.img
#
# The v0.0.9 image was built, exported, checksummed and flashed without anyone
# noticing that its boot partition had no kernel in it. Every check up to that
# point looked at the export directory - which files were copied, whether their
# checksums matched - and none of them looked inside the image that was about
# to be written to a card. U-Boot found out first:
#
#     Failed to load '/Image'
#
# So this reads partition 1 out of the image itself and asks whether the files
# U-Boot loads are there. Nothing is mounted (that needs root) and the image is
# opened read-only: the partition is copied out with dd and inspected with
# debugfs, both of which are already build dependencies because the image is
# assembled with mkfs.ext4.
set -uo pipefail

IMG="${1:-}"
[ -n "${IMG}" ] || { echo "usage: $(basename "$0") <sysimage-sdcard.img>" >&2; exit 2; }
[ -f "${IMG}" ] || { echo "ERROR: no such image: ${IMG}" >&2; exit 2; }

command -v debugfs >/dev/null 2>&1 || {
    echo "ERROR: debugfs (e2fsprogs) is required to look inside the image." >&2
    echo "       Refusing to call an image good without having read it." >&2
    exit 2
}

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# ---- partition 1, straight out of the MBR ------------------------------------
# Entry 0 of the table at offset 446: LBA start at +8, sector count at +12,
# both 32-bit little-endian. Parsing this rather than shelling out to parted
# keeps the check free of a tool the build does not otherwise need.
le32() { od -An -tu4 -j "$1" -N4 "${IMG}" | tr -d ' '; }

SIG="$(od -An -tx2 -j 510 -N2 "${IMG}" | tr -d ' ')"
[ "${SIG}" = "aa55" ] || {
    echo "ERROR: ${IMG} has no MBR signature (found 0x${SIG:-????})." >&2
    exit 1
}

P1_LBA="$(le32 $((446 + 8)))"
P1_CNT="$(le32 $((446 + 12)))"
[ "${P1_LBA:-0}" -gt 0 ] && [ "${P1_CNT:-0}" -gt 0 ] || {
    echo "ERROR: ${IMG} has no first partition in its MBR." >&2
    exit 1
}

P1="${WORK}/p1.img"
dd if="${IMG}" of="${P1}" bs=4M iflag=skip_bytes,count_bytes \
   skip=$((P1_LBA * 512)) count=$((P1_CNT * 512)) status=none 2>/dev/null

debugfs -R "ls /" "${P1}" >/dev/null 2>&1 || {
    echo "ERROR: partition 1 of ${IMG} is not a readable ext filesystem." >&2
    exit 1
}

# ---- the files U-Boot loads --------------------------------------------------
failed=0
note() { echo "  $*"; }
bad()  { echo "  MISSING: $*" >&2; failed=$((failed + 1)); }

# Size of a path inside the partition, or empty when it is not there at all.
size_of() {
    debugfs -R "stat \"$1\"" "${P1}" 2>/dev/null \
        | sed -n 's/.*[[:space:]]Size: \([0-9][0-9]*\).*/\1/p' | head -1
}

# Reads a pointer file (lcd_dtb / hdmi_dtb) - one line naming a real dtb.
content_of() { debugfs -R "cat \"$1\"" "${P1}" 2>/dev/null | tr -d '\0\r' | head -1; }

require_file() {
    local path="$1" what="$2" sz
    sz="$(size_of "${path}")"
    if [ -z "${sz}" ]; then
        bad "${path} (${what}) is not in the boot partition"
        return 1
    fi
    if [ "${sz}" -eq 0 ]; then
        bad "${path} (${what}) is present but empty"
        return 1
    fi
    note "ok  ${path} (${what}), ${sz} bytes"
    return 0
}

echo "Boot partition of ${IMG} (partition 1, $((P1_CNT / 2048)) MiB):"

# The kernel. This is the one whose absence shipped.
require_file /Image "kernel" || true

# The opensbi/u-boot payload U-Boot loads before the kernel.
require_file /fw_jump_add_uboot_head.bin "firmware payload" || true

# Each pointer file names the device tree to load; both the pointer and the
# device tree it names have to be there, or U-Boot loads a name and then fails
# to find it - which is a different failure with the same result.
for ptr in lcd_dtb hdmi_dtb; do
    if require_file "/${ptr}" "device-tree selector"; then
        dtb="$(content_of "/${ptr}")"
        if [ -z "${dtb}" ]; then
            bad "/${ptr} names no device tree"
        else
            require_file "/${dtb}" "device tree named by ${ptr}" || true
        fi
    fi
done

if [ "${failed}" -ne 0 ]; then
    echo "" >&2
    echo "IMAGE GATE: FAIL - ${failed} boot-critical file(s) missing from ${IMG}." >&2
    echo "            This image cannot boot. It must not be released or flashed." >&2
    exit 1
fi

echo "IMAGE GATE: PASS - every boot-critical file is present and non-empty."
exit 0
