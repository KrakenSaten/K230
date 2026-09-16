#!/bin/bash
# Wave in the running app, and the shell's side of it.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell), and wave_app_test
# beside it. No audio device is used: the app test drives a scripted fake
# helper, and the real shell is opened on Wave and closed again without
# anything being started.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${WAVE_APP_TEST:-$(dirname "$SHELL_BIN")/wave_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy WAVE_FAKE_HELPER=tests/fake_pos_wave.sh "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|wave_app_test:'
    check "Wave end to end, tapped and typed, against a fake helper" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL wave_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

check "the shell knows about Wave" \
    "$(grep -q 'extern const struct pocketos_app app_wave;' ui/shell/shell.c && echo 1 || echo 0)"
check "it is on the launcher, after Settings" \
    "$(grep -q '&app_settings, &app_wave' ui/shell/shell.c && echo 1 || echo 0)"
for src in wave_app.c wave_view.c wave_session.c wave_text.c; do
    check "the shell builds $src" \
        "$(grep -q "apps/wave/$src" ui/shell/CMakeLists.txt && echo 1 || echo 0)"
done
check "the shell does not link the modem or the audio layer" \
    "$(grep -qE 'wave_modem|pocketaudio|asound' ui/shell/CMakeLists.txt && echo 0 || echo 1)"
check "the app test is a host-only target" \
    "$(awk '/if\(POCKETOS_DISPLAY STREQUAL "sdl"\)/,/^endif\(\)/' ui/shell/CMakeLists.txt |
       grep -q 'add_executable(wave_app_test' && echo 1 || echo 0)"

# The real shell: open Wave, leave it. Nothing is started, so no helper runs.
RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" POCKETOS_WAVE_HELPER=/nonexistent/pos-wave \
    "$SHELL_BIN" --open wave --exit-after-ms 1200 >"$LOGD/out" 2>&1
rc=$?
check "the shell opens Wave" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "the shell wrote its log" "$([ -s "$LOGD/shell.log" ] && echo 1 || echo 0)"
check "and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 0 || echo 1)"
check "Wave reports itself open" "$(grep -q 'open app wave' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "and closed" "$(grep -q 'close app wave' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
# The shell derives the grid from the display (tests/display_geometry_shell_test.sh):
# in portrait, Wave's eleventh tile needs a sixth row.
check "the launcher grid has a sixth row for it" \
    "$(grep -q 'launcher: 2 column(s), 6 row(s)' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "opening Wave takes no audio lock and stores nothing" \
    "$([ ! -e "$RUN/audio.lock" ] && [ -z "$(ls -A "$STATE" 2>/dev/null)" ] && echo 1 || echo 0)"
rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"

echo "wave_shell_test: $failed failure(s)"
exit $((failed > 0))
