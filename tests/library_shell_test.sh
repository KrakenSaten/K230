#!/bin/bash
# Library in the real shell (the simulator, built with -DPOCKETOS_WITH_LIBRARY=ON)
# with pos-library and a real archive (docs/apps/LIBRARY.md "Validation"):
# a reading journey played by POCKETOS_LIBRARY_STEPS - search, open a result,
# follow a link inside the collection, BACK, bookmark, the collection's page -
# in portrait and landscape, captured at each step; the bookmark on the start
# page after the app is closed and opened again; no helper left after
# closing; a missing collection; and the whole journey with no network.
#
#   SHELL_BIN=... LIBRARY_ARCHIVE=... tests/library_shell_test.sh tools/library/pos-library
#
# LIBRARY_SHOTS=<dir> keeps the screenshots. The words fit
# wikipedia_en_climate-change_maxi_2026-10 (docs/apps/library/CONTENT.md).
set -u
HELPER=$(realpath "${1:?usage: library_shell_test.sh pos-library}")
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary built with POCKETOS_WITH_LIBRARY=ON}
ARCHIVE=${LIBRARY_ARCHIVE:?set LIBRARY_ARCHIVE to a .zim file}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
POS=$(pwd)/tools/pos/pos
[ -x "$POS" ] || { echo "FAIL build it first: $POS"; exit 1; }
export SDL_VIDEODRIVER=dummy POCKETOS_LIBRARY_HELPER="$HELPER" POCKETOS_LIBRARY_STEP_MS=2500
T=$(mktemp -d)
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
STEPS='find:greenhouse effect|link:Greenhouse effect|link:greenhouse gases|back|mark|open:https://library.invalid/~library/about'

start_shell() { # <name> <rotation> [prefix command...]
    local name=$1 rot=$2
    shift 2
    local d="$T/$name"
    mkdir -p "$d/run" "$d/log" "$d/cfg" "$d/state"
    POCKETOS_RUNTIME_DIR="$d/run" POCKETOS_LOG_DIR="$d/log" POCKETOS_CONFIG_DIR="$d/cfg" \
    POCKETOS_STATE_DIR="${STATE_DIR:-$d/state}" "$@" \
        "$SHELL_BIN" --no-lock --rotation "$rot" --exit-after-ms 300000 >"$d/out" 2>&1 &
    PID=$!
    export POCKETOS_RUNTIME_DIR="$d/run"
    for _ in $(seq 50); do [ -S "$d/run/shell.sock" ] && break; sleep 0.1; done
}
stop_shell() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=""; }
logs() { cat "$T/$1/out" "$T/$1/log/shell.log" 2>/dev/null; }
helpers() { pgrep -f "^$HELPER session" | wc -l; }
shot() { "$POS" shell screenshot "$1" >/dev/null 2>&1; [ -n "${LIBRARY_SHOTS:-}" ] && cp "$1" "$LIBRARY_SHOTS/"; }
wait_log() { # <name> <pattern> [seconds]
    local i
    for i in $(seq $((${3:-20} * 10))); do logs "$1" | grep -q "$2" && return 0; sleep 0.1; done
    return 1
}
quiet() {
    local hits
    hits=$(logs "$1" | grep -E ' ERROR | WARN |\[(Warn|Error)\]|assert' | grep -v 'radiod unavailable')
    [ -n "$hits" ] && echo "$hits" | head -3
    [ -z "$hits" ] && echo 1 || echo 0
}

