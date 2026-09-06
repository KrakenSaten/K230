#!/bin/bash
# PocketTimber structural rules (see apps/timber/engine/timber_types.h):
#   1. the game engine stays free of LVGL, so it is unit-tested natively;
#   2. the engine does no I/O at all: like PocketRadar, the store (when it
#      exists) lives beside the app, outside engine/;
#   3. timber_store.c is the only part of the app that touches the filesystem;
#   4. the engine uses no floating point and calls no libm, so a run replays
#      bit-identically on the host and on the K230;
#   5. the engine takes no entropy from the platform: no rand(), no clock.
# Fails the build when any of them is broken.
set -u
cd "$(dirname "$0")/.." || exit 1
APP=apps/timber
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
# before P7; the rule then holds vacuously, which is the correct answer.
others=$(find $APP -name '*.c' -not -name 'timber_store.c' 2>/dev/null | tr '\n' ' ')
check "only timber_store.c touches the filesystem" "$others" "$IO_CALLS"

# The view model, when it exists, stays free of LVGL for the same reason the
# engine does: it is what makes a headless test of the screens possible.
view=$(ls $APP/ui/timber_view.c $APP/ui/timber_view.h 2>/dev/null | tr '\n' ' ')
if [ -n "$view" ]; then
    check "the view model is free of LVGL" "$view" \
          'include[[:space:]]*[<"]lvgl|lv_obj_|lv_style_'
fi

echo "timber_lint: $failed failure(s)"
exit $((failed > 0))
