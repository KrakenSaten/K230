#!/bin/bash
# PocketClock layering, the same rules the other apps are held to, plus the
# three this app exists to get right: one place reads a clock, one place
# writes a file, and neither happens on a tick.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

PURE="apps/clock/clock_engine.c apps/clock/clock_engine.h \
      apps/clock/clock_alert.c apps/clock/clock_alert.h \
      apps/clock/clock_store.c apps/clock/clock_store.h \
      apps/clock/clock_time.c apps/clock/clock_time.h \
      apps/clock/clock_runtime.c apps/clock/clock_runtime.h"

hits=$(grep -lE 'lvgl|lv_obj|lv_label|lv_timer' $PURE 2>/dev/null)
check "everything but the app is free of LVGL" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

hits=$(grep -lE 'fopen|\bopen\(|unlink|mkdir|rename|opendir' \
    apps/clock/clock_engine.c apps/clock/clock_time.c apps/clock/clock_alert.c \
    apps/clock/clock_runtime.c apps/clock/clock_app.c 2>/dev/null)
check "only clock_store.c touches the filesystem" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# The whole design rests on this: the engine is handed both clocks and never
# reads one, which is what makes every rule testable at any instant.
hits=$(grep -nE 'clock_gettime|\btime\(|localtime|gmtime|mktime|strftime' \
    apps/clock/clock_engine.c apps/clock/clock_store.c apps/clock/clock_alert.c \
    apps/clock/clock_runtime.c apps/clock/clock_app.c 2>/dev/null)
check "only clock_time.c reads a clock" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# One engine, one stepper. An alarm that two callers could advance is an
# alarm that can ring twice, and an app that owns the engine is an app whose
# alarms stop existing the moment it is closed (clock_runtime.h).
hits=$(grep -nE 'struct clock_engine [a-z_]+;|clock_engine_init|clock_engine_step' \
    apps/clock/clock_app.c 2>/dev/null)
check "the app owns no engine and never steps one" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the runtime does" \
    "$(grep -q 'clock_engine_step' apps/clock/clock_runtime.c && echo 1 || echo 0)"
