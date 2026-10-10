#!/bin/bash
# DeskBuddy's layering (docs/apps/DESKBUDDY.md):
#   1. the vision boundary: nothing in DeskBuddy includes the Vision app,
#      core/pocketvision or the camera - vision arrives as db_vision_event
#      from a provider, and that is the whole contract;
#   2. the core (brain, face, guard log, preferences, vision queue and mock)
#      is pure: no LVGL, no I/O, no clock of its own;
#   3. db_store.c is the only file that touches the filesystem;
#   4. the identity interface is declarations only: no recognizer exists in
#      v0.1, and no embedding reaches the brain or the screen;
#   5. no colour or font is named (style_lint.sh covers every app);
#   6. the app is registered, built and tested.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

APP=apps/deskbuddy
CORE="$APP/db_brain.c $APP/db_brain.h $APP/db_face.c $APP/db_face.h $APP/db_gesture.c $APP/db_gesture.h $APP/db_guard.c $APP/db_guard.h
      $APP/db_prefs.c $APP/db_prefs.h $APP/db_vision.c $APP/db_vision.h $APP/db_vision_mock.c $APP/db_vision_mock.h"
STORE="$APP/db_store.c $APP/db_store.h"
SCREEN="$APP/deskbuddy_app.c $APP/deskbuddy_app.h"
IDENTITY="$APP/db_identity.h"
ALL="$CORE $STORE $SCREEN $IDENTITY"

strip_prose() { grep -vE '^[[:space:]]*(/\*|\*|//)'; }
# Code only: whole-line comments and trailing comments go.
code() { cat "$@" 2>/dev/null | strip_prose | sed -e 's|/\*.*\*/||g' -e 's|//.*$||'; }

for f in $ALL; do
    check "$f exists" "$([ -f "$f" ] && echo 1 || echo 0)"
done

IO='\b(fopen|freopen|open|openat|creat|unlink|rename|mkdir|remove|fwrite|fread|fsync|opendir|write|read)[[:space:]]*\('

# ---- 1. the vision boundary -------------------------------------------------
hits=$(code $ALL | grep -nE '#include[[:space:]]*[<"]([^">]*/)?(vision_|pocketvision|pocketcam|camera_)')
check "DeskBuddy includes nothing of Vision, pocketvision or the camera" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
hits=$(grep -rnE 'apps/deskbuddy|db_vision|deskbuddy' apps/vision apps/camera core/pocketvision core/pocketcam 2>/dev/null)
check "and nothing of Vision or the camera knows about DeskBuddy" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3

# ---- 2. the core is pure ------------------------------------------------------
hits=$(code $CORE | grep -nE '#include[[:space:]]*[<"](lvgl|lv_|pocketui|pos_|app\.h)|lv_[a-z]+_')
check "the core names no LVGL and no shell" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $CORE | grep -nE "$IO|\bprintf\(|stdout|stderr|\bgetenv\(")
check "the core does no I/O and reads no environment" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $CORE | grep -nE '\b(rand|srand|random|time|clock|gettimeofday|clock_gettime|sleep|usleep|nanosleep)[[:space:]]*\(')
check "the core takes no clock or entropy of its own, and never sleeps" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $CORE | grep -nE '\b(malloc|calloc|realloc|free)[[:space:]]*\(')
check "the core allocates nothing: every buffer is fixed" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3

# ---- 3. files ---------------------------------------------------------------------
hits=$(code $CORE $SCREEN | grep -nE "$IO")
check "only db_store.c touches the filesystem" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
check "the store's files are private (0700 directory, 0600 files)" \
    "$(grep -q 'pocketos_mkdir_p(db_store_dir(), 0700)' $APP/db_store.c && grep -q 'O_CREAT | O_TRUNC | O_CLOEXEC, 0600' $APP/db_store.c && echo 1 || echo 0)"

# ---- 4. identity ------------------------------------------------------------------
hits=$(code $IDENTITY | grep -nE '^[^#]*\)[[:space:]]*\{')
check "db_identity.h declares, it implements nothing" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(code $CORE $SCREEN $STORE | grep -nE 'db_identity\.h|db_face_embedding|db_owner_profile|db_recognizer')
check "no embedding or recognizer reaches the brain, the store or the screen" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3

# ---- 5. colour and type -------------------------------------------------------------
hits=$(code $ALL | grep -nE 'lv_color_hex|lv_color_make|lv_palette_|lv_color_white|lv_color_black|&pos_font_|lv_font_montserrat_|lv_obj_set_style_(bg|text|border|outline|line|shadow)_color')
check "no colour or font is named" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3

# ---- 6. registered, built, tested ---------------------------------------------------
check "the shell registers app_deskbuddy" \
    "$(grep -q 'extern const struct pocketos_app app_deskbuddy;' ui/shell/shell.c && grep -q '&app_deskbuddy' ui/shell/shell.c && echo 1 || echo 0)"
