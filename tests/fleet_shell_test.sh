#!/bin/bash
# PocketFleet persistence in the running shell: a match is stored after a
# resolved turn, a stored match is offered again, a finished one is not, and
# every failure path (missing, unwritable, damaged, foreign) leaves the app
# usable with no persistence rather than blocking it.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell). Headless via SDL's
# dummy driver, exactly like tests/shell_ipc_test.sh.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
export SDL_VIDEODRIVER=dummy
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
POCKETOS_CONFIG_DIR=$(mktemp -d)
POCKETOS_STATE_DIR=$(mktemp -d)
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR POCKETOS_CONFIG_DIR POCKETOS_STATE_DIR
OUT=$(mktemp -d)
SAVE="$POCKETOS_STATE_DIR/fleet/save.v1"
SAVE_BYTES=898
failed=0

check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
has() { if printf '%s' "$3" | grep -q -- "$2"; then echo "ok   $1"; else
        echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | tail -5; failed=$((failed + 1)); fi; }
hasnt() { if printf '%s' "$3" | grep -q -- "$2"; then
        echo "FAIL $1: unexpected '$2'"; failed=$((failed + 1)); else echo "ok   $1"; fi; }

# Run the shell once on a chosen PocketFleet screen and return its log.
run() { # <screen|-> <png>
    if [ "$1" = "-" ]; then unset POCKETFLEET_SCREEN; else export POCKETFLEET_SCREEN="$1"; fi
    "$SHELL_BIN" --open fleet --screenshot "$OUT/$2.png" --exit-after-ms 900 \
        >"$OUT/$2.log" 2>&1
    cat "$OUT/$2.log"
}

# 1. A resolved turn stores the match.
log=$(run battle a)
check "battle screenshot rendered" "$([ -s "$OUT/a.png" ] && echo 1 || echo 0)"
check "a resolved turn wrote a save" "$([ -f "$SAVE" ] && echo 1 || echo 0)"
check "the save is one blob" \
      "$([ "$(wc -c < "$SAVE" 2>/dev/null)" = "$SAVE_BYTES" ] && echo 1 || echo 0)"
check "no temporary file left" "$([ -f "$SAVE.tmp" ] && echo 0 || echo 1)"
hasnt "no error while saving" 'ERROR' "$log"

# 2. The next launch finds it and offers to continue.
log=$(run - b)
has "the stored match is offered" 'resumable match' "$log"

# 3. A finished match is not offered again: the slot is cleared.
log=$(run result c)
check "a finished match clears the save" "$([ -f "$SAVE" ] && echo 0 || echo 1)"
log=$(run - d)
hasnt "nothing is offered after a win" 'resumable match' "$log"

# 4. A damaged save must never block the app.
mkdir -p "$(dirname "$SAVE")"
printf 'this is not a PocketFleet save at all' > "$SAVE"
log=$(run - e)
has "a damaged save is rejected" 'rejected, starting fresh' "$log"
check "the app still rendered" "$([ -s "$OUT/e.png" ] && echo 1 || echo 0)"
hasnt "a damaged save is not an error" 'ERROR' "$log"

# 5. A save of the right size but wrong content (a version or checksum change
#    would look like this) is rejected the same way.
head -c "$SAVE_BYTES" /dev/zero | tr '\0' 'x' > "$SAVE"
log=$(run - f)
has "a foreign save of the right size is rejected" 'rejected, starting fresh' "$log"
check "the app still rendered" "$([ -s "$OUT/f.png" ] && echo 1 || echo 0)"

# 6. An unwritable directory switches persistence off for the session.
rm -f "$SAVE"
chmod 0555 "$(dirname "$SAVE")"
if [ -w "$(dirname "$SAVE")" ]; then
    echo "skip unwritable-directory checks (running as root)"
else
    log=$(run battle g)
    has "an unwritable directory is reported" 'continuing without persistence' "$log"
    check "the app still rendered" "$([ -s "$OUT/g.png" ] && echo 1 || echo 0)"
    hasnt "losing persistence is not an error" 'ERROR' "$log"
fi
chmod 0755 "$(dirname "$SAVE")"

rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_CONFIG_DIR" \
       "$POCKETOS_STATE_DIR" "$OUT"
echo "fleet_shell_test: $failed failure(s)"
exit $((failed > 0))
