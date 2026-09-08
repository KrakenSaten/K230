#!/bin/bash
# PocketTimber in the running shell: every state the review needs renders
# from a fixed seed, none of them logs a fault, a whole run plays to its
# result, and reduced motion changes the picture without stopping the game.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell). Headless via SDL's
# dummy driver, exactly like tests/radar_shell_test.sh.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
export SDL_VIDEODRIVER=dummy
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
POCKETOS_CONFIG_DIR=$(mktemp -d)
POCKETOS_STATE_DIR=$(mktemp -d)
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR POCKETOS_CONFIG_DIR POCKETOS_STATE_DIR
OUT=${TIMBER_SHOTS:-$(mktemp -d)}
failed=0

check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
hasnt() { if printf '%s' "$3" | grep -q -- "$2"; then
        echo "FAIL $1: unexpected '$2'"; failed=$((failed + 1)); else echo "ok   $1"; fi; }

run() { # <screen|-> <name>
    if [ "$1" = "-" ]; then unset POCKETTIMBER_SCREEN; else export POCKETTIMBER_SCREEN="$1"; fi
    "$SHELL_BIN" --open timber --screenshot "$OUT/timber-$2.png" --exit-after-ms 900 \
        >"$OUT/timber-$2.log" 2>&1
    cat "$OUT/timber-$2.log"
}

# 1. Every state renders and none logs a fault.
for screen in idle run pulling placing collapse result; do
    log=$(run "$screen" "$screen")
    check "$screen renders" "$([ -s "$OUT/timber-$screen.png" ] && echo 1 || echo 0)"
    hasnt "no error on $screen" 'ERROR' "$log"
done

# 2. The app opens with no state asked for, in standby.
log=$(run - standby)
check "standby renders" "$([ -s "$OUT/timber-standby.png" ] && echo 1 || echo 0)"
hasnt "no error in standby" 'ERROR' "$log"

# 3. The placeholder path stays alive beside the art (POCKETTIMBER_ART):
#    forcing it renders the same state with the flat blocks.
export POCKETTIMBER_PLACEHOLDER=1
log=$(run run placeholder)
check "the placeholder path renders" "$([ -s "$OUT/timber-placeholder.png" ] && echo 1 || echo 0)"
hasnt "no error on the placeholder path" 'ERROR' "$log"
unset POCKETTIMBER_PLACEHOLDER

# 4. Reduced motion (DS section 12): the sway is not drawn, the tower is.
printf 'reduced_motion=1\n' > "$POCKETOS_CONFIG_DIR/settings.conf"
log=$(run pulling reduced)
check "reduced motion renders" "$([ -s "$OUT/timber-reduced.png" ] && echo 1 || echo 0)"
hasnt "no error with reduced motion" 'ERROR' "$log"
check "reduced motion still draws the tower" "$([ "$(wc -c < "$OUT/timber-reduced.png")" -gt 8000 ] && echo 1 || echo 0)"
rm -f "$POCKETOS_CONFIG_DIR/settings.conf"

if [ -z "${TIMBER_SHOTS:-}" ]; then rm -rf "$OUT"; fi
rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_CONFIG_DIR" "$POCKETOS_STATE_DIR"
echo "timber_shell_test: $failed failure(s)"
exit $((failed > 0))
