#!/bin/bash
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# GNU grep only: the supported build host (docs/BUILD_ENVIRONMENT.md) has no
# ripgrep.
set -euo pipefail
cd "$(dirname "$0")/.."
# none <grep args>: pass only when grep ran and matched nothing. A match
# (status 0) fails, and so does a grep error (status 2), so a missing file or
# a bad pattern can never pass as "nothing found".
none() {
    local rc=0
    grep "$@" || rc=$?
    [ "${rc}" = 1 ]
}
none -rnE --include='*.c' 'lvgl|#include.*(app\.h|pocketui)|\b(malloc|calloc|free|rand|srand|time|fopen|socket)\(' apps/poker/engine ||
    { echo 'FAIL poker engine must remain graphical/platform/heap independent'; exit 1; }
test "$(grep -c 'pos_input_add_obj' apps/poker/poker_app.c)" = 1 ||
    { echo 'FAIL poker app must register exactly one key sink'; exit 1; }
none -rnE --include='*.c' 'lv_timer_create|pthread_create|fork\(' apps/poker ||
    { echo 'FAIL poker must start no timers, threads or processes'; exit 1; }
grep -q 'bj_draw_face' apps/poker/ui/poker_table.c
grep -q 'bj_felt_image' apps/poker/ui/poker_table.c
grep -q '&app_poker' ui/shell/shell.c
grep -qE '"poker".*HOME_FOLDER_GAMES' ui/shell/home_layout.c
# Its own launcher icon: the mask on the tile and the portal art the shell loads.
grep -q 'icon_mask = &pos_app_icon_poker,' apps/poker/poker_app.c
grep -q 'pos_app_icon_poker = {' ui/pocketui/pos_app_icons.c
grep -q '  ui/assets/doors/icon-poker.bin  ' ui/assets/doors/MANIFEST.txt
test -s ui/assets/doors/icon-poker.bin
echo 'poker_lint: layering, reuse, lifecycle, launcher and icon checks passed'
