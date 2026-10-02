#!/bin/bash
# System in the running app, and the shell's side of it.
#
# tests/system_brand_shell_test.sh checks the Doors mark in its identity row;
# this runs system_app_test (the app tapped and scrolled in portrait and in
# landscape against a scripted sysd), the wiring and the lint, and then opens
# System in the real shell both ways up.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell). Run from the
# repository root. No sysd is started, so the screen is the same short one on
# every host.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${SYSTEM_APP_TEST:-$(dirname "$SHELL_BIN")/system_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|system_app_test:'
    check "System end to end, tapped and scrolled, in portrait and landscape" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    # LVGL reports a layout it cannot do (a pointer off the display, a flex
    # item it cannot place) as a warning and carries on, so a clean result has
    # to mean a clean log as well.
    printf '%s\n' "$log" | grep -E '^\[(Warn|Error)\]' | head -3
    check "and LVGL warned about nothing while it ran" \
        "$(printf '%s\n' "$log" | grep -qE '^\[(Warn|Error)\]' && echo 0 || echo 1)"
else
    echo "FAIL system_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

check "the shell knows about System" "$(grep -q '&app_system' ui/shell/shell.c && echo 1 || echo 0)"
for src in system_app.c system_view.c; do
    check "the shell builds $src" "$(grep -q "apps/system/$src" ui/shell/CMakeLists.txt && echo 1 || echo 0)"
done
check "the app test is a host-only target" \
    "$(awk '/if\(POCKETOS_DISPLAY STREQUAL "sdl"\)/,/^endif\(\)/' ui/shell/CMakeLists.txt |
       grep -q 'add_executable(system_app_test' && echo 1 || echo 0)"
bash tests/system_lint.sh >/dev/null 2>&1
check "the System lint passes" "$([ $? = 0 ] && echo 1 || echo 0)"

# Both orientations in the real shell, drawn: the panels where the app test
# lays them out - one column across the body in portrait, two with the 22 px
# gap between them in landscape - and nothing but the screen's background in
# the rounded corner squares at the foot of the panel (DS section 21.1; the
# simulator's default corners are the reference panel's 30 px).
#
# look <png> <theme> <mode> <y> <x>...: for each x at row y, 1 if the pixel is
# the background there and 0 if something is drawn; then 1 if the foot corner
# squares are background only.
look() {
    python3 - "$@" <<'PY'
import json, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
path, theme, mode, y = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
xs = [int(v) for v in sys.argv[5:]]
tok = json.load(open("docs/design/themes.json", encoding="utf-8"))["themes"][theme]["modes"][mode]
hexrgb = lambda s: tuple(int(s[i:i + 2], 16) for i in (1, 3, 5))
bg = hexrgb(tok["bg"])
W, H, rows = read_png(path)
near = lambda p, c: all(abs(p[k] - c[k]) <= 3 for k in range(3))
out = ["1" if near(rows[y][x][:3], bg) else "0" for x in xs]
c = 30
corners = all(near(rows[yy][xx][:3], bg) for yy in range(H - c, H) for xx in list(range(0, c)) + list(range(W - c, W)))
out.append("1" if corners else "0")
print(" ".join(out))
PY
}
for o in portrait landscape; do
    RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
    SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
    POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
        "$SHELL_BIN" --rotation $o --theme ice --mode normal --open system --screenshot "$LOGD/system.png" \
        --exit-after-ms 1200 >"$LOGD/out" 2>&1
    rc=$?
    check "$o: the shell opens System with no sysd running, and draws it" \
        "$([ "$rc" = "0" ] && [ -s "$LOGD/system.png" ] && echo 1 || echo 0)"
    check "$o: System reports itself open" \
        "$(grep -q 'open app system' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
    check "$o: and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null &&
                                    echo 0 || echo 1)"
    # The simulator has no radiod and no sysd, and says so about radiod; nothing
    # else may warn.
    hits=$(grep -hE ' WARN |\[Warn\]' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null | grep -v 'radiod unavailable')
    check "$o: and no warning but the simulator's missing radiod" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
    check "$o: opening System writes nothing to the app state directory" \
        "$([ -z "$(ls -A "$STATE" 2>/dev/null)" ] && echo 1 || echo 0)"
    # A row in the top padding of the first panel of each column, under the
    # tabs (DS §52.5; in portrait the freshness line has a line of its own
    # under them, so the panels start lower): it crosses the panel's left
    # border, the inside, its right border and the background beside it.
    if [ $o = portrait ]; then
        set -- $(look "$LOGD/system.png" ice normal 215 20 283 547 557)
        check "$o: one panel across the body, 20..547" "$([ "$1$2$3$4" = "0101" ] && echo 1 || echo 0)"
        corners=$5
    else
        set -- $(look "$LOGD/system.png" ice normal 205 20 604 615 627 1211 1221)
        check "$o: two panels side by side, 20..604 and 627..1211, the gap between them empty" \
            "$([ "$1$2$3$4$5$6" = "001001" ] && echo 1 || echo 0)"
        corners=$7
    fi
    check "$o: nothing is drawn in the rounded corner squares at the foot of the panel" "${corners:-0}"
    rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"
done

echo "system_shell_test: $failed failure(s)"
exit $((failed > 0))
