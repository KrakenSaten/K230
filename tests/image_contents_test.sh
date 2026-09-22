#!/bin/bash
# verify_image.sh has to look inside an SD-card image and refuse one that
# cannot boot.
#
# This is the gate that was missing for v0.0.9: the image was built, exported,
# checksummed, flashed and read back byte-for-byte, and every one of those
# steps passed on an image whose boot partition had no kernel in it. U-Boot
# found out first, on the bench: `Failed to load '/Image'`.
#
# The cases below build small SD-card images - a real MBR, a real ext4 boot
# partition - and check that a complete one passes and that each way of being
# incomplete fails. The negative controls matter more than the positive one:
# a gate that cannot fail is what we had before.
set -u
cd "$(dirname "$0")/.." || exit 1
VERIFY=platforms/k230/scripts/verify_image.sh
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# This suite needs to build ext4 filesystems. Without those tools it cannot
# say anything about the gate, and saying "0 failure(s)" would be the exact
# failure mode it exists to prevent - so it reports NOT RUN and exits non-zero
# (77, the automake convention), the same shape the other release gates use.
. tests/mkbootimg.sh

missing="$(mkbootimg_missing_tools)"
command -v debugfs >/dev/null 2>&1 || missing="${missing} debugfs"
if [ -n "${missing}" ]; then
    echo "NOT RUN image_contents_test: missing${missing}, so whether the image gate works cannot be checked."
    echo "        This is a release gate; not running it is not a pass."
    if [ "${POCKETOS_ALLOW_SKIPPED_GATES:-0}" = "1" ]; then
        echo "        POCKETOS_ALLOW_SKIPPED_GATES=1 - continuing, but this gate did NOT pass."
        exit 0
    fi
    exit 77
fi

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

LBA="${MKBOOTIMG_LBA}"
make_img() { mkbootimg_make "$1" "$2"; }
populate_complete() { mkbootimg_populate "$1"; }

run_gate() {  # prints the exit code of the gate over image $1
    bash "${VERIFY}" "$1" >"${TMP}/out.txt" 2>&1
    echo "$?"
}

# ---- positive control --------------------------------------------------------
D="${TMP}/complete"; populate_complete "${D}"
if make_img "${D}" "${TMP}/complete.img"; then
    rc="$(run_gate "${TMP}/complete.img")"
    check "a complete boot partition passes the gate" "$([ "${rc}" = "0" ] && echo 1 || echo 0)"
    check "a passing gate says so" \
        "$(grep -q 'IMAGE GATE: PASS' "${TMP}/out.txt" && echo 1 || echo 0)"
    check "the kernel is reported by name and size" \
        "$(grep -q '/Image (kernel)' "${TMP}/out.txt" && echo 1 || echo 0)"
else
    check "could not build a test filesystem (mkfs.ext4 -d unsupported?)" 0
fi

# ---- negative control: exactly the v0.0.9 regression -------------------------
# This is the case that shipped. If it ever passes again, the gate is broken.
D="${TMP}/nokernel"; populate_complete "${D}"; rm -f "${D}/Image"
if make_img "${D}" "${TMP}/nokernel.img"; then
    rc="$(run_gate "${TMP}/nokernel.img")"
    check "NEGATIVE CONTROL: an image with no /Image is refused" \
        "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
    check "NEGATIVE CONTROL: the refusal names the missing kernel" \
        "$(grep -q 'MISSING: /Image' "${TMP}/out.txt" && echo 1 || echo 0)"
    check "NEGATIVE CONTROL: the refusal does not claim the gate passed" \
        "$(grep -q 'IMAGE GATE: PASS' "${TMP}/out.txt" && echo 0 || echo 1)"
    check "NEGATIVE CONTROL: the refusal says the image must not be flashed" \
        "$(grep -q 'must not be released or flashed' "${TMP}/out.txt" && echo 1 || echo 0)"
fi

# ---- a kernel that is present but empty --------------------------------------
# A truncated or zero-length copy is not a bootable kernel either, and "the
# file exists" is the check that would have missed it.
D="${TMP}/emptykernel"; populate_complete "${D}"; : > "${D}/Image"
if make_img "${D}" "${TMP}/emptykernel.img"; then
    rc="$(run_gate "${TMP}/emptykernel.img")"
    check "a zero-length /Image is refused" \
        "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
    check "the refusal says it is present but empty" \
        "$(grep -q 'present but empty' "${TMP}/out.txt" && echo 1 || echo 0)"
fi

# ---- the device tree the selector names is not there -------------------------
# v0.0.9 had this too: lcd_dtb named k230-canmv-rm69a10.dtb and no such file
# was in the partition. U-Boot would have failed on the dtb had it got past
# the kernel, so a gate that only checked the selector would still pass a
# broken image.
D="${TMP}/nodtb"; populate_complete "${D}"; rm -f "${D}/k230-canmv-rm69a10.dtb"
if make_img "${D}" "${TMP}/nodtb.img"; then
    rc="$(run_gate "${TMP}/nodtb.img")"
    check "a selector naming an absent device tree is refused" \
        "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
    check "the refusal names the device tree and its selector" \
        "$(grep -q 'named by lcd_dtb' "${TMP}/out.txt" && echo 1 || echo 0)"
