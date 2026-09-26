#!/bin/bash
# Recorder in the running app (tests/rec_app_test.c), and in the real shell:
# opened and closed in both orientations with the real helper on the
# file-backed sound card, nothing recorded without a tap, no helper left.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell) with rec_app_test
# beside it, and make's tests/pos-record-testhooks. REC_SHOTS=<dir> keeps
# screenshots (the app test's bodies, and the real shell's screens).
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

HOOK=$(pwd)/tests/pos-record-testhooks
BIN=${REC_APP_TEST:-$(dirname "$SHELL_BIN")/rec_app_test}
if [ -x "$BIN" ] && [ -x "$HOOK" ]; then
    log=$(SDL_VIDEODRIVER=dummy RECORD_HELPER="$HOOK" "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|rec_app_test:|^     '
    check "Recorder end to end, tapped, against the real helper" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL rec_app_test or pos-record-testhooks missing: $BIN $HOOK"; failed=$((failed + 1))
fi
check "the app test is a host-only target" \
    "$(awk '/if\(POCKETOS_DISPLAY STREQUAL "sdl"\)/,/^endif\(\)/' ui/shell/CMakeLists.txt |
       grep -q 'add_executable(rec_app_test' && echo 1 || echo 0)"

# The real shell: open Recorder, leave it, in both orientations. The repair
# helper runs at open; nothing records.
for o in portrait landscape; do
    T=$(mktemp -d)
    mkdir -p "$T/run" "$T/log" "$T/cfg" "$T/state" "$T/audio" "$T/home"
    echo 1 > "$T/audio/route"
    echo 10000000000 > "$T/free"
    SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$T/run" POCKETOS_LOG_DIR="$T/log" POCKETOS_CONFIG_DIR="$T/cfg" \
    POCKETOS_STATE_DIR="$T/state" HOME="$T/home" POCKETOS_RECORD_HELPER="$HOOK" \
    POS_RECORD_FAKE_AUDIO="$T/audio" POS_RECORD_FREE_FILE="$T/free" \
        timeout 30 "$SHELL_BIN" --rotation $o --no-lock --open recorder --screenshot "$T/shot.png" \
        --exit-after-ms 1500 >"$T/log/out" 2>&1
    rc=$?
    check "$o: the shell opens Recorder and exits cleanly" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    check "$o: it reports the app open and closed" \
        "$(grep -q 'open app recorder' "$T/log/shell.log" && grep -q 'close app recorder' "$T/log/shell.log" &&
           echo 1 || echo 0)"
    check "$o: and logs no fault" "$(grep -qE ' ERROR |assert' "$T/log/out" "$T/log/shell.log" 2>/dev/null && echo 0 || echo 1)"
    check "$o: the Recordings folder is in HOME, private, and holds nothing" \
        "$([ -d "$T/home/Recordings" ] && [ "$(stat -c %a "$T/home/Recordings")" = 700 ] &&
           [ -z "$(ls -A "$T/home/Recordings")" ] && echo 1 || echo 0)"
    check "$o: opening it never opened the microphone" \
        "$(grep -q 'pcm capture' "$T/audio/log" 2>/dev/null && echo 0 || echo 1)"
    check "$o: the screenshot was written" "$([ -s "$T/shot.png" ] && echo 1 || echo 0)"
    [ -n "${REC_SHOTS:-}" ] && cp "$T/shot.png" "$REC_SHOTS/shell-$o.png"
    check "$o: no recorder helper is left" "$(pgrep -f 'pos-record-testhooks' >/dev/null && echo 0 || echo 1)"
    rm -rf "$T"
done

echo "recorder_shell_test: $failed failure(s)"
exit $((failed > 0))