# Everywhere that calls it, which is the defining file and one caller.
steppers=$(grep -lE 'clock_engine_step' apps/clock/*.c ui/shell/*.c 2>/dev/null |
           grep -v 'clock_engine\.c')
check "and it is the only caller anywhere" \
    "$([ "$steppers" = "apps/clock/clock_runtime.c" ] && echo 1 || echo 0)"
[ "$steppers" != "apps/clock/clock_runtime.c" ] && echo "$steppers"
check "the app loads and saves through the runtime, not the store" \
    "$(grep -q 'clock_store_load\|clock_store_save' apps/clock/clock_app.c && echo 0 || echo 1)"

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

# The restart handoff (clock_store.h). Two files, two lifetimes, and the
# difference between them is which directory each is in: the settings outlive
# a power cut, and the handoff must not, because every instant in it is
# measured on a clock that starts again at the boot.
check "the settings file is under the state directory" \
    "$(grep -q 'getenv("POCKETOS_STATE_DIR")' apps/clock/clock_store.c && echo 1 || echo 0)"
check "and the handoff under the runtime one, which is a tmpfs" \
    "$(grep -q 'getenv("POCKETOS_RUNTIME_DIR")' apps/clock/clock_store.c &&
       grep -q '#define CLOCK_HANDOFF_DEFAULT_DIR "/run/pocketos"' apps/clock/clock_store.h &&
       echo 1 || echo 0)"
check "the handoff is refused when the monotonic clock has gone backwards" \
    "$(grep -q 'now->mono_ms < v\[0\]' apps/clock/clock_store.c && echo 1 || echo 0)"
check "it is consumed, so one exit hands off to one start" \
    "$(sed -n '/^int clock_handoff_load/,/^}/p' apps/clock/clock_store.c |
       grep -q 'unlink(path);' && echo 1 || echo 0)"
check "a countdown whose deadline went by comes back expired, never running" \
    "$(sed -n '/^static void handoff_settle/,/^}/p' apps/clock/clock_store.c |
       grep -q 'CLOCK_TIMER_EXPIRED' && echo 1 || echo 0)"
# It is the shell's way out and nothing else. A tick that wrote it would put a
# write behind every second the device is awake.
check "only the shell writes the handoff" \
    "$([ "$(grep -rl 'clock_runtime_handoff_save' apps/ ui/ 2>/dev/null |
            grep -v 'apps/clock/clock_runtime\.' | tr '\n' ' ')" = "ui/shell/shell.c " ] &&
       echo 1 || echo 0)"
check "and no app does" \
    "$(grep -rq 'clock_handoff_save\|clock_handoff_load' apps/clock/clock_app.c && echo 0 || echo 1)"
hits=$(sed -n '/^static void on_tick/,/^}/p' ui/shell/shell.c | grep -nE 'handoff')
check "the shell's tick does not write it" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"
check "the shell writes it once, on the way out" \
    "$([ "$(grep -c 'clock_runtime_handoff_save' ui/shell/shell.c)" = "1" ] && echo 1 || echo 0)"
tick=$(sed -n '/^static void on_refresh/,/^}/p' apps/clock/clock_app.c)
check "the refresh timer exists" "$([ -n "$tick" ] && echo 1 || echo 0)"
hits=$(printf '%s' "$tick" | grep -nE 'clock_store_save|clock_runtime_save|save\(|clock_runtime_step')
check "and neither writes to storage nor advances the engine" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
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

# DS 21.2, 22.3: the layout is chosen from the body the app is given, not from
# the orientation; the corner clearance comes from the platform's description
# of the panel rather than a number of the app's own; and a change of shape
# moves the one set of objects rather than building another.
APP=apps/clock/clock_app.c
layout() { sed -n '/^\/\* ---- the layout/,/^\/\* ---- the app/p' "$APP" | grep -vE '^[[:space:]]*(/\*|\*|//)'; }
hits=$(layout | grep -nE 'orientation|rotation|POS_ROTATION_|POCKETOS_ROTATION_|landscape|portrait')
check "the layout never asks which way the display is turned" \
    "$([ -n "$(layout)" ] && [ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(layout | grep -nE 'build_(main|add|confirm|alarm_list|[a-z]+_pane)\(|lv_obj_clean|lv_obj_delete|_create\(' |
       grep -v 'lv_obj_create(root)')
check "and builds nothing there: it only shapes what exists" \
    "$([ -n "$(layout)" ] && [ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
create=$(sed -n '/^static void \*clock_create/,/^}/p' "$APP")
check "each screen is built once, when the app is created" \
    "$([ "$(printf '%s\n' "$create" | grep -cE '^    build_(main|add|confirm)\(a\);')" = 3 ] &&
       [ "$(grep -cE '^    build_(main|add|confirm)\(a\);' "$APP")" = 3 ] && echo 1 || echo 0)"
check "the screens sit in one frame that is the body's content box" \
    "$(grep -q 'lv_obj_set_size(frame, LV_PCT(100), LV_PCT(100));' "$APP" && echo 1 || echo 0)"
check "they are shaped again when the body changes size" \
    "$(printf '%s\n' "$create" |
       grep -q 'lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);' && echo 1 || echo 0)"
check "nothing calls back into the app once it is freed" \
    "$(sed -n '/^static void clock_destroy/,/^}/p' "$APP" |
       grep -q 'lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);' && echo 1 || echo 0)"
hits=$(code | grep -nE 'corners|top_left|top_right|bottom_left|bottom_right')
check "the corner clearance is PocketUI's one rule, read from the display geometry" \
    "$(grep -q 'pos_display_rect_insets(pocketui_display_geometry(),' "$APP" && [ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the wide shape keeps both halves at least portrait-wide" \
    "$(grep -q 'w > h && w >= 2 \* CLOCK_COLUMN_W + POCKETUI_PAD' "$APP" && echo 1 || echo 0)"

echo "clock_lint: $failed failure(s)"
exit $((failed > 0))
