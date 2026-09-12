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
