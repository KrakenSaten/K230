#!/bin/sh
# pos-hwcheck: first-boot hardware inventory for the LILYGO T-Display K230.
#
# Collects the facts needed to turn DOCUMENTED claims in
# docs/hardware/T-DISPLAY-K230.md into VERIFIED ones. Runs on BusyBox sh.
#
# Read-only by default: SAFE TO RUN WITH RADIOD ACTIVE.
# "--lora" performs an SX1262 register read, which drives the LoRa power and
# reset lines and talks on the SPI bus: REQUIRES RADIOD STOPPED (or radiod on
# the mock backend). The probe refuses to run while any of the four LoRa lines
# has a consumer or a radiod with the sx1262 backend is running, and it exits
# 3 in that case. It never leaves a gpioset process behind (every gpioset is
# a tracked child killed by the script, also on interruption) and every
# hardware step is bounded by a timeout.
#
# Usage: pos-hwcheck [--lora] [output_dir]
#        Output: <output_dir>/hwcheck-<timestamp>/ (default /root/hwcheck)
#        Exit: 0, or 3 when --lora was refused, 4 when its tools are missing
#        (gpioset, gpioinfo, pos-spixfer), 5 when the chip did not answer
#        (BUSY never low, or wrong registers).
#        POCKETOS_SX1262_SPI overrides the spidev node (default /dev/spidev0.0).

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

