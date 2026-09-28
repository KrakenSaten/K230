#!/bin/bash
# PG 2048 layering, and the rules the game is held to:
#   1. the engine is pure: no LVGL, no I/O, no floating point, no platform
#      entropy, so a game replays bit-identically on the host and the K230;
#   2. the view model is LVGL-free too, and does no I/O;
#   3. g2048_store.c is the only file that touches the filesystem;
#   4. nothing in the game names a colour or a font (style_lint.sh covers
#      every app; this repeats it for the game's own files so a reviewer of
#      the branch sees it here);
#   5. input is one key stream and one map, and no keyboard is created;
#   6. layout comes from the view, not from the panel's size;
#   7. the app is registered, built and tested.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

APP=apps/2048
ENGINE="$APP/engine/g2048_rng.c $APP/engine/g2048_rng.h $APP/engine/g2048_rules.c $APP/engine/g2048_rules.h"
VIEW="$APP/ui/g2048_view.c $APP/ui/g2048_view.h"
LVGL_SIDE="$APP/g2048_app.c $APP/ui/g2048_board.c $APP/ui/g2048_board.h"
STORE="$APP/g2048_store.c $APP/g2048_store.h"
ALL="$ENGINE $VIEW $LVGL_SIDE $STORE $APP/g2048_app.h"

strip_prose() { grep -vE '^[[:space:]]*(/\*|\*|//)'; }
code() { cat "$@" 2>/dev/null | strip_prose; }

for f in $ALL; do
    check "$f exists" "$([ -f "$f" ] && echo 1 || echo 0)"
done

IO='\b(fopen|freopen|open|openat|creat|unlink|rename|mkdir|remove|fwrite|fread|fsync|opendir)[[:space:]]*\('

# ---- the engine ----------------------------------------------------------
hits=$(code $ENGINE | grep -nE '#include[[:space:]]*[<"](lvgl|lv_)|lv_[a-z]+_|LV_[A-Z]+_')
check "the engine names no LVGL" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $ENGINE | grep -nE "$IO|\bprintf\(|stdout|stderr")
check "the engine does no I/O" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $ENGINE | grep -nE 'include[[:space:]]*<math\.h>|\b(float|double)\b')
check "the engine uses no floating point" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $ENGINE | grep -nE '\b(rand|srand|random|srandom|time|clock|gettimeofday|clock_gettime|getpid|getenv)[[:space:]]*\(')
check "the engine takes no entropy or environment from the platform" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3

# ---- the view model ------------------------------------------------------
hits=$(code $VIEW | grep -nE '#include[[:space:]]*[<"](lvgl|lv_|pocketui|pos_styles|app\.h)|lv_[a-z]+_')
check "the view model names no LVGL and no shell" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $VIEW | grep -nE "$IO|\bgetenv\(|\btime\(")
check "the view model does no I/O and reads no clock" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $VIEW $ENGINE | grep -nE '\b(568|1232|1176|1060)\b')
check "no panel dimension appears in the rules or the view" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3

# ---- files ------------------------------------------------------------------
hits=$(code $ENGINE $VIEW $LVGL_SIDE | grep -nE "$IO")
check "only g2048_store.c touches the filesystem" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3

# ---- colour and type --------------------------------------------------------
hits=$(code $ALL | grep -nE 'lv_color_hex|lv_color_make|lv_palette_|lv_color_white|lv_color_black|0x[0-9a-fA-F]{6}\b|&pos_font_|lv_font_montserrat_|lv_obj_set_style_(bg|text|border|outline)_(color|font)')
check "no colour or font is named in the game" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
check "the board repaints on a theme change" \
    "$(code $APP/ui/g2048_board.c | grep -q 'pos_theme_watch' && echo 1 || echo 0)"

# ---- input -------------------------------------------------------------------
hits=$(grep -nE 'pos_keyboard\.h|lv_keyboard|pocketos_shell_keyboard|pocketui_text_field|lv_textarea' $ALL)
check "no keyboard is created or asked for" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
check "exactly one object joins the focus group" \
    "$([ "$(code $APP/g2048_app.c | grep -c 'pos_input_add_obj')" = "1" ] && echo 1 || echo 0)"
check "keys go through the view's map" \
    "$(code $APP/g2048_app.c | grep -q 'g2048_view_command_for_key' && echo 1 || echo 0)"
hits=$(code $APP/g2048_app.c | grep -nE "pos_input_push_key|'[wasdnWASDN]'|LV_KEY_(UP|DOWN|LEFT|RIGHT) *[:)]")
check "the app maps no key itself" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
check "buttons do not take focus when tapped" \
    "$(code $APP/g2048_app.c | grep -q 'LV_OBJ_FLAG_CLICK_FOCUSABLE' && echo 1 || echo 0)"
check "the view's key numbers are asserted against LVGL's" \
    "$([ "$(code $APP/g2048_app.c | grep -c '_Static_assert(G2048_KEY_')" = "7" ] && echo 1 || echo 0)"

# ---- layout and motion -------------------------------------------------------
check "blocks are placed from the view's layout" \
    "$(code $APP/g2048_app.c | grep -q 'g2048_view_layout' && echo 1 || echo 0)"
check "and placed again when the area changes size" \
    "$(code $APP/g2048_app.c | grep -q 'LV_EVENT_SIZE_CHANGED' && echo 1 || echo 0)"
check "motion honours reduced motion" \
    "$(code $APP/g2048_app.c | grep -q 'pocketos_shell_reduced_motion' && echo 1 || echo 0)"

# ---- the launcher and the build ------------------------------------------------
check "the app is declared in the shell" \
    "$(grep -q 'extern const struct pocketos_app app_2048;' ui/shell/shell.c && echo 1 || echo 0)"
check "registered once" "$([ "$(grep -c '&app_2048\b' ui/shell/shell.c)" = "1" ] && echo 1 || echo 0)"
# Where it is shown is the launcher's table, not the registry order
# (ui/shell/home_layout.c): a game, in PLAY, in the games colour.
check "the launcher shows it in PLAY" \
    "$(grep -q '{ "2048", HOME_GROUP_PLAY, HOME_HUE_GAMES }' ui/shell/home_layout.c && echo 1 || echo 0)"
for src in g2048_app.c g2048_store.c engine/g2048_rng.c engine/g2048_rules.c ui/g2048_view.c ui/g2048_board.c; do
    check "the shell builds $src" "$(grep -q "apps/2048/$src" ui/shell/CMakeLists.txt && echo 1 || echo 0)"
done
check "the app test is a host-only target" \
    "$(awk '/if\(POCKETOS_DISPLAY STREQUAL "sdl"\)/,/^endif\(\)/' ui/shell/CMakeLists.txt |
       grep -q 'add_executable(g2048_app_test' && echo 1 || echo 0)"
for t in g2048_rules_test g2048_store_test g2048_view_test g2048_theme_test; do
    check "make test runs $t" "$(grep -q "./tests/$t" Makefile && grep -q "/tests/$t\$" .gitignore && echo 1 || echo 0)"
done

echo "g2048_lint: $failed failure(s)"
exit $((failed > 0))
