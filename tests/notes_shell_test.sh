#!/bin/bash
# PocketNotes in the running app, and the shell's ownership of the keyboard.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${NOTES_APP_TEST:-$(dirname "$SHELL_BIN")/notes_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|notes_app_test:'
    check "PocketNotes end to end, tapped on a keyboard, in portrait and landscape" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    # LVGL reports a layout it cannot do as a warning and carries on, so a
    # clean result has to mean a clean log as well.
    printf '%s\n' "$log" | grep -E '^\[(Warn|Error)\]' | head -3
    check "and LVGL warned about nothing while it ran" \
        "$(printf '%s\n' "$log" | grep -qE '^\[(Warn|Error)\]' && echo 0 || echo 1)"
else
    echo "FAIL notes_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

# The shell must build exactly one keyboard, and it must start hidden.
check "the shell creates the one keyboard" \
    "$(grep -q 'sh.keyboard = pos_keyboard_create' ui/shell/shell.c && echo 1 || echo 0)"
check "exactly one, not one per app" \
    "$([ "$(grep -c 'pos_keyboard_create' ui/shell/shell.c)" = "1" ] && echo 1 || echo 0)"
check "no app creates a keyboard" \
    "$(grep -rq 'pos_keyboard_create' apps/ && echo 0 || echo 1)"
check "closing an app puts the keyboard away" \
    "$(grep -q 'pocketos_shell_keyboard_hide' ui/shell/shell.c && echo 1 || echo 0)"

# Notes is on the launcher and answers over shell.*.
RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
    "$SHELL_BIN" --open notes --exit-after-ms 900 >"$LOGD/out" 2>&1
rc=$?
check "the shell opens Notes" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" && echo 0 || echo 1)"
check "Notes reports itself open" \
    "$(grep -q 'open app notes' "$LOGD/log/shell.log" 2>/dev/null ||
       grep -q 'open app notes' "$LOGD/out" && echo 1 || echo 0)"
rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"

# Both orientations in the real shell, drawn: where New note lands on an empty
# store, and that nothing but the screen's background is drawn in the rounded
# corner squares at the foot of the panel (DS 21.1; the simulator's corners
# are the reference panel's 30 px).
#
# look <png> <theme> <mode> <New note x1 y1 x2 y2>
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
inside = [(x1 + 8, y1 + 8), (x2 - 8, y1 + 8), (x1 + 8, y2 - 8), (x2 - 8, y2 - 8)]
outside = [(x1 - 3, (y1 + y2) // 2), ((x1 + x2) // 2, y2 + 3)]
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
        "$SHELL_BIN" --rotation $o --open notes --screenshot "$LOGD/notes.png" \
        --exit-after-ms 1200 >"$LOGD/out" 2>&1
    rc=$?
    check "$o: the shell opens Notes and draws it" "$([ "$rc" = "0" ] && [ -s "$LOGD/notes.png" ] && echo 1 || echo 0)"
    check "$o: and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null &&
                                    echo 0 || echo 1)"
    # The simulator has no radiod, and says so; nothing else may warn.
    hits=$(grep -hE ' WARN |\[Warn\]' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null | grep -v 'radiod unavailable')
    check "$o: and no warning but the simulator's missing radiod" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
    if [ $o = portrait ]; then
        set -- $(look "$LOGD/notes.png" ice normal 20 328 547 391)
        check "$o: New note is drawn under the empty state, where notes_app_test lays it out" "${1:-0}"
    else
        set -- $(look "$LOGD/notes.png" ice normal 924 152 1211 215)
        check "$o: New note is drawn in the rail beside the empty state, where notes_app_test lays it out" "${1:-0}"
    fi
    check "$o: nothing is drawn in the rounded corner squares at the foot of the panel" "${2:-0}"
    rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"
done

echo "notes_shell_test: $failed failure(s)"
exit $((failed > 0))
