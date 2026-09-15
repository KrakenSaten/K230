/*
 * SDL backend: a window of the logical display size - 568x1232, or
 * 1232x568 when the geometry is landscape - scaled down by POCKETOS_SDL_ZOOM
 * (default 0.5) so it fits a laptop screen. The mouse points in logical
 * coordinates already, so there is no touch transform to derive here; the
 * DRM backend's is tested on its own (tests/display_touch_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "platform.h"


#include <stdlib.h>
#include <unistd.h>

static lv_indev_t *keyboard;

lv_display_t *pocketos_platform_init(const struct pos_panel *panel, struct pos_display_geometry *geometry)
{
    const char *zoom_env = getenv("POCKETOS_SDL_ZOOM");
    float zoom = zoom_env ? (float)atof(zoom_env) : 0.5f;
    lv_display_t *disp;

    (void)panel;
    disp = lv_sdl_window_create(geometry->width, geometry->height);
    if (!disp) {
        return NULL;
    }
    if (zoom > 0.1f && zoom <= 2.0f) {
        lv_sdl_window_set_zoom(disp, zoom);
    }
    lv_sdl_window_set_title(disp, "Doors shell (simulator)");
    lv_sdl_mouse_create();
    /* Created here, adopted by the shell: it is a source of the one logical
     * key stream, not a second input path (DS §17.4), and it stands in for
     * the physical keyboard that comes later. */
    keyboard = lv_sdl_keyboard_create();
    return disp;
}

lv_indev_t *pocketos_platform_keyboard(void)
{
    return keyboard;
}

void pocketos_platform_sleep_ms(unsigned ms)
{
    usleep(ms * 1000u);
}
