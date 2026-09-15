#!/bin/bash
# platforms/k230/scripts/verify_splash.sh has to find the committed splash in a
# built image's boot partition, and refuse every way of not having it.
#
# The images are the same small MBR + ext4 boot partitions the image gate's
# own test builds (tests/mkbootimg.sh). The negative controls matter most: a
# splash that is missing, a byte short or the vendor's still boots, so nothing
# but this check would notice before the panel does.
set -u
cd "$(dirname "$0")/.." || exit 1
VERIFY=platforms/k230/scripts/verify_splash.sh
SPLASH=platforms/k230/rootfs_overlay/logo.xrgb
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

. tests/mkbootimg.sh
missing="$(mkbootimg_missing_tools)"
command -v debugfs >/dev/null 2>&1 || missing="${missing} debugfs"
if [ -n "${missing}" ]; then
    echo "NOT RUN splash_image_test: missing${missing}, so whether the splash check works cannot be checked."
    echo "        This is a release gate; not running it is not a pass."
    if [ "${POCKETOS_ALLOW_SKIPPED_GATES:-0}" = "1" ]; then
        echo "        POCKETOS_ALLOW_SKIPPED_GATES=1 - continuing, but this gate did NOT pass."
        exit 0
    fi
    exit 77
fi

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

run() { # <image> [expected]: prints the exit code, output in out.txt
    bash "${VERIFY}" "$@" >"${TMP}/out.txt" 2>&1
    echo "$?"
}
image_with() { # <name> <splash file or ""> - a complete boot partition plus the splash
    local d="${TMP}/$1"
    mkbootimg_populate "${d}"
    [ -n "$2" ] && cp "$2" "${d}/logo.xrgb"
    mkbootimg_make "${d}" "${TMP}/$1.img"
}

# ---- positive control --------------------------------------------------------
if image_with good "${SPLASH}"; then
    rc="$(run "${TMP}/good.img")"
    check "an image carrying the committed splash passes" "$([ "${rc}" = "0" ] && echo 1 || echo 0)"
    check "and says so, with the size" \
        "$(grep -q 'SPLASH: PASS' "${TMP}/out.txt" && grep -q '2799104 bytes' "${TMP}/out.txt" && echo 1 || echo 0)"
else
    check "could not build a test filesystem (mkfs.ext4 -d unsupported?)" 0
fi

# ---- negative controls -------------------------------------------------------
refused() { # <label> <image name> <message fragment>
    rc="$(run "${TMP}/$2.img")"
    check "$1 is refused" "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
    check "$1: the refusal says why" "$(grep -q -- "$3" "${TMP}/out.txt" && echo 1 || echo 0)"
    check "$1: the refusal does not claim a pass" "$(grep -q 'SPLASH: PASS' "${TMP}/out.txt" && echo 0 || echo 1)"
}

image_with nosplash "" && refused "an image with no splash" nosplash "MISSING: /logo.xrgb"

head -c 2798848 "${SPLASH}" > "${TMP}/short.xrgb"
image_with short "${TMP}/short.xrgb" && refused "a splash of 2,798,848 bytes" short "WRONG SIZE"

# Same size, different picture: what an SDK tree still holding the vendor's
# splash would produce. One black pixel of the background turned white.
cp "${SPLASH}" "${TMP}/other.xrgb"
printf '\377\377\377' | dd of="${TMP}/other.xrgb" bs=1 seek=$(((10 * 568 + 10) * 4)) conv=notrunc status=none
check "the altered splash really differs" "$(cmp -s "${TMP}/other.xrgb" "${SPLASH}" && echo 0 || echo 1)"
image_with other "${TMP}/other.xrgb" && refused "a splash of the right size with other pixels" other "DIFFERS"

head -c 1048576 /dev/urandom > "${TMP}/garbage.img"
rc="$(run "${TMP}/garbage.img")"
check "a file with no MBR is refused" "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
rc="$(run "${TMP}/missing.img")"
check "a missing image is refused" "$([ "${rc}" != "0" ] && echo 1 || echo 0)"
bash "${VERIFY}" >/dev/null 2>&1
check "no argument is refused" "$([ "$?" != "0" ] && echo 1 || echo 0)"

echo "splash_image_test: $failed failure(s)"
[ "$failed" -eq 0 ]
