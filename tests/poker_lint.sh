#!/bin/bash
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
if rg -n 'lvgl|#include.*(app\.h|pocketui)|\b(malloc|calloc|free|rand|srand|time|fopen|socket)\(' apps/poker/engine --glob '*.c'; then
    echo 'FAIL poker engine must remain graphical/platform/heap independent'; exit 1
fi
test "$(rg -c 'pos_input_add_obj' apps/poker/poker_app.c)" = 1
! rg -n 'lv_timer_create|pthread_create|fork\(' apps/poker --glob '*.c'
rg -q 'bj_draw_face' apps/poker/ui/poker_table.c
rg -q 'bj_felt_image' apps/poker/ui/poker_table.c
rg -q '&app_poker' ui/shell/shell.c
rg -q '"poker".*HOME_FOLDER_GAMES' ui/shell/home_layout.c
echo 'poker_lint: layering, reuse, lifecycle and launcher checks passed'
