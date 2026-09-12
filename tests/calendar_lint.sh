#!/bin/bash
# PocketCalendar layering, and the two rules this app exists to keep: it reads
# no clock of its own, and it is a calendar rather than a scheduler.
#
# The scope rules are the mirror image of the ones in clock_lint.sh. That file
# keeps calendars out of the clock; this one keeps appointments out of the
# calendar, which is the direction a month view drifts if nobody is watching.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

PURE="apps/calendar/cal_date.c apps/calendar/cal_date.h \
      apps/calendar/cal_view.c apps/calendar/cal_view.h"
ALL="apps/calendar/cal_date.c apps/calendar/cal_date.h \
     apps/calendar/cal_view.c apps/calendar/cal_view.h \
     apps/calendar/cal_app.c"

# Prose is not code. Comment lines are dropped before any of the bans below
# are looked for, because these files explain at length what they deliberately
# do not call - a comment saying "a module that called strftime would be
# reading a clock" must not read as one calling it. LVGL's own event
# vocabulary goes too, or every lv_obj_add_event_cb would read as a calendar
# event; what is left of the word "event" is the app's own.
strip_prose() {
    grep -vE '^[[:space:]]*(/\*|\*|//)' |
        sed -E 's/lv_event[a-z_]*//g; s/LV_EVENT_[A-Z_]*//g; s/add_event_cb//g'
}
code() { cat $ALL 2>/dev/null | strip_prose; }

check "the app exists" "$([ -f apps/calendar/cal_app.c ] && echo 1 || echo 0)"

hits=$(grep -lE 'lvgl|lv_obj|lv_label|lv_timer' $PURE 2>/dev/null)
check "the date arithmetic and the model are free of LVGL" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# Nothing is stored. There are no events to keep and a selection is not worth
# a file, so there is no store to get wrong and no file to corrupt.
hits=$(grep -lE 'fopen|\bopen\(|unlink|mkdir|rename|opendir|fsync' $ALL 2>/dev/null)
check "nothing in the app touches the filesystem" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# The rule the whole design rests on: the date is handed in. One reader of the
# wall clock means one answer to what day it is, and one place that decides
# whether the board knows - PocketClock's, through the shell.
hits=$(code | grep -nE 'clock_gettime|\btime\(|localtime|gmtime|mktime|strftime|CLOCK_REALTIME|CLOCK_MONOTONIC')
check "nothing in the app reads a clock" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "it asks the shell for the date instead" \
    "$(grep -q 'pocketos_shell_system_day' apps/calendar/cal_app.c && echo 1 || echo 0)"
# And does not reach into PocketClock to get it either.
hits=$(grep -nE '#include[[:space:]]*"clock_' $ALL 2>/dev/null)
check "and does not include PocketClock's headers" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# An unset clock is the ordinary state on a board with no RTC. The epoch must
# never be drawn as a date, and no cell may be marked today while the date is
# not set.
check "the model refuses a date it was not given" \
    "$(grep -q 'yyyymmdd < 0' apps/calendar/cal_date.c && echo 1 || echo 0)"
check "and the shell says so with -1 rather than the epoch" \
    "$(grep -q 'w->valid ? w->day : -1' ui/shell/shell.c && echo 1 || echo 0)"
check "today is marked only when the date is set" \
    "$(grep -q 'v->have_today && cal_date_equal' apps/calendar/cal_view.c && echo 1 || echo 0)"
check "and the app has a state for a date it does not know" \
    "$(grep -q '"Date not set"' apps/calendar/cal_app.c && echo 1 || echo 0)"

# Today and the selection are different things and are drawn differently, so
# neither relies on colour alone (DS 2) and a day can be both at once.
check "the selection is the DS selected outline" \
    "$(grep -q 'POS_STYLE_SELECTED' apps/calendar/cal_app.c && echo 1 || echo 0)"
check "and that role carries no fill of its own" \
    "$(sed -n '/styles\[POS_STYLE_SELECTED\]/,/^$/p' ui/pocketui/pos_styles.c |
       grep -q 'bg_color' && echo 0 || echo 1)"
check "today is the accent, and says so in words too" \
    "$(grep -q 'POS_STYLE_ACCENT_TEXT' apps/calendar/cal_app.c &&
       grep -q 'TODAY_SUFFIX' apps/calendar/cal_view.c && echo 1 || echo 0)"

# Monday-first is the column order, and it is the arithmetic that decides it,
# not a table somebody can reorder.
check "the weekday is Monday-first" \
    "$(grep -q '0 = Monday' apps/calendar/cal_date.h && echo 1 || echo 0)"
check "the grid's leading blanks are the first's weekday" \
    "$(grep -q 'lead = cal_weekday(&first)' apps/calendar/cal_date.c && echo 1 || echo 0)"

# An empty cell must keep its place in the row. LV_OBJ_FLAG_HIDDEN would take
# it out of the flex layout, close the gap and shift the week left - which is
# exactly the leading gap that puts the first of the month in the right column.
hits=$(sed -n '/^static void paint_cell/,/^}/p' apps/calendar/cal_app.c | strip_prose |
       grep -nE 'set_hidden\(cell|LV_OBJ_FLAG_HIDDEN')
check "a blank cell is blanked rather than hidden" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# No motion at all, so there is nothing for the reduced-motion setting to
# switch off (DS 12): a month change is applied immediately.
hits=$(grep -nE 'lv_anim|lv_timer_create' $ALL 2>/dev/null)
check "there is no animation and no timer of its own" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# DS 17.4: the app asks the shell for a keyboard or does without. This one has
# no text to enter and must not grow a keyboard, a text field or a key handler.
hits=$(grep -nE 'pos_keyboard|lv_keyboard|pocketui_text_field|pos_input|LV_KEY_|pocketos_shell_keyboard' \
    $ALL 2>/dev/null)
check "it needs no keyboard and names none" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# Scope: a calendar, not a scheduler.
for banned in appointment event reminder alarm notification recurrence rrule ical \
              agenda sync exchange gcal attendee invite timezone ntp cron; do
    hits=$(code | grep -niE "[a-z_]*${banned}[a-z_]*[[:space:]]*[(=]|${banned}_[a-z_]*")
    check "no $banned machinery in the app" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
done

# Nothing persists, which is also a scope rule: a store is where events would
# arrive first.
hits=$(code | grep -niE 'cal_store|store_save|store_load|POCKETOS_DATA|pocketpaths')
check "there is no store" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3

# Registered once, in the one launcher table.
check "the app is registered in the launcher" \
    "$(grep -q '&app_calendar' ui/shell/shell.c && echo 1 || echo 0)"
check "and declared there" \
    "$(grep -q 'extern const struct pocketos_app app_calendar;' ui/shell/shell.c && echo 1 || echo 0)"
check "exactly once" \
    "$([ "$(grep -c '&app_calendar' ui/shell/shell.c)" -eq 1 ] && echo 1 || echo 0)"
check "and built into the shell" \
    "$(grep -q 'apps/calendar/cal_app.c' ui/shell/CMakeLists.txt && echo 1 || echo 0)"

echo "calendar_lint: $failed failure(s)"
exit $((failed > 0))
