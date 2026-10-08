#!/bin/bash
# PG 2048 in the running app, and the shell's side of it.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${G2048_APP_TEST:-$(dirname "$SHELL_BIN")/g2048_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy timeout 300 "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|g2048_app_test:'
    check "PG 2048 end to end: keys, swipes, buttons, saves, every mode" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL g2048_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

ROT=portrait
run_shell() { # <state dir> <log dir> [env...]
    local state=$1 logd=$2; shift 2
    env SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$logd" \
        POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$state" "$@" \
        "$SHELL_BIN" --no-lock --rotation "$ROT" --open 2048 --exit-after-ms 1500 >"$logd/out" 2>&1
}

# The DOORS launcher lists it with the other games (ui/shell/home_layout.c,
# PLAY); there is no fixed tile grid to fit any more (DS §31).
L0=$(mktemp -d); R0=$(mktemp -d); C0=$(mktemp -d); S0=$(mktemp -d)
env SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$R0" POCKETOS_LOG_DIR="$L0" POCKETOS_CONFIG_DIR="$C0" \
    POCKETOS_STATE_DIR="$S0" "$SHELL_BIN" --no-lock --exit-after-ms 800 >"$L0/out" 2>&1
check "the launcher builds with the game among its apps" \
    "$(grep -qE 'launcher: [0-9]+ group\(s\), 27 app\(s\)' "$L0/shell.log" 2>/dev/null && echo 1 || echo 0)"
rm -rf "$L0" "$R0" "$C0" "$S0"

RUN=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d); LOGD=$(mktemp -d)
run_shell "$STATE" "$LOGD"; rc=$?
check "the shell opens 2048" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "the shell wrote its log" "$([ -s "$LOGD/shell.log" ] && echo 1 || echo 0)"
check "and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 0 || echo 1)"
check "2048 reports itself open" "$(grep -q 'open app 2048' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "and closed" "$(grep -q 'close app 2048' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "the shell did not call it unknown" \
    "$(grep -q 'unknown app 2048' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 0 || echo 1)"
check "opening an untouched game writes nothing" \
    "$([ -z "$(ls -A "$STATE" 2>/dev/null)" ] && echo 1 || echo 0)"

# Every review state renders without a fault and without touching the store,
# in both orientations (DS §21).
for ROT in portrait landscape; do
    for screen in play confirm won over; do
        L2=$(mktemp -d)
        run_shell "$STATE" "$L2" PG2048_SCREEN=$screen; rc=$?
        check "$ROT: review state $screen renders" "$([ "$rc" = "0" ] &&
            ! grep -qE ' ERROR |assert' "$L2/out" "$L2/shell.log" 2>/dev/null && echo 1 || echo 0)"
        rm -rf "$L2"
    done
done
ROT=portrait
check "no review state wrote a save" "$([ -z "$(ls -A "$STATE" 2>/dev/null)" ] && echo 1 || echo 0)"

# A damaged save does not stop the app opening.
mkdir -p "$STATE/2048" && printf 'garbage' > "$STATE/2048/game.v1"
L3=$(mktemp -d)
run_shell "$STATE" "$L3"; rc=$?
check "a damaged save still opens cleanly" "$([ "$rc" = "0" ] &&
    ! grep -qE ' ERROR |assert' "$L3/out" "$L3/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "and is left in place until there is something to save" \
    "$([ "$(cat "$STATE/2048/game.v1")" = "garbage" ] && echo 1 || echo 0)"

rm -rf "$RUN" "$CFG" "$STATE" "$LOGD" "$L3"
echo "g2048_shell_test: $failed failure(s)"
exit $((failed > 0))
