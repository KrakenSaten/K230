#!/bin/bash
# DeskBuddy in the running app, and the shell's side of it.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${DESKBUDDY_APP_TEST:-$(dirname "$SHELL_BIN")/deskbuddy_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy timeout 600 "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|^note|deskbuddy_app_test:'
    check "DeskBuddy end to end: events, buttons, saves, reopen, timers, both orientations" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL deskbuddy_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

ROT=portrait
run_shell() { # <state dir> <log dir> [env...]
    local state=$1 logd=$2; shift 2
    env SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$logd" \
        POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$state" "$@" \
        "$SHELL_BIN" --no-lock --rotation "$ROT" --open deskbuddy --exit-after-ms 2500 >"$logd/out" 2>&1
}
clean() { ! grep -qE ' ERROR |assert|AddressSanitizer|runtime error' "$1/out" "$1/shell.log" 2>/dev/null; }

# The DOORS launcher lists it in WORKSPACE (ui/shell/home_layout.c).
L0=$(mktemp -d); R0=$(mktemp -d); C0=$(mktemp -d); S0=$(mktemp -d)
env SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$R0" POCKETOS_LOG_DIR="$L0" POCKETOS_CONFIG_DIR="$C0" \
    POCKETOS_STATE_DIR="$S0" "$SHELL_BIN" --no-lock --exit-after-ms 800 >"$L0/out" 2>&1
check "the launcher builds with DeskBuddy among its apps" \
    "$(grep -qE 'launcher: [0-9]+ group\(s\), 25 app\(s\)' "$L0/shell.log" 2>/dev/null && echo 1 || echo 0)"
rm -rf "$L0" "$R0" "$C0" "$S0"

RUN=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d); LOGD=$(mktemp -d)
run_shell "$STATE" "$LOGD"; rc=$?
check "the shell opens DeskBuddy" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "and logs no fault" "$(clean "$LOGD" && echo 1 || echo 0)"
check "DeskBuddy reports itself open" "$(grep -q 'open app deskbuddy' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "and closed" "$(grep -q 'close app deskbuddy' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "the shell did not call it unknown" \
    "$(grep -q 'unknown app deskbuddy' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 0 || echo 1)"
check "opening it without vision writes nothing" "$([ -z "$(ls -A "$STATE" 2>/dev/null)" ] && echo 1 || echo 0)"

# The simulation path (docs/apps/DESKBUDDY.md, "Test and demo controls"):
# every mode, with a script that walks the states, in both orientations
# (DS §21), without a fault and without touching the store.
SCRIPT="100:person 900:owner@930 1500:none 1700:unknown@800 2200:none"
for ROT in portrait landscape; do
    for mode in companion armed night; do
        L2=$(mktemp -d)
        run_shell "$STATE" "$L2" DESKBUDDY_SIM="$SCRIPT" DESKBUDDY_MODE=$mode; rc=$?
        check "$ROT: $mode with simulated vision runs" "$([ "$rc" = "0" ] && clean "$L2" && echo 1 || echo 0)"
        rm -rf "$L2"
    done
done
ROT=portrait
check "no simulation wrote to the store" "$([ -z "$(ls -A "$STATE" 2>/dev/null)" ] && echo 1 || echo 0)"

# Damaged files do not stop it opening, and are left until there is
# something to save.
mkdir -p "$STATE/deskbuddy" && printf 'garbage\n\001\002' > "$STATE/deskbuddy/prefs.v1" &&
    printf 'e x y\n' > "$STATE/deskbuddy/guard.v1"
L3=$(mktemp -d)
run_shell "$STATE" "$L3"; rc=$?
check "damaged files still open cleanly" "$([ "$rc" = "0" ] && clean "$L3" && echo 1 || echo 0)"
check "and are left in place" "$([ "$(head -1 "$STATE/deskbuddy/prefs.v1")" = "garbage" ] &&
    [ "$(cat "$STATE/deskbuddy/guard.v1")" = "e x y" ] && echo 1 || echo 0)"

rm -rf "$RUN" "$CFG" "$STATE" "$LOGD" "$L3"
echo "deskbuddy_shell_test: $failed failure(s)"
exit $((failed > 0))
