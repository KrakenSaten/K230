#!/bin/bash
# Files in the running app (tests/files_app_test.c), and in the real shell:
# registered, opened from --open in both orientations, drawn, and quiet.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${FILES_APP_TEST:-$(dirname "$SHELL_BIN")/files_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|files_app_test:'
    check "Files end to end, tapped and typed, in portrait and landscape" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    # LVGL reports a layout it cannot do as a warning and carries on, so a
    # clean result has to mean a clean log as well.
    printf '%s\n' "$log" | grep -E '^\[(Warn|Error)\]' | head -3
    check "and LVGL warned about nothing while it ran" \
        "$(printf '%s\n' "$log" | grep -qE '^\[(Warn|Error)\]' && echo 0 || echo 1)"
else
    echo "FAIL files_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

# In the real shell: both orientations, a home with something in it, and
# nothing drawn in the rounded corner squares at the foot of the panel
# (DS 21.1; the simulator's corners are the reference panel's 30 px).
corners_clear() {
    python3 - "$@" <<'PY'
import json, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
path = sys.argv[1]
data = json.load(open("docs/design/themes.json", encoding="utf-8"))
tok = data["themes"][data["fallback_theme"]]["modes"]["normal"]
bg = tuple(int(tok["bg"][i:i + 2], 16) for i in (1, 3, 5))
W, H, rows = read_png(path)
near = lambda p: all(abs(p[k] - bg[k]) <= 3 for k in range(3))
c = 30
print("1" if all(near(rows[y][x][:3]) for y in range(H - c, H)
                 for x in list(range(0, c)) + list(range(W - c, W))) else "0")
PY
}
for o in portrait landscape; do
    RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d); HOMED=$(mktemp -d)
    mkdir -p "$HOMED/Documents"; printf 'hello\n' > "$HOMED/readme.txt"
    HOME="$HOMED" SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
    POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
        "$SHELL_BIN" --rotation $o --open files --screenshot "$LOGD/files.png" \
        --exit-after-ms 1200 >"$LOGD/out" 2>&1
    rc=$?
    check "$o: the shell opens Files and draws it" "$([ "$rc" = "0" ] && [ -s "$LOGD/files.png" ] && echo 1 || echo 0)"
    check "$o: Files reports itself open" \
        "$(grep -qh 'open app files' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
    check "$o: and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null &&
                                    echo 0 || echo 1)"
    # The simulator has no radiod, and says so; nothing else may warn.
    hits=$(grep -hE ' WARN |\[Warn\]' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null | grep -v 'radiod unavailable')
    check "$o: and no warning but the simulator's missing radiod" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
    if [ -s "$LOGD/files.png" ]; then
        check "$o: nothing is drawn in the rounded corner squares at the foot of the panel" \
            "$(corners_clear "$LOGD/files.png")"
        [ -n "${FILES_SHOTS:-}" ] && cp "$LOGD/files.png" "$FILES_SHOTS/files-$o.png"
    fi
    rm -rf "$RUN" "$LOGD" "$CFG" "$STATE" "$HOMED"
done

# Registered like any other app: in the registry, on the launcher, with its
# icon and its colour.
check "Files is in the shell's registry" \
    "$(grep -q '&app_files' ui/shell/shell.c && echo 1 || echo 0)"
check "on the launcher, under DEVICE, in the files colour" \
    "$(grep -q '{ "files", HOME_GROUP_DEVICE, HOME_HUE_FILES[ ,}]' ui/shell/home_layout.c && echo 1 || echo 0)"
check "with its portal icon and its mask" \
    "$([ -f ui/assets/doors/icon-files.bin ] && grep -q 'pos_app_icon_files = {' ui/pocketui/pos_app_icons.c &&
       echo 1 || echo 0)"

echo "files_shell_test: $failed failure(s)"
exit $((failed > 0))
