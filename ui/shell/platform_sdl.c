/*
 * SDL backend: a 568x1232 window scaled down by POCKETOS_SDL_ZOOM (default
 * 0.5) so it fits a laptop screen. Mouse acts as touch.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "platform.h"

#include <stdlib.h>
#include <unistd.h>

lv_display_t *pocketos_platform_init(void)
{
    const char *zoom_env = getenv("POCKETOS_SDL_ZOOM");
    float zoom = zoom_env ? (float)atof(zoom_env) : 0.5f;
    lv_display_t *disp = lv_sdl_window_create(POCKETOS_PANEL_W, POCKETOS_PANEL_H);

    if (!disp) {
        return NULL;
    }
    if (zoom > 0.1f && zoom <= 2.0f) {
        lv_sdl_window_set_zoom(disp, zoom);
    }
    lv_sdl_window_set_title(disp, "PocketOS shell (simulator)");
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();
    return disp;
}

void pocketos_platform_sleep_ms(unsigned ms)
{
    usleep(ms * 1000u);
}
