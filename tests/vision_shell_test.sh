#!/bin/bash
# Vision in the real shell (docs/apps/VISION.md): opened from --open in both
# orientations on the fake camera and the fake detector, drawn with a live
# picture, quiet, and with no helper left when it closes; the mode picker and
# Traffic's setup drawn over the picture in both orientations; the stored
# settings obeyed (a stored mode opens in it, a damaged value falls back) and
# never rewritten by merely opening.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell) and make's
# tools/vision/pos-vision (the host build: fake camera, fake detector).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

HELPER=$(pwd)/tools/vision/pos-vision
if [ ! -x "$HELPER" ]; then
    echo "FAIL build the helper first: make tools/vision/pos-vision"
    exit 1
fi

# How much of a screenshot is the fake camera's yellow bar: the picture is
# on screen, or a sheet covers it.
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

# run NAME ORIENTATION [VAR=value...]: the shell opens Vision, a screenshot,
# the logs. $STATE may hold vision/settings.v1 beforehand.
run() {
    local name=$1 o=$2
    shift 2
    RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d)
    env "$@" SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
        POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
        POCKETOS_VISION_HELPER="$HELPER" POCKETOS_CAMERA_BACKEND=fake POCKETOS_CAMERA_FAKE=period=20 \
        POCKETOS_VISION_KPU_SCRIPT="box=2:800:100:120:120:80:3:0,box=0:700:400:40:60:160" \
        timeout 60 "$SHELL_BIN" --rotation "$o" --no-lock --open vision --screenshot "$LOGD/$name.png" \
        --exit-after-ms 3500 >"$LOGD/out" 2>&1
    rc=$?
    check "$o $name: the shell opens Vision and draws it" "$([ "$rc" = "0" ] && [ -s "$LOGD/$name.png" ] && echo 1 || echo 0)"
    check "$o $name: and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null &&
                                           echo 0 || echo 1)"
    hits=$(grep -hE ' WARN |\[Warn\]' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null | grep -v 'radiod unavailable')
    check "$o $name: and no warning but the simulator's missing radiod" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
    check "$o $name: no helper outlives the shell" "$(pgrep -f "$HELPER session" >/dev/null && echo 0 || echo 1)"
    [ -n "${VISION_SHOTS:-}" ] && cp "$LOGD/$name.png" "$VISION_SHOTS/vision-$name-$o.png"
}

for o in portrait landscape; do
    STATE=$(mktemp -d)
    run detect "$o"
    y=$(yellow_px "$LOGD/detect.png")
    check "$o detect: the live picture is on screen ($y yellow px)" "$([ "$y" -gt 5000 ] && echo 1 || echo 0)"
    check "$o detect: it opened in DETECT, the default" \
        "$(grep -qh 'vision: opening in DETECT' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
    check "$o detect: merely opening stores nothing" "$([ ! -e "$STATE/vision/settings.v1" ] && echo 1 || echo 0)"
    rm -rf "$RUN" "$LOGD" "$CFG"

    run picker "$o" POCKETOS_VISION_SHEET=modes
    y=$(yellow_px "$LOGD/picker.png")
    check "$o picker: the mode picker covers the picture ($y yellow px)" "$([ "$y" -lt 500 ] && echo 1 || echo 0)"
    rm -rf "$RUN" "$LOGD" "$CFG"

    mkdir -p "$STATE/vision"
    printf 'mode=traffic\ntraffic.speed=wide\n' >"$STATE/vision/settings.v1"
    chmod 600 "$STATE/vision/settings.v1"
    before=$(md5sum <"$STATE/vision/settings.v1")
    run setup "$o" POCKETOS_VISION_SHEET=setup
    check "$o setup: a stored TRAFFIC opens in TRAFFIC" \
        "$(grep -qh 'vision: opening in TRAFFIC' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
    y=$(yellow_px "$LOGD/setup.png")
    check "$o setup: Traffic's setup covers the picture ($y yellow px)" "$([ "$y" -lt 500 ] && echo 1 || echo 0)"
    check "$o setup: and the settings file is left as it was" \
        "$([ "$(md5sum <"$STATE/vision/settings.v1")" = "$before" ] && echo 1 || echo 0)"
    rm -rf "$RUN" "$LOGD" "$CFG"

    printf 'mode=hologram\n' >"$STATE/vision/settings.v1"
    run damaged "$o"
    check "$o damaged: a mode this build never wrote opens in DETECT" \
        "$(grep -qh 'vision: opening in DETECT' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
    rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"
done

echo "vision_shell_test: $failed failure(s)"
exit $((failed > 0))
