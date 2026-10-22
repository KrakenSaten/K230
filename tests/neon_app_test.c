/* Relay Nine real LVGL controls, held input, lifecycle and production captures.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "neon_app.h"
#include "pocketui.h"
#include "src/libs/lodepng/lodepng.h"
#include "relay_pilot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;static bool off,locked;static int motion;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);abort();}}while(0)
bool shell_power_screen_off(void){return off;}bool shell_lock_is_locked(void){return locked;}
int pocketos_shell_reduced_motion(void){return motion;}static int volume=40,muted;
int pocketos_shell_volume_effective(void){return muted?0:volume;}
int pocketos_shell_volume_get(void){return volume;}int pocketos_shell_volume_muted(void){return muted;}
int pocketos_shell_volume_available(void){return 1;}
int pocketos_shell_volume_set(int v){volume=v;return 0;}int pocketos_shell_volume_set_muted(int m){muted=m;return 0;}
void pocketos_shell_set_status_hint(const char *s){(void)s;}void pocketos_shell_go_home(void){}
static uint8_t buffer[1920*20*4];static lv_point_t point;static lv_indev_state_t finger;
static void flush(lv_display_t *d,const lv_area_t *r,uint8_t *p){(void)r;(void)p;lv_display_flush_ready(d);}
static void pointer(lv_indev_t *i,lv_indev_data_t *d){(void)i;d->point=point;d->state=finger;}
static void pump(unsigned ms){for(unsigned i=0;i<ms;i+=5){lv_tick_inc(5);lv_timer_handler();}}
static int timers(void){int n=0;lv_timer_t *t=NULL;while((t=lv_timer_get_next(t)))n++;return n;}
static void touch(void *a,unsigned id,unsigned ms,double x,double y)
{
    lv_area_t r;lv_obj_get_coords(neon_app_button(a,id),&r);point=(lv_point_t){r.x1+(int)((r.x2-r.x1)*x),r.y1+(int)((r.y2-r.y1)*y)};finger=LV_INDEV_STATE_PRESSED;pump(ms);finger=LV_INDEV_STATE_RELEASED;pump(60);
}
static void shot(void *a,const char *mode,const char *state)
{
    const char *dir=getenv("RELAY_SHOTS");if(!dir)return;
    neon_app_refresh(a);lv_obj_update_layout(lv_screen_active());lv_draw_buf_t *snap=lv_snapshot_take(lv_screen_active(),LV_COLOR_FORMAT_RGB888);CHECK(snap);
    unsigned char *rgb=malloc((size_t)snap->header.w*snap->header.h*3);CHECK(rgb);
    for(unsigned y=0;y<snap->header.h;y++)for(unsigned x=0;x<snap->header.w;x++){const unsigned char *s=snap->data+y*snap->header.stride+x*3;unsigned char *p=rgb+((size_t)y*snap->header.w+x)*3;p[0]=s[2];p[1]=s[1];p[2]=s[0];}
    char path[512];snprintf(path,sizeof(path),"%s/%s-%s.png",dir,mode,state);CHECK(!lodepng_encode24_file(path,rgb,snap->header.w,snap->header.h));free(rgb);lv_draw_buf_destroy(snap);
    if(!strcmp(mode,"landscape")&&strcmp(state,"controls")&&strcmp(state,"sound")&&strcmp(state,"pause")&&strcmp(state,"death")&&strcmp(state,"victory")){
        static struct rn_frame native;
        if(!strcmp(state,"title"))rn_title(&native);else CHECK(rn_render(neon_app_game(a),&native));
        unsigned char raw[RN_W*RN_H*3];for(int i=0;i<RN_W*RN_H;i++){uint16_t c=native.pixels[i];raw[i*3]=(c>>11)*255/31;raw[i*3+1]=((c>>5)&63)*255/63;raw[i*3+2]=(c&31)*255/31;}
        snprintf(path,sizeof(path),"%s/native-%s.png",dir,state);CHECK(!lodepng_encode24_file(path,raw,RN_W,RN_H));
    }
}
static void bounds(void *a,lv_obj_t *body)
{
    lv_obj_update_layout(body);lv_area_t b;lv_obj_get_coords(body,&b);
    for(unsigned i=0;i<14;i++){lv_obj_t *v=neon_app_button(a,i);if(lv_obj_has_flag(v,LV_OBJ_FLAG_HIDDEN))continue;lv_area_t r;lv_obj_get_coords(v,&r);CHECK(r.x1>=b.x1&&r.x2<=b.x2&&r.y1>=b.y1&&r.y2<=b.y2);CHECK(lv_obj_get_width(v)>=64&&lv_obj_get_height(v)>=64);}
}
int main(void)
{
    lv_init();lv_display_t *d=lv_display_create(568,1232);lv_display_set_buffers(d,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    lv_indev_t *p=lv_indev_create();lv_indev_set_type(p,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(p,pointer);pos_input_init();pocketui_init();pocketui_style_screen(lv_screen_active());
    const int sizes[][2]={{568,1232},{1232,568},{1280,720}};const char *names[]={"portrait","landscape","hdmi720"};
    for(int text=0;text<3;text++){CHECK(!pos_theme_select_text_size(text));motion=text==2;
      for(int mode=0;mode<3;mode++){
        point=(lv_point_t){0,0};lv_display_set_resolution(d,sizes[mode][0],sizes[mode][1]);
        lv_obj_t *title=pocketui_label(lv_screen_active(),"RELAY NINE / ENGINE CAPTURE",POS_STYLE_TITLE);lv_obj_set_pos(title,28,8);
        lv_obj_t *body=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(body);lv_obj_set_pos(body,24,80);lv_obj_set_size(body,sizes[mode][0]-48,sizes[mode][1]-104);lv_obj_set_flex_flow(body,LV_FLEX_FLOW_COLUMN);pump(40);
        int base=timers();void *a=app_neon_outpost.create(body);CHECK(a);pump(60);CHECK(timers()==base+1);struct rn_game *g=neon_app_game(a);CHECK(g->paused);bounds(a,body);
        if(text==2)shot(a,names[mode],"title");
        touch(a,9,60,.5,.5);bounds(a,body);int v=volume;touch(a,11,60,.5,.5);CHECK(volume==v+5);touch(a,10,60,.5,.5);CHECK(volume==v);touch(a,12,60,.5,.5);CHECK(muted);touch(a,12,60,.5,.5);CHECK(!muted);if(text==2)shot(a,names[mode],"sound");touch(a,13,60,.5,.5);
        touch(a,5,60,.5,.5);bounds(a,body);if(text==2)shot(a,names[mode],"controls");touch(a,13,60,.5,.5);
        touch(a,7,60,.5,.5);CHECK(!g->paused);bounds(a,body);
        /* W + right turn + fire held at once, without reliance on repeat. */
        CHECK(pos_input_game_event(0,39,'w',true));CHECK(pos_input_game_event(0,1,LV_KEY_RIGHT,true));CHECK(pos_input_game_event(0,5,' ',true));
        double x=g->x,y=g->y,angle=g->angle;int ammo=g->mag;pump(180);CHECK(hypot(g->x-x,g->y-y)>.2);CHECK(g->angle>angle+.2&&g->mag<ammo);
        pos_input_game_event(0,39,0,false);pos_input_game_event(0,1,0,false);pos_input_game_event(0,5,0,false);pump(60);x=g->x;y=g->y;pump(120);CHECK(g->x==x&&g->y==y);
        /* Both Space contacts are one action; first release must not stop it. */
        pos_input_game_event(0,5,' ',true);pos_input_game_event(0,14,' ',true);pos_input_game_event(0,5,0,false);unsigned held;pos_input_game_take(&held,NULL,NULL);CHECK(held&POS_GAME_FIRE);pos_input_game_event(0,14,0,false);
        /* Compound touch applies movement, turn and fire from ONE pointer. */
        ammo=g->mag;angle=g->angle;x=g->x;y=g->y;touch(a,1,320,.75,.15);CHECK(g->mag<ammo&&fabs(g->angle-angle)>.2&&hypot(g->x-x,g->y-y)>.2);
        x=g->x;y=g->y;pump(120);CHECK(g->x==x&&g->y==y);
        lv_area_t pad;lv_obj_get_coords(neon_app_button(a,0),&pad);
        point=(lv_point_t){(pad.x1+pad.x2)/2,pad.y1+15};finger=LV_INDEV_STATE_PRESSED;pump(80);
        lv_obj_send_event(neon_app_button(a,0),LV_EVENT_PRESS_LOST,NULL);x=g->x;y=g->y;pump(80);CHECK(g->x==x&&g->y==y);
        finger=LV_INDEV_STATE_RELEASED;pump(60);
        pos_input_game_event(0,39,'w',true);pos_input_game_reset(0);pump(60);CHECK(g->paused);
        pos_input_game_event(0,39,0,false);touch(a,7,60,.5,.5);
        off=true;pump(60);CHECK(g->paused);off=false;touch(a,7,60,.5,.5);
        locked=true;pump(60);CHECK(g->paused);locked=false;touch(a,7,60,.5,.5);
        lv_group_t *other=lv_group_create();CHECK(pos_input_push_group(other));pump(60);CHECK(g->paused);pos_input_pop_group();lv_group_delete(other);touch(a,7,60,.5,.5);
        lv_tick_inc(1000);lv_timer_handler();CHECK(g->paused);if(text==2)shot(a,names[mode],"pause");
        touch(a,6,60,.5,.5);CHECK(!g->paused&&g->health==100);
        if(text==2){unsigned goal=0;bool combat=false,stairs=false,upper=false,shotgun=false,optional=false;
            for(int t=0;t<60000&&g->state==RN_PLAY;t++){
                if(relay_goal_done(g,goal)&&goal+1<RELAY_GOALS)goal++;
                rn_advance(g,relay_pilot(g,relay_tour[goal][0],relay_tour[goal][1]),10);
                if(!combat&&g->flash&&g->kills>=1){shot(a,names[mode],"combat");combat=true;}
                if(!stairs&&g->x< -4.1&&g->x> -5.2&&g->y>11.4&&g->y<11.8){shot(a,names[mode],"stairs");stairs=true;}
                if(!shotgun&&g->flash&&g->weapon==RN_SHOTGUN){shot(a,names[mode],"shotgun");shotgun=true;}
                if(!optional&&g->secret&&g->sectors[g->sector].room==8){shot(a,names[mode],"secret");optional=true;}
                if(!upper&&g->sectors[g->sector].room==4){shot(a,names[mode],"upper-control");upper=true;}
            }
            CHECK(g->state==RN_WON&&g->kills==9&&g->relay&&g->shotgun&&g->secret&&shotgun&&optional);shot(a,names[mode],"victory");bounds(a,body);
            /* Real touch and keyboard switching, independent of fire/move. */
            pump(60);touch(a,6,60,.5,.5);CHECK(g->state==RN_PLAY&&!g->shotgun);
            g->x=-2;g->y=10.4;g->sector=rn_locate(g,g->x,g->y);rn_advance(g,(struct rn_input){0},10);CHECK(g->shotgun);
            touch(a,8,60,.5,.5);CHECK(g->weapon==RN_CARBINE);touch(a,8,60,.5,.5);CHECK(g->weapon==RN_SHOTGUN);
            CHECK(pos_input_game_event(0,22,'q',true));pump(60);CHECK(g->weapon==RN_CARBINE);pump(60);CHECK(g->weapon==RN_CARBINE);pos_input_game_event(0,22,0,false);
            touch(a,6,60,.5,.5);for(int i=0;i<5000&&g->state==RN_PLAY;i++)rn_advance(g,(struct rn_input){0},10);CHECK(g->state==RN_DEAD);shot(a,names[mode],"death");
            CHECK(rn_restart(g,true));g->reduced_motion=motion;
            for(int t=0;t<1000&&!g->flash;t++)rn_advance(g,relay_pilot(g,-3.5,6.5),10);
            CHECK(g->flash);shot(a,names[mode],"slice-combat");

        }
        app_neon_outpost.destroy(a);lv_obj_delete(body);lv_obj_delete(title);pump(60);CHECK(timers()==base);CHECK(!pos_input_game_active()&&!pos_input_raw_target());
      }
    }
    point=(lv_point_t){0,0};
    for(int i=0;i<12;i++){lv_obj_t *body=lv_obj_create(lv_screen_active());lv_obj_set_pos(body,12,90);lv_obj_set_size(body,512,560);lv_obj_set_flex_flow(body,LV_FLEX_FLOW_COLUMN);int base=timers();void *a=app_neon_outpost.create(body);pump(40);touch(a,7,50,.5,.5);CHECK(!neon_app_game(a)->paused);pos_input_game_event(1,26,'w',true);app_neon_outpost.destroy(a);lv_obj_delete(body);pos_input_game_event(1,26,0,false);pump(30);CHECK(timers()==base);errno=0;CHECK(waitpid(-1,NULL,WNOHANG)==-1&&errno==ECHILD);}
    /* Opt-in device timing: a summary with every field, and nothing left on the display once closed. */
    {char path[]="/tmp/relay-stats-XXXXXX";int fd=mkstemp(path);CHECK(fd>=0);close(fd);setenv("DOORS_RELAY_STATS",path,1);uint32_t events=lv_display_get_event_count(d);
     lv_obj_t *body=lv_obj_create(lv_screen_active());lv_obj_set_size(body,512,560);lv_obj_set_flex_flow(body,LV_FLEX_FLOW_COLUMN);void *a=app_neon_outpost.create(body);CHECK(lv_display_get_event_count(d)>events);
     pump(40);touch(a,7,50,.5,.5);pump(400);app_neon_outpost.destroy(a);lv_obj_delete(body);unsetenv("DOORS_RELAY_STATS");pump(40);CHECK(lv_display_get_event_count(d)==events);
     char text[4096]={0};FILE *f=fopen(path,"r");CHECK(f);CHECK(fread(text,1,sizeof(text)-1,f)>0);fclose(f);unlink(path);
     CHECK(strstr(text,"interval_ms=")&&strstr(text,"game_ms=")&&strstr(text,"scene_ms=")&&strstr(text,"refresh_ms=")&&strstr(text,"draw_ms=")&&strstr(text,"flush_ms=")&&strstr(text,"# closed"));}
    pos_input_deinit();lv_indev_delete(p);lv_display_delete(d);lv_deinit();printf("relay_app_test: %u checks passed\n",checks);return 0;
}
