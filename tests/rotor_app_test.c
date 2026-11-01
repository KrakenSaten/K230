/* Real LVGL pointer/keyboard input, safe layouts, lifecycle and engine captures.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "rotor_app.h"
#include "pocketui.h"
#include "src/libs/lodepng/lodepng.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks;
static bool off,locked;
static int motion,volume=40,muted;
static uint8_t display_buffer[1920*24*4];
static lv_point_t point;
static lv_indev_state_t finger;
#define CHECK(x) do { checks++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);abort();} } while(0)
bool shell_power_screen_off(void) { return off; }
bool shell_lock_is_locked(void) { return locked; }
int pocketos_shell_reduced_motion(void) { return motion; }
int pocketos_shell_volume_effective(void) { return muted?0:volume; }
void pocketos_shell_set_fullscreen(bool value) { CHECK(value); }
void pocketos_shell_go_home(void) {}
static void flush(lv_display_t *d,const lv_area_t *r,uint8_t *p)
{ (void)r;(void)p;lv_display_flush_ready(d); }
static void pointer(lv_indev_t *i,lv_indev_data_t *d)
{ (void)i;d->point=point;d->state=finger; }
static void pump(unsigned ms)
{ for(unsigned i=0;i<ms;i+=8) { lv_tick_inc(8);lv_timer_handler(); } }
static int timers(void)
{ int n=0;lv_timer_t *t=NULL;while((t=lv_timer_get_next(t)))n++;return n; }

static void tap(void *a,unsigned id,unsigned ms,float x,float y)
{
    lv_obj_update_layout(lv_screen_active());
    lv_area_t r;lv_obj_get_coords(rotor_app_button(a,id),&r);
    point=(lv_point_t){r.x1+(int)((r.x2-r.x1)*x),r.y1+(int)((r.y2-r.y1)*y)};
    finger=LV_INDEV_STATE_PRESSED;pump(ms);
    finger=LV_INDEV_STATE_RELEASED;pump(64);
}

static void bounds(void *a,lv_obj_t *body)
{
    rotor_app_refresh(a);lv_obj_update_layout(body);
    lv_area_t b;lv_obj_get_coords(body,&b);
    for(unsigned i=0;i<RTB_COUNT;i++) {
        lv_obj_t *button=rotor_app_button(a,i);
        if(lv_obj_has_flag(button,LV_OBJ_FLAG_HIDDEN))continue;
        lv_area_t r;lv_obj_get_coords(button,&r);
        if(r.x1<b.x1 || r.x2>b.x2 || r.y1<b.y1 || r.y2>b.y2)fprintf(stderr,"button %u: %d,%d %d,%d body %d,%d\n",i,r.x1,r.y1,r.x2,r.y2,b.x2,b.y2);
        CHECK(r.x1>=b.x1 && r.x2<=b.x2 && r.y1>=b.y1 && r.y2<=b.y2);
        CHECK(lv_obj_get_width(button)>=64 && lv_obj_get_height(button)>=64);
        bool safe=pos_display_rect_is_safe(pocketui_display_geometry(),r.x1,r.y1,r.x2,r.y2);
        if(!safe)fprintf(stderr,"unsafe button %u at %d,%d %d,%d; geometry %dx%d\n",i,r.x1,r.y1,r.x2,r.y2,pocketui_display_geometry()->width,pocketui_display_geometry()->height);
        CHECK(safe);
        lv_obj_t *t=lv_obj_get_child(button,0);
        CHECK(lv_obj_get_height(t)<=lv_obj_get_height(button));
        for(unsigned j=i+1;j<RTB_COUNT;j++) {
            lv_obj_t *other=rotor_app_button(a,j);
            if(lv_obj_has_flag(other,LV_OBJ_FLAG_HIDDEN))continue;
            lv_area_t s;lv_obj_get_coords(other,&s);
            bool separate=r.x2<s.x1 || s.x2<r.x1 || r.y2<s.y1 || s.y2<r.y1;
            if(!separate)fprintf(stderr,"buttons overlap: %u at %d,%d %d,%d and %u at %d,%d %d,%d\n",i,r.x1,r.y1,r.x2,r.y2,j,s.x1,s.y1,s.x2,s.y2);
            CHECK(separate);
        }
    }
}

static void shot(void *a,const char *mode,const char *state,int size)
{
    const char *dir=getenv("ROTOR_SHOTS");if(!dir)return;
    rotor_app_refresh(a);lv_obj_update_layout(lv_screen_active());
    lv_draw_buf_t *snap=lv_snapshot_take(lv_screen_active(),LV_COLOR_FORMAT_RGB888);CHECK(snap);
    unsigned char *rgb=malloc((size_t)snap->header.w*snap->header.h*3);CHECK(rgb);
    for(unsigned y=0;y<snap->header.h;y++)for(unsigned x=0;x<snap->header.w;x++) {
        const unsigned char *s=snap->data+y*snap->header.stride+x*3;
        unsigned char *p=rgb+((size_t)y*snap->header.w+x)*3;
        p[0]=s[2];p[1]=s[1];p[2]=s[0];
    }
    char path[512];snprintf(path,sizeof(path),"%s/%s-%d-%s.png",dir,mode,size,state);
    CHECK(!lodepng_encode24_file(path,rgb,snap->header.w,snap->header.h));
    free(rgb);lv_draw_buf_destroy(snap);
}

static void exercise(lv_display_t *d,int mode,int text_size)
{
    static const int sizes[][2]={{568,1232},{1232,568},{1280,720}};
    static const char *const names[]={"portrait","landscape","hdmi720"};
    int w=sizes[mode][0],h=sizes[mode][1];
    lv_display_set_resolution(d,w,h);
    struct pos_panel panel={.width=mode==2?w:568,.height=mode==2?h:1232,
        .corners=mode==2?(struct pos_corners){0}:(struct pos_corners){50,50,50,50}};
    struct pos_display_geometry geometry;
    pos_display_geometry_init(&geometry,&panel,mode==1?POS_ROTATION_90:POS_ROTATION_0);
    pocketui_set_display_geometry(&geometry);
    point=(lv_point_t){0,0};finger=LV_INDEV_STATE_RELEASED;
    lv_obj_t *body=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(body);
    lv_obj_set_size(body,w,h);lv_obj_set_flex_flow(body,LV_FLEX_FLOW_COLUMN);
    pump(40);int before=timers();
    void *a=app_rotorterror.create(body);CHECK(a);pump(64);
    CHECK(timers()==before+1);struct rt_game *g=rotor_app_game(a);
    CHECK(g->phase==RT_TITLE);bounds(a,body);shot(a,names[mode],"title",text_size);
    tap(a,RTB_HELP,64,.5f,.5f);bounds(a,body);shot(a,names[mode],"controls",text_size);
    CHECK(app_rotorterror.back(a));tap(a,RTB_TERROR,64,.5f,.5f);
    tap(a,RTB_START,64,.5f,.5f);CHECK(g->phase==RT_FLIGHT && !g->paused && g->difficulty==2);
    bounds(a,body);
    CHECK(pos_input_game_event(0,39,'w',true));CHECK(pos_input_game_event(0,1,LV_KEY_RIGHT,true));
    CHECK(pos_input_game_event(0,5,' ',true));float y=g->y;
    pump(320);CHECK(g->y<y && g->angle>0 && g->rockets<g->rocket_max);
    pos_input_game_event(0,39,0,false);pos_input_game_event(0,1,0,false);pos_input_game_event(0,5,0,false);
    pump(128);unsigned launches=g->sounds[RT_LAUNCH];
    g->rockets=g->rocket_max;g->rocket_ms=0;y=g->y;
    tap(a,RTB_PAD,400,.8f,.1f);CHECK(g->y<y && g->angle>0 && g->sounds[RT_LAUNCH]>launches);
    pump(400);CHECK(fabsf(g->angle)<.08f);
    launches=g->sounds[RT_LAUNCH];pump(600);CHECK(g->sounds[RT_LAUNCH]==launches);
    /* A press-lost must clear the compound actions, including held rockets. */
    lv_obj_send_event(rotor_app_button(a,RTB_PAD),LV_EVENT_PRESS_LOST,NULL);
    tap(a,RTB_PAUSE,64,.5f,.5f);CHECK(g->paused);bounds(a,body);shot(a,names[mode],"pause",text_size);
    unsigned steps=g->steps;pump(128);CHECK(g->steps==steps);
    tap(a,RTB_RESUME,64,.5f,.5f);CHECK(!g->paused);
    off=true;pump(64);CHECK(g->paused);off=false;tap(a,RTB_RESUME,64,.5f,.5f);
    locked=true;pump(64);CHECK(g->paused);locked=false;tap(a,RTB_RESUME,64,.5f,.5f);
    pos_input_game_reset(0);pump(64);CHECK(g->paused);tap(a,RTB_RESUME,64,.5f,.5f);
    lv_group_t *other=lv_group_create();CHECK(pos_input_push_group(other));pump(64);CHECK(g->paused);
    pos_input_pop_group();lv_group_delete(other);tap(a,RTB_RESUME,64,.5f,.5f);
    lv_tick_inc(1000);lv_timer_handler();CHECK(g->paused);tap(a,RTB_RESUME,64,.5f,.5f);

    /* Genuine simulation, sampled after an automated flight for screenshots. */
    for(int i=0;i<520;i++) {
        g->safe_ms=1000;
        struct rt_input in={.lift=sinf(i*.03f)*.6f,.aim=sinf(i*.02f)*.4f,.rocket=i%100>60};
        rt_advance(g,in,16);
    }
    rotor_app_refresh(a);shot(a,names[mode],"combat",text_size);
    g->wave=3;g->wave_ms=12496;rt_advance(g,(struct rt_input){0},16);
    for(int i=0;i<100;i++) { g->safe_ms=1000;rt_advance(g,(struct rt_input){.rocket=true},16); }
    rotor_app_refresh(a);shot(a,names[mode],"boss",text_size);

    g->phase=RT_HANGAR;g->credits=650;rotor_app_refresh(a);bounds(a,body);
    tap(a,RTB_SHIELD,64,.5f,.5f);CHECK(g->level[0]==1 && g->shield_max==130 && g->credits==570);
    tap(a,RTB_CANNON,64,.5f,.5f);CHECK(g->level[1]==1);
    tap(a,RTB_MAGAZINE,64,.5f,.5f);CHECK(g->level[2]==1 && g->rocket_max==4);
    shot(a,names[mode],"workshop",text_size);
    tap(a,RTB_NEXT,64,.5f,.5f);CHECK(g->wave==4 && g->phase==RT_FLIGHT && !g->paused);
    CHECK(pos_input_push_key_mods('h',0));pump(64);CHECK(g->paused);
    CHECK(app_rotorterror.back(a));CHECK(g->paused);tap(a,RTB_RESUME,64,.5f,.5f);
    g->safe_ms=0;g->shield=0;rt_damage(g,1000);pump(64);
    if(g->phase!=RT_OVER)fprintf(stderr,"death state mode %d text %d: phase %d hull %d safe %d paused %d\n",mode,text_size,g->phase,g->hull,g->safe_ms,g->paused);
    CHECK(g->phase==RT_OVER);bounds(a,body);shot(a,names[mode],"over",text_size);
    tap(a,RTB_RETRY,64,.5f,.5f);CHECK(g->phase==RT_FLIGHT && g->hull==100 && g->wave==1 && !g->level[0]);
    CHECK(app_rotorterror.back(a));CHECK(g->paused);
    tap(a,RTB_MENU,64,.5f,.5f);CHECK(g->phase==RT_TITLE);
    CHECK(pos_input_push_key_mods('1',0));CHECK(pos_input_push_key_mods(LV_KEY_ENTER,0));pump(80);
    CHECK(g->phase==RT_FLIGHT && g->difficulty==0);
    app_rotorterror.destroy(a);lv_obj_delete(body);pump(64);
    CHECK(timers()==before && !pos_input_game_active() && !pos_input_raw_target());
    errno=0;CHECK(waitpid(-1,NULL,WNOHANG)==-1 && errno==ECHILD);
}

int main(void)
{
    char dir[]="/tmp/rotor-app-XXXXXX";CHECK(mkdtemp(dir));setenv("POCKETOS_STATE_DIR",dir,1);
    setenv("DOORS_AUDIO_EFFECTS_HELPER","/bin/false",1);
    lv_init();lv_display_t *d=lv_display_create(568,1232);
    lv_display_set_buffers(d,display_buffer,NULL,sizeof(display_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    lv_indev_t *p=lv_indev_create();lv_indev_set_type(p,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(p,pointer);
    pos_input_init();pocketui_init();pocketui_style_screen(lv_screen_active());
    for(int size=0;size<3;size++) {
        CHECK(!pos_theme_select_text_size(size));motion=size==2;
        for(int mode=0;mode<3;mode++)exercise(d,mode,size);
    }
    pos_input_deinit();lv_indev_delete(p);lv_display_delete(d);lv_deinit();
    char path[256];snprintf(path,sizeof(path),"%s/rotorterror/scores.bin",dir);unlink(path);
    snprintf(path,sizeof(path),"%s/rotorterror",dir);rmdir(path);rmdir(dir);
    printf("rotor_app_test: %u checks passed; portrait, landscape, 720p; all text sizes\n",checks);
    return 0;
}
