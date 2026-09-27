#!/bin/bash
# Browser in the real shell (the simulator): registered, opened in both
# orientations, a page drawn from the fake network with its picture, an error
# page, a theme change under an open page, thirty open/close cycles that
# leave no helper, no picture directory and no growing memory behind, and a
# helper that goes when the shell is killed.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell, which has the
# simulator's test hooks), tools/pos/pos and tools/browser/pos-browser.
# BROWSER_SHOTS=<dir> keeps the screenshots.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
POS=$(pwd)/tools/pos/pos
HELPER=$(pwd)/tools/browser/pos-browser
for b in "$POS" "$HELPER"; do
    [ -x "$b" ] || { echo "FAIL build it first: $b"; exit 1; }
done
export SDL_VIDEODRIVER=dummy POCKETOS_BROWSER_HELPER="$HELPER" POCKETOS_BROWSER_BACKEND=fake
T=$(mktemp -d)
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

start_shell() { # <name> <rotation> [url]
    local d="$T/$1"
    mkdir -p "$d/run" "$d/log" "$d/cfg" "$d/state"
    POCKETOS_RUNTIME_DIR="$d/run" POCKETOS_LOG_DIR="$d/log" POCKETOS_CONFIG_DIR="$d/cfg" \
    POCKETOS_STATE_DIR="$d/state" POCKETOS_BROWSER_OPEN="${3:-}" \
        "$SHELL_BIN" --no-lock --rotation "$2" --exit-after-ms 300000 >"$d/out" 2>&1 &
    PID=$!
    export POCKETOS_RUNTIME_DIR="$d/run"
    for _ in $(seq 50); do [ -S "$d/run/shell.sock" ] && break; sleep 0.1; done
}
stop_shell() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=""; }
logs() { cat "$T/$1/out" "$T/$1/log/shell.log" 2>/dev/null; }
helpers() { pgrep -P "$PID" -f pos-browser | wc -l; }
rss() { awk '/VmRSS/{print $2}' "/proc/$PID/status"; }
shot() { "$POS" shell screenshot "$1" >/dev/null 2>&1; [ -n "${BROWSER_SHOTS:-}" ] && cp "$1" "$BROWSER_SHOTS/"; }
quiet() { # <name>: no fault and no warning but the simulator's missing radiod
    local hits
    hits=$(logs "$1" | grep -E ' ERROR | WARN |\[(Warn|Error)\]|assert' | grep -v 'radiod unavailable')
    [ -n "$hits" ] && echo "$hits" | head -3
    [ -z "$hits" ] && echo 1 || echo 0
}

# ---- the start page ------------------------------------------------------------------
start_shell start portrait
"$POS" app start browser >/dev/null 2>&1; sleep 1
check "Browser opens from the registry on its start page" \
    "$(logs start | grep -q 'open app browser' && logs start | grep -q 'browser: start page in' && echo 1 || echo 0)"
check "the start page starts no helper" "$([ "$(helpers)" = 0 ] && echo 1 || echo 0)"
shot "$T/browser-start-portrait.png"
check "and draws (screenshot)" "$([ -s "$T/browser-start-portrait.png" ] && echo 1 || echo 0)"
"$POS" app home >/dev/null 2>&1; sleep 0.3
check "the start page: quiet" "$(quiet start)"
stop_shell

