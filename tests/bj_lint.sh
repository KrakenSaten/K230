#!/bin/bash
# PG Blackjack layering, and the rules the game is held to:
#   1. cards, shoe, rules and chips are pure: no LVGL, no I/O, no floating
#      point, no platform entropy;
#   2. the view model is LVGL-free and does no I/O;
#   3. bj_store.c is the only file that touches the filesystem, writes
#      atomically and restores only what the rules call valid;
#   4. no colour is written in the game's C sources: interface colours are
#      tokens, card colours are art in apps/blackjack/art/cards_palette.txt;
#   5. the felt is PocketTimber's, reached through bj_felt.c alone;
#   6. the Pocket Cards copies match their PG Solitaire origin in shape
#      (the prefix is the only intended difference);
#   7. input is one key sink and one map; nothing tapped takes the focus;
#   8. the scope stays small: no split, insurance, surrender or side bets;
#   9. the app is registered, built and tested.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

APP=apps/blackjack
ENGINE="$APP/engine/bj_rng.c $APP/engine/bj_rng.h $APP/engine/bj_cards.c $APP/engine/bj_cards.h \
        $APP/engine/bj_rules.c $APP/engine/bj_rules.h"
VIEW="$APP/ui/bj_view.c $APP/ui/bj_view.h"
LVGL_SIDE="$APP/bj_app.c $APP/bj_app.h $APP/ui/bj_card_draw.c $APP/ui/bj_card_draw.h \
           $APP/ui/bj_table_widget.c $APP/ui/bj_table_widget.h $APP/ui/bj_felt.c $APP/ui/bj_felt.h \
           $APP/ui/bj_palette.h"
ALL="$ENGINE $VIEW $LVGL_SIDE"

# Prose is not code: whole comment lines go, and so do comments at the end of
# a line - "double" is a word this game uses in both.
strip_prose() { grep -vE '^[[:space:]]*(/\*|\*|//)' | sed -e 's#/\*.*\*/##g' -e 's#//.*$##'; }
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
check "the rules use no floating point, so 3:2 is integer arithmetic" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $ENGINE | grep -nE '\b(rand|srand|random|srandom|time|clock|gettimeofday|clock_gettime|getpid|getenv)[[:space:]]*\(')
check "the rules take no entropy from the platform" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3

hits=$(code $VIEW | grep -nE '#include[[:space:]]*[<"](lvgl|lv_|pocketui|pos_|app\.h)|lv_[a-z]+_')
check "the view model names no LVGL and no shell" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $VIEW | grep -nE "$IO|\bgetenv\(|\btime\(")
check "the view model does no I/O and reads no clock" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $VIEW $ENGINE | grep -nE '\b(568|1232|1176|1060)\b')
check "no panel dimension appears in the rules or the view" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3

STORE="$APP/bj_store.c $APP/bj_store.h"
for f in $STORE; do
    check "$f exists" "$([ -f "$f" ] && echo 1 || echo 0)"
done
hits=$(code $ALL | grep -nE "$IO|POCKETOS_STATE_DIR|pocketpaths|settings_(get|set)")
check "only bj_store.c touches the filesystem" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
hits=$(code $STORE | grep -nE '#include[[:space:]]*[<"](lvgl|lv_|pocketui|pos_)|lv_[a-z]+_')
check "the store names no LVGL" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
check "the store writes atomically: temp file, fsync, rename" \
    "$(code $APP/bj_store.c | grep -q 'fsync(' && code $APP/bj_store.c | grep -q 'rename(tmp, path)' && echo 1 || echo 0)"
check "and restores only what the rules call valid" "$(code $APP/bj_store.c | grep -q 'bj_game_valid(&tmp)' && echo 1 || echo 0)"
check "the app saves from the shell's tick and on close" \
    "$(code $APP/bj_app.c | grep -q '\.tick = blackjack_tick' && code $APP/bj_app.c | grep -q 'bj_store_save' && echo 1 || echo 0)"
check "the shell builds the store" "$(grep -q 'apps/blackjack/bj_store.c' ui/shell/CMakeLists.txt && echo 1 || echo 0)"

hits=$(code $ALL | grep -nE 'lv_color_hex|lv_color_make|lv_palette_|lv_color_white|lv_color_black|0x[0-9a-fA-F]{6}\b|&pos_font_|lv_font_montserrat_|lv_obj_set_style_(bg|text|border|outline)_(color|font)|\.(red|green|blue)[[:space:]]*=')
check "no colour or font is written in the game's C sources" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -3
for name in paper edge black red back back_line slot felt; do
    check "the palette defines $name as RRGGBB" \
        "$(grep -qE "^$name[[:space:]]+[0-9A-Fa-f]{6}([[:space:]]|$)" $APP/art/cards_palette.txt && echo 1 || echo 0)"
