/*
 * SDL backend: a 568x1232 window scaled down by POCKETOS_SDL_ZOOM (default
 * 0.5) so it fits a laptop screen. Mouse acts as touch.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "platform.h"

#include "pocketlog/pocketlog.h"
#include "pos_input.h"

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
    /* The host keyboard is a source of the one logical key stream, not a
     * second input path (DS §17.4). It stands in for the physical keyboard:
     * if a field answers a host key exactly as it answers a pushed key, the
     * driver that arrives later has nothing new to design.
     *
     * Logged either way, because a silently unadopted keyboard would look
     * exactly like a dead one: keys would reach LVGL and go nowhere. */
    if (pos_input_add_source(lv_sdl_keyboard_create())) {
        LOG_INFO("host keyboard adopted as an input source");
    } else {
        LOG_WARN("host keyboard not adopted; typing will not reach fields");
    }
    return disp;
}

void pocketos_platform_sleep_ms(unsigned ms)
{
    usleep(ms * 1000u);
}
