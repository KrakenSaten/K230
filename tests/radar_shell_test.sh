#!/bin/bash
# PocketRadar in the running shell: every screen renders, a finished run
# stores its record, the next launch reads it back, and every storage failure
# path leaves the game fully playable rather than blocking it.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell). Headless via SDL's
# dummy driver, exactly like tests/fleet_shell_test.sh.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
export SDL_VIDEODRIVER=dummy
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
POCKETOS_CONFIG_DIR=$(mktemp -d)
POCKETOS_STATE_DIR=$(mktemp -d)
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR POCKETOS_CONFIG_DIR POCKETOS_STATE_DIR
OUT=$(mktemp -d)
RECORD="$POCKETOS_STATE_DIR/radar/record.v1"
RECORD_BYTES=30
failed=0

check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
has() { if printf '%s' "$3" | grep -q -- "$2"; then echo "ok   $1"; else
        echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | tail -5; failed=$((failed + 1)); fi; }
hasnt() { if printf '%s' "$3" | grep -q -- "$2"; then
        echo "FAIL $1: unexpected '$2'"; failed=$((failed + 1)); else echo "ok   $1"; fi; }

run() { # <screen|-> <name>
    if [ "$1" = "-" ]; then unset POCKETRADAR_SCREEN; else export POCKETRADAR_SCREEN="$1"; fi
    "$SHELL_BIN" --open radar --screenshot "$OUT/$2.png" --exit-after-ms 900 \
        >"$OUT/$2.log" 2>&1
    cat "$OUT/$2.log"
}

# 1. Every state the design review needs renders, and none of them logs a fault.
for screen in idle scan selected acquired decoy; do
    log=$(run "$screen" "$screen")
    check "$screen renders" "$([ -s "$OUT/$screen.png" ] && echo 1 || echo 0)"
    hasnt "no error on $screen" 'ERROR' "$log"
done

# 2. A finished run stores its record, atomically.
log=$(run result result)
check "result renders" "$([ -s "$OUT/result.png" ] && echo 1 || echo 0)"
check "a finished run wrote a record" "$([ -f "$RECORD" ] && echo 1 || echo 0)"
check "the record is one blob" \
      "$([ "$(wc -c < "$RECORD" 2>/dev/null)" = "$RECORD_BYTES" ] && echo 1 || echo 0)"
check "no temporary file left" "$([ -f "$RECORD.tmp" ] && echo 0 || echo 1)"
hasnt "no error while storing" 'ERROR' "$log"

# 3. The next launch reads it back and has a best score to beat.
log=$(run - reopen)
has "the stored record is read back" 'best score' "$log"

# 4. A damaged record is refused, and the app opens anyway with nothing to beat.
printf 'PRR1................damaged...' > "$RECORD"
log=$(run - damaged)
has "a damaged record is rejected" 'rejected' "$log"
check "the app still renders" "$([ -s "$OUT/damaged.png" ] && echo 1 || echo 0)"
check "a damaged record is left alone" "$([ -f "$RECORD" ] && echo 1 || echo 0)"
hasnt "a damaged record is not an error" 'ERROR' "$log"

# 5. A file of exactly the right size that is not ours at all.
head -c "$RECORD_BYTES" /dev/zero | tr '\0' 'Z' > "$RECORD"
log=$(run - foreign)
has "a foreign record of the right size is rejected" 'rejected' "$log"
check "the app still renders" "$([ -s "$OUT/foreign.png" ] && echo 1 || echo 0)"

# 6. An unwritable directory is reported and never stops play. It is the
#    directory the file itself goes in that has to be locked: the parent
#    being read-only does not stop a write into a subdirectory that already
#    exists, which is what the earlier steps left behind.
rm -f "$RECORD"
if [ "$(id -u)" != "0" ]; then
    chmod 0555 "$POCKETOS_STATE_DIR/radar"
    log=$(run result unwritable)
    check "the run still completes" "$([ -s "$OUT/unwritable.png" ] && echo 1 || echo 0)"
    has "a failed write is reported" 'session-only' "$log"
    hasnt "a failed write is not an error" 'ERROR' "$log"
    check "nothing was written" "$([ -f "$RECORD" ] && echo 0 || echo 1)"
    chmod 0755 "$POCKETOS_STATE_DIR/radar"
else
    echo "skip unwritable-directory checks (running as root)"
fi

# 7. Reduced motion (DS section 12): the sweep and the burst are gone, the
#    game is not. The scope still paints every contact, so the file is smaller
#    but far from empty.
printf 'reduced_motion=1\n' > "$POCKETOS_CONFIG_DIR/settings.conf"
log=$(run scan reduced)
check "reduced motion renders" "$([ -s "$OUT/reduced.png" ] && echo 1 || echo 0)"
hasnt "no error with reduced motion" 'ERROR' "$log"
motion=$(wc -c < "$OUT/scan.png")
still=$(wc -c < "$OUT/reduced.png")
check "reduced motion drops the sweep" "$([ "$still" -lt "$motion" ] && echo 1 || echo 0)"
check "reduced motion still draws the scope" "$([ "$still" -gt 8000 ] && echo 1 || echo 0)"
rm -f "$POCKETOS_CONFIG_DIR/settings.conf"

rm -rf "$OUT" "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_CONFIG_DIR" \
       "$POCKETOS_STATE_DIR"
echo "radar_shell_test: $failed failure(s)"
exit $((failed > 0))
