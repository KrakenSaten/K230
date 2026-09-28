#!/bin/bash
# PG Solitaire in the running app, and the shell's side of it.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${SOL_APP_TEST:-$(dirname "$SHELL_BIN")/sol_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy timeout 300 "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|sol_app_test:'
    check "PG Solitaire end to end: keys, taps, buttons, every mode" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL sol_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

check "the launcher grid still has six rows" \
    "$(sed -n '/^static void home_create/,/^}/p' ui/shell/shell.c |
       grep -o 'LV_GRID_CONTENT' | wc -l | grep -qx 6 && echo 1 || echo 0)"
check "and twelve apps, which six rows of two hold" \
    "$(sed -n '/^static const struct pocketos_app \*apps\[\]/,/};/p' ui/shell/shell.c |
       grep -o '&app_[a-z0-9]*' | wc -l | grep -qx 12 && echo 1 || echo 0)"

BUILD=$(dirname "$SHELL_BIN")
check "the configured build generated the card palette" "$([ -s "$BUILD/sol_palette.c" ] && echo 1 || echo 0)"
check "with every ink the palette declares" \
    "$([ "$(grep -cE '^[[:space:]]+\[SOL_INK_[A-Z_]+\] = \{' "$BUILD/sol_palette.c" 2>/dev/null)" = "8" ] &&
       echo 1 || echo 0)"

RUN=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
for screen in "" deal selected keyboard confirm won; do
    LOGD=$(mktemp -d)
    env SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" POCKETOS_CONFIG_DIR="$CFG" \
        POCKETOS_STATE_DIR="$STATE" ${screen:+PGSOLITAIRE_SCREEN=$screen} \
        "$SHELL_BIN" --open solitaire --exit-after-ms 1500 >"$LOGD/out" 2>&1
    rc=$?
    name=${screen:-a fresh deal}
    check "the shell opens Solitaire on $name" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    check "$name: no fault logged" "$([ -s "$LOGD/shell.log" ] &&
        ! grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" && echo 1 || echo 0)"
    check "$name: opened and closed" "$(grep -q 'open app solitaire' "$LOGD/shell.log" &&
        grep -q 'close app solitaire' "$LOGD/shell.log" && echo 1 || echo 0)"
    rm -rf "$LOGD"
done
check "an untouched deal and the review states wrote nothing" "$([ -z "$(ls -A "$STATE")" ] && echo 1 || echo 0)"

# A damaged save does not stop the app opening, and is left for the next save.
mkdir -p "$STATE/solitaire" && printf 'garbage' > "$STATE/solitaire/game.v1"
LOGD=$(mktemp -d)
env SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" POCKETOS_CONFIG_DIR="$CFG" \
    POCKETOS_STATE_DIR="$STATE" "$SHELL_BIN" --open solitaire --exit-after-ms 1500 >"$LOGD/out" 2>&1
rc=$?
check "a damaged save still opens cleanly" "$([ "$rc" = "0" ] &&
    ! grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "and is left in place until there is something to save" \
    "$([ "$(cat "$STATE/solitaire/game.v1")" = "garbage" ] && echo 1 || echo 0)"
rm -rf "$RUN" "$CFG" "$STATE" "$LOGD"

echo "sol_shell_test: $failed failure(s)"
exit $((failed > 0))