done
# One CMake step makes both card games' palettes (games_card_palette).
check "CMake generates the palette into the build tree" \
    "$(grep -q 'games_card_palette("PG Blackjack" bj blackjack BJ_PALETTE_C)' ui/shell/CMakeLists.txt &&
       grep -q 'set(src "${REPO_DIR}/apps/${game}/art/cards_palette.txt")' ui/shell/CMakeLists.txt &&
       grep -q 'set(out "${CMAKE_BINARY_DIR}/${prefix}_palette.c")' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
check "and no generated palette is committed" "$([ ! -e $APP/ui/bj_palette.c ] && echo 1 || echo 0)"

hits=$(grep -lE 'timber' $ALL | grep -v 'bj_felt\.[ch]$')
check "only bj_felt.c reaches into PocketTimber" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits"
check "and it reuses Timber's felt" "$(code $APP/ui/bj_felt.c | grep -q 'timber_art_felt()' && echo 1 || echo 0)"
check "the Pocket Cards copies say where they came from" \
    "$(grep -q "PG Solitaire's sol_cards.h" $APP/engine/bj_cards.h &&
       grep -q "PG Solitaire's sol_card_draw" $APP/ui/bj_card_draw.h && echo 1 || echo 0)"
check "and the card API has the shared shape" \
    "$([ "$(grep -cE '^(bj_card_t |int |enum bj_suit |const char \*|void )bj_(card|card_rank|card_suit|card_is_red|card_valid|rank_text|suit_name|deck_fill|deck_shuffle)\(' $APP/engine/bj_cards.h)" = "9" ] &&
       echo 1 || echo 0)"

hits=$(grep -nE 'pos_keyboard\.h|lv_keyboard|pocketos_shell_keyboard|pocketui_text_field|lv_textarea' $ALL)
check "no keyboard is created or asked for" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
check "exactly one object joins the focus group" \
    "$([ "$(code $APP/bj_app.c | grep -c 'pos_input_add_obj')" = "1" ] && echo 1 || echo 0)"
check "keys go through the view model" "$(code $APP/bj_app.c | grep -q 'bj_view_command_for_key' && echo 1 || echo 0)"
check "buttons come from the view model" "$(code $APP/bj_app.c | grep -q 'bj_view_buttons' && echo 1 || echo 0)"
hits=$(code $APP/bj_app.c | grep -nE "pos_input_push_key|'[hsdnHSDN+=-]'")
check "the app maps no key itself" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3
check "buttons do not take focus when tapped" \
    "$(code $APP/bj_app.c | grep -q 'LV_OBJ_FLAG_CLICK_FOCUSABLE' && echo 1 || echo 0)"
check "the view's key numbers are asserted against LVGL's" \
    "$([ "$(code $APP/bj_app.c | grep -c '_Static_assert(BJ_KEY_')" = "7" ] && echo 1 || echo 0)"
check "blocks are placed from the view's layout, again on every resize" \
    "$(code $APP/bj_app.c | grep -q 'bj_view_screen' && code $APP/bj_app.c | grep -q 'LV_EVENT_SIZE_CHANGED' &&
       echo 1 || echo 0)"

hits=$(code $ENGINE $VIEW $APP/bj_app.c | grep -niE '\b(split|insurance|surrender|side_?bet)')
check "no split, insurance, surrender or side bets" "$([ -z "$hits" ] && echo 1 || echo 0)"; [ -n "$hits" ] && echo "$hits" | head -3

check "the app is declared in the shell" \
    "$(grep -q 'extern const struct pocketos_app app_blackjack;' ui/shell/shell.c && echo 1 || echo 0)"
check "registered once" "$([ "$(grep -c '&app_blackjack\b' ui/shell/shell.c)" = "1" ] && echo 1 || echo 0)"
# Where it is shown is the launcher's table, not the registry order
# (ui/shell/home_layout.c): a game, in PLAY, in the games colour.
check "the launcher shows it in PLAY" \
    "$(grep -q '{ "blackjack", HOME_GROUP_PLAY, HOME_HUE_GAMES }' ui/shell/home_layout.c && echo 1 || echo 0)"
for src in bj_app.c engine/bj_rng.c engine/bj_cards.c engine/bj_rules.c ui/bj_view.c ui/bj_card_draw.c \
           ui/bj_table_widget.c ui/bj_felt.c; do
    check "the shell builds $src" "$(grep -q "apps/blackjack/$src" ui/shell/CMakeLists.txt && echo 1 || echo 0)"
done
check "the app test is a host-only target" \
    "$(awk '/if\(POCKETOS_DISPLAY STREQUAL "sdl"\)/,/^endif\(\)/' ui/shell/CMakeLists.txt |
       grep -q 'add_executable(bj_app_test' && echo 1 || echo 0)"
for t in bj_rules_test bj_view_test bj_store_test; do
    check "make test runs $t" "$(grep -q "./tests/$t" Makefile && grep -q "/tests/$t\$" .gitignore && echo 1 || echo 0)"
done

echo "bj_lint: $failed failure(s)"
exit $((failed > 0))
