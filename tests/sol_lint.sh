#!/bin/bash
# PG Solitaire layering, and the rules the game is held to:
#   1. cards, shuffle and rules are pure: no LVGL, no I/O, no floating point,
#      no platform entropy;
#   2. the view model - layout, hits, the keyboard and touch interaction - is
#      LVGL-free and does no I/O;
#   3. sol_store.c is the only file that touches the filesystem, and the rules
#      and the view model stay out of it;
#   4. no colour is written in the game's C sources: interface colours are
#      tokens, card colours are art in apps/solitaire/art/cards_palette.txt,
#      generated into the build by CMake;
#   5. the felt is PocketTimber's, reached through sol_felt.c alone;
#   6. input is one key sink and one map; no keyboard is created; nothing
#      tapped takes the focus; there is no drag;
#   7. the app is registered, built and tested.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

APP=apps/solitaire
ENGINE="$APP/engine/sol_rng.c $APP/engine/sol_rng.h $APP/engine/sol_cards.c $APP/engine/sol_cards.h \
        $APP/engine/sol_rules.c $APP/engine/sol_rules.h"
VIEW="$APP/ui/sol_view.c $APP/ui/sol_view.h"
LVGL_SIDE="$APP/sol_app.c $APP/sol_app.h $APP/ui/sol_card_draw.c $APP/ui/sol_card_draw.h \
           $APP/ui/sol_table_widget.c $APP/ui/sol_table_widget.h $APP/ui/sol_felt.c $APP/ui/sol_felt.h \
           $APP/ui/sol_palette.h"
ALL="$ENGINE $VIEW $LVGL_SIDE"

strip_prose() { grep -vE '^[[:space:]]*(/\*|\*|//)'; }
code() { cat "$@" 2>/dev/null | strip_prose; }

for f in $ALL $APP/art/cards_palette.txt; do
    check "$f exists" "$([ -f "$f" ] && echo 1 || echo 0)"
done

IO='\b(fopen|freopen|open|openat|creat|unlink|rename|mkdir|remove|fwrite|fread|fsync|opendir)[[:space:]]*\('

hits=$(code $ENGINE | grep -nE '#include[[:space:]]*[<"](lvgl|lv_)|lv_[a-z]+_')
check "the rules name no LVGL" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $ENGINE | grep -nE "$IO|\bprintf\(|stdout|stderr")
check "the rules do no I/O" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $ENGINE | grep -nE 'include[[:space:]]*<math\.h>|\b(float|double)\b')
check "the rules use no floating point" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $ENGINE | grep -nE '\b(rand|srand|random|srandom|time|clock|gettimeofday|clock_gettime|getpid|getenv)[[:space:]]*\(')
check "the rules take no entropy from the platform" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3

hits=$(code $VIEW | grep -nE '#include[[:space:]]*[<"](lvgl|lv_|pocketui|pos_|app\.h)|lv_[a-z]+_')
check "the view model names no LVGL and no shell" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $VIEW | grep -nE "$IO|\bgetenv\(|\btime\(")
check "the view model does no I/O and reads no clock" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $VIEW $ENGINE | grep -nE '\b(568|1232|1176|1060)\b')
check "no panel dimension appears in the rules or the view" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3

STORE="$APP/sol_store.c $APP/sol_store.h"
for f in $STORE; do
    check "$f exists" "$([ -f "$f" ] && echo 1 || echo 0)"
done
hits=$(code $ALL | grep -nE "$IO|POCKETOS_STATE_DIR|pocketpaths|settings_(get|set)")
check "only sol_store.c touches the filesystem" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $STORE | grep -nE '#include[[:space:]]*[<"](lvgl|lv_|pocketui|pos_)|lv_[a-z]+_')
check "the store names no LVGL" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
check "the store writes atomically: temp file, fsync, rename" \
    "$(code $APP/sol_store.c | grep -q 'fsync(' && code $APP/sol_store.c | grep -q 'rename(tmp, path)' && echo 1 || echo 0)"
check "and refuses what the rules call impossible" "$(code $APP/sol_store.c | grep -q 'sol_game_valid(&tmp)' && echo 1 || echo 0)"
check "the shell builds the store" "$(grep -q 'apps/solitaire/sol_store.c' ui/shell/CMakeLists.txt && echo 1 || echo 0)"