check "the launcher places it" "$(grep -qE '\{ "deskbuddy", HOME_GROUP_[A-Z]+, HOME_HUE_[A-Z]+, HOME_FOLDER_[A-Z]+ \}' ui/shell/home_layout.c && echo 1 || echo 0)"
check "the shell builds every DeskBuddy source" \
    "$(n=0; for f in $APP/*.c; do grep -q "\${REPO_DIR}/$f" ui/shell/CMakeLists.txt || n=1; done; [ $n = 0 ] && echo 1 || echo 0)"
check "make test runs the DeskBuddy suites and this lint" \
    "$(grep -q 'DESKBUDDY_TEST_RUN' Makefile && grep -q 'bash tests/deskbuddy_lint.sh' Makefile && echo 1 || echo 0)"
check "the timer is deleted in destroy" \
    "$(awk '/^static void deskbuddy_destroy/,/^}/' $APP/deskbuddy_app.c | grep -q 'lv_timer_delete' && echo 1 || echo 0)"
check "the developer keys and the script exist only behind \$DESKBUDDY_SIM" \
    "$(grep 'getenv("DESKBUDDY_' $APP/deskbuddy_app.c | grep -vc 'DESKBUDDY_VISION' | grep -qx 2 && grep -q 'a->sim && key' $APP/deskbuddy_app.c && echo 1 || echo 0)"

# ---- 7. the bridge to Vision ----------------------------------------------------------
# apps/deskbuddy_vision is the one place DeskBuddy's boundary and Vision's
# helper client meet: the camera and the models stay in Vision's helper.
BRIDGE=apps/deskbuddy_vision
check "the bridge includes DeskBuddy's boundary and Vision's helper client" \
    "$(code $BRIDGE/*.c $BRIDGE/*.h | grep -q '#include "db_vision' && code $BRIDGE/*.c | grep -q '#include "vision_session.h"' && echo 1 || echo 0)"
hits=$(code $BRIDGE/*.c $BRIDGE/*.h | grep -nE '#include[[:space:]]*[<"]([^">]*/)?(pocketcam|pocketvision|vision_kpu|vision_app|vision_model|lvgl|lv_|pocketui)')
check "and nothing else of Vision, the camera, the models or the screen" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $BRIDGE/*.c | grep -nE "$IO")
check "the bridge touches no file (the owner stays in Vision's helper)" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
check "the shell builds the bridge" \
    "$(n=0; for f in $BRIDGE/*.c; do grep -q "\${REPO_DIR}/$f" ui/shell/CMakeLists.txt || n=1; done; [ $n = 0 ] && echo 1 || echo 0)"
check "DeskBuddy picks it unless simulated or \$DESKBUDDY_VISION is none" \
    "$(awk '/^static void choose_provider/,/^}/' $APP/deskbuddy_app.c | grep -q 'db_vision_pipeline_ops' && awk '/^static void choose_provider/,/^}/' $APP/deskbuddy_app.c | grep -q '"none"' && echo 1 || echo 0)"
check "stop() is called in destroy" \
    "$(awk '/^static void deskbuddy_destroy/,/^}/' $APP/deskbuddy_app.c | grep -q 'provider.ops->stop' && echo 1 || echo 0)"
# Companion plays by touch: the camera is started only for Guard and Night,
# in one place, and stopped again on the way back to Companion.
SYNC=$(awk '/^static void sync_provider\(struct deskbuddy_app \*a, int64_t now\)$/,/^}/' $APP/deskbuddy_app.c | strip_prose)
check "the provider is started and stopped only by the mode (sync_provider)" \
    "$(printf '%s\n' "$SYNC" | grep -q 'DB_MODE_COMPANION' && printf '%s\n' "$SYNC" | grep -q 'ops->start' &&
       printf '%s\n' "$SYNC" | grep -q 'ops->stop' &&
       [ "$(code $APP/deskbuddy_app.c | grep -c 'ops->start')" = 1 ] && echo 1 || echo 0)"

# ---- 8. personality needs no camera ---------------------------------------------------
# Touch, the snack and rest reach the brain as a db_stimulus. None of that
# path may start, poll or name a vision provider: DeskBuddy plays the same
# with no camera and no model.
PLAY=$(awk '/^\/\* ---- stimuli and the snack/,/^\/\* ---- layout/' $APP/deskbuddy_app.c
       for f in on_face_press on_face_pressing on_face_release on_face_lost feed_or_give rest_or_wake on_feed on_rest refresh_care; do
           awk "/^static void $f\\(/,/^}/" $APP/deskbuddy_app.c
       done)
check "the play path is in place" "$(printf '%s\n' "$PLAY" | grep -q 'db_brain_stimulus' && echo 1 || echo 0)"
hits=$(printf '%s\n' "$PLAY" | strip_prose | grep -nE 'provider|db_vision_|pipeline')
check "and starts, polls or names no vision provider" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $APP/db_gesture.c $APP/db_gesture.h | grep -nE '#include[[:space:]]*"db_(vision|brain)')
check "the gesture classifier knows nothing of vision or the brain" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(awk '/^bool db_brain_stimulus\(/,/^}/' $APP/db_brain.c | strip_prose | grep -nE 'db_vision|seen')
check "a stimulus never stands in for what vision saw" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3

echo "deskbuddy_lint: $failed failure(s)"
exit $((failed > 0))
