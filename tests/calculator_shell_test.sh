#!/bin/bash
# PocketCalculator in the running app, and the shell's side of it.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${CALC_APP_TEST:-$(dirname "$SHELL_BIN")/calc_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|calc_app_test:'
    check "PocketCalculator end to end, tapped and typed" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL calc_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

# The app is on the launcher, after Calendar, and is the shell's to build.
check "the shell knows about Calculator" \
    "$(grep -q 'extern const struct pocketos_app app_calculator;' ui/shell/shell.c && echo 1 || echo 0)"
check "it is on the launcher, after Calendar" \
    "$(grep -q '&app_calendar, &app_calculator' ui/shell/shell.c && echo 1 || echo 0)"
for src in calc_app.c calc_engine.c calc_view.c; do
    check "the shell builds $src" \
        "$(grep -q "apps/calculator/$src" ui/shell/CMakeLists.txt && echo 1 || echo 0)"
done
check "and has the app's directory on its include path" \
    "$(grep -q '"${REPO_DIR}/apps/calculator"' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
check "the app test is a host-only target" \
    "$(awk '/if\(POCKETOS_DISPLAY STREQUAL "sdl"\)/,/^endif\(\)/' ui/shell/CMakeLists.txt |
       grep -q 'add_executable(calc_app_test' && echo 1 || echo 0)"

# Scope, checked here as well as in calculator_lint.sh because this is the
# file a reviewer reaches for when asking what the app is allowed to be.
check "Calculator creates no keyboard of its own" \
    "$(grep -rqE 'pos_keyboard|lv_keyboard' apps/calculator/ && echo 0 || echo 1)"
check "has no store" \
    "$(ls apps/calculator/*store* >/dev/null 2>&1 && echo 0 || echo 1)"
check "and no tick, because it follows no clock" \
    "$(grep -q '\.tick = NULL,' apps/calculator/calc_app.c && echo 1 || echo 0)"

# The real shell: open it, run it, close it.
RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
    "$SHELL_BIN" --open calculator --exit-after-ms 1200 >"$LOGD/out" 2>&1
rc=$?
check "the shell opens Calculator" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
# The log file is read as well as the output, so it has to be there: a check
# for faults in a file that was never written would pass by looking at nothing.
check "the shell wrote its log" "$([ -s "$LOGD/shell.log" ] && echo 1 || echo 0)"
check "and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null &&
                             echo 0 || echo 1)"
check "Calculator reports itself open" \
    "$(grep -q 'open app calculator' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "and closed" \
    "$(grep -q 'close app calculator' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
# Six rows since Wave became the eleventh app (tests/wave_shell_test.sh). The
# shell derives the grid from the display (tests/display_geometry_shell_test.sh),
# so this reads what the portrait launcher was built with.
check "the launcher grid has a row for every app" \
    "$(grep -q 'launcher: 2 column(s), 6 row(s)' "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
check "the shell did not call it unknown" \
    "$(grep -q 'unknown app calculator' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 0 || echo 1)"
# A calculation is not kept, so opening and leaving must write nothing at all.
check "opening Calculator writes nothing to the store" \
    "$([ -z "$(ls -A "$STATE" 2>/dev/null)" ] && echo 1 || echo 0)"
rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"

echo "calculator_shell_test: $failed failure(s)"
exit $((failed > 0))
