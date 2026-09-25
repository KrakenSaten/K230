#!/bin/bash
# Zabbix in the running app (tests/zabbix_app_test.c), and in the real shell:
# registered, opened from --open in both orientations on the fake server,
# drawn with its data, quiet, and with no helper left when it closes.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell) and make's
# tools/zabbix/pos-zabbix. ZABBIX_SHOTS=<dir> keeps the screenshots.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

HELPER=$(pwd)/tools/zabbix/pos-zabbix
if [ ! -x "$HELPER" ]; then
    echo "FAIL build the helper first: make tools/zabbix/pos-zabbix"
    exit 1
fi

BIN=${ZABBIX_APP_TEST:-$(dirname "$SHELL_BIN")/zabbix_app_test}
if [ -x "$BIN" ]; then
    log=$(ZABBIX_HELPER="$HELPER" SDL_VIDEODRIVER=dummy timeout 400 "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|zabbix_app_test:|^     '
    check "Zabbix end to end, tapped, in portrait and landscape, and every fault" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    # LVGL reports a layout it cannot do as a warning and carries on, so a
    # clean result has to mean a clean log as well.
    printf '%s\n' "$log" | grep -E '^\[(Warn|Error)\]' | head -3
    check "and LVGL warned about nothing while it ran" \
        "$(printf '%s\n' "$log" | grep -qE '^\[(Warn|Error)\]' && echo 0 || echo 1)"
else
    echo "FAIL zabbix_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

# How much of the screen is the error colour: the disaster headline and the
# DOWN hosts are, on the demo's OVERVIEW. (The Doors theme's status_error is
# a red; any strongly red pixel counts.)
red_seen() {
    python3 - "$@" <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
W, H, rows = read_png(sys.argv[1])
red = sum(1 for r in rows for p in r if p[0] > 180 and p[1] < 110 and p[2] < 110)
print("1" if red > 400 else "0", red)
PY
}
for o in portrait landscape; do
    RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
    SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
    POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
    POCKETOS_ZABBIX_HELPER="$HELPER" POCKETOS_ZABBIX_BACKEND=fake POCKETOS_ZABBIX_FAKE=demo \
        timeout 60 "$SHELL_BIN" --rotation $o --open zabbix --screenshot "$LOGD/zabbix.png" \
        --exit-after-ms 2500 >"$LOGD/out" 2>&1
    rc=$?
    check "$o: the shell opens Zabbix and draws it" "$([ "$rc" = "0" ] && [ -s "$LOGD/zabbix.png" ] && echo 1 || echo 0)"
    check "$o: Zabbix reports itself open" \
        "$(grep -qh 'open app zabbix' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
    check "$o: and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null &&
                                    echo 0 || echo 1)"
    hits=$(grep -hE ' WARN |\[Warn\]' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null | grep -v 'radiod unavailable')
    check "$o: and no warning but the simulator's missing radiod" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
    if [ -s "$LOGD/zabbix.png" ]; then
        set -- $(red_seen "$LOGD/zabbix.png")
        check "$o: the disaster is on screen in the error colour ($2 px)" "$1"
        [ -n "${ZABBIX_SHOTS:-}" ] && cp "$LOGD/zabbix.png" "$ZABBIX_SHOTS/zabbix-$o.png"
    fi
    check "$o: the helper logged its session" \
        "$(grep -q 'zabbix: session start, fake' "$LOGD/pos-zabbix.log" 2>/dev/null && echo 1 || echo 0)"
    check "$o: no helper outlives the shell" \
        "$(pgrep -f "$HELPER session" >/dev/null && echo 0 || echo 1)"
    rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"
done

# Registered like any other app.
check "Zabbix is in the shell's registry" \
    "$(grep -q '&app_zabbix' ui/shell/shell.c && echo 1 || echo 0)"
check "fullscreen (DS §30.8)" \
    "$(grep -q '.chrome = POCKETOS_CHROME_NONE' apps/zabbix/zabbix_app.c && echo 1 || echo 0)"
check "the simulator alone defaults to the fake" \
    "$(grep -q 'ZABBIX_FAKE_DEFAULT="demo"' ui/shell/CMakeLists.txt &&
       [ "$(grep -c 'ZABBIX_FAKE_DEFAULT="demo"' ui/shell/CMakeLists.txt)" = 1 ] && echo 1 || echo 0)"

echo "zabbix_shell_test: $failed failure(s)"
exit $((failed > 0))
