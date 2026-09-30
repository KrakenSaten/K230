#!/bin/bash
# The keyboard base's own keys in the running shell (docs/hardware/
# HARDWARE_CONTROLS.md): raw controller events injected through shell.key take
# the physical key's own path - the key map, the driver's action queue, the
# shell's action host - and shell.action runs an action by name. Checked here:
# every default mapping reaches the right app through the normal launch path,
# one instance per app, Back as the back slab and the launcher's Esc, the lock,
# the level bounds and their persistence (volume, brightness, keyboard light
# against a fake PWM), F7's screenshot, and the microphone and camera activity
# that drive the privacy LEDs, from a fake /proc and /sys.
#
# Requires: SHELL_BIN (CMake-built pocketos-shell, SDL) and `make all` (pos).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
is() { [ "$1" = "$2" ] && echo 1 || echo 0; }

export SDL_VIDEODRIVER=dummy
TMP=$(mktemp -d)
SP=""
cleanup() { [ -n "$SP" ] && kill "$SP" 2>/dev/null; wait 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

# A board as the shell sees it: the panel's backlight, the keyboard light's
# PWM (pwmchip3 = pwm3_5, channel 1 exported), the camera's capture nodes,
# and the sound card's capture substream.
SYS=$TMP/sys
PROC=$TMP/proc
mkdir -p "$SYS/class/backlight/rm69a10" "$SYS/class/pwm/pwmchip3/device/of_node" "$SYS/class/pwm/pwmchip3/pwm1"
printf '255\n' > "$SYS/class/backlight/rm69a10/max_brightness"
printf '254\n' > "$SYS/class/backlight/rm69a10/brightness"
printf 'pwm3_5' > "$SYS/class/pwm/pwmchip3/device/of_node/name"
for a in enable period duty_cycle polarity export; do : > "$SYS/class/pwm/pwmchip3/pwm1/$a"; done
: > "$SYS/class/pwm/pwmchip3/export"
for v in video0:mvx video1:vvcam-video.0.0 video2:vvcam-video.0.1; do
    mkdir -p "$SYS/class/video4linux/${v%%:*}"
    printf '%s\n' "${v#*:}" > "$SYS/class/video4linux/${v%%:*}/name"
done
mkdir -p "$PROC/asound/card0/pcm0c/sub0" "$PROC/1/fd"
printf 'closed\n' > "$PROC/asound/card0/pcm0c/sub0/status"
ln -s /dev/null "$PROC/1/fd/0"

export POCKETOS_RUNTIME_DIR=$TMP/run POCKETOS_LOG_DIR=$TMP/log POCKETOS_STATE_DIR=$TMP/state
export POCKETOS_CONFIG_DIR=$TMP/cfg POCKETOS_TEST_SYSFS_ROOT=$SYS POCKETOS_TEST_PROC_ROOT=$PROC
mkdir -p "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_STATE_DIR" "$POCKETOS_CONFIG_DIR"
printf 'audio_volume=90\n' > "$POCKETOS_CONFIG_DIR/settings.conf"

"$SHELL_BIN" --no-lock >"$POCKETOS_LOG_DIR/out" 2>&1 & SP=$!
for _ in $(seq 1 80); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
check "the shell starts" "$([ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && echo 1 || echo 0)"

LOG="$POCKETOS_LOG_DIR/shell.log"
# One field of a pos JSON answer, by a dotted path.
field() { # <json> <path>
    printf '%s' "$1" | python3 -c '
import json, sys
try:
    v = json.load(sys.stdin)
    for k in sys.argv[1].split("."):
        v = v[k]
    print(json.dumps(v) if isinstance(v, bool) else v)
except Exception:
    print("?")' "$2"
}
key() { "$POS" call shell shell.key code="$1" 2>&1; sleep 0.3; }
act() { "$POS" call shell shell.action action="$1" 2>&1; }
cur() { field "$("$POS" call shell shell.info 2>&1)" current; }
hw() { field "$("$POS" call shell shell.info 2>&1)" "hardware.$1"; }
opens() { grep -c "open app $1\$" "$LOG"; }

# ---- apps, one instance each -------------------------------------------------
key 64 >/dev/null
check "F8 (code 64) opens Terminal" "$(is "$(cur)" terminal)"
key 64 >/dev/null
key 8 >/dev/null
check "F8 again and LILYGO (code 8): still the one Terminal" "$(is "$(cur)" terminal)"
check "opened exactly once" "$(is "$(opens terminal)" 1)"
check "the repeats are logged as no-ops" "$(is "$(grep -c 'action terminal from keyboard: noop' "$LOG")" 2)"
key 11 >/dev/null
check "the microphone key (code 11) opens Wave" "$(is "$(cur)" wave)"
out=$(act vision)
check "shell.action vision opens Vision" "$(is "$(field "$out" current)" vision)"
out=$(act vision)
check "Vision again: no second instance" "$(is "$(field "$out" result)" noop)"
check "Vision opened once" "$(is "$(opens vision)" 1)"
key 63 >/dev/null
check "F9 (code 63) opens RIFT" "$(is "$(cur)" rift)"
key 60 >/dev/null
check "F2 (code 60) opens Settings" "$(is "$(cur)" settings)"
key 50 >/dev/null
check "F1 (code 50) goes home" "$(is "$(cur)" home)"
key 8 >/dev/null
check "LILYGO from home opens Terminal" "$(is "$(cur)" terminal)"

# ---- Back ---------------------------------------------------------------------
out=$(act back)
check "Back in the Terminal goes home (the back slab)" "$(is "$(field "$out" current)" home)"
check "and closed it the ordinary way" "$(grep -q 'close app terminal' "$LOG" && echo 1 || echo 0)"
"$POS" call shell shell.folder id=games >/dev/null 2>&1
out=$(act back)
check "Back in a launcher folder closes it" \
    "$(is "$(field "$("$POS" call shell shell.info 2>&1)" launcher.folder)" None)"
out=$(act back)
check "Back at the launcher's page: nothing to do" "$(is "$(field "$out" result)" noop)"
"$POS" call shell shell.controls >/dev/null 2>&1
out=$(act back)
check "Back closes Controls" "$(is "$(field "$out" result)" done)"
out=$(act back)
check "and the next Back has nothing left to close" "$(is "$(field "$out" result)" noop)"
act rift >/dev/null
out=$(act back)
check "Back in RIFT at its top level goes home" "$(is "$(field "$out" current)" home)"

# ---- the lock ------------------------------------------------------------------
"$POS" call shell shell.lock >/dev/null 2>&1
out=$(key 64)
check "locked: F8 opens nothing" "$(is "$(cur)" home)"
check "and says it was refused" "$(grep -q 'action terminal from keyboard: refused' "$LOG" && echo 1 || echo 0)"
out=$(act back)
check "locked: Back refused" "$(is "$(field "$out" result)" refused)"
out=$(act volume_down)
check "locked: the volume still steps" "$(is "$(field "$out" result)" done)"
"$POS" call shell shell.unlock >/dev/null 2>&1

# ---- levels --------------------------------------------------------------------
"$POS" shell volume 90 >/dev/null 2>&1
key 66 >/dev/null
check "F6 (code 66) raises the volume to 100" "$(grep -qx 'audio_volume=100' "$POCKETOS_CONFIG_DIR/settings.conf" && echo 1 || echo 0)"
out=$(act volume_up)
check "at 100: no-op, no wrap" "$(is "$(field "$out" result)/$(field "$out" value)" noop/100)"
for _ in $(seq 1 12); do key 67 >/dev/null; done
check "F5 x12 stops at 10" "$(grep -qx 'audio_volume=10' "$POCKETOS_CONFIG_DIR/settings.conf" && echo 1 || echo 0)"
"$POS" shell volume 70 >/dev/null 2>&1

bright() { field "$("$POS" call shell shell.brightness 2>&1)" percent; }
key 62 >/dev/null
check "F10 (code 62) dims the panel to 90 %" "$(is "$(bright)" 90)"
key 61 >/dev/null
key 61 >/dev/null
check "F11 (code 61) x2 stops at 100 %" "$(is "$(bright)" 100)"
check "and the level is persisted" "$(grep -qx 'display_brightness=100' "$POCKETOS_CONFIG_DIR/settings.conf" && echo 1 || echo 0)"

check "the keyboard light starts dark (nothing stored)" "$(is "$(hw keyboard_light)" 0)"
out=$(act keyboard_light_down)
check "F3 at off: no-op, no wrap" "$(is "$(field "$out" result)" noop)"
key 68 >/dev/null
check "F4 (code 68) lights it to 10 %" "$(is "$(hw keyboard_light)" 10)"
check "10 % is duty 18000 of 20000, inverted, enabled" \
    "$(is "$(cat "$SYS/class/pwm/pwmchip3/pwm1/duty_cycle")/$(cat "$SYS/class/pwm/pwmchip3/pwm1/period")/$(cat "$SYS/class/pwm/pwmchip3/pwm1/polarity")/$(cat "$SYS/class/pwm/pwmchip3/pwm1/enable")" 18000/20000/inversed/1)"
check "and it is persisted" "$(grep -qx 'keyboard_backlight=10' "$POCKETOS_CONFIG_DIR/settings.conf" && echo 1 || echo 0)"
for _ in $(seq 1 12); do key 68 >/dev/null; done
check "F4 x12 stops at 100" "$(is "$(hw keyboard_light)" 100)"
key 59 >/dev/null
check "F3 (code 59) dims it to 90" "$(is "$(hw keyboard_light)" 90)"

# ---- F7 ---------------------------------------------------------------------------
key 65 >/dev/null
sleep 0.5
check "F7 (code 65) writes a screenshot" \
    "$(ls "$POCKETOS_STATE_DIR"/screenshots/screenshot-*.png >/dev/null 2>&1 && echo 1 || echo 0)"
check "and counts it" "$(is "$(hw screenshot.saved)" 1)"

# ---- typing is not an action ------------------------------------------------------
before=$(grep -c ' action ' "$LOG")
key 29 >/dev/null
key 7 >/dev/null
key 18 >/dev/null
check "A, Shift and Z hand on no action" "$(is "$(grep -c ' action ' "$LOG")" "$before")"
check "and change no screen" "$(is "$(cur)" home)"

# ---- the privacy LEDs' sources ------------------------------------------------------
check "nothing listening at the start" "$(is "$(hw microphone)" false)"
printf 'state: RUNNING\n' > "$PROC/asound/card0/pcm0c/sub0/status"
sleep 0.8
check "a capture stream open: microphone in use" "$(is "$(hw microphone)" true)"
check "logged once" "$(is "$(grep -c 'microphone in use' "$LOG")" 1)"
printf 'closed\n' > "$PROC/asound/card0/pcm0c/sub0/status"
sleep 0.8
check "closed: released" "$(is "$(hw microphone)" false)"
mkdir -p "$PROC/4242/fd"
ln -s /dev/video2 "$PROC/4242/fd/5"
sleep 1.5
check "a process holding video2: camera in use" "$(is "$(hw camera)" true)"
rm -rf "$PROC/4242"
sleep 1.5
check "the process gone (a crashed helper): released" "$(is "$(hw camera)" false)"
mkdir -p "$PROC/4343/fd"
ln -s /dev/video0 "$PROC/4343/fd/5"
sleep 1.5
check "the VPU (video0) is not the camera" "$(is "$(hw camera)" false)"

# ---- refusals -------------------------------------------------------------------------
out=$("$POS" call shell shell.action action=reboot 2>&1)
check "an unknown action is error 2" "$(printf '%s' "$out" | grep -q 'code 2' && echo 1 || echo 0)"
out=$("$POS" call shell shell.key code=0 2>&1)
check "code 0 is error 2" "$(printf '%s' "$out" | grep -q 'code 2' && echo 1 || echo 0)"
out=$("$POS" call shell shell.key code=200 2>&1)
check "code 200 is error 2" "$(printf '%s' "$out" | grep -q 'code 2' && echo 1 || echo 0)"
check "the shell survived all of it" "$(kill -0 "$SP" 2>/dev/null && echo 1 || echo 0)"
check "no error in its log" "$(grep -c ' ERROR ' "$LOG" | grep -qx 0 && echo 1 || echo 0)"

kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null; SP=""

echo "hw_actions_shell_test.sh: $failed failure(s)"
exit $((failed > 0))
