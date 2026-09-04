#!/bin/sh
# pos-hwcheck: first-boot hardware inventory for the LILYGO T-Display K230.
#
# Collects the facts needed to turn DOCUMENTED claims in
# docs/hardware/T-DISPLAY-K230.md into VERIFIED ones. Runs on BusyBox sh.
# Read-only by default. "--lora" performs an SX1262 register read, which
# needs the vendor launcher stopped and drives the LoRa power/reset lines.
#
# Usage: pos-hwcheck [--lora] [output_dir]
#        Output: <output_dir>/hwcheck-<timestamp>/ (default /root/hwcheck)

set -u

DO_LORA=0
OUT_BASE=/root/hwcheck
for a in "$@"; do
    case "$a" in
        --lora) DO_LORA=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) OUT_BASE="$a" ;;
    esac
done

TS=$(date +%Y%m%d_%H%M%S)
OUT="$OUT_BASE/hwcheck-$TS"
mkdir -p "$OUT" || { echo "cannot create $OUT"; exit 1; }
REPORT="$OUT/report.txt"

say() { printf '%s\n' "$*" | tee -a "$REPORT"; }
section() { say ""; say "=== $*"; }
run() {
    # run <label> <command...>: capture stdout+stderr into the report.
    label=$1; shift
    say "--- $label: $*"
    "$@" >>"$REPORT" 2>&1 || say "(exit $?)"
}
have() { command -v "$1" >/dev/null 2>&1; }

say "pos-hwcheck $TS"
say "output: $OUT"

section "System"
run model cat /proc/device-tree/model
run kernel uname -a
run cmdline cat /proc/cmdline
run uptime cat /proc/uptime
run cpuinfo cat /proc/cpuinfo
run meminfo head -5 /proc/meminfo
run version cat /etc/version/release_version
run pocketos cat /etc/pocketos-release
run os-release cat /etc/os-release

section "Storage"
run partitions cat /proc/partitions
run mounts cat /proc/mounts
run df df -h

section "Device nodes"
run dev ls -la /dev/spidev* /dev/gpiochip* /dev/i2c-* /dev/ttyS* /dev/video* /dev/dri /dev/input /dev/mmcblk* /dev/rtc* /dev/watchdog*

section "GPIO"
if have gpiodetect; then
    run gpiodetect gpiodetect
    run gpioinfo gpioinfo
else
    run gpiochips ls -la /sys/class/gpio /sys/bus/gpio/devices
fi

section "I2C"
run i2c-buses ls -la /sys/bus/i2c/devices
for b in /sys/bus/i2c/devices/i2c-*; do
    [ -e "$b/name" ] && say "$(basename "$b"): $(cat "$b/name")"
done
if have i2cdetect; then
    for n in /dev/i2c-*; do
        run "i2cdetect $n" i2cdetect -y -r "${n#/dev/i2c-}"
    done
else
    say "(i2cdetect not installed; vendor image has none)"
fi

section "SPI"
run spi-devices ls -la /sys/bus/spi/devices
for s in /sys/bus/spi/devices/*; do
    [ -e "$s/modalias" ] && say "$(basename "$s"): $(cat "$s/modalias")"
done

section "Input"
for i in /sys/class/input/event*; do
    [ -e "$i/device/name" ] && say "$(basename "$i"): $(cat "$i/device/name")"
done

section "Display"
run drm ls -la /sys/class/drm
for c in /sys/class/drm/card*-*; do
    [ -e "$c/status" ] && say "$(basename "$c"): $(cat "$c/status") $(cat "$c/modes" 2>/dev/null | head -1)"
done
run backlight ls -la /sys/class/backlight
run fb cat /sys/class/graphics/fb0/virtual_size

section "Video / camera"
run v4l ls -la /sys/class/video4linux
for v in /sys/class/video4linux/*; do
    [ -e "$v/name" ] && say "$(basename "$v"): $(cat "$v/name")"
done

section "Network"
run interfaces ls /sys/class/net
run ip-addr ip addr
run wifi-modules lsmod
run sdio ls -la /sys/bus/sdio/devices
for d in /sys/bus/sdio/devices/*; do
    [ -e "$d/vendor" ] && say "$(basename "$d"): vendor $(cat "$d/vendor") device $(cat "$d/device")"
done
if have iw; then run iw-dev iw dev; fi
run usb-net ls -la /sys/class/net/*/device/driver 2>/dev/null

