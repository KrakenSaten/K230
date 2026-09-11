#!/bin/bash
# PocketClock layering, the same rules the other apps are held to, plus the
# three this app exists to get right: one place reads a clock, one place
# writes a file, and neither happens on a tick.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

PURE="apps/clock/clock_engine.c apps/clock/clock_engine.h apps/clock/clock_alert.c apps/clock/clock_alert.h apps/clock/clock_store.c apps/clock/clock_store.h apps/clock/clock_time.c apps/clock/clock_time.h"

hits=$(grep -lE 'lvgl|lv_obj|lv_label|lv_timer' $PURE 2>/dev/null)
check "the engine, the store, the clock reader and the alert are free of LVGL" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

hits=$(grep -lE 'fopen|\bopen\(|unlink|mkdir|rename|opendir' \
    apps/clock/clock_engine.c apps/clock/clock_time.c apps/clock/clock_alert.c \
    apps/clock/clock_app.c 2>/dev/null)
check "only clock_store.c touches the filesystem" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# The whole design rests on this: the engine is handed both clocks and never
# reads one, which is what makes every rule testable at any instant.
hits=$(grep -nE 'clock_gettime|\btime\(|localtime|gmtime|mktime|strftime' \
    apps/clock/clock_engine.c apps/clock/clock_store.c apps/clock/clock_alert.c \
    apps/clock/clock_app.c 2>/dev/null)
check "only clock_time.c reads a clock" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

check "and it reads both of them" \
    "$(grep -q 'CLOCK_MONOTONIC' apps/clock/clock_time.c &&
       grep -q 'CLOCK_REALTIME' apps/clock/clock_time.c && echo 1 || echo 0)"

# A countdown is elapsed time. Subtracting wall-clock timestamps would make
# it jump whenever the clock is set, which on this board is every boot.
check "the timer counts against the monotonic clock" \
    "$(grep -q 'deadline_mono = now->mono_ms' apps/clock/clock_engine.c && echo 1 || echo 0)"
check "and the stopwatch too" \
    "$(grep -q 'started_mono = now->mono_ms' apps/clock/clock_engine.c && echo 1 || echo 0)"
hits=$(grep -nE 'wall\.(epoch|second)' apps/clock/clock_engine.c 2>/dev/null)
check "no elapsed time is derived from the wall clock" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# An alarm fires once a day however often the engine is stepped.
check "an alarm records the day it fired" \
    "$(grep -q 'fired_day = w->day' apps/clock/clock_engine.c && echo 1 || echo 0)"
check "and that is what stops it firing again" \
    "$(grep -q 'fired_day == w->day' apps/clock/clock_engine.c && echo 1 || echo 0)"

# Storage: the Notes write, and never from the refresh timer.
for want in 'fsync' 'rename' '\.tmp'; do
    check "the store write uses $want" \
        "$(grep -qE "$want" apps/clock/clock_store.c && echo 1 || echo 0)"
done
tick=$(sed -n '/^static void on_refresh/,/^}/p' apps/clock/clock_app.c)
check "the refresh timer exists" "$([ -n "$tick" ] && echo 1 || echo 0)"
hits=$(printf '%s' "$tick" | grep -nE 'clock_store_save|save\(')
check "and writes nothing to storage" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# DS 17.4: an app asks the shell for the keyboard and never holds one.
hits=$(grep -nE 'pos_keyboard|lv_keyboard' apps/clock/*.c apps/clock/*.h 2>/dev/null)
check "Clock never names a keyboard (DS 17.4)" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "Clock asks the shell for it instead" \
    "$(grep -q 'pocketos_shell_keyboard_show' apps/clock/clock_app.c && echo 1 || echo 0)"

# No invented hardware. The board has no buzzer and no vibration motor, and
# the audio path has never been exercised (clock_alert.h); nothing here may
# quietly start driving one. Prose is not code, so comments are dropped.
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' apps/clock/*.c apps/clock/*.h 2>/dev/null; }
hits=$(code | grep -niE 'alsa|snd_|/dev/snd|aplay|/dev/mem|gpiod|ioctl|/sys/class|max98357')
check "no audio, GPIO or PWM device is opened" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
check "the shipped alert backend claims only the screen" \
    "$(grep -q '\.channels = CLOCK_ALERT_VISUAL,' apps/clock/clock_alert.c && echo 1 || echo 0)"
check "the alert channel is declared rather than assumed" \
    "$(grep -q 'clock_alert_channels' apps/clock/clock_alert.c && echo 1 || echo 0)"
check "and the app tells the owner what it can do" \
    "$(grep -q 'clock_alert_why' apps/clock/clock_app.c && echo 1 || echo 0)"

# Scope: this is a clock, not a calendar. None of these belong in it.
for banned in ntp timezone recurrence rrule cron ical; do
    hits=$(code | grep -niE "[a-z_]*${banned}[a-z_]*[[:space:]]*[(=]|${banned}_")
    check "no $banned machinery in the app" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
done

echo "clock_lint: $failed failure(s)"
exit $((failed > 0))
