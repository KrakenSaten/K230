#!/bin/bash
# PocketCalculator layering, and the scope it is held to: a simple calculator
# that computes and forgets. The engine and the view are pure; the app is a
# screen over them that stores nothing, reads no clock, talks to nobody and
# brings no keyboard of its own, because its keypad is the input.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

PURE="apps/calculator/calc_engine.c apps/calculator/calc_engine.h \
      apps/calculator/calc_view.c apps/calculator/calc_view.h"
APP="apps/calculator/calc_app.c"
ALL="$PURE $APP"

# Prose is not code: these files explain at length what they deliberately do
# not do, and a comment naming a banned call must not read as one making it.
strip_prose() { grep -vE '^[[:space:]]*(/\*|\*|//)'; }
code() { cat "$@" 2>/dev/null | strip_prose; }

for f in $ALL; do
    check "$f exists" "$([ -f "$f" ] && echo 1 || echo 0)"
done

# ---- the pure half ------------------------------------------------------

hits=$(code $PURE | grep -nE '#include[[:space:]]*[<"](lvgl|lv_)|lv_[a-z]+_|LV_[A-Z]+_' )
check "the engine and the view include and name no LVGL" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

hits=$(code $PURE | grep -nE '#include[[:space:]]*"(app|pocketui|pos_[a-z]+)\.h"')
check "nor the shell's or PocketUI's headers" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# ---- nothing written, nothing read, nothing timed, nobody called ---------

hits=$(code $ALL | grep -nE '\bfopen\(|\bfreopen\(|\bopen\(|\bopenat\(|\bwrite\(|\bfwrite\(|\bfputs\(|\bfprintf\(|\bunlink\(|\bmkdir\(|\brename\(|\bopendir\(|\bfsync\(|\bremove\(')
check "no file I/O anywhere in the app" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(code $ALL | grep -nE '\bprintf\(|\bputs\(|\bputchar\(|stdout|stderr|LOG_(INFO|WARN|ERROR)')
check "and no console output or logging from it" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

hits=$(code $ALL | grep -nE 'clock_gettime|\btime\(|gettimeofday|localtime|gmtime|mktime|strftime|CLOCK_REALTIME|CLOCK_MONOTONIC|lv_tick_get|pocketos_shell_system_day')
check "no clock" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

hits=$(code $ALL | grep -nE '\bsocket\(|\bconnect\(|\bsend\(|\brecv\(|getaddrinfo|pocketipc|shell_ipc|curl|http')
check "no network and no IPC" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

hits=$(code $ALL | grep -nE '\bgetenv\(|\bsystem\(|\bpopen\(|\bfork\(|\bexec[lv]p?e?\(|pthread_|\bsleep\(|\busleep\(')
check "no environment, no processes, no threads, no sleeping" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# Nothing persists: no store, no settings, no paths to put one under.
hits=$(code $ALL | grep -niE 'store|POCKETOS_(STATE|CONFIG|DATA)|pocketpaths|settings_(get|set)')
check "there is no store" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "and no store file beside the app" \
    "$(ls apps/calculator/*store* >/dev/null 2>&1 && echo 0 || echo 1)"

# ---- the input ------------------------------------------------------------

# DS 17.4: the calculator's keypad is its input. It creates no touch keyboard,
# asks the shell for none, and has no text field for one to type into.
hits=$(grep -nE 'pos_keyboard\.h|lv_keyboard|pocketos_shell_keyboard|pocketui_text_field|lv_textarea' \
    $ALL 2>/dev/null)
check "no keyboard is created, included or asked for" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# One key sink in the focus group, and one map from key to action. The app
# does not look at which device a key came from.
check "exactly one object joins the focus group" \
    "$([ "$(code $APP | grep -c 'pos_input_add_obj')" = "1" ] && echo 1 || echo 0)"
check "keys are received as LV_EVENT_KEY" \
    "$(code $APP | grep -q 'LV_EVENT_KEY' && echo 1 || echo 0)"
check "and mapped only by the view" \
    "$(code $APP | grep -q 'calc_view_action_for_key' && echo 1 || echo 0)"
hits=$(code $APP | grep -nE "lv_indev_get_type|lv_indev_active|pos_input_indev|pos_input_push_key|'[0-9+*/=.,xXcCnN-]'")
check "the app neither branches on a key's source nor maps keys itself" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "keypad keys do not take focus from the sink when tapped" \
    "$(code $APP | grep -q 'lv_obj_clear_flag([a-z_]*, LV_OBJ_FLAG_CLICK_FOCUSABLE)' && echo 1 || echo 0)"
check "the view's key numbers are asserted against LVGL's" \
    "$([ "$(code $APP | grep -cE '_Static_assert\(CALC_KEY_(ENTER|ESC|BACKSPACE) == LV_KEY_')" = "3" ] &&
       echo 1 || echo 0)"

# ---- the arithmetic -------------------------------------------------------

