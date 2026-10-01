#!/bin/bash
# MP3 in the running app (tests/mp3_app_test.c), and in the real shell:
# opened and closed in both orientations with the real helper on the
# file-backed sound card, nothing played without a tap, no helper left.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell) with mp3_app_test
# beside it, and make's tests/pos-mp3-testhooks. MP3_SHOTS=<dir> keeps
# screenshots (the app test's bodies, and the real shell's screens).
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

HOOK=$(pwd)/tests/pos-mp3-testhooks
BIN=${MP3_APP_TEST:-$(dirname "$SHELL_BIN")/mp3_app_test}
if [ -x "$BIN" ] && [ -x "$HOOK" ]; then
    log=$(SDL_VIDEODRIVER=dummy MP3_HELPER="$HOOK" "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|mp3_app_test:|^note heap|^     '
    check "MP3 end to end, tapped, against the real helper" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL mp3_app_test or pos-mp3-testhooks missing: $BIN $HOOK"; failed=$((failed + 1))
fi
check "the app test is a host-only target" \
    "$(awk '/if\(POCKETOS_DISPLAY STREQUAL "sdl"\)/,/^endif\(\)/' ui/shell/CMakeLists.txt |
       grep -q 'add_executable(mp3_app_test' && echo 1 || echo 0)"

# The real shell: open MP3, leave it, in both orientations. The places are
# read; nothing plays.
for o in portrait landscape; do
    T=$(mktemp -d)
    mkdir -p "$T/run" "$T/log" "$T/cfg" "$T/state" "$T/audio" "$T/home"
    echo 0 > "$T/audio/route"
    SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$T/run" POCKETOS_LOG_DIR="$T/log" POCKETOS_CONFIG_DIR="$T/cfg" \
    POCKETOS_STATE_DIR="$T/state" HOME="$T/home" POCKETOS_MP3_HELPER="$HOOK" POCKETOS_MP3_MEDIA_ROOTS= \
    POS_MP3_FAKE_AUDIO="$T/audio" \
        timeout 30 "$SHELL_BIN" --rotation $o --no-lock --open mp3 --screenshot "$T/shot.png" \
        --exit-after-ms 1500 >"$T/log/out" 2>&1
    rc=$?
    check "$o: the shell opens MP3 and exits cleanly" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    check "$o: it reports the app open and closed" \
        "$(grep -q 'open app mp3' "$T/log/shell.log" && grep -q 'close app mp3' "$T/log/shell.log" &&
           echo 1 || echo 0)"
    check "$o: and logs no fault" "$(grep -qE ' ERROR |assert' "$T/log/out" "$T/log/shell.log" 2>/dev/null && echo 0 || echo 1)"
    check "$o: the Music folder is made in HOME, and holds nothing" \
        "$([ -d "$T/home/Music" ] && [ -z "$(ls -A "$T/home/Music")" ] && echo 1 || echo 0)"
    check "$o: opening it never opened the audio device" \
        "$(grep -q 'pcm' "$T/audio/log" 2>/dev/null && echo 0 || echo 1)"
    check "$o: the screenshot was written" "$([ -s "$T/shot.png" ] && echo 1 || echo 0)"
    [ -n "${MP3_SHOTS:-}" ] && cp "$T/shot.png" "$MP3_SHOTS/shell-$o.png"
    check "$o: no player helper is left" "$(pgrep -f 'pos-mp3-testhooks' >/dev/null && echo 0 || echo 1)"
    rm -rf "$T"
done

echo "mp3_shell_test: $failed failure(s)"
exit $((failed > 0))