fi

# ---- the selector itself missing ---------------------------------------------
D="${TMP}/nosel"; populate_complete "${D}"; rm -f "${D}/lcd_dtb"
if make_img "${D}" "${TMP}/nosel.img"; then
    rc="$(run_gate "${TMP}/nosel.img")"
    check "a missing lcd_dtb selector is refused" \
        "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
fi

# ---- the firmware payload missing --------------------------------------------
D="${TMP}/nofw"; populate_complete "${D}"; rm -f "${D}/fw_jump_add_uboot_head.bin"
if make_img "${D}" "${TMP}/nofw.img"; then
    rc="$(run_gate "${TMP}/nofw.img")"
    check "a missing firmware payload is refused" \
        "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
fi

# ---- inputs that are not images ----------------------------------------------
head -c 1048576 /dev/urandom > "${TMP}/garbage.img"
rc="$(run_gate "${TMP}/garbage.img")"
check "a file with no MBR is refused" "$([ "${rc}" != "0" ] && echo 1 || echo 0)"

rc="$(run_gate "${TMP}/does-not-exist.img")"
check "a missing image file is refused" "$([ "${rc}" != "0" ] && echo 1 || echo 0)"

bash "${VERIFY}" >/dev/null 2>&1
check "no argument is refused" "$([ "$?" != "0" ] && echo 1 || echo 0)"

# ---- an ext4 partition 1 that is not a filesystem at all ---------------------
D="${TMP}/complete"
if make_img "${D}" "${TMP}/scrambled.img"; then
    dd if=/dev/urandom of="${TMP}/scrambled.img" bs=512 seek="${LBA}" count=64 conv=notrunc status=none
    rc="$(run_gate "${TMP}/scrambled.img")"
    check "an unreadable boot filesystem is refused" \
        "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
fi

# ---- the rootfs identity (ADR-005 Phase 3) -----------------------------------
# An image that boots into two shell services is the same class of fault as one
# with no kernel: it looks complete and it does not work. The gate reads
# partition 2 and says which identity is in it.
rootfs_img() { # <out.img> <populate-fn>
    local out="$1" fn="$2" b r
    b="${TMP}/boot.$$"; r="${TMP}/root.$$"
    rm -rf "${b}" "${r}"; mkdir -p "${b}" "${r}"
    populate_complete "${b}"
    "${fn}" "${r}"
    make_img "${b}" "${out}" || return 1
    mkbootimg_rootfs "${r}" "${out}" || return 1
    rm -rf "${b}" "${r}"
}

