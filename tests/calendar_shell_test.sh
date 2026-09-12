#!/bin/bash
# PocketCalendar in the running app, and the shell's side of it.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${CAL_APP_TEST:-$(dirname "$SHELL_BIN")/cal_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|cal_app_test:'
    check "PocketCalendar end to end, tapped on the panel" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL cal_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

# The app is on the launcher and is the shell's, not its own.
check "the shell knows about Calendar" \
    "$(grep -q 'app_calendar' ui/shell/shell.c && echo 1 || echo 0)"
check "and builds it" \
    "$(grep -q 'apps/calendar/cal_app.c' ui/shell/CMakeLists.txt && echo 1 || echo 0)"

# The date comes from the shell, once a second. Both halves matter: an app
# with no tick would never hear that the clock had been set, and an app that
# read a clock would be a second answer to what day it is.
check "the shell answers what day it is" \
    "$(grep -q 'int64_t pocketos_shell_system_day(void)' ui/shell/shell.c && echo 1 || echo 0)"
check "declared in the app API, for any app to use" \
    "$(grep -q 'pocketos_shell_system_day' ui/shell/app.h && echo 1 || echo 0)"
check "from the reading the shell already took" \
    "$(sed -n '/^int64_t pocketos_shell_system_day/,/^}/p' ui/shell/shell.c |
       grep -q 'clock_runtime_now' && echo 1 || echo 0)"
check "and says -1 rather than the epoch when there is none" \
    "$(sed -n '/^int64_t pocketos_shell_system_day/,/^}/p' ui/shell/shell.c |
       grep -q ': -1' && echo 1 || echo 0)"
check "Calendar has a tick, which is how it recovers" \
    "$(grep -q '\.tick = calendar_tick,' apps/calendar/cal_app.c && echo 1 || echo 0)"
# Twice in the code: once when the app is built, once on every tick. Comment
# lines are dropped, because the file explains the arrangement in prose too.
sites=$(grep -vE '^[[:space:]]*(/\*|\*|//)' apps/calendar/cal_app.c |
        grep -c 'pocketos_shell_system_day')
check "and reads the date only there and at create" \
    "$([ "$sites" = "2" ] && echo 1 || echo 0)"

# Scope, checked here as well as in calendar_lint.sh because this is the file
# a reviewer reaches for when asking what the app is allowed to be.
check "Calendar creates no keyboard of its own" \
    "$(grep -rq 'pos_keyboard_create' apps/calendar/ && echo 0 || echo 1)"
check "and has no store" \
    "$(ls apps/calendar/*store* >/dev/null 2>&1 && echo 0 || echo 1)"

# The selected outline is a role, not a colour set on an object (DS 7).
check "the selected outline is a shared style role" \
    "$(grep -q 'POS_STYLE_SELECTED' ui/pocketui/pos_styles.h && echo 1 || echo 0)"
check "defined once, in the theme engine" \
    "$([ "$(grep -c 'styles\[POS_STYLE_SELECTED\]' ui/pocketui/pos_styles.c)" = "1" ] &&
       echo 1 || echo 0)"

RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
    "$SHELL_BIN" --open calendar --exit-after-ms 1200 >"$LOGD/out" 2>&1
rc=$?
check "the shell opens Calendar" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" && echo 0 || echo 1)"
check "Calendar reports itself open" \
    "$(grep -q 'open app calendar' "$LOGD/log/shell.log" 2>/dev/null ||
       grep -q 'open app calendar' "$LOGD/out" && echo 1 || echo 0)"
# There is nothing to store, so opening and leaving must write nothing at all.
check "opening Calendar writes nothing to the store" \
    "$([ -z "$(ls -A "$STATE" 2>/dev/null)" ] && echo 1 || echo 0)"
rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"

echo "calendar_shell_test: $failed failure(s)"
exit $((failed > 0))