# What reaches the display is formatted in one place, by rule. No %f or %g
# anywhere: either would be a second, different idea of how a number looks.
hits=$(code $ALL | grep -nE '%[-+ 0#]*[0-9]*(\.[0-9*]+)?(l|L)?[fgG]')
check "numbers are never printed with %f or %g" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "results go through calc_format_number" \
    "$(code apps/calculator/calc_engine.c | grep -q 'calc_format_number(r, ' && echo 1 || echo 0)"
hits=$(code $ALL | grep -nE '\bstrto[dlf]\(|\batof\(|\bsscanf\(|setlocale')
check "and no number is read back through the C library's locale" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# Scope: simple, not scientific. The product owner omitted percent.
for banned in percent sqrt memory recall parenthes sin cos tan log exp pow history; do
    hits=$(code $PURE | grep -niE "CALC_ACT_[A-Z_]*${banned}|\b${banned}\(")
    check "no $banned in the engine or the view" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
done
check "and exactly 19 actions" \
    "$([ "$(code apps/calculator/calc_engine.h | grep -cE '^[[:space:]]*CALC_ACT_[A-Z0-9_]+,')" = "19" ] &&
       echo 1 || echo 0)"

# ---- the screen -------------------------------------------------------------

# No motion at all, so there is nothing for reduced motion to switch off
# (DS 12), and no timer of its own.
hits=$(code $APP | grep -nE 'lv_anim|lv_timer_create|anim_duration|transition')
check "no animation and no timer" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "it has no tick" "$(code $APP | grep -q '\.tick = NULL,' && echo 1 || echo 0)"

# Role styles only (tests/style_lint.sh covers colours and fonts for every
# app); the one accent fill on this screen is = (DS 1, primary button).
check "= is the primary button" \
    "$(code $APP | grep -q 'POS_STYLE_BUTTON_PRIMARY_PRESSED' && echo 1 || echo 0)"
check "and the only one" \
    "$([ "$(code $APP | grep -c 'POS_STYLE_BUTTON_PRIMARY,')" = "1" ] && echo 1 || echo 0)"
hits=$(code $APP | grep -nE 'POS_STYLE_KEY_ENGAGED|POS_STYLE_CHIP_(RX|TX)')
check "no other bright fill" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# DS 22: the layout is chosen from the body the app is given, not from the
# orientation, and the corner clearance comes from the platform's description
# of the panel rather than a number of the app's own.
hits=$(code $APP | grep -nE 'pocketos_shell_orientation|POS_ROTATION_|lv_display_get_rotation|landscape|portrait')
check "the layout never asks which way the display is turned" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "it is redone when the body changes size" \
    "$(code $APP | grep -q 'LV_EVENT_SIZE_CHANGED, a);' && echo 1 || echo 0)"
check "the corner clearance is read from the display geometry" \
    "$(code $APP | grep -q 'pocketui_display_geometry()' && echo 1 || echo 0)"
hits=$(code $APP | grep -nE 'corners|top_left|top_right|bottom_left|bottom_right')
check "by PocketUI's one rule for it, not a copy of the app's own" \
    "$(code $APP | grep -q 'pos_display_rect_insets(pocketui_display_geometry(),' && [ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "and the wide layout is held to the touch minimum" \
    "$(code $APP | grep -q 'PAD_MIN_H (CALC_PAD_ROWS \* POCKETUI_TOUCH_MIN' && echo 1 || echo 0)"
check "keys meet the 64 px minimum" \
    "$(code $APP | grep -qE 'POCKETUI_TOUCH_MIN' && echo 1 || echo 0)"

# ---- the launcher -------------------------------------------------------------

check "the app is registered in the launcher" \
    "$(grep -q '&app_calculator' ui/shell/shell.c && echo 1 || echo 0)"
check "declared there" \
    "$(grep -q 'extern const struct pocketos_app app_calculator;' ui/shell/shell.c && echo 1 || echo 0)"
check "exactly once" \
    "$([ "$(grep -c '&app_calculator' ui/shell/shell.c)" -eq 1 ] && echo 1 || echo 0)"
check "right after Calendar" \
    "$(grep -q '&app_calendar, &app_calculator' ui/shell/shell.c && echo 1 || echo 0)"
check "built into the shell" \
    "$(grep -q 'apps/calculator/calc_app.c' ui/shell/CMakeLists.txt &&
       grep -q 'apps/calculator/calc_engine.c' ui/shell/CMakeLists.txt &&
       grep -q 'apps/calculator/calc_view.c' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
check "with its directory on the include path" \
    "$(grep -q '"${REPO_DIR}/apps/calculator"' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
check "and its host tests run by make test" \
    "$(grep -q './tests/calc_engine_test' Makefile && grep -q './tests/calc_view_test' Makefile &&
       echo 1 || echo 0)"

echo "calculator_lint: $failed failure(s)"
exit $((failed > 0))
