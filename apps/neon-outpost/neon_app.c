/* DOORS integration: single pointer controls and discrete keyboard pulses.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "neon_app.h"
#include "pocketui.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "shell_power.h"
#include "shell_lock.h"
#include <stdlib.h>
#include <string.h>

struct neon_app {
    struct neon_game game;
    lv_obj_t *root, *image, *hud, *message, *message_box, *button[10];
    lv_timer_t *timer;
    lv_image_dsc_t frame;
    uint16_t pixels[NEON_VIEW_W*NEON_VIEW_H];
    uint32_t last, pulse_end;
    struct neon_input pulse;
    int held; /* one pointer: one action, no multitouch required */
    bool help;
};
static void advance_clock(struct neon_app *a, uint32_t now);
static const char *const labels[] = {"LEFT", "FWD", "RIGHT", "BACK", "FIRE", "USE", "PAUSE", "HELP", "RETRY", "PLAY"};
static void refresh(struct neon_app *a)
{
    char text[90];
    lv_snprintf(text,sizeof(text),"HP %d   AMMO %d   %d/%d",a->game.health,a->game.ammo,a->game.kills,NEON_ENEMIES);
    if (strcmp(lv_label_get_text(a->hud),text)) lv_label_set_text(a->hud,text);
    const char *message;
    if (a->help) message = "NEON OUTPOST\nFind the green exit frame. Stand inside and USE.\nW/S or Up/Down: step. A/D or Left/Right: turn.\nQ/E: sidestep. Space: fire. F: use. P: pause.\nTouch: hold one action at a time; release to stop.\nDiamond: crawler. Triangle: ranged sentry.\nBars: ammo. Cross: health. Pink: door; USE nearby.\nLeaving or rotating abandons this run. No save.\nVisual effects only. Scroll help; PLAY to enter.";
    else if (a->game.state == NEON_DEAD) message = "SIGNAL LOST\nYou fell at the outpost. RETRY for a new run.";
    else if (a->game.state == NEON_WON) message = "OUTPOST CLEARED\nExit reached. You made it home. RETRY to play again.";
    else if (a->game.paused) message = "PAUSED\nPLAY to resume. Leaving abandons this run.";
    else message = "Find the exit frame. USE pink doors. P pauses.";
    if (strcmp(lv_label_get_text(a->message),message)) lv_label_set_text(a->message,message);
    bool panel = a->help || a->game.paused || a->game.state != NEON_PLAY;
    if (panel) lv_obj_add_flag(a->image,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(a->image,LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 10; ++i) {
        bool show = panel ? (i == 7 || i == 8 || (i == 9 && a->game.state == NEON_PLAY)) : i < 8;
        if (show) lv_obj_remove_flag(a->button[i],LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(a->button[i],LV_OBJ_FLAG_HIDDEN);
    }
    if (!panel) {
        neon_render(&a->game,a->pixels,NEON_VIEW_W*NEON_VIEW_H);
        lv_image_cache_drop(&a->frame); lv_obj_invalidate(a->image);
    }
}
static void release(struct neon_app *a)
{
    a->held = -1; memset(&a->pulse,0,sizeof(a->pulse)); a->pulse_end = 0;
}
static void pause_game(struct neon_app *a)
{
    neon_pause(&a->game,true); release(a); a->last = lv_tick_get();
}
static void layout(struct neon_app *a)
{
    int w = lv_obj_get_content_width(a->root), h = lv_obj_get_content_height(a->root);
    if (w < 1 || h < 1) return;
    int hudh = pocketui_role_line_height(POS_STYLE_TEXT_PRIMARY)+12;
    int rowh = 68, gap = 8;
    bool wide = w > h*3/2;
    bool panel = a->help || a->game.paused || a->game.state != NEON_PLAY;
    int controls = wide ? w/3 : w;
    int left = wide ? w-controls-16 : w;
    int area = wide ? h-hudh-60 : h-hudh-3*(rowh+gap)-60;
    int vw = left-16, vh = vw*NEON_VIEW_H/NEON_VIEW_W;
    if (vh > area) { vh = area; vw = vh*NEON_VIEW_W/NEON_VIEW_H; }
    if (vw < 1) vw = 1;
    if (vh < 1) vh = 1;
    lv_obj_set_pos(a->hud,8,4); lv_obj_set_width(a->hud,left-16);
    lv_obj_set_pos(a->image,(left-NEON_VIEW_W)/2,hudh+(vh-NEON_VIEW_H)/2);
    uint32_t scale = (uint32_t)vw*256/NEON_VIEW_W;
    lv_image_set_scale(a->image,scale ? scale : 1);
    lv_obj_set_pos(a->message_box,8,panel ? hudh+8 : hudh+vh+8);
    lv_obj_set_size(a->message_box,left-16,panel ? (wide ? h-hudh-16 : h-hudh-2*(rowh+gap)-16) : 52);
    lv_obj_set_width(a->message,LV_PCT(100)); lv_obj_set_height(a->message,LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(a->message,LV_TEXT_ALIGN_CENTER,0);
    int cols = wide ? 2 : 3;
    int bw = (controls-8*(cols+1))/cols;
    for (int i = 0; i < 10; ++i) {
        int col = i%cols, row = i/cols;
        if (panel) { col = wide ? 0 : i == 7 ? 0 : i == 8 ? 1 : 2; row = wide ? i-7 : 2; }
        lv_obj_set_size(a->button[i],panel && wide ? controls-16 : bw,rowh);
        lv_obj_set_pos(a->button[i],(wide ? left+16 : 0)+8+col*(bw+gap),
                      wide ? hudh+row*(rowh+gap) : h-3*(rowh+gap)+row*(rowh+gap));
    }
}
static void action(struct neon_app *a, int id)
{
    if (id == 6) { if (!a->help && !a->game.paused) pause_game(a); else { a->help = false; neon_pause(&a->game,false); } }
    if (id == 7) { pause_game(a); a->help = !a->help; }
    if (id == 8) { neon_restart(&a->game); a->help = false; release(a); a->last = lv_tick_get(); }
    if (id == 9) { a->help = false; neon_pause(&a->game,false); release(a); a->last = lv_tick_get(); }
    refresh(a); layout(a);
}
static void touch(lv_event_t *e)
{
    struct neon_app *a = lv_event_get_user_data(e);
    int id = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
        advance_clock(a,lv_tick_get());
    if (code == LV_EVENT_PRESSED && id < 6 && !a->help && !a->game.paused && a->game.state == NEON_PLAY) {
        release(a); a->held = id;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) release(a);
    if (code == LV_EVENT_CLICKED && id >= 6) action(a,id);
}
static struct neon_input input_for(int id)
{
    struct neon_input in = {0};
    if (id == 0) in.turn = -1;
    if (id == 1) in.forward = 1;
    if (id == 2) in.turn = 1;
    if (id == 3) in.forward = -1;
    if (id == 4) in.fire = true;
    if (id == 5) in.use = true;
    return in;
}
static void key(lv_event_t *e)
{
    struct neon_app *a = lv_event_get_user_data(e);
    advance_clock(a,lv_tick_get());
    uint32_t k = lv_event_get_key(e); unsigned mods;
    pos_key_t raw;
    if (pos_input_raw_decode(k,&raw,&mods)) k = raw;
    if (k >= 'A' && k <= 'Z') k += 'a'-'A';
    if (k == 'p' || k == LV_KEY_ESC) { action(a,6); return; }
    if (k == 'h') { action(a,7); return; }
    if (k == 'r') { action(a,8); return; }
    if (k == LV_KEY_ENTER && (a->help || a->game.paused)) { action(a,9); return; }
    if (a->help || a->game.paused || a->game.state != NEON_PLAY) return;
    int id = -1;
    if (k == 'a' || k == LV_KEY_LEFT) id = 0;
    if (k == 'w' || k == LV_KEY_UP) id = 1;
    if (k == 'd' || k == LV_KEY_RIGHT) id = 2;
    if (k == 's' || k == LV_KEY_DOWN) id = 3;
    if (k == ' ') id = 4;
    if (k == 'f') id = 5;
    release(a); a->pulse = input_for(id);
    if (k == 'q') a->pulse.strafe = -1;
    if (k == 'e') a->pulse.strafe = 1;
    a->pulse_end = lv_tick_get()+120;
}
static void resized(lv_event_t *e) { layout(lv_event_get_user_data(e)); }
static void lost(lv_event_t *e)
{
    struct neon_app *a = lv_event_get_user_data(e);
    pause_game(a); refresh(a); layout(a);
}
static bool available(struct neon_app *a)
{
    return pos_input_focused() == a->root && !pos_input_group_redirected() &&
        !shell_power_screen_off() && !shell_lock_is_locked();
}
static void advance_clock(struct neon_app *a, uint32_t now)
{
    uint32_t from = a->last, dt = now-from;
    a->last = now;
    if (!available(a) || dt > 250) { pause_game(a); return; }
    if (a->help || a->game.paused || a->game.state != NEON_PLAY) return;
    struct neon_input in = input_for(a->held);
    if (a->pulse_end && (int32_t)(a->pulse_end-from) > 0) {
        uint32_t part = a->pulse_end-from;
        if (part > dt) part = dt;
        neon_advance(&a->game,a->pulse,part);
        neon_advance(&a->game,in,dt-part);
    } else neon_advance(&a->game,in,dt);
    if (a->pulse_end && (int32_t)(a->pulse_end-now) <= 0) release(a);
}
static void frame(lv_timer_t *timer)
{
    struct neon_app *a = lv_timer_get_user_data(timer);
    advance_clock(a,lv_tick_get());
    refresh(a); layout(a);
}
static void *create(lv_obj_t *body)
{
    struct neon_app *a = lv_malloc_zeroed(sizeof(*a));
    if (!a) return NULL;
    neon_restart(&a->game); neon_pause(&a->game,true); a->help = true; a->held = -1;
    a->root = lv_obj_create(body); lv_obj_remove_style_all(a->root);
    lv_obj_set_width(a->root,LV_PCT(100)); lv_obj_set_flex_grow(a->root,1);
    lv_obj_remove_flag(a->root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(a->root,lv_color_hex(0x0b1020),0);
    lv_obj_set_style_bg_opa(a->root,LV_OPA_COVER,0);
    a->frame.header.magic = LV_IMAGE_HEADER_MAGIC;
    a->frame.header.cf = LV_COLOR_FORMAT_RGB565; a->frame.header.w = NEON_VIEW_W; a->frame.header.h = NEON_VIEW_H;
    a->frame.header.stride = NEON_VIEW_W*2; a->frame.data_size = sizeof(a->pixels); a->frame.data = (const uint8_t *)a->pixels;
    a->image = lv_image_create(a->root); lv_image_set_src(a->image,&a->frame);
    lv_image_set_antialias(a->image,false);
    a->hud = pocketui_label(a->root,"",POS_STYLE_TEXT_PRIMARY);
    lv_obj_set_style_text_color(a->hud,lv_color_hex(0xd4f9ef),0);
    a->message_box = lv_obj_create(a->root); lv_obj_remove_style_all(a->message_box);
    lv_obj_set_scroll_dir(a->message_box,LV_DIR_VER);
    a->message = pocketui_label(a->message_box,"",POS_STYLE_TEXT_PRIMARY);
    lv_obj_set_style_text_color(a->message,lv_color_hex(0xd4f9ef),0);
    lv_label_set_long_mode(a->message,LV_LABEL_LONG_WRAP);
    for (int i = 0; i < 10; ++i) {
        a->button[i] = lv_button_create(a->root); lv_obj_remove_style_all(a->button[i]);
        lv_obj_remove_flag(a->button[i],LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE);
        pos_style_add(a->button[i],POS_STYLE_BUTTON_SECONDARY,0);
        pos_style_add(a->button[i],POS_STYLE_BUTTON_PRIMARY_PRESSED,LV_STATE_PRESSED);
        lv_obj_t *label = pocketui_label(a->button[i],labels[i],POS_STYLE_BUTTON_LABEL); lv_obj_center(label);
        lv_obj_set_user_data(a->button[i],(void *)(intptr_t)i);
        lv_obj_add_event_cb(a->button[i],touch,LV_EVENT_ALL,a);
    }
    lv_obj_add_event_cb(a->root,resized,LV_EVENT_SIZE_CHANGED,a);
    lv_obj_add_event_cb(a->root,key,LV_EVENT_KEY,a);
    lv_obj_add_event_cb(a->root,lost,LV_EVENT_DEFOCUSED,a);
    pos_input_add_obj(a->root); pos_input_focus(a->root); pos_input_set_raw_target(a->root);
    a->last = lv_tick_get(); a->timer = lv_timer_create(frame,30,a);
    lv_obj_update_layout(a->root); refresh(a); layout(a);
    return a;
}
static void destroy(void *priv)
{
    struct neon_app *a = priv;
    if (!a) return;
    release(a); lv_timer_delete(a->timer); lv_image_cache_drop(&a->frame);
    /* Detach the descriptor before its backing memory goes away. */
    lv_image_set_src(a->image,NULL); pos_input_set_raw_target(NULL);
    lv_obj_remove_event_cb(a->root,lost); lv_obj_remove_event_cb(a->root,key); lv_obj_remove_event_cb(a->root,resized);
    lv_free(a);
}
const struct pocketos_app app_neon_outpost = {
    .id = "neon-outpost", .name = "Neon Outpost", .icon = LV_SYMBOL_PLAY,
    .create = create, .destroy = destroy
};
struct neon_game *neon_app_game(void *priv) { return priv ? &((struct neon_app *)priv)->game : NULL; }
lv_obj_t *neon_app_button(void *priv, unsigned i) { return priv && i < 10 ? ((struct neon_app *)priv)->button[i] : NULL; }

void neon_app_refresh(void *priv) { if (priv) { refresh(priv); layout(priv); } }
