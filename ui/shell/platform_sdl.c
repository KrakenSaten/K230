/*
 * SDL backend: a window of the logical display size - 568x1232, or
 * 1232x568 when the geometry is landscape - scaled down by POCKETOS_SDL_ZOOM
 * (default 0.5) so it fits a laptop screen. The mouse points in logical
 * coordinates already, so there is no touch transform to derive here; the
 * DRM backend's is tested on its own (tests/display_touch_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "platform.h"
#include "pos_input.h"
#include <SDL.h>


#include <stdlib.h>
#include <unistd.h>

static lv_indev_t *keyboard;
/* SDL may invoke watches from another producer thread. Copy fixed records
 * under SDL's spinlock; only the shell thread calls PocketUI. */
static SDL_SpinLock event_lock;
static struct { unsigned id,key; bool down,lost; } action_queue[64];
static unsigned action_head,action_tail;
static bool action_overflow;
static int game_watch(void *user,SDL_Event *event)
{
    (void)user;
    unsigned key=0,id=0;bool down=false,lost=false;
    if(event->type==SDL_KEYDOWN||event->type==SDL_KEYUP){
        id=(unsigned)event->key.keysym.scancode;down=event->type==SDL_KEYDOWN;
        if(event->key.repeat)return 1;
        SDL_Keycode k=event->key.keysym.sym;
        if(k==SDLK_LEFT)key=LV_KEY_LEFT;else if(k==SDLK_RIGHT)key=LV_KEY_RIGHT;
        else if(k==SDLK_UP)key=LV_KEY_UP;else if(k==SDLK_DOWN)key=LV_KEY_DOWN;
        else if(k==SDLK_ESCAPE)key=LV_KEY_ESC;else if(k>=32&&k<127)key=(unsigned)k;
    }else if(event->type==SDL_WINDOWEVENT && event->window.event==SDL_WINDOWEVENT_FOCUS_LOST)lost=true;
    else return 1;
    SDL_AtomicLock(&event_lock);
    if(action_head-action_tail>=64)action_overflow=true;
    else {unsigned at=action_head++%64;action_queue[at].id=id;action_queue[at].key=key;action_queue[at].down=down;action_queue[at].lost=lost;}
    SDL_AtomicUnlock(&event_lock);return 1;
}
static void game_events(lv_timer_t *timer)
{
    (void)timer;
    SDL_AtomicLock(&event_lock);
    if(action_overflow){action_tail=action_head;action_overflow=false;pos_input_game_reset(1);}
    while(action_tail<action_head){unsigned at=action_tail++%64;if(action_queue[at].lost)pos_input_game_reset(1);else pos_input_game_event(1,action_queue[at].id,action_queue[at].key,action_queue[at].down);}
    SDL_AtomicUnlock(&event_lock);
}
static void game_display_deleted(lv_event_t *e)
{
    lv_timer_t *timer=lv_event_get_user_data(e);lv_timer_delete(timer);SDL_DelEventWatch(game_watch,NULL);pos_input_game_reset(1);
}

int pocketos_platform_external_display(void)
{
    return 0;
}

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
    SDL_AddEventWatch(game_watch,NULL);
    lv_timer_t *actions=lv_timer_create(game_events,5,NULL);
    lv_display_add_event_cb(disp,game_display_deleted,LV_EVENT_DELETE,actions);
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