section "USB"
if have lsusb; then run lsusb lsusb; fi
for u in /sys/bus/usb/devices/*; do
    if [ -e "$u/idVendor" ]; then
        say "$(basename "$u"): $(cat "$u/idVendor"):$(cat "$u/idProduct") $(cat "$u/manufacturer" 2>/dev/null) $(cat "$u/product" 2>/dev/null)"
    fi
done

section "Bluetooth"
run bt ls -la /sys/class/bluetooth
if have hciconfig; then run hciconfig hciconfig -a; fi

section "Audio"
run alsa-cards cat /proc/asound/cards
run alsa-pcm cat /proc/asound/pcm

section "Thermal / power"
for t in /sys/class/thermal/thermal_zone*; do
    [ -e "$t/temp" ] && say "$(basename "$t") $(cat "$t/type"): $(cat "$t/temp") (milli-degC)"
done
run power-supply ls -la /sys/class/power_supply
for p in /sys/class/power_supply/*; do
    [ -e "$p/uevent" ] && run "$(basename "$p")" cat "$p/uevent"
done

section "Processes and services"
run init-scripts ls /etc/init.d
run ps ps

section "Logs"
dmesg > "$OUT/dmesg.txt" 2>&1 && say "dmesg saved to $OUT/dmesg.txt ($(wc -l < "$OUT/dmesg.txt") lines)"
[ -f /var/log/k230_phone_ui.log ] && cp /var/log/k230_phone_ui.log "$OUT/" && say "copied /var/log/k230_phone_ui.log"

if [ "$DO_LORA" -eq 1 ]; then
    section "LoRa SX1262 probe (spidev0.0)"
    # Pin map is DOCUMENTED from the LILYGO BSP and launcher source:
    #   power enable GPIO44 -> gpiochip1 line 12, reset GPIO5 -> gpiochip0 line 5,
    #   BUSY GPIO19 -> gpiochip0 line 19, DIO1 GPIO20 -> gpiochip0 line 20.
    # Chip select is hardware-driven (iomux alt1 on IO14). SPI is mode 0, 4 MHz.
    # Register 0x0740/0x0741 is the LoRa sync word, reset default 0x14 0x24
    # (SX1261/2 datasheet register table). ReadRegister opcode is 0x1D.
    if ! have gpioset || ! have spi-pipe; then
        say "gpioset or spi-pipe missing; skipping LoRa probe"
    elif [ ! -e /dev/spidev0.0 ]; then
        say "/dev/spidev0.0 missing; skipping LoRa probe"
    else
        if [ -x /etc/init.d/S99zz_k230_phone_ui ]; then
            say "stopping vendor launcher (restart with /etc/init.d/S99zz_k230_phone_ui start)"
            /etc/init.d/S99zz_k230_phone_ui stop >/dev/null 2>&1
            killall -q k230_phone_ui 2>/dev/null
            sleep 1
        fi
        say "power on (GPIO44 high)"
        gpioset -z --chip gpiochip1 12=1 2>>"$REPORT"
        sleep 0.05
        say "reset pulse (GPIO5 low then high)"
        gpioset --chip gpiochip0 --hold-period 5ms 5=0 2>>"$REPORT"
        gpioset -z --chip gpiochip0 5=1 2>>"$REPORT"
        sleep 0.05
        say "BUSY (GPIO19): $(gpioget --chip gpiochip0 19 2>&1)"
        say "DIO1 (GPIO20): $(gpioget --chip gpiochip0 20 2>&1)"
        say "ReadRegister 0x0740..0x0741 (expect .. .. .. .. 14 24):"
        printf '\035\007\100\000\000\000' | spi-pipe -d /dev/spidev0.0 -m 0 -s 1000000 -b 6 2>>"$REPORT" | od -An -tx1 | tee -a "$REPORT"
        say "GetStatus 0xC0 (expect chip mode bits in byte 2):"
        printf '\300\000' | spi-pipe -d /dev/spidev0.0 -m 0 -s 1000000 -b 2 2>>"$REPORT" | od -An -tx1 | tee -a "$REPORT"
        say "releasing GPIO daemons"
        killall -q gpioset 2>/dev/null
    fi
fi

section "Done"
say "report: $REPORT"