LORA_RC=0
if [ "$DO_LORA" -eq 1 ]; then
    section "LoRa SX1262 probe (REQUIRES RADIOD STOPPED)"
    # Pin map is DOCUMENTED from the LILYGO BSP and launcher source and was
    # VERIFIED on unit A on 2026-09-07:
    #   power enable GPIO44 -> gpiochip1 line 12, reset GPIO5 -> gpiochip0 line 5,
    #   BUSY GPIO19 -> gpiochip0 line 19, DIO1 GPIO20 -> gpiochip0 line 20.
    # Chip select is hardware-driven (iomux alt1 on IO14). SPI is mode 0.
    # Registers 0x0740/0x0741 hold the LoRa sync word, reset default 0x14 0x24
    # (SX1261/2 datasheet). ReadRegister opcode 0x1D, GetStatus 0xC0.
    #
    # The ReadRegister frame here is byte-for-byte what radiod's RadioLib
    # builds (SX126x SPIreadRegisterBurst -> SPItransferStream): opcode 0x1D,
    # address 0x07 0x40, then a status NOP and the data NOPs, sync word at
    # returned offset 4. On unit A the same frame read 14 24 through spi-pipe
    # at bringup (2026-09-07), 24 b4 through spi-pipe with status a2 on the
    # v0.0.4 and v0.0.5 retests (2026-09-08/09), and 14 24 with status aa
    # through pos-spixfer at 4 MHz, spi-pipe at 1 MHz and spi-pipe at 4 MHz
    # side by side on 2026-09-09 (docs/hardware/V0.0.6_M7_BENCH.md): the
    # value depends on the chip's state after reset, not on the frame or the
    # transport. Since v0.0.6 every command goes through pos-spixfer, which
    # performs the transaction exactly as radiod's HAL does: one CS-framed
    # SPI_IOC_MESSAGE(1) per command, mode 0, 8 bits, radiod's 4 MHz clock
    # set on the transfer itself, the node opened O_RDWR and flock'ed. That
    # is hardening (no spi-pipe dependency, radiod's parameters, loud
    # failure), not a fix for 24 b4. The returned window is captured at two
    # transfer lengths (6 and 8 bytes) and dumped, and when spi-pipe is
    # installed the 6-byte window is read once more through it, for
    # comparison only, so a recurrence shows both transports on the same
    # chip state.
    #
    # gpioset (libgpiod 2) holds its lines until it is killed; without -z it
    # used to wait forever for a terminal here. Every gpioset is therefore a
    # tracked background child with stdin from /dev/null, stopped by
    # lora_stop or by the EXIT/INT/TERM trap, so nothing survives the script.
    LORA_SPIDEV=${POCKETOS_SX1262_SPI:-/dev/spidev0.0}
    LORA_PIDS=""
    lora_cleanup() {
        for p in $LORA_PIDS; do kill "$p" 2>/dev/null; done
        for p in $LORA_PIDS; do wait "$p" 2>/dev/null; done
        LORA_PIDS=""
    }
    trap lora_cleanup EXIT INT TERM
    lora_hold() { # <chip> <line> <value>: drive a line until lora_stop $HOLD_PID
        gpioset --chip "$1" "$2=$3" </dev/null >>"$REPORT" 2>&1 &
        HOLD_PID=$!
        LORA_PIDS="$LORA_PIDS $HOLD_PID"
    }
    lora_stop() { kill "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
    lora_bounded() { # <tenths-of-a-second> <cmd...>: run, kill when the budget expires
        budget=$1; shift
        "$@" </dev/null &
        bp=$!
        LORA_PIDS="$LORA_PIDS $bp"
        n=0
        while kill -0 "$bp" 2>/dev/null && [ "$n" -lt "$budget" ]; do sleep 0.1; n=$((n + 1)); done
        kill "$bp" 2>/dev/null
        wait "$bp" 2>/dev/null
        return $?
    }
    lora_xfer() { # <tenths budget> <out.bin> <hex bytes...>: one CS-framed SPI_IOC_MESSAGE transaction (MOSI as given, MISO into the file)
        xb=$1; xo=$2; shift 2
        lora_bounded "$xb" sh -c "pos-spixfer $LORA_SPIDEV $* > '$xo' 2>>'$REPORT'"
    }
    lora_status_mode() { # GetStatus 0xC0: prints the chip-mode field (2 STDBY_RC, 3 STDBY_XOSC), or nothing
        lora_xfer 20 "$OUT/lora_ready.bin" C0 00
        st=$(od -An -tu1 "$OUT/lora_ready.bin" 2>/dev/null | awk 'NF {print $NF}')
        [ -n "$st" ] && echo $(( (st / 16) % 8 ))
    }
    lora_wait_ready() { # <max tenths of a second>: true once the chip is really out of reset
        # What radiod does after RST goes high (RadioLib SX126x::reset): it
        # never trusts one BUSY sample, it issues a command and requires an
        # accepted standby status, retrying for up to 1 s, "because SX126x
        # often refuses the first few commands after reset". Unit A, v0.0.4:
        # BUSY sampled 0 ms after the release still read the pre-reset low,
        # the probe read the registers mid-initialisation and got a transient
        # 24 b4 while GetStatus a moment later was a clean STDBY_RC. Ready is
        # therefore BUSY low AND a standby chip mode from GetStatus, seen on
        # two consecutive polls 100 ms apart, so at least one poll interval of
        # settle separates the first good status from the register read. No
        # SPI is touched while BUSY is active.
        n=0
        good=0
        READY_MS=0
        while [ "$n" -lt "$1" ]; do
            case "$(gpioget --chip gpiochip0 19 2>/dev/null)" in
                *inactive*)
                    mode=$(lora_status_mode)
                    if [ "$mode" = 2 ] || [ "$mode" = 3 ]; then
                        good=$((good + 1))
                        if [ "$good" -ge 2 ]; then READY_MS=$((n * 100)); READY_MODE=$mode; return 0; fi
                    else
                        good=0
                    fi
                    ;;
                *) good=0 ;;
            esac
            n=$((n + 1))
            sleep 0.1
        done
        return 1
    }
    lora_owners() { # consumers currently holding the four LoRa lines
        { gpioinfo --chip gpiochip0 2>/dev/null | grep -E 'line +(5|19|20):'
          gpioinfo --chip gpiochip1 2>/dev/null | grep -E 'line +12:'; } | grep -o 'consumer=[^ ]*' | sort -u | tr '\n' ' '
    }
    lora_radiod_sx1262() { # a radiod with the sx1262 backend is running
        for c in /proc/[0-9]*/cmdline; do
            tr '\0' ' ' < "$c" 2>/dev/null | grep -q 'radiod .*--backend sx1262' && return 0
        done
        return 1
    }
    owners=$(lora_owners)
    if [ -n "$owners" ] || lora_radiod_sx1262; then
        say "REFUSED: the SX1262 lines are in use (${owners:-radiod --backend sx1262 running})."
        say "This probe REQUIRES RADIOD STOPPED: /etc/init.d/S60radiod stop, or RADIOD_BACKEND=mock."
        say "The read-only inventory above is complete and unaffected."
        LORA_RC=3
    elif ! have gpioset || ! have gpioinfo || ! have pos-spixfer; then
        say "gpioset, gpioinfo or pos-spixfer missing; LoRa probe not run"
        LORA_RC=4
    elif [ ! -e "$LORA_SPIDEV" ]; then
        say "$LORA_SPIDEV missing; LoRa probe not run"
        LORA_RC=4
    else
        if [ -x /etc/init.d/S99zz_k230_phone_ui ] && [ -s /var/run/k230_phone_ui.pid ]; then
            say "stopping vendor launcher (restart with /etc/init.d/S99zz_k230_phone_ui start)"
            /etc/init.d/S99zz_k230_phone_ui stop >/dev/null 2>&1
            killall -q k230_phone_ui 2>/dev/null
            sleep 1
        fi
        say "power on (GPIO44 high, held for the probe)"
        lora_hold gpiochip1 12 1; PWR_PID=$HOLD_PID
        sleep 0.05
        # The same electrical lifecycle radiod uses (RadioLib SX126x::reset
        # through hal_linux, VERIFIED on unit A): RST driven low for at least
        # 1 ms (the datasheet asks for 100 us), then driven high and held for
        # the whole session, then every SPI command waits for BUSY low first.
        # v0.0.3 released RST after the low pulse and read 50 ms later; on
        # unit A the pad did not rest high, BUSY stayed active and the read
        # returned ff (M7, 2026-09-08). Nothing here relies on a pull-up.
        say "reset: GPIO5 driven low for 20 ms, then driven high and held"
        lora_hold gpiochip0 5 0; RST_PID=$HOLD_PID
        sleep 0.02
        lora_stop "$RST_PID"
        lora_hold gpiochip0 5 1; RST_PID=$HOLD_PID
        if lora_wait_ready 20; then
            say "SX126x ready ${READY_MS} ms after reset: BUSY (GPIO19) low, GetStatus chip mode ${READY_MODE} (standby), held over two polls; DIO1 (GPIO20): $(gpioget --chip gpiochip0 20 2>&1)"
            # ReadRegister across the sync word, the RadioLib frame exactly
            # (MOSI 1D 07 40 00 00 00), each window one CS-framed transaction.
            # 6 bytes is what radiod sends; 8 bytes clocks two more NOPs so the
            # sync word is still captured if the data/status boundary sits a
            # byte later than offset 4. Both windows are dumped as the raw evidence.
            say "ReadRegister 0x0740.. (opcode 1D, addr 07 40; sync word 14 24 expected after the status byte):"
            lora_xfer 50 "$OUT/lora_reg6.bin" 1D 07 40 00 00 00
            regs6=$(od -An -tx1 "$OUT/lora_reg6.bin" 2>/dev/null | tr -s ' \n' ' ')
            say "  6-byte window: $regs6"
            lora_xfer 50 "$OUT/lora_reg8.bin" 1D 07 40 00 00 00 00 00
            regs8=$(od -An -tx1 "$OUT/lora_reg8.bin" 2>/dev/null | tr -s ' \n' ' ')
            say "  8-byte window: $regs8"
            say "GetStatus 0xC0 (expect chip mode bits in byte 2):"
            lora_xfer 50 "$OUT/lora_status.bin" C0 00
            lora_status="$(od -An -tx1 "$OUT/lora_status.bin" 2>/dev/null | tr -s ' \n' ' ')"
            say "  $lora_status"
            if have spi-pipe; then
                # The v0.0.5 transport (spi-pipe, 1 MHz), same frame, read once
                # more for comparison only: it does not enter the verdict below.
                lora_bounded 50 sh -c "printf '\\035\\007\\100\\000\\000\\000' | spi-pipe -d $LORA_SPIDEV -m 0 -s 1000000 -b 6 > '$OUT/lora_reg6_spipipe.bin'"
                say "  6-byte window through spi-pipe, for comparison only: $(od -An -tx1 "$OUT/lora_reg6_spipipe.bin" 2>/dev/null | tr -s ' \n' ' ')"
            fi
            # The chip mode field of GetStatus (byte 2, bits 6:4): 2 STDBY_RC or
            # 3 STDBY_XOSC means the SX126x is answering on the bus. This is the
            # reliable liveness signal; the sync word is the stronger check when the
            # transaction delivers it.
            status_ok=0
            case "$lora_status" in *" 2"[0-9a-f]|*" 3"[0-9a-f]|*" 2"[0-9a-f]" "*|*" 3"[0-9a-f]" "*) status_ok=1 ;; esac
            case "$regs6 | $regs8" in
                *"14 24"*) say "sync word registers read 14 24: SX126x answers on $LORA_SPIDEV (VERIFIED)" ;;
                *)
                    say "sync word 14 24 not seen in either window (both read through pos-spixfer, one CS-framed"
                    say "  SPI_IOC_MESSAGE at radiod's 4 MHz, the same transaction radiod's HAL performs). On unit A"
                    say "  this read 24 b4 with status a2 (2026-09-08/09) and 14 24 with status aa through every"
                    say "  transport (2026-09-07, 2026-09-09): chip-state dependent, not a frame or transport effect;"
                    say "  the windows above, and the spi-pipe comparison line when present, are the evidence."
                    if [ "$status_ok" -eq 1 ]; then
                        say "GetStatus reports the chip in standby, so the SX126x is powered and answering on $LORA_SPIDEV."
                    else
                        say "UNEXPECTED: GetStatus did not report a standby chip mode either (check power, reset, wiring)."
                    fi
                    LORA_RC=5
                    ;;
            esac
        else
            say "UNEXPECTED: SX126x not ready 2000 ms after reset with RST driven high (BUSY never low with a standby status on two consecutive polls); no register read attempted"
            LORA_RC=5
        fi
        say "power off (releasing GPIO5, GPIO44 and every gpioset child)"
        lora_stop "$RST_PID"
        lora_stop "$PWR_PID"
        lora_cleanup
        left=$(ps 2>/dev/null | grep '[g]pioset' | grep -v grep)
        [ -z "$left" ] && say "no gpioset process left behind" || { say "WARNING: gpioset still running: $left"; }
    fi
fi

section "Done"
say "report: $REPORT"
exit $LORA_RC