hits=$(code $ALL | grep -nE 'lv_color_hex|lv_color_make|lv_palette_|lv_color_white|lv_color_black|0x[0-9a-fA-F]{6}\b|&pos_font_|lv_font_montserrat_|lv_obj_set_style_(bg|text|border|outline)_(color|font)|\.(red|green|blue)[[:space:]]*=')
check "no colour or font is written in the game's C sources" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
for name in paper edge black red back back_line slot felt; do
    check "the palette defines $name as RRGGBB" \
        "$(grep -qE "^$name[[:space:]]+[0-9A-Fa-f]{6}([[:space:]]|$)" $APP/art/cards_palette.txt && echo 1 || echo 0)"
done
check "CMake generates the palette into the build tree" \
    "$(grep -q 'apps/solitaire/art/cards_palette.txt' ui/shell/CMakeLists.txt &&
       grep -q 'sol_palette.c' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
check "and no generated palette is committed" "$([ ! -e $APP/ui/sol_palette.c ] && echo 1 || echo 0)"
check "interface marks use tokens" \
    "$(code $APP/ui/sol_table_widget.c | grep -q 'pos_theme_color(POS_COLOR_FOCUS)' && echo 1 || echo 0)"

hits=$(grep -lE 'timber' $ALL | grep -v 'sol_felt\.[ch]$')
check "only sol_felt.c reaches into PocketTimber" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits"
check "and it reuses Timber's felt rather than drawing one" \
    "$(code $APP/ui/sol_felt.c | grep -q 'timber_art_felt()' && echo 1 || echo 0)"

hits=$(grep -nE 'pos_keyboard\.h|lv_keyboard|pocketos_shell_keyboard|pocketui_text_field|lv_textarea' $ALL)
check "no keyboard is created or asked for" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
check "exactly one object joins the focus group" \
    "$([ "$(code $APP/sol_app.c | grep -c 'pos_input_add_obj')" = "1" ] && echo 1 || echo 0)"
check "keys go through the view model" "$(code $APP/sol_app.c | grep -q 'sol_view_key' && echo 1 || echo 0)"
check "taps go through the view model" "$(code $APP/sol_app.c | grep -q 'sol_view_tap' && echo 1 || echo 0)"
hits=$(code $APP/sol_app.c $APP/ui/sol_table_widget.c | grep -nE "pos_input_push_key|LV_KEY_(UP|DOWN|LEFT|RIGHT|ENTER|ESC)[^)]*[:]")
check "the LVGL side maps no key itself" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
check "the table and the buttons do not take focus when tapped" \
    "$([ "$(code $APP/sol_app.c $APP/ui/sol_table_widget.c | grep -c 'LV_OBJ_FLAG_CLICK_FOCUSABLE')" -ge 2 ] && echo 1 || echo 0)"
hits=$(code $ALL | grep -nE 'LV_EVENT_PRESSING|lv_indev_get_vect|DRAG')
check "there is no drag" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
check "the view's key numbers are asserted against LVGL's" \
    "$([ "$(code $APP/sol_app.c | grep -c '_Static_assert(SOL_KEY_')" = "7" ] && echo 1 || echo 0)"
check "blocks are placed from the view's layout, again on every resize" \
    "$(code $APP/sol_app.c | grep -q 'sol_view_screen' && code $APP/sol_app.c | grep -q 'LV_EVENT_SIZE_CHANGED' &&
       echo 1 || echo 0)"

check "the app is declared in the shell" \
    "$(grep -q 'extern const struct pocketos_app app_solitaire;' ui/shell/shell.c && echo 1 || echo 0)"
check "registered once, after Wave" \
    "$([ "$(grep -c '&app_solitaire' ui/shell/shell.c)" = "1" ] && grep -q '&app_wave, &app_solitaire' ui/shell/shell.c &&
       echo 1 || echo 0)"
for src in sol_app.c engine/sol_rng.c engine/sol_cards.c engine/sol_rules.c ui/sol_view.c ui/sol_card_draw.c \
           ui/sol_table_widget.c ui/sol_felt.c; do
    check "the shell builds $src" "$(grep -q "apps/solitaire/$src" ui/shell/CMakeLists.txt && echo 1 || echo 0)"
done
check "the app test is a host-only target" \
    "$(awk '/if\(POCKETOS_DISPLAY STREQUAL "sdl"\)/,/^endif\(\)/' ui/shell/CMakeLists.txt |
       grep -q 'add_executable(sol_app_test' && echo 1 || echo 0)"
for t in sol_rules_test sol_view_test sol_store_test; do
    check "make test runs $t" "$(grep -q "./tests/$t" Makefile && grep -q "/tests/$t\$" .gitignore && echo 1 || echo 0)"
done

echo "sol_lint: $failed failure(s)"
exit $((failed > 0))
