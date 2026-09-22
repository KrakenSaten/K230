#!/bin/bash
# PocketCalculator in the running app, and the shell's side of it.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${CALC_APP_TEST:-$(dirname "$SHELL_BIN")/calc_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|calc_app_test:'
    check "PocketCalculator end to end, tapped and typed, in portrait and landscape" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    # LVGL reports a layout it cannot do (a grid without its rows, a pointer
    # off the display) as a warning and carries on, so a clean result has to
    # mean a clean log as well.
    printf '%s\n' "$log" | grep -E '^\[(Warn|Error)\]' | head -3
    check "and LVGL warned about nothing while it ran" \
        "$(printf '%s\n' "$log" | grep -qE '^\[(Warn|Error)\]' && echo 0 || echo 1)"
else
    echo "FAIL calc_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

# The app is on the launcher, after Calendar, and is the shell's to build.
check "the shell knows about Calculator" \
    "$(grep -q 'extern const struct pocketos_app app_calculator;' ui/shell/shell.c && echo 1 || echo 0)"
check "it is on the launcher, after Calendar" \
    "$(grep -q '&app_calendar, &app_calculator' ui/shell/shell.c && echo 1 || echo 0)"
for src in calc_app.c calc_engine.c calc_view.c; do
    check "the shell builds $src" \
        "$(grep -q "apps/calculator/$src" ui/shell/CMakeLists.txt && echo 1 || echo 0)"
done
check "and has the app's directory on its include path" \
    "$(grep -q '"${REPO_DIR}/apps/calculator"' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
check "the app test is a host-only target" \
    "$(awk '/if\(POCKETOS_DISPLAY STREQUAL "sdl"\)/,/^endif\(\)/' ui/shell/CMakeLists.txt |
       grep -q 'add_executable(calc_app_test' && echo 1 || echo 0)"

# Scope, checked here as well as in calculator_lint.sh because this is the
# file a reviewer reaches for when asking what the app is allowed to be.
check "Calculator creates no keyboard of its own" \
    "$(grep -rqE 'pos_keyboard|lv_keyboard' apps/calculator/ && echo 0 || echo 1)"
check "has no store" \
    "$(ls apps/calculator/*store* >/dev/null 2>&1 && echo 0 || echo 1)"
check "and no tick, because it follows no clock" \
    "$(grep -q '\.tick = NULL,' apps/calculator/calc_app.c && echo 1 || echo 0)"

# The real shell: open it, run it, close it.
RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
    "$SHELL_BIN" --open calculator --exit-after-ms 1200 >"$LOGD/out" 2>&1
rc=$?
check "the shell opens Calculator" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
# The log file is read as well as the output, so it has to be there: a check
# for faults in a file that was never written would pass by looking at nothing.
check "the shell wrote its log" "$([ -s "$LOGD/shell.log" ] && echo 1 || echo 0)"
check "and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null &&
                             echo 0 || echo 1)"
check "Calculator reports itself open" \
    "$(grep -q 'open app calculator' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "and closed" \
    "$(grep -q 'close app calculator' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
# The DOORS launcher groups the apps (DS §31, tests/home_layout_test.c); this
# reads what the portrait launcher was built with.
check "the launcher holds every app in its groups" \
    "$(grep -q 'launcher: 4 group(s), 12 app(s), portrait' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "the shell did not call it unknown" \
    "$(grep -q 'unknown app calculator' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 0 || echo 1)"
# A calculation is not kept, so opening and leaving must write nothing at all.
check "opening Calculator writes nothing to the store" \
    "$([ -z "$(ls -A "$STATE" 2>/dev/null)" ] && echo 1 || echo 0)"
rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"

# Both orientations in the real shell, drawn: where the = key lands, and that
# nothing but the screen's background is drawn in the rounded corner squares
# below the status bar (DS section 21.1; the corners are the simulator's
# default 30 px, the reference panel's).
#
# look <png> <theme> <mode> <= key x1 y1 x2 y2>
look() {
    python3 - "$@" <<'PY'
import json, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
path, theme, mode = sys.argv[1:4]
x1, y1, x2, y2 = (int(v) for v in sys.argv[4:8])
tok = json.load(open("docs/design/themes.json", encoding="utf-8"))["themes"][theme]["modes"][mode]
hexrgb = lambda s: tuple(int(s[i:i + 2], 16) for i in (1, 3, 5))
acc, bg = hexrgb(tok["accent_primary"]), hexrgb(tok["bg"])
W, H, rows = read_png(path)
px = lambda x, y: rows[y][x][:3]
near = lambda p, c: all(abs(p[k] - c[k]) <= 3 for k in range(3))
ok = []
# The key's fill a little in from each of its corners, clear of the rounded
# corners and of the glyph, and the background just outside it.
inside = [(x1 + 8, y1 + 8), (x2 - 8, y1 + 8), (x1 + 8, y2 - 8), (x2 - 8, y2 - 8)]
outside = [(x2 + 3, (y1 + y2) // 2), ((x1 + x2) // 2, y2 + 3)]
ok.append(all(near(px(x, y), acc) for x, y in inside) and all(near(px(x, y), bg) for x, y in outside))
c = 30
corners = [(x, y) for y in range(H - c, H) for x in list(range(0, c)) + list(range(W - c, W))]
ok.append(all(near(px(x, y), bg) for x, y in corners))
print(" ".join("1" if v else "0" for v in ok))
PY
}
for o in portrait landscape; do
    RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
    SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
    POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
        "$SHELL_BIN" --rotation $o --open calculator --screenshot "$LOGD/calc.png" \
        --exit-after-ms 1200 >"$LOGD/out" 2>&1
    rc=$?
    check "$o: the shell opens Calculator and draws it" "$([ "$rc" = "0" ] && [ -s "$LOGD/calc.png" ] && echo 1 || echo 0)"
    check "$o: and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null &&
                                    echo 0 || echo 1)"
    # The simulator has no radiod, and says so; nothing else may warn.
    hits=$(grep -hE ' WARN |\[Warn\]' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null | grep -v 'radiod unavailable')
    check "$o: and no warning but the simulator's missing radiod" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
    if [ $o = portrait ]; then
        check "$o: on the portrait launcher" \
            "$(grep -q 'launcher: 4 group(s), 12 app(s), portrait' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
        set -- $(look "$LOGD/calc.png" ice normal 422 1074 547 1201)
    else
        check "$o: on the landscape launcher" \
            "$(grep -q 'launcher: 4 group(s), 12 app(s), landscape' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
        # The keypad is 410 px tall under the 32 px COMPACT bar of DS section
        # 30 (386 under the 56 px one), so its bottom row is 75 px, not 70.
        set -- $(look "$LOGD/calc.png" ice normal 1072 463 1211 537)
    fi
    check "$o: the = key is drawn where calc_app_test lays it out, bottom right" "${1:-0}"
    check "$o: nothing is drawn in the rounded corner squares at the foot of the panel" "${2:-0}"
    rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"
done

echo "calculator_shell_test: $failed failure(s)"
exit $((failed > 0))