if rootfs_img "${TMP}/doors.img" mkbootimg_rootfs_doors; then
    rc="$(run_gate "${TMP}/doors.img")"
    check "a rootfs with the Doors shell service passes" "$([ "${rc}" = "0" ] && echo 1 || echo 0)"
    check "the gate names the service it found" \
        "$(grep -q '/usr/bin/doors-shell (the Doors shell service)' "${TMP}/out.txt" && echo 1 || echo 0)"
    check "and says the PocketOS-era paths are absent" \
        "$(grep -q '/usr/bin/pocketos-shell absent' "${TMP}/out.txt" && echo 1 || echo 0)"
    check "a complete installation names meshcored with its init script" \
        "$(grep -q 'meshcored: /usr/sbin/meshcored and /etc/init.d/S65meshcored, both executable' "${TMP}/out.txt" && echo 1 || echo 0)"
    check "and every binary is the build the release file names" \
        "$(grep -q '/usr/sbin/meshcored is build abc1234' "${TMP}/out.txt" && echo 1 || echo 0)"

    both() { mkbootimg_rootfs_doors "$1"; printf '#!/bin/sh\n' > "$1/etc/init.d/S90pocketos-shell"; }
    if rootfs_img "${TMP}/two-services.img" both; then
        rc="$(run_gate "${TMP}/two-services.img")"
        check "NEGATIVE CONTROL: two shell init scripts are refused" \
            "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
        check "NEGATIVE CONTROL: the refusal names the one that must go" \
            "$(grep -q 'PRESENT: /etc/init.d/S90pocketos-shell' "${TMP}/out.txt" && echo 1 || echo 0)"
    fi

    oldbin() { mkbootimg_rootfs_doors "$1"; printf '#!/bin/sh\n' > "$1/usr/bin/pocketos-shell"; }
    if rootfs_img "${TMP}/old-binary.img" oldbin; then
        rc="$(run_gate "${TMP}/old-binary.img")"
        check "NEGATIVE CONTROL: the PocketOS-era shell binary is refused" \
            "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
    fi

    noshell() { mkdir -p "$1/usr/bin" "$1/etc/init.d"; }
    if rootfs_img "${TMP}/no-shell.img" noshell; then
        rc="$(run_gate "${TMP}/no-shell.img")"
        check "NEGATIVE CONTROL: an image with no shell service at all is refused" \
            "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
        check "NEGATIVE CONTROL: it says which service is missing" \
            "$(grep -q 'MISSING: /usr/bin/doors-shell' "${TMP}/out.txt" && echo 1 || echo 0)"
    fi

    # ---- every service whole, and one build (tools/release/check_rootfs.sh) --
    # Unit A, 2026-09-22: /usr/sbin/meshcored installed, /etc/init.d/S65meshcored
    # not, and a release file naming a build none of the new binaries were. Each
    # half of that, and its mirror image, must stop an image here.

    gate_refuses() { # <label> <image name> <populate-fn> <message the refusal must carry>
        if rootfs_img "${TMP}/$2.img" "$3"; then
            rc="$(run_gate "${TMP}/$2.img")"
            check "NEGATIVE CONTROL: $1 is refused" "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
            check "NEGATIVE CONTROL: $1 - the refusal says why" \
                "$(grep -qF -- "$4" "${TMP}/out.txt" && echo 1 || echo 0)"
            check "NEGATIVE CONTROL: $1 - the gate does not claim a pass" \
                "$(grep -q 'IMAGE GATE: PASS' "${TMP}/out.txt" && echo 0 || echo 1)"
        else
            check "could not build the image for: $1" 0
        fi
    }
    mcd_noinit() { mkbootimg_rootfs_doors "$1"; rm -f "$1/etc/init.d/S65meshcored"; }
    gate_refuses "meshcored installed with no init script (unit A's state)" mcd-noinit mcd_noinit \
        "/usr/sbin/meshcored is installed and /etc/init.d/S65meshcored is not"
    mcd_nobin() { mkbootimg_rootfs_doors "$1"; rm -f "$1/usr/sbin/meshcored"; }
    gate_refuses "an init script for a meshcored that is not installed" mcd-nobin mcd_nobin \
        "/etc/init.d/S65meshcored is installed and /usr/sbin/meshcored is not"
    mcd_mode() { mkbootimg_rootfs_doors "$1"; chmod 0644 "$1/etc/init.d/S65meshcored"; }
    gate_refuses "a meshcored init script that is not executable" mcd-mode mcd_mode \
        "/etc/init.d/S65meshcored is not executable"
    mcd_binmode() { mkbootimg_rootfs_doors "$1"; chmod 0644 "$1/usr/sbin/meshcored"; }
    gate_refuses "a meshcored binary that is not executable" mcd-binmode mcd_binmode \
        "/usr/sbin/meshcored is not executable"
    mcd_mixed() { mkbootimg_rootfs_doors "$1"; mkbootimg_stamped "$1/usr/sbin/meshcored" 3e89c9c; }
    gate_refuses "a meshcored from another build than the release file" mcd-mixed mcd_mixed \
        "/usr/sbin/meshcored is build 3e89c9c, and /etc/doors-release says abc1234"
    mcd_unstamped() { mkbootimg_rootfs_doors "$1"; printf '\177ELF old\000' > "$1/usr/sbin/meshcored"; }
    gate_refuses "a binary with no build stamp" mcd-unstamped mcd_unstamped \
        "/usr/sbin/meshcored carries no build stamp"
    nosup() { mkbootimg_rootfs_doors "$1"; rm -f "$1/usr/bin/pos-supervise"; }
    gate_refuses "an image with no pos-supervise" no-supervise nosup \
        "/usr/bin/pos-supervise is missing or not executable"
    mcd_daemon() { mkbootimg_rootfs_doors "$1"; sed -i 's#^DAEMON=.*#DAEMON=/usr/sbin/radiod#' "$1/etc/init.d/S65meshcored"; }
    gate_refuses "a meshcored init script that starts another binary" mcd-daemon mcd_daemon \
        "/etc/init.d/S65meshcored starts '/usr/sbin/radiod', not /usr/sbin/meshcored"
    mcd_default() { mkbootimg_rootfs_doors "$1"; printf 'MESHCORED_ENABLE=1\n' > "$1/etc/default/meshcored"; }
    gate_refuses "a per-unit meshcored switch shipped in the image" mcd-default mcd_default \
        "/etc/default/meshcored is in the tree; it is per-unit"
    norel() { mkbootimg_rootfs_doors "$1"; rm -f "$1/etc/doors-release"; }
    gate_refuses "an image with no release file" no-release norel \
        "/etc/doors-release is missing or names no BUILD_ID"

    shipped() { mkbootimg_rootfs_doors "$1"; printf 'ENABLE=1\n' > "$1/etc/default/doors-shell"; }
    if rootfs_img "${TMP}/shipped-settings.img" shipped; then
        rc="$(run_gate "${TMP}/shipped-settings.img")"
        check "NEGATIVE CONTROL: a settings file shipped in the image is refused" \
            "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
        check "NEGATIVE CONTROL: it names the file that must not be there" \
            "$(grep -q 'etc/default/doors-shell must not be in the image' "${TMP}/out.txt" && echo 1 || echo 0)"
    fi
else
    check "could not build a two-partition test image (mkfs.ext4 -d unsupported?)" 0
fi

echo "$failed failure(s)"
[ "$failed" -eq 0 ]
