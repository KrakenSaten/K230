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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHECK_ROOTFS="${SCRIPT_DIR}/../../../tools/release/check_rootfs.sh"
ROOTFS_CHECKED=0
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

# ---- the rootfs: one shell service, and only one -----------------------------
# ADR-005 Phase 3. A boot-critical file being absent is what this gate was
# written for; two shell services is the same class of fault seen from the
# other side - the image boots, and then two processes fight for DRM master and
# for shell.sock. So the rootfs is asked directly what identity it carries,
# rather than trusting that whatever assembled it removed the old one.
P2_LBA="$(le32 $((446 + 16 + 8)))"
P2_CNT="$(le32 $((446 + 16 + 12)))"
if [ "${P2_LBA:-0}" -gt 0 ] && [ "${P2_CNT:-0}" -gt 0 ]; then
    P2="${WORK}/p2.img"
    dd if="${IMG}" of="${P2}" bs=4M iflag=skip_bytes,count_bytes \
       skip=$((P2_LBA * 512)) count=$((P2_CNT * 512)) status=none 2>/dev/null
    if debugfs -R "ls /" "${P2}" >/dev/null 2>&1; then
        echo ""
        echo "Root partition of ${IMG} (partition 2, $((P2_CNT / 2048)) MiB):"
        rootfs_has() { debugfs -R "stat \"$1\"" "${P2}" 2>/dev/null | grep -q '^Inode:'; }

        for path in /usr/bin/doors-shell /etc/init.d/S90doors-shell; do
            if rootfs_has "${path}"; then
                note "ok  ${path} (the Doors shell service)"
            else
                bad "${path} (the Doors shell service) is not in the root partition"
            fi
        done
        # The PocketOS-era service, and the settings files: a shipped
        # /etc/default file would decide panel ownership for every unit from
        # the image instead of from the unit, which is why neither is packaged.
        for path in /usr/bin/pocketos-shell /etc/init.d/S90pocketos-shell \
                    /etc/default/doors-shell /etc/default/pocketos-shell \
                    /etc/default/k230_phone_ui; do
            if rootfs_has "${path}"; then
                echo "  PRESENT: ${path} must not be in the image (ADR-005 Phase 3)" >&2
                failed=$((failed + 1))
            else
                note "ok  ${path} absent, as it must be"
            fi
        done
        # With no settings file the defaults decide the first boot of a fresh
        # card, and it has to be Doors': no vendor launcher, the shell on.
        rootfs_cat() { debugfs -R "cat \"$1\"" "${P2}" 2>/dev/null; }
        for path in /etc/init.d/S99zz_k230_phone_ui /root/app/k230_phone_ui \
                    /root/music /root/videos /root/notification; do
            if rootfs_has "${path}"; then
                bad "${path}: the vendor launcher is still in the image"
            else
                note "ok  ${path} absent (no vendor launcher; first boot is Doors)"
            fi
        done
        if rootfs_has /etc/init.d/S90doors-shell; then
            if rootfs_cat /etc/init.d/S90doors-shell | grep -q '^ENABLE=1$'; then
                note "ok  /etc/init.d/S90doors-shell on by default"
            else
                bad "/etc/init.d/S90doors-shell is not on by default: a fresh card would leave the panel to nobody"
            fi
        fi
        # Every service whole, and every binary the build the release file
        # names: the same check deploy.sh makes of the tree it copies from
        # (tools/release/check_rootfs.sh), made here of what the card will
        # carry. Unit A had meshcored with no init script and a release file
        # naming another build; an image with either fault stops here. The
        # files the check reads are dumped out of the partition and given the
        # mode the partition records for them (read with stat, set with chmod,
        # rather than debugfs dump -p, whose chown part needs root), so an init
        # script that is not executable in the image is not executable in the
        # copy either.
        MINI="${WORK}/rootfs"
        mkdir -p "${MINI}"
        # The paths are the checker's own (check_rootfs.sh --list), so the two
        # cannot fall out of step.
        for path in $(bash "${CHECK_ROOTFS}" --list 2>/dev/null); do
            rootfs_has "/${path}" || continue
            mkdir -p "${MINI}/$(dirname "${path}")"
            mode=$(debugfs -R "stat \"/${path}\"" "${P2}" 2>/dev/null \
                   | sed -n 's/.*Mode: *\(0[0-7]*\).*/\1/p' | head -1)
            debugfs -R "dump \"/${path}\" \"${MINI}/${path}\"" "${P2}" >/dev/null 2>&1 \
                || : > "${MINI}/${path}"
            chmod "${mode:-0644}" "${MINI}/${path}"
        done
        if [ ! -f "${CHECK_ROOTFS}" ]; then
            echo "  MISSING: ${CHECK_ROOTFS}; the installation cannot be checked" >&2
            failed=$((failed + 1))
        elif ! bash "${CHECK_ROOTFS}" "${MINI}" | sed 's/^/  /'; then
            failed=$((failed + 1))
        fi
        ROOTFS_CHECKED=1
    else
        echo "  note: partition 2 is not a readable ext filesystem; rootfs identity not checked"
    fi
else
    echo "  note: ${IMG} has no second partition; rootfs identity not checked"
fi

if [ "${failed}" -ne 0 ]; then
    echo "" >&2
    echo "IMAGE GATE: FAIL - ${failed} problem(s) in ${IMG}." >&2
    echo "            This image must not be released or flashed." >&2
    exit 1
fi

if [ "${ROOTFS_CHECKED}" = 1 ]; then
    echo "IMAGE GATE: PASS - every boot-critical file is present and non-empty, the root partition carries exactly one shell service, and every service is whole and from one build."
else
    # Said, not implied: a pass that never read the root partition is a pass
    # about the boot partition only.
    echo "IMAGE GATE: PASS (boot partition only) - every boot-critical file is present and non-empty; the root partition was NOT checked."
fi
exit 0
