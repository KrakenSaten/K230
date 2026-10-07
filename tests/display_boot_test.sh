#!/bin/bash
# pos-display-boot (tools/display/pos-display-boot.sh) against a fake boot
# partition: the pointer files post-image.sh writes, real-looking device-tree
# blobs, and every refusal that must leave the partition untouched.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
cd "$(dirname "$0")/.." || exit 1
TOOL=tools/display/pos-display-boot.sh
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

LCD=k230-canmv-rm69a10.dtb
HDMI=k230-canmv-rm69a10-hdmi.dtb

# A boot partition as post-image.sh leaves it (LILYGO BSP bb831ab).
populate() {
    rm -rf "$1"
    mkdir -p "$1"
    printf '\xd0\x0d\xfe\xed' > "$1/${LCD}"
    head -c 100 /dev/zero >> "$1/${LCD}"
    printf '\xd0\x0d\xfe\xed' > "$1/${HDMI}"
    head -c 100 /dev/zero >> "$1/${HDMI}"
    ln -s "${LCD}" "$1/k.dtb"
    printf '%s\n' "${LCD}" > "$1/lcd_dtb"
    printf '%s\n' "${HDMI}" > "$1/hdmi_dtb"
}

run() { POS_BOOT_DIR="$1" sh "${TOOL}" "${@:2}"; }
sum() { (cd "$1" && find . -type f -exec sha256sum {} + | sort) }

B="${TMP}/boot"
populate "${B}"
out=$(run "${B}" status)
check "status on a fresh image: lcd, no force_dtb" "$(printf '%s' "${out}" | grep -q '^next boot: lcd' && echo 1 || echo 0)"
check "status names both trees" \
    "$(printf '%s' "${out}" | grep -q "hdmi tree: ${HDMI}" && printf '%s' "${out}" | grep -q "lcd tree : ${LCD}" && echo 1 || echo 0)"

before=$(sum "${B}")
run "${B}" hdmi >/dev/null
rc=$?
check "hdmi succeeds" "$([ ${rc} -eq 0 ] && echo 1 || echo 0)"
check "hdmi writes force_dtb naming the HDMI tree, one line" \
    "$([ "$(cat "${B}/force_dtb")" = "${HDMI}" ] && [ "$(wc -l < "${B}/force_dtb")" -eq 1 ] && echo 1 || echo 0)"
check "hdmi leaves no temporary file" "$([ ! -e "${B}/force_dtb.tmp" ] && echo 1 || echo 0)"
check "hdmi changes no device tree and no pointer" \
    "$([ "$(sum "${B}" | grep -v ' ./force_dtb$')" = "${before}" ] && echo 1 || echo 0)"
check "k.dtb is still the symlink to the panel tree" \
    "$([ -L "${B}/k.dtb" ] && [ "$(readlink "${B}/k.dtb")" = "${LCD}" ] && echo 1 || echo 0)"
out=$(run "${B}" status)
check "status after hdmi: hdmi" "$(printf '%s' "${out}" | grep -q "^next boot: hdmi (${HDMI}" && echo 1 || echo 0)"
run "${B}" hdmi >/dev/null
check "hdmi twice is harmless" "$([ $? -eq 0 ] && [ "$(cat "${B}/force_dtb")" = "${HDMI}" ] && echo 1 || echo 0)"

run "${B}" lcd >/dev/null
check "lcd removes force_dtb" "$([ $? -eq 0 ] && [ ! -e "${B}/force_dtb" ] && echo 1 || echo 0)"
check "lcd leaves the partition as it was" "$([ "$(sum "${B}")" = "${before}" ] && echo 1 || echo 0)"
out=$(run "${B}" lcd)
check "lcd when already lcd: nothing to change, success" \
    "$([ $? -eq 0 ] && printf '%s' "${out}" | grep -q 'nothing to change' && echo 1 || echo 0)"

# Refusals: each must exit 1 and write nothing.
refuse() { # <label> <setup command>
    populate "${B}"
    eval "$2"
    local pre
    pre=$(sum "${B}")
    run "${B}" hdmi >/dev/null 2>"${TMP}/err"
    local rc=$?
    check "refused: $1" "$([ ${rc} -eq 1 ] && [ ! -e "${B}/force_dtb" ] && [ "$(sum "${B}")" = "${pre}" ] && echo 1 || echo 0)"
}
refuse "no hdmi_dtb pointer" 'rm -f "${B}/hdmi_dtb"'
refuse "hdmi_dtb names the panel tree (image built without HDMI)" 'printf "%s\n" "${LCD}" > "${B}/hdmi_dtb"'
refuse "hdmi_dtb names a missing file" 'printf "missing.dtb\n" > "${B}/hdmi_dtb"'
refuse "HDMI tree is not a device tree" 'printf "not a dtb" > "${B}/${HDMI}"'
refuse "HDMI tree is empty" ': > "${B}/${HDMI}"'
refuse "hdmi_dtb names a path, not a file in /boot" 'printf "../etc/passwd\n" > "${B}/hdmi_dtb"'
refuse "panel tree damaged: no way back" 'printf "junk" > "${B}/${LCD}"'
refuse "no lcd_dtb pointer (not a boot partition)" 'rm -f "${B}/lcd_dtb"'
check "a refusal says why" "$(grep -q 'pos-display-boot:' "${TMP}/err" && echo 1 || echo 0)"

populate "${B}"
printf '%s\r\n' "${HDMI}" > "${B}/hdmi_dtb"
run "${B}" hdmi >/dev/null
check "a CRLF pointer file still yields a clean name" \
    "$([ "$(cat "${B}/force_dtb")" = "${HDMI}" ] && echo 1 || echo 0)"

populate "${B}"
chmod a-w "${B}"
if [ ! -w "${B}" ]; then
    run "${B}" hdmi >/dev/null 2>&1
    check "read-only boot directory: refused" "$([ $? -eq 1 ] && [ ! -e "${B}/force_dtb" ] && echo 1 || echo 0)"
fi
chmod u+w "${B}"

for args in "" "both" "hdmi lcd" "--help"; do
    # shellcheck disable=SC2086  # the words are the point
    run "${B}" ${args} >/dev/null 2>&1
    check "usage error for '${args}' exits 2" "$([ $? -eq 2 ] && echo 1 || echo 0)"
done

check "script is POSIX sh (no bash-only syntax)" "$(sh -n "${TOOL}" && echo 1 || echo 0)"
if command -v busybox >/dev/null 2>&1; then
    populate "${B}"
    POS_BOOT_DIR="${B}" busybox sh "${TOOL}" hdmi >/dev/null
    check "runs under BusyBox sh" "$([ $? -eq 0 ] && [ "$(cat "${B}/force_dtb")" = "${HDMI}" ] && echo 1 || echo 0)"
fi

echo "display_boot_test: ${failed} failure(s)"
exit $((failed > 0))
