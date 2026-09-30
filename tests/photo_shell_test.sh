#!/bin/bash
# Photo in the running app (tests/photo_app_test.c), and in the real shell:
# registered, opened from --open in both orientations on a library of photos,
# drawn with their thumbnails, quiet, with no helper left when it closes and
# the library exactly as it was.
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

BIN=${PHOTO_APP_TEST:-$(dirname "$SHELL_BIN")/photo_app_test}
if [ -x "$BIN" ]; then
    log=$(CAMERA_HELPER="$HOOKS" SDL_VIDEODRIVER=dummy timeout 400 "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|photo_app_test:|^     |^note'
    check "Photo end to end, tapped, in portrait and landscape, and every fault" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    # LVGL reports a layout it cannot do as a warning and carries on, so a
    # clean result has to mean a clean log as well.
    printf '%s\n' "$log" | grep -E '^\[(Warn|Error)\]' | head -3
    check "and LVGL warned about nothing while it ran" \
        "$(printf '%s\n' "$log" | grep -qE '^\[(Warn|Error)\]' && echo 0 || echo 1)"
else
    echo "FAIL photo_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

# A library of solid magenta photos: no theme draws that colour, so what of it
# is on screen is the thumbnails.
make_library() {
    python3 - "$1" <<'PY'
import os, sys
d = sys.argv[1]
os.makedirs(d, exist_ok=True)
for i in range(1, 8):
    w, h = (64, 36) if i % 2 else (36, 64)
    with open(os.path.join(d, "IMG_%04d.ppm" % i), "wb") as f:
        f.write(b"P6\n# doors-software Doors test\n%d %d\n255\n" % (w, h))
        f.write(b"\xff\x00\xff" * (w * h))
PY
}
magenta_seen() {
    python3 - "$1" <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
W, H, rows = read_png(sys.argv[1])
n = sum(1 for r in rows for p in r if p[0] > 200 and p[1] < 60 and p[2] > 200)
print("1" if n > 7 * 2000 else "0", n)
PY
}

for o in portrait landscape; do
    RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
    make_library "$STATE/camera"
    before=$(cd "$STATE/camera" && sha256sum * | sha256sum)
    SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
    POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" HOME="$STATE/home" \
    POCKETOS_CAMERA_HELPER="$HELPER" \
        timeout 60 "$SHELL_BIN" --rotation $o --open photo --screenshot "$LOGD/photo.png" \
        --exit-after-ms 3000 >"$LOGD/out" 2>&1
    rc=$?
    check "$o: the shell opens Photo and draws it" "$([ "$rc" = "0" ] && [ -s "$LOGD/photo.png" ] && echo 1 || echo 0)"
    check "$o: Photo reports itself open" \
        "$(grep -qh 'open app photo' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
    check "$o: and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null &&
                                    echo 0 || echo 1)"
    hits=$(grep -hE ' WARN |\[Warn\]' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null | grep -v 'radiod unavailable')
    check "$o: and no warning but the simulator's missing radiod" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
    if [ -s "$LOGD/photo.png" ]; then
        set -- $(magenta_seen "$LOGD/photo.png")
        check "$o: the seven thumbnails are on screen ($2 px of them)" "$1"
        [ -n "${PHOTO_SHOTS:-}" ] && cp "$LOGD/photo.png" "$PHOTO_SHOTS/photo-$o.png"
    fi
    check "$o: no helper outlives the shell" \
        "$(pgrep -f "$HELPER library" >/dev/null && echo 0 || echo 1)"
    check "$o: and none ever opened a camera" \
        "$(pgrep -f "$HELPER session" >/dev/null && echo 0 || echo 1)"
    after=$(cd "$STATE/camera" && sha256sum * | sha256sum)
    check "$o: opening and closing leaves the library exactly as it was" \
        "$([ "$before" = "$after" ] && [ "$(ls "$STATE/camera" | wc -l)" = 7 ] && echo 1 || echo 0)"
    rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"
done

# Camera's gallery is unchanged by being shared: CAMERA is still its way back.
check "Camera's own gallery still has CAMERA" \
    "$(grep -q 'out->left = g->standalone ? NULL : "CAMERA";' apps/camera/camera_gallery.c && echo 1 || echo 0)"

echo "photo_shell_test: $failed failure(s)"
exit $((failed > 0))
