#!/bin/sh
# Which physical control is which: a bench watcher, run ON THE DEVICE while
# the owner presses a control (docs/hardware/HARDWARE_CONTROLS.md §3).
#
# It watches every source a button on this board is known or documented to
# report through, and prints one line per event with its source:
#
#   pmu-pwrkey   /dev/input/event0, the PMU power key (KEY_POWER, 116).
#                Read only: the kernel's own 5 s hold-to-power-off is not
#                touched, and nothing here acts on a press.
#   boot0        GPIO0, the BOOT0 button (vendor pin map: idle high, pressed
#                low). Its pad is not a GPIO on the Doors image (iomux word
#                0xAC4), so with --boot0 the pad is switched to GPIO input
#                with pull-up (0x344, the vendor launcher's own value) for the
#                run and put back exactly as found on exit.
#   keyboard     the shell's log: every hardware action and every press at a
#                matrix position no table names (ui/shell/shell_kbd.c).
#
# usage: sh hw_buttons_watch.sh [seconds] [--boot0]     (default 60 s)
#
# Nothing is remapped, nothing is written except the BOOT0 pad with --boot0,
# and RESET is not reachable from software at all.
set -u
SECS=60
BOOT0=0
for a in "$@"; do
    case "$a" in
        --boot0) BOOT0=1 ;;
        *[!0-9]*) echo "usage: $0 [seconds] [--boot0]" >&2; exit 2 ;;
        *) SECS=$a ;;
    esac
done
LOG=${POCKETOS_LOG_DIR:-/var/lib/pocketos/log}/shell.log
IOMUX_IO0=0x91105000
PIDS=""
SAVED_IO0=""

cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    if [ -n "$SAVED_IO0" ]; then
        devmem $IOMUX_IO0 32 "$SAVED_IO0"
        echo "boot0: pad restored to $SAVED_IO0 ($(devmem $IOMUX_IO0 32))"
    fi
}
trap cleanup EXIT INT TERM

stamp() { date +%H:%M:%S; }

echo "watching for ${SECS}s: pmu-pwrkey, keyboard$( [ $BOOT0 = 1 ] && echo ', boot0')"

# The power key, as the kernel reports it. struct input_event on riscv64:
# 16 bytes of time, u16 type, u16 code, s32 value.
python3 -u - <<'EOF' &
import os, struct, time
fd = os.open('/dev/input/event0', os.O_RDONLY)
while True:
    b = os.read(fd, 24)
    if len(b) < 24:
        continue
    _, _, typ, code, val = struct.unpack('qqHHi', b)
    if typ == 1:
        print('%s pmu-pwrkey code=%d %s' % (time.strftime('%H:%M:%S'), code,
              {0: 'release', 1: 'press', 2: 'repeat'}.get(val, str(val))), flush=True)
EOF
PIDS="$PIDS $!"

if [ $BOOT0 = 1 ]; then
    SAVED_IO0=$(devmem $IOMUX_IO0 32)
    devmem $IOMUX_IO0 32 0x344
    echo "boot0: pad $SAVED_IO0 -> $(devmem $IOMUX_IO0 32), level now $(gpioget -c gpiochip0 0 2>&1)"
    gpiomon -c gpiochip0 -F "%U boot0 %E" 0 2>&1 &
    PIDS="$PIDS $!"
fi

# BusyBox grep has no --line-buffered, so the filter is the loop itself.
tail -n 0 -f "$LOG" 2>/dev/null | while read -r line; do
    case "$line" in
        *" action "*|*"unnamed matrix code"*) echo "$(stamp) keyboard ${line#* shell }" ;;
    esac
done &
PIDS="$PIDS $!"

sleep "$SECS"
echo "done"
