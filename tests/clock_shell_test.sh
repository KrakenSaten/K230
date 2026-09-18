#!/bin/bash
# PocketClock in the running app, and the shell's side of it.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${CLOCK_APP_TEST:-$(dirname "$SHELL_BIN")/clock_app_test}
if [ -x "$BIN" ]; then
    log=$("$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|clock_app_test:'
    check "PocketClock end to end, tapped on the panel" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL clock_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

# The app is on the launcher and is the shell's, not its own.
check "the shell knows about Clock" \
    "$(grep -q 'app_clock' ui/shell/shell.c && echo 1 || echo 0)"
check "Clock creates no keyboard of its own" \
    "$(grep -rq 'pos_keyboard_create' apps/clock/ && echo 0 || echo 1)"

# The alarms belong to the shell, which is the only reason one can ring with
# PocketClock shut (clock_runtime.h).
check "the shell starts the clock runtime" \
    "$(grep -q 'clock_runtime_init' ui/shell/shell.c && echo 1 || echo 0)"
check "and steps it on its tick" \
    "$(sed -n '/^static void on_tick/,/^}/p' ui/shell/shell.c |
       grep -q 'clock_runtime_step' && echo 1 || echo 0)"
# Its own declaration and definition aside, nothing under apps/ may start it:
# an app that started the runtime would be an app that could restart it.
starters=$(grep -rl 'clock_runtime_init' apps/ 2>/dev/null |
           grep -v 'apps/clock/clock_runtime\.')
check "no app starts it instead" "$([ -z "$starters" ] && echo 1 || echo 0)"
[ -n "$starters" ] && echo "$starters"

# One alert, built once, owned by the shell.
check "the shell builds the one alarm alert" \
    "$(grep -q 'shell_alarm_create' ui/shell/shell.c && echo 1 || echo 0)"
check "exactly one, not one per app" \
    "$([ "$(grep -c 'shell_alarm_create' ui/shell/shell.c)" = "1" ] && echo 1 || echo 0)"
check "no app builds an alarm alert" \
    "$(grep -rq 'shell_alarm_create' apps/ && echo 0 || echo 1)"
check "and PocketClock has no ringing screen left in it" \
    "$(grep -q 'SCREEN_RING' apps/clock/clock_app.c && echo 0 || echo 1)"

# The status bar is held to the same rule as the clock face: this board
# forgets the time at every power cut, and a corner reading 01:00 would be
# the most-looked-at lie on the device.
check "the status bar uses the wall-clock validity rule" \
    "$(sed -n '/^static void status_update/,/^}/p' ui/shell/shell.c |
       grep -q 'clock_format_wall' && echo 1 || echo 0)"
check "and does not format the time itself" \
    "$(sed -n '/^static void status_update/,/^}/p' ui/shell/shell.c |
       grep -qE 'strftime|%H:%M' && echo 0 || echo 1)"

RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
    "$SHELL_BIN" --open clock --exit-after-ms 1200 >"$LOGD/out" 2>&1
rc=$?
check "the shell opens Clock" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" && echo 0 || echo 1)"
check "Clock reports itself open" \
    "$(grep -q 'open app clock' "$LOGD/log/shell.log" 2>/dev/null ||
       grep -q 'open app clock' "$LOGD/out" && echo 1 || echo 0)"

# Opening the app and leaving it must not write anything: alarms are saved
# when they change, and a stopwatch is never written at all.
check "just opening Clock writes nothing to the store" \
    "$([ -z "$(ls -A "$STATE" 2>/dev/null)" ] && echo 1 || echo 0)"
# Nor a restart handoff: this shell had no alarm, no stopwatch and no
# countdown, so it has nothing that only it knew (clock_store.h).
check "and leaves no restart handoff behind" \
    "$([ ! -e "$RUN/clock/restart.state" ] && echo 1 || echo 0)"
check "and says nothing about one, because there was nothing to say" \
    "$(cat "$LOGD/log/shell.log" "$LOGD/out" 2>/dev/null |
       grep -q 'clock: runtime state' && echo 0 || echo 1)"
rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"

# And the other way round: a shell with an alarm in its store knows something
# about today that the next one would otherwise work out from scratch, so it
# does leave a handoff - and the shell after it takes it (clock_store.h).
RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
mkdir -p "$STATE/clock"
printf 'pocketclock 1\ntimer 0\nalarm 1 7 30 1 Wake up\n' >"$STATE/clock/clock.conf"
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
    "$SHELL_BIN" --exit-after-ms 2500 >"$LOGD/out" 2>&1
rc=$?
check "a shell with an alarm runs and stops cleanly" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" && echo 0 || echo 1)"
check "it hands its clock runtime to the next shell" \
    "$(cat "$LOGD/log/shell.log" "$LOGD/out" 2>/dev/null |
       grep -q 'clock: runtime state handed to the next shell' && echo 1 || echo 0)"
check "in the runtime directory, not the state one" \
    "$([ -s "$RUN/clock/restart.state" ] && [ ! -e "$STATE/clock/restart.state" ] &&
       echo 1 || echo 0)"
check "and the handoff says what format it is" \
    "$(head -1 "$RUN/clock/restart.state" 2>/dev/null | grep -q '^pocketclock-restart 1$' &&
       echo 1 || echo 0)"
check "the alarms are still the ones in the settings file" \
    "$(grep -q 'alarm 1 7 30 1 Wake up' "$STATE/clock/clock.conf" && echo 1 || echo 0)"

# The next shell in the same boot takes it, and it is gone afterwards: one
# exit hands off to exactly one start.
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
    "$SHELL_BIN" --exit-after-ms 2500 >"$LOGD/out2" 2>&1
rc=$?
check "the shell after it runs too" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "and took the handoff" \
    "$(cat "$LOGD/log/shell.log" "$LOGD/out2" 2>/dev/null |
       grep -q 'clock: runtime state taken from the shell before this one' && echo 1 || echo 0)"
check "leaving one of its own for the next" \
    "$([ -s "$RUN/clock/restart.state" ] && echo 1 || echo 0)"
rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"

echo "clock_shell_test: $failed failure(s)"
exit $((failed > 0))
