#!/bin/bash
# PocketFleet structural rules (see apps/fleet/engine/fleet_ai.h):
#   1. the game engine stays free of LVGL, so it is unit-tested natively;
#   2. the AI cannot see the hidden state: fleet_ai.[ch] must not include
#      fleet_rules.h nor name struct fleet_board or struct fleet_game;
#   3. fleet_store.c is the only part of the game that touches the filesystem.
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
echo "fleet_lint: $failed failure(s)"
exit $((failed > 0))
