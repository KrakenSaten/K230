#!/bin/sh
# pos-display-boot: which display the T-Display K230 drives from the next
# boot on - the built-in AMOLED (lcd) or the HDMI connector (hdmi).
# POSIX sh for BusyBox.
#
# Usage: pos-display-boot status | lcd | hdmi
#
# Why a reboot: the K230 has one MIPI DSI transmitter, and on this board its
# lanes run to both the AMOLED connector and the LT9611 DSI-to-HDMI bridge.
# The kernel drives either the panel or the bridge, chosen by the device tree
# U-Boot loads, so the output is a boot-time choice and the two never run
# together (docs/hardware/HDMI_OUTPUT.md).
#
# How: U-Boot's k230_set_dtb (vendor board code, run by bootcmd on every
# boot) loads the device tree named in /boot/force_dtb when that file exists
# and is not empty. Otherwise it probes I2C3 - which the LILYGO U-Boot does not
# enable - and falls back to the name in /boot/lcd_dtb. So:
#   hdmi  writes the name from /boot/hdmi_dtb into /boot/force_dtb
#   lcd   removes /boot/force_dtb
# No device tree is copied or changed. (The vendor launcher's HDMI switch
# copies the HDMI tree over /boot/k.dtb, which this image makes a symlink to
# the panel's tree: it would overwrite the panel's device tree. Not used.)
#
# The choice persists until changed. Recovery when the HDMI boot is not
# usable: over SSH or the serial console, `pos-display-boot lcd && reboot`;
# or stop U-Boot's autoboot on the console and boot the panel tree once
# (docs/hardware/HDMI_GATE.md, "Recovery").
#
# Environment (tests): POS_BOOT_DIR (default /boot).
#
# Exit status: 0 done; 1 refused or failed (nothing changed); 2 usage.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).

BOOT_DIR="${POS_BOOT_DIR:-/boot}"
FORCE="${BOOT_DIR}/force_dtb"

say() { printf '%s\n' "$*"; }
fail() { printf 'pos-display-boot: %s\n' "$*" >&2; exit 1; }

# First line of a pointer file, without CR/LF; empty when absent.
pointer() {
    [ -f "$1" ] || return 0
    head -n 1 "$1" | tr -d '\r\n'
}

# A device-tree blob starts with the FDT magic d00dfeed.
is_dtb() {
    [ -s "$1" ] || return 1
    [ "$(od -A n -t x1 -N 4 "$1" 2>/dev/null | tr -d ' \n')" = "d00dfeed" ]
}

# A plain file name in the boot partition's root, as U-Boot's ext4load
# /<name> needs it.
plain_name() {
    case "$1" in
        '' | */* | .* | *' '*) return 1 ;;
    esac
    return 0
}

writable_boot() {
    [ -w "${BOOT_DIR}" ] && return 0
    [ -n "${POS_BOOT_DIR:-}" ] && return 1
    mount -o remount,rw "${BOOT_DIR}" 2>/dev/null && [ -w "${BOOT_DIR}" ]
}

lcd_name="$(pointer "${BOOT_DIR}/lcd_dtb")"
hdmi_name="$(pointer "${BOOT_DIR}/hdmi_dtb")"

cmd_status() {
    forced="$(pointer "${FORCE}")"
    if [ -n "${forced}" ]; then
        if [ "${forced}" = "${hdmi_name}" ]; then
            say "next boot: hdmi (${forced}, from ${FORCE})"
        elif [ "${forced}" = "${lcd_name}" ]; then
            say "next boot: lcd (${forced}, from ${FORCE})"
        else
            say "next boot: ${forced} (from ${FORCE}; neither the lcd nor the hdmi tree)"
        fi
    else
        say "next boot: lcd (${lcd_name:-no /boot/lcd_dtb}; no ${FORCE})"
    fi
    say "lcd tree : ${lcd_name:-missing}"
    say "hdmi tree: ${hdmi_name:-missing}"
    if [ -r /proc/device-tree/model ] && [ -z "${POS_BOOT_DIR:-}" ]; then
        say "running  : $(tr -d '\0' < /proc/device-tree/model)"
    fi
    return 0
}

cmd_hdmi() {
    [ -n "${lcd_name}" ] || fail "${BOOT_DIR}/lcd_dtb is missing: is ${BOOT_DIR} the boot partition?"
    [ -n "${hdmi_name}" ] || fail "${BOOT_DIR}/hdmi_dtb is missing: this image has no HDMI device tree"
    plain_name "${hdmi_name}" || fail "${BOOT_DIR}/hdmi_dtb names '${hdmi_name}', not a file in ${BOOT_DIR}"
    [ "${hdmi_name}" != "${lcd_name}" ] ||
        fail "hdmi_dtb names the panel tree (${hdmi_name}): this image was built without an HDMI device tree"
    is_dtb "${BOOT_DIR}/${hdmi_name}" || fail "${BOOT_DIR}/${hdmi_name} is missing or not a device tree"
    is_dtb "${BOOT_DIR}/${lcd_name}" ||
        fail "${BOOT_DIR}/${lcd_name} (the panel tree) is missing or damaged; not switching away from it"
    writable_boot || fail "${BOOT_DIR} is not writable"
    printf '%s\n' "${hdmi_name}" > "${FORCE}.tmp" || fail "cannot write ${FORCE}.tmp"
    sync
    mv -f "${FORCE}.tmp" "${FORCE}" || { rm -f "${FORCE}.tmp"; fail "cannot write ${FORCE}"; }
    sync
    say "next boot: hdmi (${hdmi_name})"
    say "  The AMOLED is not driven in this mode. The HDMI tree is the vendor's CanMV-K230 v3"
    say "  description: no LoRa (spi0), no touch, no uart1, no camera sensor node."
    say "  Back to the panel: pos-display-boot lcd && reboot"
    return 0
}

cmd_lcd() {
    if [ ! -e "${FORCE}" ]; then
        say "next boot: lcd (${lcd_name:-no /boot/lcd_dtb}; nothing to change)"
        return 0
    fi
    writable_boot || fail "${BOOT_DIR} is not writable"
    rm -f "${FORCE}" || fail "cannot remove ${FORCE}"
    sync
    say "next boot: lcd (${lcd_name:-no /boot/lcd_dtb})"
    return 0
}

[ $# -eq 1 ] || { say "usage: pos-display-boot status | lcd | hdmi" >&2; exit 2; }
case "$1" in
    status) cmd_status ;;
    hdmi) cmd_hdmi ;;
    lcd) cmd_lcd ;;
    *) say "usage: pos-display-boot status | lcd | hdmi" >&2; exit 2 ;;
esac
