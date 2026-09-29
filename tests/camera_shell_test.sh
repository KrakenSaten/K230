#!/bin/bash
# Camera in the running app (tests/camera_app_test.c), and in the real shell:
# registered, opened from --open in both orientations on the fake backend,
# drawn with a live picture, quiet, and with no helper left when it closes.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell) and make's
# tests/pos-camera-testhooks and tools/camera/pos-camera.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

HOOKS=$(pwd)/tests/pos-camera-testhooks
HELPER=$(pwd)/tools/camera/pos-camera
if [ ! -x "$HOOKS" ] || [ ! -x "$HELPER" ]; then
    echo "FAIL build the helper first: make tools/camera/pos-camera tests/pos-camera-testhooks"
    exit 1
fi

BIN=${CAMERA_APP_TEST:-$(dirname "$SHELL_BIN")/camera_app_test}
if [ -x "$BIN" ]; then
    log=$(CAMERA_HELPER="$HOOKS" SDL_VIDEODRIVER=dummy timeout 300 "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|camera_app_test:|^     '
    check "Camera end to end, tapped, in portrait and landscape, and every fault" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    # LVGL reports a layout it cannot do as a warning and carries on, so a
    # clean result has to mean a clean log as well.
    printf '%s\n' "$log" | grep -E '^\[(Warn|Error)\]' | head -3
    check "and LVGL warned about nothing while it ran" \
        "$(printf '%s\n' "$log" | grep -qE '^\[(Warn|Error)\]' && echo 0 || echo 1)"
else
    echo "FAIL camera_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

# The picture in a screenshot: how much of the body is the fake's orange
# marker and its colour bars, rather than the panel's plain surface.
bars_seen() {
    python3 - "$@" <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
W, H, rows = read_png(sys.argv[1])
orange = sum(1 for r in rows for p in r if p[0] > 215 and 100 < p[1] < 160 and p[2] < 40)
yellow = sum(1 for r in rows for p in r if p[0] > 215 and p[1] > 215 and p[2] < 40)
print("1" if orange > 500 and yellow > 5000 else "0", orange, yellow)
PY
}
for o in portrait landscape; do
    RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
    SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
    POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
    POCKETOS_CAMERA_HELPER="$HELPER" POCKETOS_CAMERA_BACKEND=fake \
        timeout 60 "$SHELL_BIN" --rotation $o --open camera --screenshot "$LOGD/camera.png" \
        --exit-after-ms 2500 >"$LOGD/out" 2>&1
    rc=$?
    check "$o: the shell opens Camera and draws it" "$([ "$rc" = "0" ] && [ -s "$LOGD/camera.png" ] && echo 1 || echo 0)"
    check "$o: Camera reports itself open and closed" \
        "$(grep -qh 'open app camera' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
    check "$o: and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null &&
                                    echo 0 || echo 1)"
    hits=$(grep -hE ' WARN |\[Warn\]' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null | grep -v 'radiod unavailable')
    check "$o: and no warning but the simulator's missing radiod" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
    if [ -s "$LOGD/camera.png" ]; then
        set -- $(bars_seen "$LOGD/camera.png")
        check "$o: the live picture is on screen (marker $2 px, yellow bar $3 px)" "$1"
        [ -n "${CAMERA_SHOTS:-}" ] && cp "$LOGD/camera.png" "$CAMERA_SHOTS/camera-$o.png"
    fi
    check "$o: no helper outlives the shell" \
        "$(pgrep -f "$HELPER session" >/dev/null && echo 0 || echo 1)"
    check "$o: opening and closing takes no photo" "$([ -z "$(ls -A "$STATE/camera" 2>/dev/null)" ] && echo 1 || echo 0)"
    rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"
done

# Registered like any other app.
check "Camera is in the shell's registry" \
    "$(grep -q '&app_camera' ui/shell/shell.c && echo 1 || echo 0)"
check "on the launcher, under DEVICE, in the tools colour" \
    "$(grep -q '{ "camera", HOME_GROUP_DEVICE, HOME_HUE_TOOLS[ ,}]' ui/shell/home_layout.c && echo 1 || echo 0)"
check "with its portal icon and its mask" \
    "$([ -f ui/assets/doors/icon-camera.bin ] && grep -q 'pos_app_icon_camera = {' ui/pocketui/pos_app_icons.c &&
       echo 1 || echo 0)"
check "fullscreen (DS §30.8)" \
    "$(grep -q '.chrome = POCKETOS_CHROME_NONE' apps/camera/camera_app.c && echo 1 || echo 0)"

echo "camera_shell_test: $failed failure(s)"
exit $((failed > 0))
