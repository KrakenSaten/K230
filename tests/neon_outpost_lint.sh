#!/bin/bash
# Relay Nine layering, hardware ownership and opt-in source adapter boundaries.
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
none() { local rc=0; grep "$@" || rc=$?; [ "$rc" = 1 ]; }
none -nE '#include.*(lvgl|app.h|pocketui)|\b(malloc|calloc|free|fopen|socket|time|rand)\(' \
    apps/neon-outpost/relay.c apps/neon-outpost/relay_render.c apps/neon-outpost/relay_art.c
none -nE 'pocketaudio|alsa|/dev/(snd|gpio)' apps/neon-outpost/neon_app.c apps/neon-outpost/relay_audio.c
none -nE 'fork\(|pthread_create|exec' apps/neon-outpost/neon_app.c
grep -q 'lv_timer_delete(a->timer)' apps/neon-outpost/neon_app.c
grep -q 'pos_input_game_unsubscribe' apps/neon-outpost/neon_app.c
grep -q 'SDL_DelEventWatch' ui/shell/platform_sdl.c
grep -q '&app_neon_outpost' ui/shell/shell.c
grep -qE '"neon-outpost".*HOME_FOLDER_GAMES' ui/shell/home_layout.c
grep -q 'name="Relay Nine"' apps/neon-outpost/neon_app.c
grep -q 'pos-audio-effects' Makefile
echo 'relay lint: pure sector/renderer, audio ownership, opt-in lifecycle and registration passed'
