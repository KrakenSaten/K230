#!/bin/bash
# An external display (HDMI) in the running shell (ui/shell/shell_display.h,
# DS §38.10): the simulator stands one in with POCKETOS_EXTERNAL_DISPLAY, and
# the keyboard base with POCKETOS_TEST_KEYBOARD_FILE (auto_rotation_shell_test).
#
#   1. The monitor runs at rotation 0 in its own size, landscape, with no
#      corner insets; the unit's hold is still the mode's and the keyboard's.
#   2. Mating or unmating the keyboard changes the hold and never restarts
#      the shell to turn the monitor; --rotation portrait does not turn it.
#   3. "0" keeps the panel; a value that is none of 0, 1 or WxH is refused
#      with a warning and the panel stays.
#   4. Vision on the monitor with keys only (shell.key, the keyboard base's
#      own path): the first arrow shows the ring, Enter opens the mode
#      picker over the picture, Esc closes it.
#
# Requires: SHELL_BIN (the CMake-built simulator with the test hooks), pos
# and tools/vision/pos-vision (make all). Run from the repository root.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
cd "$(dirname "$0")/.." || exit 1
export SDL_VIDEODRIVER=dummy
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

KBD=$OUT/keyboard
say() { printf '%s\n' "$1" > "$KBD"; }
export POCKETOS_TEST_KEYBOARD_FILE=$KBD
HELPER=$(pwd)/tools/vision/pos-vision

fresh() {
    export POCKETOS_RUNTIME_DIR=$(mktemp -d -p "$OUT") POCKETOS_LOG_DIR=$(mktemp -d -p "$OUT")
    export POCKETOS_CONFIG_DIR=$(mktemp -d -p "$OUT") POCKETOS_STATE_DIR=$(mktemp -d -p "$OUT")
}
info() { "$POS" shell info 2>/dev/null | tr -d ' \t\n'; }
field() { info | grep -oE "\"$1\":[^,}]*" | head -1 | cut -d: -f2 | tr -d '"'; }
size() { info | grep -oE '"width":[0-9]+,"height":[0-9]+' | head -1; }
start_shell() { # [args]
    "$SHELL_BIN" --no-lock "$@" >"$POCKETOS_LOG_DIR/run.log" 2>&1 &
    SP=$!
    for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
    sleep 0.4
}
stop_shell() { kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null; }
restarts() { grep -c 'restarting in place' "$POCKETOS_LOG_DIR/run.log" 2>/dev/null; true; }
yellow_px() {
    python3 - "$1" <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
W, H, rows = read_png(sys.argv[1])
print(sum(1 for r in rows for p in r if p[0] > 215 and p[1] > 215 and p[2] < 40))
PY
}

# ---- 1. the monitor, the keyboard attached ---------------------------------------
fresh; say present
POCKETOS_EXTERNAL_DISPLAY=1920x1080 start_shell
check "external: 1920x1080 at rotation 0 ($(size), rotation $(field rotation))" \
    "$([ "$(size)" = '"width":1920,"height":1080' ] && [ "$(field rotation)" = 0 ] && echo 1 || echo 0)"
check "external: landscape, and it says it is external" \
    "$([ "$(field orientation)" = landscape ] && [ "$(field external)" = true ] && echo 1 || echo 0)"
check "external: the unit on its keyboard is held at 270 ($(field hold_rotation))" \
    "$([ "$(field hold_rotation)" = 270 ] && echo 1 || echo 0)"
check "external: no corner insets" \
    "$(grep -q 'display: 1920x1080 at rotation 0, corners 0,0,0,0' "$POCKETOS_LOG_DIR/run.log" && echo 1 || echo 0)"
check "external: logged as such" \
    "$(grep -q 'display: external display (HDMI), rotation 0' "$POCKETOS_LOG_DIR/run.log" && echo 1 || echo 0)"

# ---- 2. the keyboard comes and goes; the monitor never turns ------------------------
say absent; sleep 4.5
check "unmated: the hold goes to portrait ($(field hold_rotation)), the monitor stays at 0" \
    "$([ "$(field hold_rotation)" = 0 ] && [ "$(field rotation)" = 0 ] && echo 1 || echo 0)"
say present; sleep 4.5
check "mated again: the hold is 270 again" "$([ "$(field hold_rotation)" = 270 ] && echo 1 || echo 0)"
check "and the shell never restarted to turn ($(restarts) restarts)" "$([ "$(restarts)" = 0 ] && echo 1 || echo 0)"
check "nothing waits to be applied" "$([ "$(field applying)" = false ] && echo 1 || echo 0)"
stop_shell