journey() { # <name> <rotation> [prefix command...]
    local name=$1 rot=$2 n
    shift 2
    POCKETOS_LIBRARY_ARCHIVE="$ARCHIVE" POCKETOS_LIBRARY_STEPS="$STEPS" start_shell "$name" "$rot" "$@"
    "$POS" app start library >/dev/null 2>&1
    check "$name: Library opens on its start page, no helper yet" \
        "$(wait_log "$name" 'library: start page in' 5 && [ "$(helpers)" = 0 ] && echo 1 || echo 0)"
    shot "$T/library-$name-0-start.png"
    for n in 1 2 3 4 5 6; do
        if wait_log "$name" "library: step $n shown" 40; then
            shot "$T/library-$name-$n.png"
        else
            echo "  (step $n never shown)"
        fi
    done
    check "$name: the search made a results page" \
        "$(logs "$name" | grep -qE 'library: page made: [0-9]+ blocks, [0-9]+ links' && echo 1 || echo 0)"
    check "$name: the steps all ran" "$(wait_log "$name" 'library: steps done' 10 && echo 1 || echo 0)"
    check "$name: the article's pictures arrived (pos-library log: pages with pictures)" \
        "$(grep -qE 'library: page in [0-9]+ ms: [0-9]+ blocks, [0-9]+ links, [1-9][0-9]* pictures' "$T/$name/log/pos-library.log" &&
           echo 1 || echo 0)"
    check "$name: no link was missing" "$(logs "$name" | grep -q 'no link with those words' && echo 0 || echo 1)"
    check "$name: exactly one helper while open" "$([ "$(helpers)" = 1 ] && echo 1 || echo 0)"
    "$POS" app home >/dev/null 2>&1
    sleep 1
    check "$name: leaving Library ends the helper" "$([ "$(helpers)" = 0 ] && echo 1 || echo 0)"
    check "$name: and leaves no picture directory" "$(ls "$T/$name/run" | grep -q '^browser\.' && echo 0 || echo 1)"
    check "$name: quiet" "$(quiet "$name")"
    stop_shell
}

journey portrait portrait
journey landscape landscape

# ---- the bookmark after reopening --------------------------------------------------------
STATE_DIR="$T/portrait/state" POCKETOS_LIBRARY_ARCHIVE="$ARCHIVE" start_shell reopen portrait
"$POS" app start library >/dev/null 2>&1
check "reopened: the start page lists one bookmark" \
    "$(wait_log reopen 'library: start page in [0-9]* ms, 1 bookmark' 5 && echo 1 || echo 0)"
check "and it is the article, kept with its title, 0600" \
    "$(grep -qE '^bookmark	https://library.invalid/Greenhouse_effect	Greenhouse effect' "$T/portrait/state/library/state" &&
       [ "$(stat -c %a "$T/portrait/state/library/state")" = 600 ] && echo 1 || echo 0)"
sleep 1
shot "$T/library-reopen-start.png"
"$POS" app home >/dev/null 2>&1
stop_shell

# ---- a light theme: the bookmark again, under another palette -------------------------------
STATE_DIR="$T/portrait/state" POCKETOS_LIBRARY_ARCHIVE="$ARCHIVE" \
    POCKETOS_LIBRARY_STEPS='open:https://library.invalid/Greenhouse_effect' start_shell light portrait
"$POS" shell theme brass outdoor >/dev/null 2>&1
"$POS" app start library >/dev/null 2>&1
check "a light theme: the bookmarked article opens and is drawn again" \
    "$(wait_log light 'library: step 1 shown' 30 && echo 1 || echo 0)"
shot "$T/library-light-article.png"
"$POS" app home >/dev/null 2>&1
check "a light theme: quiet" "$(quiet light)"
stop_shell

# ---- no collection --------------------------------------------------------------------------
POCKETOS_LIBRARY_ARCHIVE="$T/none.zim" POCKETOS_LIBRARY_STEPS='open:https://library.invalid/~library/main' \
    start_shell missing portrait
"$POS" app start library >/dev/null 2>&1
check "a missing archive: an error page, not a crash" \
    "$(wait_log missing 'library: step 1 shown' 20 && grep -q 'collection not opened' "$T/missing/log/pos-library.log" &&
       echo 1 || echo 0)"
shot "$T/library-missing.png"
"$POS" app home >/dev/null 2>&1
stop_shell

# ---- the same journey with no network at all ----------------------------------------------
if unshare -rn true 2>/dev/null; then
    journey offline portrait unshare -rn
else
    echo "skip offline: no network namespaces here"
fi

echo "library_shell_test: $failed failure(s)"
exit $((failed > 0))
