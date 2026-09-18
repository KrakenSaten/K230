#!/bin/bash
# PocketRadar structural rules (see apps/radar/engine/radar_types.h):
#   1. the game engine stays free of LVGL, so it is unit-tested natively;
#   2. the engine does no I/O at all -- unlike PocketFleet, PocketRadar keeps
#      its store outside engine/, so the engine is pure computation;
#   3. radar_store.c is the only part of the app that touches the filesystem;
#   4. the engine uses no floating point and calls no libm, so a run replays
#      bit-identically on the host and on the K230;
#   5. the engine takes no entropy from the platform: no rand(), no clock;
#   6. the layout is chosen from the body, never from an orientation, and the
#      corner clearance is taken from the one platform rule (DS 21.3, 22.2,
#      29.1);
#   7. the scope's geometry has one source, so what is drawn and what is
#      touched can never disagree;
#   8. nothing lays out on a tick. The clock steps the run and invalidates the
#      scope; it must not size, move or reshape anything, because that is what
#      would turn a 20 Hz repaint into a 20 Hz relayout.
# Fails the build when any of them is broken.
set -u
cd "$(dirname "$0")/.." || exit 1
APP=apps/radar
ENGINE=$APP/engine
IO_CALLS='\b(fopen|open|creat|unlink|rename|mkdir|remove|read|write)[[:space:]]*\('
failed=0
check() { # <label> <files> <regex>
    hits=$(grep -nE "$3" $2 /dev/null 2>/dev/null)
    if [ -n "$hits" ]; then
        echo "FAIL $1:"; echo "$hits" | head -10; failed=$((failed + 1))
    else
        echo "ok   $1"
    fi
}

check "engine is free of LVGL" "$ENGINE/*.c $ENGINE/*.h" \
      'include[[:space:]]*[<"]lvgl|lv_obj_|lv_style_'
check "engine does no I/O" "$ENGINE/*.c $ENGINE/*.h" "$IO_CALLS"
check "engine uses no floating point" "$ENGINE/*.c $ENGINE/*.h" \
      'include[[:space:]]*<math\.h>|\b(float|double)\b|\b(sqrtf?|sinf?|cosf?|tanf?|atan2f?|powf?|floorf?|ceilf?|fabsf?|roundf?)[[:space:]]*\('
check "engine takes no entropy from the platform" "$ENGINE/*.c $ENGINE/*.h" \
      '\b(rand|srand|random|srandom|time|clock|gettimeofday|clock_gettime|getpid)[[:space:]]*\('

# The store is the app's single door to the filesystem. It does not exist
# before P4; the rule then holds vacuously, which is the correct answer.
others=$(find $APP -name '*.c' -not -name 'radar_store.c' 2>/dev/null | tr '\n' ' ')
check "only radar_store.c touches the filesystem" "$others" "$IO_CALLS"

# The view model, when it exists, stays free of LVGL for the same reason the
# engine does: it is what makes a headless test of the screens possible.
view=$(ls $APP/ui/radar_view.c $APP/ui/radar_view.h 2>/dev/null | tr '\n' ' ')
if [ -n "$view" ]; then
    check "the view model is free of LVGL" "$view" \
          'include[[:space:]]*[<"]lvgl|lv_obj_|lv_style_'
fi

want() { # <label> <files> <regex>
    hits=$(grep -nE "$3" $2 /dev/null 2>/dev/null)
    if [ -z "$hits" ]; then
        echo "FAIL $1: not found"; failed=$((failed + 1))
    else
        echo "ok   $1"
    fi
}

UI="$APP/radar_app.c $APP/ui/radar_screens.c"

# 6. The shape comes from the body's size. Nothing in the app may ask which
#    way up the display is, or work the corner clearance out for itself.
check "the app never asks for the orientation" "$UI $APP/ui/radar_scope.c" \
      'pocketos_shell_orientation|pocketos_rotation_mode|POCKETOS_ROTATION|lv_display_get_rotation'
check "nor works the corner clearance out for itself" "$UI" \
      'corners\.|top_left|bottom_left|bottom_right|top_right'
want "the corner clearance comes from the platform rule" "$APP/radar_app.c" \
     'pos_display_rect_insets'
want "and the shape from the body's own box" "$APP/radar_app.c" \
     'LV_EVENT_SIZE_CHANGED'
want "a repeated layout pass is refused" "$APP/radar_app.c" \
     'memcmp\(&box, &app->laid_out'
want "the wide scope can never be larger than the tall one" "$APP/radar_app.c" \
     'RADAR_SCOPE_TALL : \(int\)h'

# 7. One stored geometry per scope. A screen may ask the scope to change size;
#    it may not work out a radius or a centre itself.
check "only the scope works in radius and centre" "$UI" \
      '->radius|->centre|RIM_INSET'
want "the scope's size has one setter" "$APP/ui/radar_scope.c" \
     'void radar_scope_set_size'
want "and the tap conversion reads the geometry the drawing uses" \
     "$APP/ui/radar_scope.c" 's->radius'

# 8. The tick path must not lay anything out. radar_screen_scan_tick() is what
#    runs twenty times a second; it may refresh values and invalidate the
#    scope, and nothing else.
tick=$(sed -n '/^void radar_screen_scan_tick/,/^}/p' $APP/ui/radar_screens.c)
if printf '%s' "$tick" | grep -qE 'lv_obj_set_(size|width|height|pos|flex|style_pad)|_relayout|lv_obj_set_parent|lv_obj_create'; then
    echo "FAIL the tick lays something out:"
    printf '%s' "$tick" | grep -nE 'lv_obj_set_(size|width|height|pos|flex|style_pad)|_relayout|lv_obj_set_parent|lv_obj_create' | head -5
    failed=$((failed + 1))
else
    echo "ok   the tick lays nothing out"
fi
if printf '%s' "$tick" | grep -q 'lv_obj_invalidate(app->scan->scope)'; then
    echo "ok   and invalidates the scope and nothing else"
else
    echo "FAIL the tick no longer invalidates just the scope"; failed=$((failed + 1))
fi

echo "radar_lint: $failed failure(s)"
exit $((failed > 0))
