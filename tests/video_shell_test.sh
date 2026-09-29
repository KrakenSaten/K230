#!/bin/bash
# Video in the running app (tests/video_app_test.c), and in the real shell:
# opened and closed in both orientations with the real helper on the fake
# backend, the list read, nothing played without a tap, no helper left.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell) with video_app_test
# beside it, and make's tests/pos-video-testhooks. VIDEO_SHOTS=<dir> keeps
# screenshots (the app test's screens, and the real shell's).
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

HOOK=$(pwd)/tests/pos-video-testhooks
BIN=${VIDEO_APP_TEST:-$(dirname "$SHELL_BIN")/video_app_test}
if [ -x "$BIN" ] && [ -x "$HOOK" ]; then
    log=$(SDL_VIDEODRIVER=dummy VIDEO_HELPER="$HOOK" "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|video_app_test:|^     '
    check "Video end to end, tapped, against the real helper" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL video_app_test or pos-video-testhooks missing: $BIN $HOOK"; failed=$((failed + 1))
fi
check "the app test is a host-only target" \
    "$(awk '/if\(POCKETOS_DISPLAY STREQUAL "sdl"\)/,/^endif\(\)/' ui/shell/CMakeLists.txt |
       grep -q 'add_executable(video_app_test' && echo 1 || echo 0)"

# The real shell: open Video, leave it, in both orientations.
for o in portrait landscape; do
    T=$(mktemp -d)
    mkdir -p "$T/run" "$T/log" "$T/cfg" "$T/state" "$T/home/Videos"
    echo "DOORS-FAKE-VIDEO w=640 h=360 fps=25 ms=3000" > "$T/home/Videos/sample.mp4"
    SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$T/run" POCKETOS_LOG_DIR="$T/log" POCKETOS_CONFIG_DIR="$T/cfg" \
    POCKETOS_STATE_DIR="$T/state" HOME="$T/home" POCKETOS_VIDEO_HELPER="$HOOK" POS_VIDEO_NO_AUDIO=1 \
        timeout 30 "$SHELL_BIN" --rotation $o --no-lock --open video --screenshot "$T/shot.png" \
        --exit-after-ms 1500 >"$T/log/out" 2>&1
    rc=$?
    check "$o: the shell opens Video and exits cleanly" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    check "$o: it reports the app open and closed" \
        "$(grep -q 'open app video' "$T/log/shell.log" && grep -q 'close app video' "$T/log/shell.log" &&
           echo 1 || echo 0)"
    check "$o: nothing is played without a tap (no player started)" \
        "$(grep -q 'video: opened' "$T/log/shell.log" && echo 0 || echo 1)"
    check "$o: no warning or error from the app" \
        "$(grep -E 'WARN|ERROR' "$T/log/shell.log" | grep -q 'video' && echo 0 || echo 1)"
    check "$o: no helper left" "$(pgrep -f "$HOOK session" >/dev/null && echo 0 || echo 1)"
    if [ -n "${VIDEO_SHOTS:-}" ] && [ -f "$T/shot.png" ]; then
        mkdir -p "$VIDEO_SHOTS" && cp "$T/shot.png" "$VIDEO_SHOTS/shell-$o.png"
    fi
    rm -rf "$T"
done

echo "video_shell_test: $failed failure(s)"
exit $((failed > 0))
