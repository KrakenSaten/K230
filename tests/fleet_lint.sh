#!/bin/bash
# PocketFleet structural rules (see apps/fleet/engine/fleet_ai.h):
#   1. the game engine stays free of LVGL, so it is unit-tested natively;
#   2. the AI cannot see the hidden state: fleet_ai.[ch] must not include
#      fleet_rules.h nor name struct fleet_board or struct fleet_game;
#   3. fleet_store.c is the only part of the game that touches the filesystem;
#   4. the layout is chosen from the body, never from an orientation, and the
#      corner clearance is taken from the one platform rule rather than worked
#      out here (DS 21.3, 22.2, 28.1);
#   5. the boards' geometry has one source, so what is drawn and what is hit
#      can never disagree;
#   6. a normal turn of Battle is on the display, whole, and nothing it needs
#      is reached by scrolling (DS 28.6).
# Fails the build when any of them is broken.
set -u
cd "$(dirname "$0")/.." || exit 1
ENGINE=apps/fleet/engine
failed=0
check() { # <label> <files> <regex>
    hits=$(grep -nE "$3" $2 2>/dev/null)
    if [ -n "$hits" ]; then
        echo "FAIL $1:"; echo "$hits" | head -10; failed=$((failed + 1))
    else
        echo "ok   $1"
    fi
}
check "engine is free of LVGL" "$ENGINE/*.c $ENGINE/*.h" 'include[[:space:]]*[<"]lvgl|lv_obj_|lv_style_'
check "the view model is free of LVGL" "apps/fleet/ui/fleet_view.c apps/fleet/ui/fleet_view.h" \
      'include[[:space:]]*[<"]lvgl|lv_obj_|lv_style_'
check "AI does not include the rules" "$ENGINE/fleet_ai.c $ENGINE/fleet_ai.h" 'include[[:space:]]*"fleet_rules\.h"'
check "AI does not name the hidden state" "$ENGINE/fleet_ai.c" 'fleet_board|fleet_game'
check "only fleet_store.c touches the filesystem" \
      "$(ls $ENGINE/*.c | grep -v fleet_store.c)" \
      '\b(fopen|open|creat|unlink|rename|mkdir|remove)[[:space:]]*\('

want() { # <label> <files> <regex>
    hits=$(grep -nE "$3" $2 2>/dev/null)
    if [ -z "$hits" ]; then
        echo "FAIL $1: not found"; failed=$((failed + 1))
    else
        echo "ok   $1"
    fi
}

UI="apps/fleet/fleet_app.c apps/fleet/ui/fleet_screen_battle.c     apps/fleet/ui/fleet_screen_command.c apps/fleet/ui/fleet_screen_deploy.c     apps/fleet/ui/fleet_screen_result.c"

# 4. The shape comes from the body's size. Nothing in the app may ask which way
#    up the display is, or reach for the rotation the shell owns.
check "the app never asks for the orientation" "$UI apps/fleet/ui/fleet_grid.c"       'pocketos_shell_orientation|pocketos_rotation_mode|POCKETOS_ROTATION|lv_display_get_rotation'
check "nor works the corner clearance out for itself" "$UI"       'corners\.|top_left|bottom_left|bottom_right|top_right'
want "the corner clearance comes from the platform rule" "apps/fleet/fleet_app.c"      'pos_display_rect_insets'
want "and the shape from the body's own box" "apps/fleet/fleet_app.c"      'LV_EVENT_SIZE_CHANGED'
want "a repeated layout pass is refused" "apps/fleet/fleet_app.c"      'memcmp\(&box, &app->laid_out'

# 5. One stored cell size per board. A screen may ask the grid to change size;
#    it may not draw or hit a cell itself.
check "only the grid works in cells" "$UI"       'FLEET_GRID_GAP|FLEET_GRID_GUTTER'
want "the span has one definition" "apps/fleet/ui/fleet_grid.c"      'int fleet_grid_span_for'
want "and the hit test reads the size the drawing uses" "apps/fleet/ui/fleet_grid.c"      'g->cell \+ FLEET_GRID_GAP'

# 6. A normal turn of Battle is on the display, whole, and no part of playing
#    it is reached by scrolling (DS 28.6). The screen may not make itself a
#    scroller and may not scroll anything into view: something that does not
#    fit has to be laid out, not scrolled. The test that holds the screen to
#    this has to still be there, and still cover both type sizes and both
#    corner shapes.
BATTLE=apps/fleet/ui/fleet_screen_battle.c
check "Battle never turns scrolling on" "$BATTLE" 'lv_obj_add_flag\([^;]*LV_OBJ_FLAG_SCROLLABLE'
check "nor scrolls anything into view" "$BATTLE" 'lv_obj_scroll_to|lv_obj_set_scroll_dir|lv_obj_scroll_by'
want "the frame scrolls only in the tall shape" "apps/fleet/fleet_app.c" \
     'lv_obj_remove_flag\(app->frame, LV_OBJ_FLAG_SCROLLABLE\)'
want "a turn is held to fitting on the display" tests/fleet_app_test.c \
     'static void check_battle_never_scrolls'
want "through a whole match played across the page" tests/fleet_app_test.c \
     'test_no_scroll_through_a_match\("normal", PANEL_CORNER\)'
want "in Outdoor type as well" tests/fleet_app_test.c \
     'test_no_scroll_through_a_match\("outdoor", PANEL_CORNER\)'
want "and on a panel with square corners" tests/fleet_app_test.c \
     'test_no_scroll_through_a_match\("(normal|outdoor)", 0\)'

echo "fleet_lint: $failed failure(s)"
exit $((failed > 0))
