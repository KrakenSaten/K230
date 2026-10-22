/* Real LVGL app: pointer, key stream, lifecycle, layout and production-run captures.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "neon_app.h"
#include "pocketui.h"
#include "src/libs/lodepng/lodepng.h"
#include "shell_power.h"
#include "shell_lock.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "neon_pilot.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); assert(x); } } while (0)
static bool off, locked;
bool shell_power_screen_off(void) { return off; }
bool shell_lock_is_locked(void) { return locked; }
void pocketos_shell_set_status_hint(const char *s) { (void)s; }
void pocketos_shell_go_home(void) { }
int pocketos_shell_reduced_motion(void) { return 1; }
static uint8_t buffer[1920*20*4];
static lv_point_t point;
static lv_indev_state_t finger;
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{ (void)a; (void)p; lv_display_flush_ready(d); }
static void pointer(lv_indev_t *d, lv_indev_data_t *data)
{ (void)d; data->point = point; data->state = finger; }
static void pump(unsigned ms)
{ for (unsigned i = 0; i < ms; i += 5) { lv_tick_inc(5); lv_timer_handler(); } }
static int timers(void)
{ int n = 0; lv_timer_t *t = NULL; while ((t = lv_timer_get_next(t))) ++n; return n; }
static void touch(void *a, unsigned id, unsigned ms)
{
    lv_area_t r; lv_obj_get_coords(neon_app_button(a,id),&r);
    point = (lv_point_t){(r.x1+r.x2)/2,(r.y1+r.y2)/2};
    finger = LV_INDEV_STATE_PRESSED; pump(ms);
    finger = LV_INDEV_STATE_RELEASED; pump(60);
}
static void key(uint32_t k) { CHECK(pos_input_push_key(k)); pump(80); }
static void shot(void *a, const char *mode, const char *state)
{
    const char *dir = getenv("NEON_SHOTS");
    if (!dir) return;
    neon_app_refresh(a); lv_obj_update_layout(lv_screen_active());
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(),LV_COLOR_FORMAT_RGB888);
    CHECK(snap != NULL);
    size_t bytes = (size_t)snap->header.w*snap->header.h*3;
    unsigned char *rgb = malloc(bytes); CHECK(rgb != NULL);
    for (unsigned y = 0; y < snap->header.h; ++y) for (unsigned x = 0; x < snap->header.w; ++x) {
        const unsigned char *src = snap->data+(size_t)y*snap->header.stride+x*3;
        unsigned char *dst = rgb+((size_t)y*snap->header.w+x)*3;
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];
    }
    char path[512]; snprintf(path,sizeof(path),"%s/%s-%s.png",dir,mode,state);
    CHECK(lodepng_encode24_file(path,rgb,snap->header.w,snap->header.h) == 0);
    free(rgb); lv_draw_buf_destroy(snap);
}
static void bounds(void *a, lv_obj_t *body)
{
    lv_obj_update_layout(body); lv_area_t b; lv_obj_get_coords(body,&b);
    for (unsigned i = 0; i < 10; ++i) {
        lv_obj_t *button = neon_app_button(a,i);
        if (lv_obj_has_flag(button,LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t r; lv_obj_get_coords(button,&r);
        CHECK(r.x1 >= b.x1 && r.y1 >= b.y1 && r.x2 <= b.x2 && r.y2 <= b.y2);
        CHECK(lv_obj_get_width(button) >= 64 && lv_obj_get_height(button) >= 64);
        lv_obj_t *label = lv_obj_get_child(button,0);
        CHECK(lv_obj_get_width(label) <= lv_obj_get_width(button));
    }
}
int main(void)
{
    lv_init(); lv_display_t *d = lv_display_create(568,1232);
    lv_display_set_buffers(d,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    lv_indev_t *p = lv_indev_create(); lv_indev_set_type(p,LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(p,pointer);
    pos_input_init(); pocketui_init(); pocketui_style_screen(lv_screen_active());
    const int sizes[][2] = {{568,1232},{1232,568},{1280,720},{1920,1080}};
    const char *names[] = {"portrait","landscape","hdmi720","hdmi1080"};
    for (int text = POS_TEXT_SIZE_SMALL; text <= POS_TEXT_SIZE_LARGE; ++text) {
      CHECK(pos_theme_select_text_size(text) == 0);
      for (unsigned mode = 0; mode < 4; ++mode) {
        int w = sizes[mode][0], h = sizes[mode][1];
        point = (lv_point_t){0,0};
        lv_display_set_resolution(d,w,h);
        /* Same content geometry as shell DEFAULT chrome/header, including panel insets. */
        struct pos_panel panel = {.width=w,.height=h,.corners={30,30,30,30}};
        if (mode >= 2) memset(&panel.corners,0,sizeof(panel.corners));
        struct pos_display_geometry geometry; pos_display_geometry_init(&geometry,&panel,POS_ROTATION_0);
        pocketui_set_display_geometry(&geometry);
        lv_obj_t *title = pocketui_label(lv_screen_active(),"NEON OUTPOST  /  DOORS",POS_STYLE_TITLE);
        lv_obj_set_pos(title,30,8);
        lv_obj_t *body = lv_obj_create(lv_screen_active()); lv_obj_remove_style_all(body);
        lv_obj_set_pos(body,mode < 2 ? 28 : 16,80);
        lv_obj_set_size(body,w-(mode < 2 ? 56 : 32),h-108);
        lv_obj_set_flex_flow(body,LV_FLEX_FLOW_COLUMN); pump(40);
        int baseline = timers();
        void *a = app_neon_outpost.create(body); CHECK(a != NULL); pump(60);
        struct neon_game *g = neon_app_game(a); CHECK(g->paused); CHECK(timers() == baseline+1);
        bounds(a,body); if (text == POS_TEXT_SIZE_LARGE) shot(a,names[mode],"help");
        touch(a,9,60); CHECK(!g->paused); bounds(a,body);
        double x = g->x; touch(a,1,180); CHECK(g->x > x); x = g->x; pump(180); CHECK(g->x == x);
        key('w'); CHECK(g->x > x); pump(200); x = g->x; pump(180); CHECK(g->x == x);
        key('p'); CHECK(g->paused); unsigned steps = g->steps; pump(400); CHECK(g->steps == steps);
        if (text == POS_TEXT_SIZE_LARGE) shot(a,names[mode],"pause");
        touch(a,9,60); CHECK(!g->paused);
        off = true; pump(60); CHECK(g->paused); off = false; pump(100); CHECK(g->paused); touch(a,9,60);
        locked = true; pump(60); CHECK(g->paused); locked = false; touch(a,9,60);
        lv_group_t *other = lv_group_create(); CHECK(pos_input_push_group(other)); pump(60); CHECK(g->paused);
        pos_input_pop_group(); lv_group_delete(other); touch(a,9,60);
        lv_obj_t *other_obj = lv_obj_create(body); pos_input_add_obj(other_obj); pos_input_focus(other_obj);
        pump(50); CHECK(g->paused);
        pos_input_focus(lv_obj_get_parent(neon_app_button(a,1))); lv_obj_delete(other_obj); touch(a,9,60);
        /* Delayed timer callback must pause instead of catching up after resume. */
        lv_tick_inc(1000); lv_timer_handler(); CHECK(g->paused); touch(a,9,60);
        if (text == POS_TEXT_SIZE_LARGE) {
            touch(a,8,60); CHECK(g->state == NEON_PLAY && g->health == 100);
            /* Production simulation driven only by inputs; render the same live game instance. */
            for (int i = 0; i < 35; ++i) neon_advance(g,pilot(g,5.5,2.5),10);
            shot(a,names[mode],"gameplay");
            for (int t = 0; t < 1000 && !g->flash; ++t) neon_advance(g,pilot(g,5.5,2.5),10);
            CHECK(g->flash > 0); shot(a,names[mode],"firing");
            const double tour[][2] = {{5.5,2.5},{3.5,8.5},{5.5,9.5},{10.5,7.5},{13.5,6.5},{10.5,2.5},{9.5,3.5},{14.5,9.5}};
            unsigned goal = 0; bool sentry = false;
            for (int t = 0; t < 40000 && g->state == NEON_PLAY; ++t) {
                if (goal < 7 && hypot(g->x-tour[goal][0],g->y-tour[goal][1]) < .35) ++goal;
                struct neon_input in = pilot(g,tour[goal][0],tour[goal][1]);
                if (goal == 7 && hypot(g->x-14.5,g->y-9.5) < .35) in.use = true;
                if (!sentry && g->enemy[1].hp > 0 && neon_visible(g,g->x,g->y,g->enemy[1].x,g->enemy[1].y)) {
                    double diff = remainder(atan2(g->enemy[1].y-g->y,g->enemy[1].x-g->x)-g->angle,2*NEON_PI);
                    if (fabs(diff) < .2) { shot(a,names[mode],"sentry"); sentry = true; }
                }
                neon_advance(g,in,10);
            }
            CHECK(g->state == NEON_WON && g->kills == NEON_ENEMIES);
            shot(a,names[mode],"victory"); bounds(a,body);
            touch(a,8,60);
            for (int i = 0; i < 10000 && g->state == NEON_PLAY; ++i) neon_advance(g,(struct neon_input){0},10);
            CHECK(g->state == NEON_DEAD); shot(a,names[mode],"death"); bounds(a,body);
        }
        app_neon_outpost.destroy(a); lv_obj_delete(body); lv_obj_delete(title); pump(60);
        CHECK(timers() == baseline); CHECK(pos_input_raw_target() == NULL); CHECK(pos_input_focused() == NULL);
      }
    }
    /* Repeated close while a held action is live cannot keep timer/input references. */
    for (int i = 0; i < 20; ++i) {
        lv_obj_t *body = lv_obj_create(lv_screen_active()); lv_obj_set_size(body,512,1000);
        lv_obj_set_flex_flow(body,LV_FLEX_FLOW_COLUMN);
        int base = timers(); void *a = app_neon_outpost.create(body); pump(50);
        touch(a,9,60); lv_obj_send_event(neon_app_button(a,1),LV_EVENT_PRESSED,NULL);
        app_neon_outpost.destroy(a); lv_obj_delete(body); pump(60); CHECK(timers() == base);
    }
    pos_input_deinit(); lv_indev_delete(p); lv_display_delete(d); lv_deinit();
    printf("neon_app_test: %u checks passed\n",checks);
}