# ---- a page, in both orientations ----------------------------------------------------------
for o in portrait landscape; do
    start_shell "page-$o" $o "https://doors.test/"
    "$POS" app start browser >/dev/null 2>&1; sleep 1.5
    check "$o: the demo page is made: 14 blocks, 5 links, 1 picture" \
        "$(logs "page-$o" | grep -q 'browser: page made: 14 blocks, 5 links, 1 pictures' && echo 1 || echo 0)"
    check "$o: one helper, on the fake network" \
        "$([ "$(helpers)" = 1 ] && grep -q 'session start, fake' "$T/page-$o/log/pos-browser.log" 2>/dev/null &&
           echo 1 || echo 0)"
    shot "$T/browser-page-$o.png"
    python3 - "$T/browser-page-$o.png" <<'PY' >"$T/px" 2>/dev/null
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "docs/design/timber-art/tools")
from pngio import read_png
W, H, rows = read_png(sys.argv[1])
# The fake picture is a colour gradient no theme colour comes near: strongly
# green pixels are its top rows, which are on screen in both orientations.
green = sum(1 for r in rows for p in r if p[1] > 150 and p[0] < 60)
print(1 if green > 500 else 0, green)
PY
    set -- $(cat "$T/px")
    check "$o: the picture is on screen ($2 px)" "${1:-0}"
    "$POS" app home >/dev/null 2>&1; sleep 0.5
    check "$o: leaving ends the helper and removes its picture directory" \
        "$([ "$(helpers)" = 0 ] && [ -z "$(ls "$T/page-$o/run" | grep '^browser\.')" ] && echo 1 || echo 0)"
    check "$o: quiet" "$(quiet "page-$o")"
    stop_shell
done

# ---- an error page, and a theme change under a page ---------------------------------------------
start_shell error portrait "refused.doors.test"
"$POS" app start browser >/dev/null 2>&1; sleep 1
check "a refused connection is reported by the helper as such" \
    "$(grep -q 'browser: failed: connect' "$T/error/log/pos-browser.log" 2>/dev/null && echo 1 || echo 0)"
shot "$T/browser-error-portrait.png"
"$POS" app home >/dev/null 2>&1; sleep 0.3
stop_shell
start_shell theme portrait "https://doors.test/long"
"$POS" app start browser >/dev/null 2>&1; sleep 1.5
"$POS" shell theme doors night >/dev/null 2>&1; sleep 1
check "a theme change under an open page makes the page again, in the new colours" \
    "$([ "$(grep -c 'browser: page made: 301 blocks' "$T/theme/out")" = 2 ] && echo 1 || echo 0)"
"$POS" app home >/dev/null 2>&1; sleep 0.3
check "and nothing faults" "$(quiet theme)"
stop_shell

# ---- thirty open/close cycles ---------------------------------------------------------------------
start_shell cycles portrait "https://doors.test/"
for i in $(seq 30); do
    "$POS" app start browser >/dev/null 2>&1; sleep 0.5
    "$POS" app home >/dev/null 2>&1; sleep 0.15
    [ "$i" = 10 ] && at10=$(rss)
done
sleep 0.5
at30=$(rss)
check "thirty open/close cycles, each loading a page: no helper left" "$([ "$(helpers)" = 0 ] && echo 1 || echo 0)"
check "no picture directory left" "$([ -z "$(ls "$T/cycles/run" | grep '^browser\.')" ] && echo 1 || echo 0)"
check "the shell's memory does not grow from the tenth cycle to the thirtieth (${at10} -> ${at30} KB)" \
    "$([ -n "$at10" ] && [ -n "$at30" ] && [ $((at30 - at10)) -lt 512 ] && echo 1 || echo 0)"
check "the shell is still up" "$(kill -0 "$PID" 2>/dev/null && echo 1 || echo 0)"
check "quiet through all of it" "$(quiet cycles)"

# ---- the shell dies ---------------------------------------------------------------------------------
stop_shell
start_shell killed portrait "https://doors.test/slow"
"$POS" app start browser >/dev/null 2>&1; sleep 0.8
hp=$(pgrep -P "$PID" -f pos-browser)
kill -9 "$PID"; wait "$PID" 2>/dev/null; PID=""
gone=0
for _ in $(seq 30); do
    if [ -n "$hp" ] && ! kill -0 "$hp" 2>/dev/null; then gone=1; break; fi
    sleep 0.1
done
check "a helper whose shell is killed leaves on its own (PR_SET_PDEATHSIG)" "$([ -n "$hp" ] && [ "$gone" = 1 ] && echo 1 || echo 0)"

echo "browser_shell_test: $failed failure(s)"
exit $((failed > 0))
