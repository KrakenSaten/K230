#!/bin/bash
# The DOORS experience in the running simulator shell (DS §31):
#
#   1. the lock screen under a real pointer and the input stream
#      (shell_lock_test, built beside the shell), and PocketUI's tile, which
#      apps still use (pocketui_tile_test);
#   2. boot -> lock -> open -> home -> app -> home over shell.*: a cold start
#      is locked, the lock draws its photograph, opening shows the launcher
#      on the home photograph, relocking and reopening twenty times ends as
#      it started and holds no more art, the door sequence runs and ends,
#      shell.open and shell.home open the device, every app opens and comes
#      home, Controls opens and closes;
#   3. the launcher in both orientations: every app has a cell, every cell's
#      portal icon is drawn exactly where shell.info says, from its own art;
#   4. with no art installed the shell still starts, locks, opens and draws
#      every app on a fallback, and says so in shell.info;
#   5. a rotation restart carries on unlocked; a fresh start locks again; the
#      lock_screen=0 setting starts open.
#
# Requires SHELL_BIN (the CMake-built pocketos-shell) and pos (make all).
# SHOTS_DIR=<dir> keeps the screenshots.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
cd "$(dirname "$0")/.." || exit 1
export SDL_VIDEODRIVER=dummy
OUT=$(mktemp -d)
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
yes_if() { if "$@"; then echo 1; else echo 0; fi; }

fresh() {
    export POCKETOS_RUNTIME_DIR=$(mktemp -d -p "$OUT") POCKETOS_LOG_DIR=$(mktemp -d -p "$OUT")
    export POCKETOS_CONFIG_DIR=$(mktemp -d -p "$OUT") POCKETOS_STATE_DIR=$(mktemp -d -p "$OUT")
}
start_shell() { # [args]
    "$SHELL_BIN" --theme ice --mode normal "$@" >"$POCKETOS_LOG_DIR/run.log" 2>&1 &
    SP=$!
    for _ in $(seq 1 60); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
    sleep 0.4
}
stop_shell() { kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null; }
info() { "$POS" shell info 2>/dev/null; }
field() { # <json path in python syntax, e.g. ["lock"]["locked"]>
    info | python3 -c "import json,sys; d=json.load(sys.stdin); print(json.dumps(d$1))" 2>/dev/null
}
call() { "$POS" call shell "$@" >/dev/null 2>&1; }
shot() { "$POS" shell screenshot "$1" >/dev/null 2>&1; }

