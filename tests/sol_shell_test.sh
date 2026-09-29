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

# The DOORS launcher lists it with the other games (ui/shell/home_layout.c,
# PLAY); there is no fixed tile grid to fit any more (DS §31).
L0=$(mktemp -d); R0=$(mktemp -d); C0=$(mktemp -d); S0=$(mktemp -d)
env SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$R0" POCKETOS_LOG_DIR="$L0" POCKETOS_CONFIG_DIR="$C0" \
    POCKETOS_STATE_DIR="$S0" "$SHELL_BIN" --no-lock --exit-after-ms 800 >"$L0/out" 2>&1
check "the launcher builds with the game among its apps" \
    "$(grep -qE 'launcher: [0-9]+ group\(s\), 21 app\(s\)' "$L0/shell.log" 2>/dev/null && echo 1 || echo 0)"
rm -rf "$L0" "$R0" "$C0" "$S0"

BUILD=$(dirname "$SHELL_BIN")
check "the configured build generated the card palette" "$([ -s "$BUILD/sol_palette.c" ] && echo 1 || echo 0)"
check "with every ink the palette declares" \
    "$([ "$(grep -cE '^[[:space:]]+\[SOL_INK_[A-Z_]+\] = \{' "$BUILD/sol_palette.c" 2>/dev/null)" = "8" ] &&
       echo 1 || echo 0)"

RUN=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
for rot in portrait landscape; do
    for screen in "" deal selected keyboard confirm won; do
        LOGD=$(mktemp -d)
        env SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" POCKETOS_CONFIG_DIR="$CFG" \
            POCKETOS_STATE_DIR="$STATE" ${screen:+PGSOLITAIRE_SCREEN=$screen} \
            "$SHELL_BIN" --no-lock --rotation "$rot" --open solitaire --exit-after-ms 1500 >"$LOGD/out" 2>&1
        rc=$?
        name=$rot\ ${screen:-a fresh deal}
        check "the shell opens Solitaire on $name" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
        check "$name: no fault logged" "$([ -s "$LOGD/shell.log" ] &&
            ! grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" && echo 1 || echo 0)"
        check "$name: opened and closed" "$(grep -q 'open app solitaire' "$LOGD/shell.log" &&
            grep -q 'close app solitaire' "$LOGD/shell.log" && echo 1 || echo 0)"
        rm -rf "$LOGD"
    done
done
check "an untouched deal and the review states wrote nothing" "$([ -z "$(ls -A "$STATE")" ] && echo 1 || echo 0)"

# A damaged save does not stop the app opening, and is left for the next save.
mkdir -p "$STATE/solitaire" && printf 'garbage' > "$STATE/solitaire/game.v1"
LOGD=$(mktemp -d)
env SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" POCKETOS_CONFIG_DIR="$CFG" \
    POCKETOS_STATE_DIR="$STATE" "$SHELL_BIN" --no-lock --open solitaire --exit-after-ms 1500 >"$LOGD/out" 2>&1
rc=$?
check "a damaged save still opens cleanly" "$([ "$rc" = "0" ] &&
    ! grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "and is left in place until there is something to save" \
    "$([ "$(cat "$STATE/solitaire/game.v1")" = "garbage" ] && echo 1 || echo 0)"
rm -rf "$RUN" "$CFG" "$STATE" "$LOGD"

echo "sol_shell_test: $failed failure(s)"
exit $((failed > 0))
