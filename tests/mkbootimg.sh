# Sourceable test helper: build a small SD-card image shaped like the one
# genimage produces - an MBR whose first partition is an ext4 "boot" filesystem
# at LBA 2048.
#
# Two suites need this. tests/image_contents_test.sh builds images that are
# deliberately incomplete, to prove the boot gate refuses them.
# tests/build_provenance_test.sh needs its fake build to produce an image that
# is actually valid, so the export path it exercises reaches the same gate a
# real release does. Keeping one copy of the byte layout means the two cannot
# drift apart.
#
# Nothing here needs root: mkfs.ext4 -d populates the filesystem from a
# directory, and the partition table is written with dd.

MKBOOTIMG_LBA=2048
MKBOOTIMG_PART_MB=8

# Names the tools this helper needs but cannot find, empty when all are present.
mkbootimg_missing_tools() {
    local t missing=""
    for t in mkfs.ext4 dd od; do
        command -v "$t" >/dev/null 2>&1 || missing="${missing} $t"
    done
    printf '%s' "${missing}"
}

_mkbootimg_le32() {
    local n="$1"
    printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
        $((n & 255)) $((n >> 8 & 255)) $((n >> 16 & 255)) $((n >> 24 & 255)))"
}

# mkbootimg_populate <dir> - the files U-Boot loads, at their real sizes.
mkbootimg_populate() {
    local d="$1"
    mkdir -p "${d}"
    head -c 4096   /dev/zero > "${d}/Image"
    head -c 270792 /dev/zero > "${d}/fw_jump_add_uboot_head.bin"
    head -c 60191  /dev/zero > "${d}/k230-canmv-rm69a10.dtb"
    head -c 56410  /dev/zero > "${d}/k230-canmv-rm69a10-hdmi.dtb"
    printf '%s\n' "k230-canmv-rm69a10.dtb"      > "${d}/lcd_dtb"
    printf '%s\n' "k230-canmv-rm69a10-hdmi.dtb" > "${d}/hdmi_dtb"
}

# mkbootimg_make <dir> <out.img> - wrap <dir> in an ext4 partition and an MBR.
mkbootimg_make() {
    local dir="$1" out="$2" part sectors
    part="$(mktemp)"
    sectors=$((MKBOOTIMG_PART_MB * 1024 * 1024 / 512))
    mkfs.ext4 -q -F -d "${dir}" -r 1 -N 0 -m 1 -L boot -O ^64bit \
        "${part}" "${MKBOOTIMG_PART_MB}M" >/dev/null 2>&1 || { rm -f "${part}"; return 1; }
    dd if=/dev/zero of="${out}" bs=512 count=$((MKBOOTIMG_LBA + sectors)) status=none
    dd if="${part}" of="${out}" bs=512 seek="${MKBOOTIMG_LBA}" conv=notrunc status=none
    rm -f "${part}"
    printf '\x00\xfe\xff\xff\x83\xfe\xff\xff' | dd of="${out}" bs=1 seek=446 conv=notrunc status=none
    _mkbootimg_le32 "${MKBOOTIMG_LBA}" | dd of="${out}" bs=1 seek=454 conv=notrunc status=none
    _mkbootimg_le32 "${sectors}"       | dd of="${out}" bs=1 seek=458 conv=notrunc status=none
    printf '\x55\xaa' | dd of="${out}" bs=1 seek=510 conv=notrunc status=none
}