# art <png> <art-name> <x> <y>: how many of the art's pixels are drawn at
# (x, y) in the screenshot. Only fully opaque art pixels are compared - they
# must be exactly the art's colour, whatever is behind them - so the answer
# is "<matched> <opaque>", and a wrong image, place or colour is a mismatch.
art() {
    python3 - "$@" <<'PY'
import struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
png, name, x0, y0 = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
d = open("ui/assets/doors/%s.bin" % name, "rb").read()
_, cf, _, w, h, _, _ = struct.unpack("<BBHHHHH", d[:12])
W, H, rows = read_png(png)
def c565(v):
    return ((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31
matched = opaque = 0
step = 1 if cf == 0x14 else 7          # icons whole, backgrounds sampled
for y in range(0, h, step):
    for x in range(0, w, step):
        i = y * w + x
        a = d[12 + w * h * 2 + i] if cf == 0x14 else 255
        if a != 255 or not (0 <= x0 + x < W and 0 <= y0 + y < H):
            continue
        opaque += 1
        want = c565(struct.unpack_from("<H", d, 12 + i * 2)[0])
        got = rows[y0 + y][x0 + x][:3]
        if all(abs(got[k] - want[k]) <= 4 for k in range(3)):
            matched += 1
print(matched, opaque)
PY
}
# region_is <png> <art> <x0> <y0> <x1> <y1>: every sampled pixel of a region
# of the screen is the background art's own pixel there.
region_is() {
    python3 - "$@" <<'PY'
import struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
png, name = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
d = open("ui/assets/doors/%s.bin" % name, "rb").read()
_, _, _, w, h, _, _ = struct.unpack("<BBHHHHH", d[:12])
W, H, rows = read_png(png)
ok = n = 0
for y in range(y0, y1, 5):
    for x in range(x0, x1, 5):
        v = struct.unpack_from("<H", d, 12 + (y * w + x) * 2)[0]
        want = (((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31)
        n += 1
        ok += all(abs(rows[y][x][k] - want[k]) <= 4 for k in range(3))
print(1 if n and ok == n else 0, ok, n)
PY
}

# ---- 1. unit tests built beside the shell -------------------------------------
for t in shell_lock_test pocketui_tile_test; do
    BIN=$(dirname "$SHELL_BIN")/$t
    if [ -x "$BIN" ]; then
        log=$("$BIN" 2>&1); rc=$?
        printf '%s\n' "$log" | grep -E '^FAIL|^[a-z_]+_test:'
        check "$t passes" "$([ "$rc" = 0 ] && echo 1 || echo 0)"
    else
        check "$t binary present ($BIN)" 0
    fi
done

# ---- 2. the path through the shell ----------------------------------------------
fresh
start_shell --rotation portrait
check "a cold start is locked" "$([ "$(field '["lock"]["locked"]')" = true ] && echo 1 || echo 0)"
check "and says why in the log" "$(yes_if grep -q 'lock: engaged (start)' "$POCKETOS_LOG_DIR/shell.log")"
shot "$OUT/p-lock.png"
set -- $(region_is "$OUT/p-lock.png" bg-lock-portrait 0 90 568 150)
check "the lock draws its photograph below the bar ($2 of $3 sampled pixels)" "${1:-0}"
check "while locked the shell is home: nothing opened" "$([ "$(field '["current"]')" = '"home"' ] && echo 1 || echo 0)"
held_locked=$(field '["art"]["bytes_held"]')
call shell.unlock
sleep 0.3
check "shell.unlock opens it at once" "$([ "$(field '["lock"]["locked"]')" = false ] && echo 1 || echo 0)"
shot "$OUT/p-home.png"
set -- $(region_is "$OUT/p-home.png" bg-home-portrait 0 1210 568 1232)
check "the launcher lies on the home photograph ($2 of $3 sampled pixels at its foot)" "${1:-0}"
held_open=$(field '["art"]["bytes_held"]')
check "opening released the lock's photograph ($held_locked -> $held_open bytes held)" \
    "$([ -n "$held_open" ] && [ "$held_open" -lt "$held_locked" ] && echo 1 || echo 0)"
for k in $(seq 1 20); do
    call shell.lock
    call shell.unlock
done
check "twenty lock/open rounds end open" "$([ "$(field '["lock"]["locked"]')" = false ] && echo 1 || echo 0)"
check "counted as twenty-one of each" \
    "$([ "$(field '["lock"]["engaged"]')" = 21 ] && [ "$(field '["lock"]["opened"]')" = 21 ] && echo 1 || echo 0)"
check "holding exactly the art it held before ($(field '["art"]["bytes_held"]') bytes)" \
    "$([ "$(field '["art"]["bytes_held"]')" = "$held_open" ] && echo 1 || echo 0)"
call shell.lock
call shell.unlock animate=true
check "an animated open is running" "$([ "$(field '["lock"]["opening"]')" = true ] && echo 1 || echo 0)"
sleep 1.5
check "and ends open" "$([ "$(field '["lock"]["locked"]')" = false ] && [ "$(field '["lock"]["opening"]')" = false ] && echo 1 || echo 0)"
check "having shown the open door (read its photograph)" \
    "$(yes_if grep -q 'art: bg-open-portrait' "$POCKETOS_LOG_DIR/shell.log")"
call shell.lock
"$POS" app start notes >/dev/null 2>&1
sleep 0.4
check "shell.open while locked opens the device and the app" \
    "$([ "$(field '["lock"]["locked"]')" = false ] && [ "$(field '["current"]')" = '"notes"' ] && echo 1 || echo 0)"
call shell.lock
"$POS" app home >/dev/null 2>&1
sleep 0.3
check "shell.home while locked opens the device at home" \
    "$([ "$(field '["lock"]["locked"]')" = false ] && [ "$(field '["current"]')" = '"home"' ] && echo 1 || echo 0)"
opened=0
for id in rift radio wave notes calendar clock calculator fleet radar timber settings system; do
    "$POS" app start "$id" >/dev/null 2>&1 && sleep 0.4 &&
        [ "$(field '["current"]')" = "\"$id\"" ] && opened=$((opened + 1))
    "$POS" app home >/dev/null 2>&1; sleep 0.2
done
check "every one of the twelve apps opens and comes home ($opened)" "$([ "$opened" = 12 ] && echo 1 || echo 0)"
check "and the shell is home again" "$([ "$(field '["current"]')" = '"home"' ] && echo 1 || echo 0)"
call shell.controls
sleep 0.3
check "shell.controls opens Controls" "$([ "$(field '["launcher"]["controls"]')" = true ] && echo 1 || echo 0)"
shot "$OUT/p-controls.png"
call shell.controls show=false
check "and show=false closes it" "$([ "$(field '["launcher"]["controls"]')" = false ] && echo 1 || echo 0)"
call shell.controls
"$POS" app start radio >/dev/null 2>&1; sleep 0.3
check "opening an app from Controls leaves Controls" "$([ "$(field '["launcher"]["controls"]')" = false ] && echo 1 || echo 0)"
"$POS" app home >/dev/null 2>&1; sleep 0.2
check "no fault logged on the way" "$(yes_if eval '! grep -qE " ERROR |assert" "$POCKETOS_LOG_DIR/shell.log" "$POCKETOS_LOG_DIR/run.log"')"
stop_shell

# ---- 3. the launcher's icons, both orientations ---------------------------------
for o in portrait landscape; do
    fresh
    start_shell --rotation "$o" --no-lock
    info > "$OUT/$o-info.json"
    shot "$OUT/$o-launcher.png"
    stop_shell
    python3 - "$OUT/$o-info.json" > "$OUT/$o-cells.txt" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for c in d["launcher"]["cells"]:
    print(c["id"], c["x"] + (c["w"] - 96) // 2, c["y"], c["w"], c["h"])
print("apps", d["launcher"]["apps"], d["launcher"]["icons_art"], d["launcher"]["icons_fallback"], d["launcher"]["scrolls"])
PY
    set -- $(grep '^apps' "$OUT/$o-cells.txt")
    check "$o: twelve apps, twelve portal icons from the art, none on a fallback, no scrolling ($2 $3 $4 $5)" \
        "$([ "$2" = 12 ] && [ "$3" = 12 ] && [ "$4" = 0 ] && [ "$5" = false ] && echo 1 || echo 0)"
    good=0
    while read -r id x y w h; do
        [ "$id" = apps ] && continue
        set -- $(art "$OUT/$o-launcher.png" "icon-$id" "$x" "$y")
        if [ "${1:-0}" = "${2:--1}" ] && [ "${2:-0}" -gt 2000 ]; then
            good=$((good + 1))
        else
            echo "     $o: $id icon at ($x, $y): $1 of $2 opaque pixels match"
        fi
        [ "$w" -ge 64 ] && [ "$h" -ge 64 ] || echo "     $o: $id cell $w x $h is below the touch minimum"
    done < "$OUT/$o-cells.txt"
    check "$o: every app's own portal icon is drawn in its cell, pixel for pixel ($good of 12)" \
        "$([ "$good" = 12 ] && echo 1 || echo 0)"
done

# ---- 4. no art installed --------------------------------------------------------
fresh
mkdir -p "$OUT/noart"
POCKETOS_ART_DIR="$OUT/noart" start_shell --rotation portrait
check "with no art the shell starts, locked" "$([ "$(field '["lock"]["locked"]')" = true ] && echo 1 || echo 0)"
check "and says it has no background" "$([ "$(field '["art"]["background"]')" = false ] && echo 1 || echo 0)"
check "every app is on the fallback frame" \
    "$([ "$(field '["launcher"]["icons_fallback"]')" = 12 ] && [ "$(field '["launcher"]["icons_art"]')" = 0 ] && echo 1 || echo 0)"
shot "$OUT/noart-lock.png"
call shell.unlock
sleep 0.2
shot "$OUT/noart-home.png"
check "it still opens" "$([ "$(field '["lock"]["locked"]')" = false ] && echo 1 || echo 0)"
"$POS" app start clock >/dev/null 2>&1; sleep 0.3
check "and apps still open" "$([ "$(field '["current"]')" = '"clock"' ] && echo 1 || echo 0)"
check "the missing files are logged as warnings, not faults" \
    "$(yes_if eval 'grep -q "art: .*No such file" "$POCKETOS_LOG_DIR/shell.log" && ! grep -qE " ERROR |assert" "$POCKETOS_LOG_DIR/shell.log"')"
stop_shell
# Only the empty frame: every app's own mask on it.
fresh
mkdir -p "$OUT/frameonly"
cp ui/assets/doors/icon-frame.bin "$OUT/frameonly/"
POCKETOS_ART_DIR="$OUT/frameonly" start_shell --rotation portrait --no-lock
shot "$OUT/frameonly.png"
python3 - "$(info)" > "$OUT/fo.txt" <<'PY'
import json, sys
d = json.loads(sys.argv[1])
for c in d["launcher"]["cells"]:
    print(c["id"], c["x"] + (c["w"] - 96) // 2, c["y"])
PY
good=0
while read -r id x y; do
    set -- $(art "$OUT/frameonly.png" icon-frame "$x" "$y")
    # The frame's opaque pixels, less the few the app's mask covers.
    [ "${2:-0}" -gt 2000 ] && [ $(( ${2:-0} - ${1:-0} )) -lt 400 ] && good=$((good + 1))
done < "$OUT/fo.txt"
check "with only the empty frame installed, every app is drawn on it ($good of 12)" "$([ "$good" = 12 ] && echo 1 || echo 0)"
stop_shell

# ---- 5. restarts ------------------------------------------------------------------
fresh
# Automatic with no keyboard: portrait. No --rotation here, or the restart
# (which keeps the arguments) would keep it.
start_shell
call shell.unlock
pid1=$SP
call shell.rotation mode=landscape
for _ in $(seq 1 40); do
    [ "$(field '["display"]["orientation"]')" = '"landscape"' ] && break
    sleep 0.2
done
check "a rotation restarts the shell in place, landscape" \
    "$([ "$(field '["display"]["orientation"]')" = '"landscape"' ] && kill -0 "$pid1" 2>/dev/null && echo 1 || echo 0)"
check "and carries on open: a rotation is not a new session" \
    "$([ "$(field '["lock"]["locked"]')" = false ] && echo 1 || echo 0)"
check "the log says why it did not lock" "$(yes_if grep -q 'lock: not engaged at start (rotation restart)' "$POCKETOS_LOG_DIR/shell.log")"
stop_shell
start_shell
check "a fresh start after that locks again" "$([ "$(field '["lock"]["locked"]')" = true ] && echo 1 || echo 0)"
stop_shell
fresh
printf 'lock_screen=0\n' > "$POCKETOS_CONFIG_DIR/settings.conf"
start_shell
check "lock_screen=0 starts open" "$([ "$(field '["lock"]["locked"]')" = false ] && echo 1 || echo 0)"
stop_shell

if [ -n "${SHOTS_DIR:-}" ]; then
    mkdir -p "$SHOTS_DIR"
    cp "$OUT"/*.png "$SHOTS_DIR"/
fi
rm -rf "$OUT"
echo "doors_shell_test: $failed failure(s)"
exit $((failed > 0))
