#!/bin/bash
# PG Blackjack in the running app, and the shell's side of it.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${BJ_APP_TEST:-$(dirname "$SHELL_BIN")/bj_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy timeout 300 "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|bj_app_test:'
    check "PG Blackjack end to end: keys, buttons, rounds, every mode" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL bj_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

check "the launcher grid still has six rows" \
    "$(sed -n '/^static void home_create/,/^}/p' ui/shell/shell.c |
       grep -o 'LV_GRID_CONTENT' | wc -l | grep -qx 6 && echo 1 || echo 0)"
check "and twelve apps, which six rows of two hold" \
    "$(sed -n '/^static const struct pocketos_app \*apps\[\]/,/};/p' ui/shell/shell.c |
       grep -o '&app_[a-z0-9]*' | wc -l | grep -qx 12 && echo 1 || echo 0)"

BUILD=$(dirname "$SHELL_BIN")
check "the configured build generated the card palette with every ink" \
    "$([ "$(grep -cE '^[[:space:]]+\[BJ_INK_[A-Z_]+\] = \{' "$BUILD/bj_palette.c" 2>/dev/null)" = "8" ] &&
       echo 1 || echo 0)"

RUN=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
for screen in "" bet play win blackjack bust broke; do
    LOGD=$(mktemp -d)
    env SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" POCKETOS_CONFIG_DIR="$CFG" \
        POCKETOS_STATE_DIR="$STATE" ${screen:+PGBLACKJACK_SCREEN=$screen} \
        "$SHELL_BIN" --open blackjack --exit-after-ms 1500 >"$LOGD/out" 2>&1
    rc=$?
    name=${screen:-a new session}
    check "the shell opens Blackjack on $name" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    check "$name: no fault logged" "$([ -s "$LOGD/shell.log" ] &&
        ! grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" && echo 1 || echo 0)"
    check "$name: opened and closed" "$(grep -q 'open app blackjack' "$LOGD/shell.log" &&
        grep -q 'close app blackjack' "$LOGD/shell.log" && echo 1 || echo 0)"
    rm -rf "$LOGD"
done
check "a new session nobody touched and the review states wrote nothing" \
    "$([ -z "$(ls -A "$STATE")" ] && echo 1 || echo 0)"

# A damaged save does not stop the app opening, and is left for the next save.
mkdir -p "$STATE/blackjack" && printf 'garbage' > "$STATE/blackjack/game.v1"
LOGD=$(mktemp -d)
env SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" POCKETOS_CONFIG_DIR="$CFG" \
    POCKETOS_STATE_DIR="$STATE" "$SHELL_BIN" --open blackjack --exit-after-ms 1500 >"$LOGD/out" 2>&1
rc=$?
check "a damaged save still opens cleanly" "$([ "$rc" = "0" ] &&
    ! grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "and is left in place until there is something to save" \
    "$([ "$(cat "$STATE/blackjack/game.v1")" = "garbage" ] && echo 1 || echo 0)"
rm -rf "$RUN" "$CFG" "$STATE" "$LOGD"

echo "bj_shell_test: $failed failure(s)"
exit $((failed > 0))