# mkbootimg_rootfs <dir> <out.img> - add <dir> as partition 2 of an image that
# mkbootimg_make already wrote. The rootfs identity gate of verify_image.sh
# reads that partition, so a test needs an image with one; everything else here
# only ever needed partition 1.
MKBOOTIMG_P2_MB="${MKBOOTIMG_P2_MB:-24}"
mkbootimg_rootfs() {
    local dir="$1" out="$2" part p1_sectors p2_lba p2_sectors size
    part="$(mktemp)"
    p1_sectors=$((MKBOOTIMG_PART_MB * 1024 * 1024 / 512))
    p2_lba=$((MKBOOTIMG_LBA + p1_sectors))
    p2_sectors=$((MKBOOTIMG_P2_MB * 1024 * 1024 / 512))
    mkfs.ext4 -q -F -d "${dir}" -r 1 -N 0 -m 1 -L rootfs -O ^64bit \
        "${part}" "${MKBOOTIMG_P2_MB}M" >/dev/null 2>&1 || { rm -f "${part}"; return 1; }
    size=$(( (p2_lba + p2_sectors) * 512 ))
    dd if=/dev/zero of="${out}" bs=1 count=0 seek="${size}" conv=notrunc status=none
    dd if="${part}" of="${out}" bs=512 seek="${p2_lba}" conv=notrunc status=none
    rm -f "${part}"
    # Entry 1 of the table: the same shape as entry 0, 16 bytes further on.
    printf '\x00\xfe\xff\xff\x83\xfe\xff\xff' | dd of="${out}" bs=1 seek=462 conv=notrunc status=none
    _mkbootimg_le32 "${p2_lba}"     | dd of="${out}" bs=1 seek=470 conv=notrunc status=none
    _mkbootimg_le32 "${p2_sectors}" | dd of="${out}" bs=1 seek=474 conv=notrunc status=none
}

# mkbootimg_rootfs_doors <dir> [build] - a root filesystem holding one complete
# Doors installation, as tools/release/check_rootfs.sh requires: every service
# a binary and the init script that names it as its DAEMON, both executable,
# pos-supervise, and /etc/doors-release naming the build every binary is
# stamped with. The binaries are stand-ins that carry the stamp the way a real
# one does - as a string in the file.
mkbootimg_rootfs_doors() {
    local d="$1" id="${2:-abc1234}" entry name bin init
    mkdir -p "${d}/usr/bin" "${d}/usr/sbin" "${d}/etc/init.d" "${d}/etc/default"
    printf '0.0.10\nBUILD_ID=%s\n' "${id}" > "${d}/etc/doors-release"
    for entry in sysd:usr/sbin/sysd:S50sysd netd:usr/sbin/netd:S55netd \
                 radiod:usr/sbin/radiod:S60radiod meshcored:usr/sbin/meshcored:S65meshcored \
                 doors-shell:usr/bin/doors-shell:S90doors-shell; do
        name="${entry%%:*}"; bin="${entry#*:}"; bin="${bin%%:*}"; init="${entry##*:}"
        mkbootimg_stamped "${d}/${bin}" "${id}"
        printf '#!/bin/sh\n# %s\nDAEMON=/%s\n' "${name}" "${bin}" > "${d}/etc/init.d/${init}"
        chmod 0755 "${d}/etc/init.d/${init}"
    done
    # The first boot of a fresh card is Doors': the shell on by default and no
    # vendor launcher in the image (apply_to_sdk.sh [3/5]).
    printf 'ENABLE=1\n' >> "${d}/etc/init.d/S90doors-shell"
    mkbootimg_stamped "${d}/usr/bin/doors" "${id}"
    printf '#!/bin/sh\n' > "${d}/usr/bin/pos-supervise"
    chmod 0755 "${d}/usr/bin/pos-supervise"
    # Vision's R0 detector (0.3.6): a stand-in, which the gate accepts only
    # through the pin mkbootimg_r0_pin writes.
    mkdir -p "${d}/usr/share/doors/vision"
    printf 'R0 stand-in\n' > "${d}/usr/share/doors/vision/det-r0-traffic6-yolox-tiny-416.kmodel"
}

# mkbootimg_r0_pin <file> - a pin (tools/vision/r0-model.sha256's shape) for
# the stand-in R0 above, for POCKETOS_R0_PIN.
mkbootimg_r0_pin() {
    printf '%s  det-r0-traffic6-yolox-tiny-416.kmodel\n' "$(printf 'R0 stand-in\n' | sha256sum | cut -d' ' -f1)" > "$1"
}

# mkbootimg_stamped <file> <build> - a stand-in binary carrying a build stamp.
mkbootimg_stamped() {
    printf '\177ELF stand-in\000DOORS_BUILD_ID=%s\000' "$2" > "$1"
    chmod 0755 "$1"
}

# mkbootimg_complete <out.img> - the common case: a valid, complete image.
mkbootimg_complete() {
    local out="$1" d
    d="$(mktemp -d)"
    mkbootimg_populate "${d}"
    mkbootimg_make "${d}" "${out}"
    local rc=$?
    rm -rf "${d}"
    return ${rc}
}
