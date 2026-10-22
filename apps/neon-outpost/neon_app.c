/* Relay Nine: held keyboard actions, single-pointer compound pads, bounded
 * software viewport and foreground audio session. Internal app id retained.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "neon_app.h"
#include "relay_audio.h"
#include "pocketui.h"
#include "shell_power.h"
#include "shell_lock.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
struct neon_app {
    struct rn_game game;struct rn_frame rendered;struct rn_audio audio;
    lv_obj_t *root,*image,*hud,*message_box,*message,*button[8];
    lv_image_dsc_t image_data;lv_timer_t *timer;uint32_t last;
    struct rn_input touch;int pad;bool help;int volume;
};
static const char *const labels[]={"MOVE + TURN","FIRE + MOVE\n+ TURN","USE","RELOAD","PAUSE","HELP","RESTART","PLAY"};
static bool panel(struct neon_app *a){return a->help||a->game.paused||a->game.state!=RN_PLAY;}
static void layout(struct neon_app *a)
{
    int w=lv_obj_get_content_width(a->root),h=lv_obj_get_content_height(a->root);if(w<64||h<64)return;
    bool wide=w>h*3/2,menu=panel(a);int left=wide?w*2/3:w,controls=wide?w-left:w;
    int hudh=pocketui_role_line_height(POS_STYLE_TEXT_PRIMARY)*2+12;
    int space=wide?h-hudh-46:h-hudh-240-48;
    int vw=left-16,vh=vw*RN_H/RN_W;if(vh>space){vh=space;vw=vh*RN_W/RN_H;}if(vw<1)vw=1;if(vh<1)vh=1;
    lv_obj_set_pos(a->hud,8,4);lv_obj_set_size(a->hud,left-16,hudh-4);
    lv_obj_set_pos(a->image,(left-RN_W)/2,hudh+(vh-RN_H)/2);
    lv_image_set_scale(a->image,(unsigned)vw*256/RN_W);
    lv_obj_set_pos(a->message_box,8,menu?hudh+4:hudh+vh+4);
    lv_obj_set_size(a->message_box,left-16,menu?(wide?h-hudh-12:h-hudh-90):44);
    lv_obj_set_width(a->message,LV_PCT(100));lv_obj_set_height(a->message,LV_SIZE_CONTENT);
    for(int i=0;i<8;i++){
        int bx,by,bw,bh;
        if(menu){bw=wide?controls-16:(w-32)/3;bh=68;bx=wide?left+8:8+(i-5)*(bw+8);by=wide?hudh+(i-5)*76:h-76;}
        else if(i<2){bw=wide?controls-16:(w-24)/2;bh=wide?128:152;bx=wide?left+8:8+i*(bw+8);by=wide?hudh+i*136:h-232;}
        else{bw=wide?(controls-24)/2:(w-40)/4;bh=68;bx=wide?left+8+((i-2)%2)*(bw+8):8+(i-2)*(bw+8);by=wide?hudh+272+((i-2)/2)*76:h-72;}
        /* In short landscape bodies pads share the top row, so essential
         * actions stay within the real shell body, with 64px targets. */
        if(wide&&!menu&&h<hudh+424){
            if(i<2){bw=(controls-24)/2;bh=136;bx=left+8+i*(bw+8);by=hudh;}
            else{by=hudh+144+((i-2)/2)*76;}
        }
        lv_obj_set_pos(a->button[i],bx,by);lv_obj_set_size(a->button[i],bw,bh);
    }
}
static void text(lv_obj_t *obj,const char *value){if(strcmp(lv_label_get_text(obj),value))lv_label_set_text(obj,value);}
static void refresh(struct neon_app *a)
{
    struct rn_game *g=&a->game;char b[180];
    lv_snprintf(b,sizeof(b),"HP %d   %d / %d   %d/%d\n%s  |  %s",g->health,g->mag,g->reserve,g->kills,g->enemy_count,rn_room_name(g->sectors[g->sector].room),a->audio.status);text(a->hud,b);
    const char *msg=a->help?"RELAY NINE\nRestore the relay in the upper control room, then use the EXIT lock.\nW/S move; A/D strafe; Left/Right turn.\nHold Space to fire. F use. R reload. P/Esc pause.\nTouch MOVE pad: up/down moves, left/right turns.\nFIRE pad combines firing with the same drag axes.\nRelease stops all pad actions. USE operates nearby doors and relay.\nClimb west stairs, cross compressor room, climb to relay, descend return stairs.\nAmmo crates and medical packs help. Leaving or rotating abandons the run.\nPLAY to begin/resume.":g->state==RN_DEAD?"SIGNAL LOST\nThe station's machines overwhelmed you. RESTART for another run.":g->state==RN_WON?"RELAY RESTORED\nExit reached. Station 9 is back online.\nRESTART to play again.":g->paused?"PAUSED\nPLAY to resume. Leaving abandons this run.":g->reloading?"RELOADING":g->relay?"Relay restored. Descend return stairs to EXIT.":g->sectors[g->sector].room==4?"Approach the relay console and USE.":"Restore the upper relay. USE nearby doors.";
    text(a->message,msg);
    bool menu=panel(a);if(menu)lv_obj_add_flag(a->image,LV_OBJ_FLAG_HIDDEN);else lv_obj_remove_flag(a->image,LV_OBJ_FLAG_HIDDEN);
    for(int i=0;i<8;i++){bool show=menu?(i==5||i==6||(i==7&&g->state==RN_PLAY)):i<6;if(show)lv_obj_remove_flag(a->button[i],LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(a->button[i],LV_OBJ_FLAG_HIDDEN);}
    if(!menu){rn_render(g,&a->rendered);lv_image_cache_drop(&a->image_data);lv_obj_invalidate(a->image);}
    layout(a);
}
static void clear(struct neon_app *a){memset(&a->touch,0,sizeof(a->touch));a->pad=-1;pos_input_game_reset(2);}
static void pause_game(struct neon_app *a){rn_pause(&a->game,true);clear(a);rn_audio_stop(&a->audio);a->last=lv_tick_get();}
static void play(struct neon_app *a)
{
    clear(a);bool lost;pos_input_game_take(NULL,NULL,&lost);a->help=false;rn_pause(&a->game,false);a->last=lv_tick_get();a->volume=pocketos_shell_volume_effective();rn_audio_poll(&a->audio,a->game.sounds);rn_audio_start(&a->audio,a->volume);
}
static void action(struct neon_app *a,int id)
{
    if(id==2)a->touch.use=true;
    else if(id==3)a->touch.reload=true;
    else if(id==4){if(a->game.paused)play(a);else pause_game(a);}
    else if(id==5){pause_game(a);a->help=!a->help;}
    else if(id==6){bool slice=a->game.slice;pause_game(a);rn_restart(&a->game,slice);a->game.reduced_motion=pocketos_shell_reduced_motion()!=0;play(a);}
    else if(id==7)play(a);
    refresh(a);
}
static void touch(lv_event_t *e)
{
    struct neon_app *a=lv_event_get_user_data(e);int id=(int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));lv_event_code_t code=lv_event_get_code(e);
    if(id<2&&!panel(a)){
        if(code==LV_EVENT_PRESSED)a->pad=id;
        if((code==LV_EVENT_PRESSING||code==LV_EVENT_PRESSED)&&a->pad==id){
            lv_indev_t *indev=lv_indev_active();if(indev){lv_point_t p;lv_indev_get_point(indev,&p);lv_area_t r;lv_obj_get_coords(a->button[id],&r);double x=(p.x-(r.x1+r.x2)/2.0)/((r.x2-r.x1)*.4),y=((r.y1+r.y2)/2.0-p.y)/((r.y2-r.y1)*.4);
                a->touch.forward=fabs(y)<.17?0:fmax(-1,fmin(1,y));a->touch.turn=fabs(x)<.17?0:fmax(-1,fmin(1,x));a->touch.fire=id==1;}
        }
        if(code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST){memset(&a->touch,0,sizeof(a->touch));a->pad=-1;}
    }else if(id>=2&&code==LV_EVENT_CLICKED)action(a,id);
}
static void key(lv_event_t *e)
{
    struct neon_app *a=lv_event_get_user_data(e);uint32_t k=lv_event_get_key(e);pos_key_t raw;unsigned mods;
    if(pos_input_raw_decode(k,&raw,&mods))k=raw;
    if(k==LV_KEY_ENTER&&panel(a)&&a->game.state==RN_PLAY)action(a,7);
    else if(k=='h'||k=='H')action(a,5);
}
static void lost(lv_event_t *e){struct neon_app *a=lv_event_get_user_data(e);pause_game(a);refresh(a);}
static void resized(lv_event_t *e){layout(lv_event_get_user_data(e));}
static void frame(lv_timer_t *timer)
{
    struct neon_app *a=lv_timer_get_user_data(timer);uint32_t now=lv_tick_get(),dt=now-a->last;a->last=now;
    unsigned held,pressed;bool input_lost;pos_input_game_take(&held,&pressed,&input_lost);
    bool available=pos_input_focused()==a->root&&!pos_input_group_redirected()&&!shell_power_screen_off()&&!shell_lock_is_locked();
    if(!available||input_lost||dt>250){if(!a->game.paused)pause_game(a);}
    if(pressed&POS_GAME_PAUSE){action(a,4);return;}
    if(!panel(a)){
        struct rn_input in=a->touch;
        in.forward+=(held&POS_GAME_FORWARD?1:0)-(held&POS_GAME_BACK?1:0);
        in.strafe+=(held&POS_GAME_STRAFE_RIGHT?1:0)-(held&POS_GAME_STRAFE_LEFT?1:0);
        in.turn+=(held&POS_GAME_TURN_RIGHT?1:0)-(held&POS_GAME_TURN_LEFT?1:0);
        in.fire|=(held&POS_GAME_FIRE)!=0;in.use|=(held&POS_GAME_USE)!=0;in.reload|=(held&POS_GAME_RELOAD)!=0;
        rn_advance(&a->game,in,dt);if(a->pad<0)memset(&a->touch,0,sizeof(a->touch));
        int volume=pocketos_shell_volume_effective();if(volume!=a->volume){rn_audio_stop(&a->audio);a->volume=volume;rn_audio_start(&a->audio,volume);}
        rn_audio_poll(&a->audio,a->game.sounds);if(a->game.state!=RN_PLAY)rn_audio_stop(&a->audio);
    }else rn_audio_poll(&a->audio,a->game.sounds);
    refresh(a);
}
static void *create(lv_obj_t *body)
{
    struct neon_app *a=lv_malloc_zeroed(sizeof(*a));if(!a)return NULL;
    bool slice=getenv("DOORS_RELAY_SLICE")!=NULL;
    if(!rn_restart(&a->game,slice)){lv_free(a);return NULL;}a->game.reduced_motion=pocketos_shell_reduced_motion()!=0;
    rn_audio_init(&a->audio);a->help=true;a->pad=-1;rn_pause(&a->game,true);
    a->root=lv_obj_create(body);lv_obj_remove_style_all(a->root);lv_obj_set_width(a->root,LV_PCT(100));lv_obj_set_flex_grow(a->root,1);lv_obj_remove_flag(a->root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(a->root,lv_color_hex(0x151914),0);lv_obj_set_style_bg_opa(a->root,LV_OPA_COVER,0);
    a->image_data.header.magic=LV_IMAGE_HEADER_MAGIC;a->image_data.header.cf=LV_COLOR_FORMAT_RGB565;a->image_data.header.w=RN_W;a->image_data.header.h=RN_H;a->image_data.header.stride=RN_W*2;a->image_data.data_size=sizeof(a->rendered.pixels);a->image_data.data=(uint8_t *)a->rendered.pixels;
    a->image=lv_image_create(a->root);lv_image_set_src(a->image,&a->image_data);lv_image_set_antialias(a->image,false);
    a->hud=pocketui_label(a->root,"",POS_STYLE_TEXT_PRIMARY);lv_obj_set_style_text_color(a->hud,lv_color_hex(0xe3dfbd),0);
    a->message_box=lv_obj_create(a->root);lv_obj_remove_style_all(a->message_box);lv_obj_set_scroll_dir(a->message_box,LV_DIR_VER);
    a->message=pocketui_label(a->message_box,"",POS_STYLE_TEXT_PRIMARY);lv_obj_set_style_text_color(a->message,lv_color_hex(0xe3dfbd),0);lv_obj_set_style_text_align(a->message,LV_TEXT_ALIGN_CENTER,0);
    for(int i=0;i<8;i++){
        a->button[i]=lv_button_create(a->root);lv_obj_remove_style_all(a->button[i]);lv_obj_remove_flag(a->button[i],LV_OBJ_FLAG_CLICK_FOCUSABLE|LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(a->button[i],LV_OBJ_FLAG_PRESS_LOCK);pos_style_add(a->button[i],POS_STYLE_BUTTON_SECONDARY,0);pos_style_add(a->button[i],POS_STYLE_BUTTON_PRIMARY_PRESSED,LV_STATE_PRESSED);
        lv_obj_t *label=pocketui_label(a->button[i],labels[i],POS_STYLE_BUTTON_LABEL);lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_width(label,LV_PCT(100));lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);lv_obj_center(label);
        lv_obj_set_user_data(a->button[i],(void *)(intptr_t)i);lv_obj_add_event_cb(a->button[i],touch,LV_EVENT_ALL,a);
    }
    lv_obj_add_event_cb(a->root,resized,LV_EVENT_SIZE_CHANGED,a);lv_obj_add_event_cb(a->root,key,LV_EVENT_KEY,a);lv_obj_add_event_cb(a->root,lost,LV_EVENT_DEFOCUSED,a);
    pos_input_add_obj(a->root);pos_input_focus(a->root);pos_input_set_raw_target(a->root);pos_input_game_subscribe(a->root);
    a->last=lv_tick_get();a->timer=lv_timer_create(frame,30,a);lv_obj_update_layout(a->root);refresh(a);return a;
}
static void destroy(void *priv)
{
    struct neon_app *a=priv;if(!a)return;lv_timer_delete(a->timer);rn_audio_stop(&a->audio);pos_input_game_unsubscribe(a->root);pos_input_set_raw_target(NULL);
    lv_image_cache_drop(&a->image_data);lv_image_set_src(a->image,NULL);
    lv_obj_remove_event_cb(a->root,lost);lv_obj_remove_event_cb(a->root,key);lv_obj_remove_event_cb(a->root,resized);
    for(int i=0;i<8;i++)lv_obj_remove_event_cb(a->button[i],touch);
    lv_free(a);
}
const struct pocketos_app app_neon_outpost={.id="neon-outpost",.name="Relay Nine",.icon=LV_SYMBOL_PLAY,.create=create,.destroy=destroy};
struct rn_game *neon_app_game(void *priv){return priv?&((struct neon_app *)priv)->game:NULL;}
lv_obj_t *neon_app_button(void *priv,unsigned i){return priv&&i<8?((struct neon_app *)priv)->button[i]:NULL;}
void neon_app_refresh(void *priv){if(priv)refresh(priv);}
