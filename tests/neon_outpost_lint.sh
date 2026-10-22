#!/bin/bash
# Architectural boundaries and native registration; no platform or engine copies.
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
none() { local rc=0; grep "$@" || rc=$?; [ "$rc" = 1 ]; }
none -nE '#include.*(lvgl|app.h|pocketui)|\b(malloc|calloc|free|fopen|socket|time|rand)\(' \
    apps/neon-outpost/neon.c apps/neon-outpost/neon_render.c
none -rnE '\b(pthread_create|fork|execv|system|popen)\(' apps/neon-outpost
grep -q 'lv_timer_delete(a->timer)' apps/neon-outpost/neon_app.c
grep -q '&app_neon_outpost' ui/shell/shell.c
grep -qE '"neon-outpost".*HOME_FOLDER_GAMES' ui/shell/home_layout.c
grep -q 'apps/neon-outpost/neon_app.c' ui/shell/CMakeLists.txt
echo 'neon_outpost_lint: pure core, lifecycle and registration passed'
