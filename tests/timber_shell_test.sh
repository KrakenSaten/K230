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
has() { if printf '%s' "$3" | grep -q -- "$2"; then echo "ok   $1"; else
        echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | tail -5; failed=$((failed + 1)); fi; }
hasnt() { if printf '%s' "$3" | grep -q -- "$2"; then
        echo "FAIL $1: unexpected '$2'"; failed=$((failed + 1)); else echo "ok   $1"; fi; }

run() { # <screen|-> <name>
    if [ "$1" = "-" ]; then unset POCKETTIMBER_SCREEN; else export POCKETTIMBER_SCREEN="$1"; fi
    "$SHELL_BIN" --open timber --screenshot "$OUT/timber-$2.png" --exit-after-ms 900 \
        >"$OUT/timber-$2.log" 2>&1
    cat "$OUT/timber-$2.log"
}

# 0. The input path under a real LVGL pointer device: the D3 bench sequence
#    (a pull let go part way, a wandering finger, leaving and reopening).
#    Built beside the shell by ui/shell/CMakeLists.txt; a missing binary is
#    a failure, not a skip.
TIMBER_INPUT_TEST=${TIMBER_INPUT_TEST:-$(dirname "$SHELL_BIN")/timber_input_test}
if [ -x "$TIMBER_INPUT_TEST" ]; then
    log=$("$TIMBER_INPUT_TEST" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|timber_input_test:'
    check "the input path under a pointer device" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL timber_input_test binary missing: $TIMBER_INPUT_TEST"; failed=$((failed + 1))
fi

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

# 3. A finished run stores its record, atomically (D2). Step 1 played the
#    "result" state to its end in this same state directory.
RECORD="$POCKETOS_STATE_DIR/timber/record.v1"
RECORD_BYTES=52
check "a finished run wrote a record" "$([ -f "$RECORD" ] && echo 1 || echo 0)"
check "the record is one blob" \
      "$([ "$(wc -c < "$RECORD" 2>/dev/null)" = "$RECORD_BYTES" ] && echo 1 || echo 0)"
check "no temporary file left" "$([ -f "$RECORD.tmp" ] && echo 0 || echo 1)"
cp "$RECORD" "$OUT/record.good" 2>/dev/null

# 4. The next launch reads it back and has a best score to beat.
log=$(run - reopen)
has "the stored record is read back" 'best score' "$log"
hasnt "no error reading it back" 'ERROR' "$log"

# 5. A damaged record is refused, and the app opens anyway with nothing to beat.
printf 'PTR1..............damaged.....' > "$RECORD"
log=$(run - damaged)
has "a damaged record is rejected" 'rejected' "$log"
check "the app still renders" "$([ -s "$OUT/timber-damaged.png" ] && echo 1 || echo 0)"
check "a damaged record is left alone" "$([ -f "$RECORD" ] && echo 1 || echo 0)"
hasnt "a damaged record is not an error" 'ERROR' "$log"

# 6. A truncated record: the good one with its tail cut off.
head -c 20 "$OUT/record.good" > "$RECORD"
log=$(run - truncated)
has "a truncated record is rejected" 'rejected' "$log"
check "the app still renders" "$([ -s "$OUT/timber-truncated.png" ] && echo 1 || echo 0)"
hasnt "a truncated record is not an error" 'ERROR' "$log"

# 7. A file of exactly the right size that is not ours at all.
head -c "$RECORD_BYTES" /dev/zero | tr '\0' 'Z' > "$RECORD"
log=$(run - foreign)
has "a foreign record of the right size is rejected" 'rejected' "$log"
check "the app still renders" "$([ -s "$OUT/timber-foreign.png" ] && echo 1 || echo 0)"

# 8. A record from a version this build does not know.
{ head -c 4 "$OUT/record.good"; printf '\002'; tail -c +6 "$OUT/record.good"; } > "$RECORD"
log=$(run - newer)
has "a newer record is rejected" 'rejected' "$log"
check "the app still renders" "$([ -s "$OUT/timber-newer.png" ] && echo 1 || echo 0)"
check "a newer record is left alone" \
      "$([ "$(wc -c < "$RECORD" 2>/dev/null)" = "$RECORD_BYTES" ] && echo 1 || echo 0)"

# 9. An unwritable directory is reported and never stops play. It is the
#    directory the file itself goes in that has to be locked.
rm -f "$RECORD"
if [ "$(id -u)" != "0" ]; then
    chmod 0555 "$POCKETOS_STATE_DIR/timber"
    log=$(run result unwritable)
    check "the run still completes" "$([ -s "$OUT/timber-unwritable.png" ] && echo 1 || echo 0)"
    has "a failed write is reported" 'session-only' "$log"
    hasnt "a failed write is not an error" 'ERROR' "$log"
    check "nothing was written" "$([ -f "$RECORD" ] && echo 0 || echo 1)"
    chmod 0755 "$POCKETOS_STATE_DIR/timber"
else
    echo "skip unwritable-directory checks (running as root)"
fi

# 10. The good record put back, another finished run folds into it and the
#     file is replaced in place.
cp "$OUT/record.good" "$RECORD"
log=$(run result again)
has "the record was read before the run" 'best score' "$log"
check "the record was replaced" \
      "$([ "$(wc -c < "$RECORD" 2>/dev/null)" = "$RECORD_BYTES" ] && echo 1 || echo 0)"
check "no temporary file left" "$([ -f "$RECORD.tmp" ] && echo 0 || echo 1)"
hasnt "no error while replacing" 'ERROR' "$log"

# 11. The placeholder path stays alive beside the art (POCKETTIMBER_ART):
#    forcing it renders the same state with the flat blocks.
export POCKETTIMBER_PLACEHOLDER=1
log=$(run run placeholder)
check "the placeholder path renders" "$([ -s "$OUT/timber-placeholder.png" ] && echo 1 || echo 0)"
hasnt "no error on the placeholder path" 'ERROR' "$log"
unset POCKETTIMBER_PLACEHOLDER

# 12. Reduced motion (DS section 12): the sway is not drawn, the tower is.
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