fresh; say absent
POCKETOS_EXTERNAL_DISPLAY=1 start_shell --rotation landscape
check "external, mode landscape and no keyboard: rotation 0, held at 270" \
    "$([ "$(size)" = '"width":1920,"height":1080' ] && [ "$(field rotation)" = 0 ] &&
       [ "$(field hold_rotation)" = 270 ] && echo 1 || echo 0)"
stop_shell
fresh; say present
POCKETOS_EXTERNAL_DISPLAY=1280x720 start_shell --rotation portrait
check "external 1280x720, mode portrait: the monitor at 0 in its size, held at 0" \
    "$([ "$(size)" = '"width":1280,"height":720' ] && [ "$(field rotation)" = 0 ] &&
       [ "$(field hold_rotation)" = 0 ] && [ "$(field orientation)" = landscape ] && echo 1 || echo 0)"
stop_shell

# ---- 3. the panel ------------------------------------------------------------
fresh; say present
POCKETOS_EXTERNAL_DISPLAY=0 start_shell
check "0: the panel, turned by the keyboard as ever ($(size), rotation $(field rotation))" \
    "$([ "$(size)" = '"width":1232,"height":568' ] && [ "$(field rotation)" = 270 ] &&
       [ "$(field external)" = false ] && [ "$(field hold_rotation)" = 270 ] && echo 1 || echo 0)"
stop_shell
fresh; say present
POCKETOS_EXTERNAL_DISPLAY=banana start_shell
check "a value that is none of 0, 1 or WxH: refused, the panel stays" \
    "$(grep -q 'POCKETOS_EXTERNAL_DISPLAY=banana is not 0, 1 or WxH; ignored' "$POCKETOS_LOG_DIR/run.log" &&
       [ "$(field external)" = false ] && echo 1 || echo 0)"
stop_shell

# ---- 4. Vision on the monitor, keys only ------------------------------------------
if [ -x "$HELPER" ]; then
    fresh; say present
    POCKETOS_EXTERNAL_DISPLAY=1920x1080 POCKETOS_VISION_HELPER="$HELPER" POCKETOS_CAMERA_BACKEND=fake \
        POCKETOS_CAMERA_FAKE=period=20 POCKETOS_VISION_KPU_SCRIPT="box=2:800:100:120:120:80:3:0" \
        start_shell --open vision
    key() { "$POS" call shell shell.key code="$1" >/dev/null 2>&1; sleep 0.4; }
    shot() { "$POS" call shell shell.screenshot path="$1" >/dev/null 2>&1; sleep 0.3; }
    sleep 2.5
    shot "$OUT/live.png"
    key 22  # Up: the ring, nothing pressed
    shot "$OUT/ring.png"
    key 21  # Enter: MODE
    shot "$OUT/picker.png"
    key 40  # Esc
    shot "$OUT/closed.png"
    live=$(yellow_px "$OUT/live.png"); ring=$(yellow_px "$OUT/ring.png")
    picker=$(yellow_px "$OUT/picker.png"); closed=$(yellow_px "$OUT/closed.png")
    check "Vision on the monitor draws its picture ($live yellow px)" "$([ "${live:-0}" -gt 2000 ] && echo 1 || echo 0)"
    check "the first key shows the ring and opens nothing ($ring)" "$([ "${ring:-0}" -gt 2000 ] && echo 1 || echo 0)"
    check "Enter on MODE opens the picker over the picture ($picker)" \
        "$([ "${picker:-1}" -lt $(( ${live:-0} / 10 )) ] && echo 1 || echo 0)"
    check "Esc closes it ($closed)" "$([ "${closed:-0}" -gt 2000 ] && echo 1 || echo 0)"
    check "the keys reached the stream ($(info | grep -oE '"delivered":[0-9]+'))" \
        "$(info | grep -qE '"delivered":[1-9]' && echo 1 || echo 0)"
    check "Vision logged no fault" \
        "$(grep -qE ' ERROR |assert|too small' "$POCKETOS_LOG_DIR/run.log" "$POCKETOS_LOG_DIR/shell.log" 2>/dev/null &&
           echo 0 || echo 1)"
    [ -n "${SHOTS_DIR:-}" ] && cp "$OUT"/*.png "$SHOTS_DIR"/
    stop_shell
else
    echo "FAIL build the helper first: make tools/vision/pos-vision"
    failed=$((failed + 1))
fi

echo "external_display_shell_test: $failed failure(s)"
[ "$failed" = 0 ]
