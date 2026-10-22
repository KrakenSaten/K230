/* Relay Nine: held keyboard actions, single-pointer compound pads, bounded
 * software viewport and foreground audio session. Internal app id retained.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "neon_app.h"
#include "relay_audio.h"
#include "relay_stats.h"
#include "pocketui.h"
#include "shell_power.h"
#include "shell_lock.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
struct neon_app {
    struct rn_game game;struct rn_frame rendered;struct rn_audio audio;
    lv_obj_t *root,*image,*hud,*message_box,*message,*button[14];
    lv_image_dsc_t image_data;lv_timer_t *timer;uint32_t last;
    struct rn_input touch;int pad;bool help,sound,started;int volume;
    struct rn_stats *stats;uint64_t tick;bool title_shown;
};
static const char *const labels[]={"MOVE + TURN","FIRE + MOVE\n+ TURN","USE","RELOAD","PAUSE","CONTROLS","RESTART","PLAY","WEAPON","SOUND","VOL -","VOL +","MUTE","BACK"};
static bool panel(struct neon_app *a){return a->help||a->game.paused||a->game.state!=RN_PLAY;}
static void layout(struct neon_app *a)
{
    int w=lv_obj_get_content_width(a->root),h=lv_obj_get_content_height(a->root);if(w<64||h<64)return;
    bool wide=w>h*3/2,menu=panel(a),title=menu&&!a->started&&!a->help&&!a->sound;
    int left=wide?w*2/3:w,controls=w-left;
    int hudh=pocketui_role_line_height(POS_STYLE_TEXT_PRIMARY)*2+12;
    int space=wide?h-hudh-70:h-hudh-240-70;if(title&&wide)space-=90;
    int vw=left-16,vh=vw*RN_H/RN_W;if(vh>space){vh=space;vw=vh*RN_W/RN_H;}if(vw<1)vw=1;if(vh<1)vh=1;
    lv_obj_set_pos(a->hud,8,4);lv_obj_set_size(a->hud,left-16,hudh-4);
    lv_obj_set_pos(a->image,(left-RN_W)/2,hudh+(vh-RN_H)/2);
    lv_image_set_scale(a->image,(unsigned)vw*256/RN_W);
    int msgy=menu&&!title?hudh+4:hudh+vh+8;
    lv_obj_set_pos(a->message_box,8,msgy);
    lv_obj_set_size(a->message_box,left-16,menu?(wide?h-msgy-12:h-msgy-88):60);
    lv_obj_set_width(a->message,LV_PCT(100));lv_obj_set_height(a->message,LV_SIZE_CONTENT);
    int count=0;for(int i=0;i<14;i++)if(!lv_obj_has_flag(a->button[i],LV_OBJ_FLAG_HIDDEN))count++;
    int at=0;static const int game_slot[14]={0,1,2,3,5,6,0,0,4,0,0,0,0,0};
    for(int i=0;i<14;i++){
        if(lv_obj_has_flag(a->button[i],LV_OBJ_FLAG_HIDDEN))continue;
        int bx,by,bw,bh=68;
        if(menu){bw=wide?controls-16:(w-8*(count+1))/count;bx=wide?left+8:8+at*(bw+8);by=wide?hudh+at*76:h-76;at++;}
        else if(i<2){bw=wide?(controls-24)/2:(w-24)/2;bh=wide?128:152;bx=wide?left+8+i*(bw+8):8+i*(bw+8);by=wide?hudh:h-232;}
        else{int slot=game_slot[i]-2;bw=wide?(controls-24)/2:(w-48)/5;bx=wide?left+8+(slot%2)*(bw+8):8+slot*(bw+8);by=wide?hudh+136+(slot/2)*76:h-72;}
        lv_obj_set_pos(a->button[i],bx,by);lv_obj_set_size(a->button[i],bw,bh);
    }
}
static void text(lv_obj_t *obj,const char *value){if(strcmp(lv_label_get_text(obj),value))lv_label_set_text(obj,value);}
static void refresh(struct neon_app *a)
{
    struct rn_game *g=&a->game;char b[180];
    int ammo=g->weapon==RN_SHOTGUN?g->loaded:g->mag,reserve=g->weapon==RN_SHOTGUN?g->shells:g->reserve;
    if(!a->started)lv_snprintf(b,sizeof(b),"STATION 09 // EMERGENCY POWER\nFIELD LINK OFFLINE");
    else lv_snprintf(b,sizeof(b),"HP %d  %s %d/%d  %d/%d\n%s | %s",g->health,g->weapon==RN_SHOTGUN?"BREACH-2":"CARBINE",ammo,reserve,g->kills,g->enemy_count,rn_room_name(g->sectors[g->sector].room),a->audio.status);
    text(a->hud,b);
    char sound_text[180];lv_snprintf(sound_text,sizeof(sound_text),"SYSTEM SOUND\nVolume %d%% | %s\nUses DOORS system volume and mute.\n%s",pocketos_shell_volume_get(),pocketos_shell_volume_muted()?"MUTED":"UNMUTED",pocketos_shell_volume_available()?"Changes apply to the next effect session.":"No system sound output is detected.");
    const char *msg=a->help?"CONTROLS\nW/S move; A/D strafe; Left/Right turn.\nHold Space to fire. F use. R reload. Q change weapon.\nP/Esc pause. H controls. Enter resumes.\nTouch MOVE: up/down moves, left/right turns.\nFIRE pad uses the same drag axes while shooting.\nRelease stops all pad actions. Stop and tap WEAPON to change guns.\nUSE opens nearby doors, scored panels, the relay and exit.\nRestore the upper relay, then use the EXIT lock.\nLeaving or rotating abandons the run.":a->sound?sound_text:g->state==RN_DEAD?"SIGNAL LOST\nThe station's machines overwhelmed you.\nRESTART to re-establish the link.":g->state==RN_WON?"RELAY RESTORED\nEmergency power stabilised. Exit reached.\nStation 09 is transmitting again.":!a->started?"Emergency power is failing.\nRestore the upper relay. Reach the exit.\nThe maintenance bay may hold useful equipment.\nNo signal gets left behind.":g->paused?"LINK SUSPENDED\nPLAY to resume. Leaving abandons this run.":g->reloading?"RELOADING":g->notice_ms?rn_notice(g):g->relay?"Relay online. Descend return stairs to EXIT.":g->sectors[g->sector].room==4?"Power reserves critical. USE the relay console.":g->sectors[g->sector].room==7?"Maintenance bay. Inspect the scored service panel.":"Restore the upper relay. USE nearby doors.";
    text(a->message,msg);
    bool menu=panel(a),title=menu&&!a->started&&!a->help&&!a->sound;
    if(menu&&!title)lv_obj_add_flag(a->image,LV_OBJ_FLAG_HIDDEN);else lv_obj_remove_flag(a->image,LV_OBJ_FLAG_HIDDEN);
    text(lv_obj_get_child(a->button[5],0),menu?"CONTROLS":"HELP");
    text(lv_obj_get_child(a->button[12],0),pocketos_shell_volume_muted()?"UNMUTE":"MUTE");
    for(int i=0;i<14;i++){
        bool show;
        if(a->sound)show=i==10||i==11||i==12||i==13;
        else if(a->help)show=i==13||(i==7&&g->state==RN_PLAY);
        else if(menu)show=i==5||i==9||(i==6&&a->started)||(i==7&&g->state==RN_PLAY);
        else show=i<6||i==8;
        if(show)lv_obj_remove_flag(a->button[i],LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(a->button[i],LV_OBJ_FLAG_HIDDEN);
    }
    /* The title is static: drawn once, not re-rendered and re-flushed every frame. */
    if((title&&!a->title_shown)||!menu){uint64_t t=rn_stats_now();if(title)rn_title(&a->rendered);else rn_render(g,&a->rendered);rn_stats_add(a->stats,RS_SCENE,t);lv_image_cache_drop(&a->image_data);lv_obj_invalidate(a->image);}
    a->title_shown=title;layout(a);
}
static void clear(struct neon_app *a){memset(&a->touch,0,sizeof(a->touch));a->pad=-1;pos_input_game_reset(2);}
static void pause_game(struct neon_app *a){rn_pause(&a->game,true);clear(a);rn_audio_stop(&a->audio);a->last=lv_tick_get();}
static void play(struct neon_app *a)
{
    clear(a);bool lost;pos_input_game_take(NULL,NULL,&lost);a->help=a->sound=false;a->started=true;rn_pause(&a->game,false);a->last=lv_tick_get();a->volume=pocketos_shell_volume_effective();rn_audio_poll(&a->audio,a->game.sounds);rn_audio_start(&a->audio,a->volume);
}
static void action(struct neon_app *a,int id)
{
    if(id==2)a->touch.use=true;
    else if(id==3)a->touch.reload=true;
    else if(id==4){if(a->game.paused)play(a);else pause_game(a);}
    else if(id==5){pause_game(a);a->help=!a->help;a->sound=false;}
    else if(id==6){bool slice=a->game.slice;pause_game(a);rn_restart(&a->game,slice);a->game.reduced_motion=pocketos_shell_reduced_motion()!=0;play(a);}
    else if(id==7)play(a);
    else if(id==8)a->touch.switch_weapon=true;
    else if(id==9){pause_game(a);a->sound=true;a->help=false;}
    else if(id==10||id==11){int v=pocketos_shell_volume_get()+(id==10?-5:5);if(v<0)v=0;if(v>100)v=100;if(pocketos_shell_volume_set(v)<0)pocketos_shell_set_status_hint("Sound setting could not be saved");}
    else if(id==12){if(pocketos_shell_volume_set_muted(!pocketos_shell_volume_muted())<0)pocketos_shell_set_status_hint("Sound setting could not be saved");}
    else if(id==13)a->help=a->sound=false;
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
    uint64_t t0=rn_stats_now();if(a->tick)rn_stats_add(a->stats,RS_INTERVAL,a->tick);a->tick=t0;bool stalled=false;
    unsigned held,pressed;bool input_lost;pos_input_game_take(&held,&pressed,&input_lost);
    bool available=pos_input_focused()==a->root&&!pos_input_group_redirected()&&!shell_power_screen_off()&&!shell_lock_is_locked();
    if(!available||input_lost||dt>250){if(!a->game.paused){stalled=dt>250;pause_game(a);}}
    if(pressed&POS_GAME_PAUSE){action(a,4);return;}
    if(!panel(a)){
        struct rn_input in=a->touch;
        in.forward+=(held&POS_GAME_FORWARD?1:0)-(held&POS_GAME_BACK?1:0);
        in.strafe+=(held&POS_GAME_STRAFE_RIGHT?1:0)-(held&POS_GAME_STRAFE_LEFT?1:0);
        in.turn+=(held&POS_GAME_TURN_RIGHT?1:0)-(held&POS_GAME_TURN_LEFT?1:0);
        in.fire|=(held&POS_GAME_FIRE)!=0;in.use|=(held&POS_GAME_USE)!=0;in.reload|=(held&POS_GAME_RELOAD)!=0;/* Selection is a press event. Adjacent touch and keyboard presses
         * remain distinct even without an idle simulation tick between. */
        if(in.switch_weapon||(pressed&POS_GAME_WEAPON))rn_switch_weapon(&a->game);
        in.switch_weapon=false;
        rn_advance(&a->game,in,dt);if(a->pad<0)memset(&a->touch,0,sizeof(a->touch));
        int volume=pocketos_shell_volume_effective();if(volume!=a->volume){rn_audio_stop(&a->audio);a->volume=volume;rn_audio_start(&a->audio,volume);}
        rn_audio_poll(&a->audio,a->game.sounds);if(a->game.state!=RN_PLAY)rn_audio_stop(&a->audio);
    }else rn_audio_poll(&a->audio,a->game.sounds);
    rn_stats_add(a->stats,RS_GAME,t0);bool playing=!panel(a);
    refresh(a);rn_stats_frame(a->stats,&a->game,playing,stalled);
}
static void *create(lv_obj_t *body)
{
    struct neon_app *a=lv_malloc_zeroed(sizeof(*a));if(!a)return NULL;
    bool slice=getenv("DOORS_RELAY_SLICE")!=NULL;
    if(!rn_restart(&a->game,slice)){lv_free(a);return NULL;}a->game.reduced_motion=pocketos_shell_reduced_motion()!=0;
    rn_audio_init(&a->audio);a->help=false;a->pad=-1;rn_pause(&a->game,true);
    a->root=lv_obj_create(body);lv_obj_remove_style_all(a->root);lv_obj_set_width(a->root,LV_PCT(100));lv_obj_set_flex_grow(a->root,1);lv_obj_remove_flag(a->root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(a->root,lv_color_hex(0x151914),0);lv_obj_set_style_bg_opa(a->root,LV_OPA_COVER,0);
    a->image_data.header.magic=LV_IMAGE_HEADER_MAGIC;a->image_data.header.cf=LV_COLOR_FORMAT_RGB565;a->image_data.header.w=RN_W;a->image_data.header.h=RN_H;a->image_data.header.stride=RN_W*2;a->image_data.data_size=sizeof(a->rendered.pixels);a->image_data.data=(uint8_t *)a->rendered.pixels;
    a->image=lv_image_create(a->root);lv_image_set_src(a->image,&a->image_data);lv_image_set_antialias(a->image,false);
    a->hud=pocketui_label(a->root,"",POS_STYLE_TEXT_PRIMARY);lv_obj_set_style_text_color(a->hud,lv_color_hex(0xe3dfbd),0);
    a->message_box=lv_obj_create(a->root);lv_obj_remove_style_all(a->message_box);lv_obj_set_scroll_dir(a->message_box,LV_DIR_VER);
    a->message=pocketui_label(a->message_box,"",POS_STYLE_TEXT_PRIMARY);lv_obj_set_style_text_color(a->message,lv_color_hex(0xe3dfbd),0);lv_obj_set_style_text_align(a->message,LV_TEXT_ALIGN_CENTER,0);
    for(int i=0;i<14;i++){
        a->button[i]=lv_button_create(a->root);lv_obj_remove_style_all(a->button[i]);lv_obj_remove_flag(a->button[i],LV_OBJ_FLAG_CLICK_FOCUSABLE|LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(a->button[i],LV_OBJ_FLAG_PRESS_LOCK);pos_style_add(a->button[i],POS_STYLE_BUTTON_SECONDARY,0);pos_style_add(a->button[i],POS_STYLE_BUTTON_PRIMARY_PRESSED,LV_STATE_PRESSED);
        lv_obj_t *label=pocketui_label(a->button[i],labels[i],POS_STYLE_BUTTON_LABEL);lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_width(label,LV_PCT(100));lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);lv_obj_center(label);
        lv_obj_set_user_data(a->button[i],(void *)(intptr_t)i);lv_obj_add_event_cb(a->button[i],touch,LV_EVENT_ALL,a);
    }
    lv_obj_add_event_cb(a->root,resized,LV_EVENT_SIZE_CHANGED,a);lv_obj_add_event_cb(a->root,key,LV_EVENT_KEY,a);lv_obj_add_event_cb(a->root,lost,LV_EVENT_DEFOCUSED,a);
    pos_input_add_obj(a->root);pos_input_focus(a->root);pos_input_set_raw_target(a->root);pos_input_game_subscribe(a->root);
    a->stats=rn_stats_open();a->last=lv_tick_get();a->timer=lv_timer_create(frame,30,a);lv_obj_update_layout(a->root);refresh(a);return a;
}
static void destroy(void *priv)
{
    struct neon_app *a=priv;if(!a)return;lv_timer_delete(a->timer);rn_audio_stop(&a->audio);rn_stats_close(a->stats);pos_input_game_unsubscribe(a->root);pos_input_set_raw_target(NULL);
    lv_image_cache_drop(&a->image_data);lv_image_set_src(a->image,NULL);
    lv_obj_remove_event_cb(a->root,lost);lv_obj_remove_event_cb(a->root,key);lv_obj_remove_event_cb(a->root,resized);
    for(int i=0;i<14;i++)lv_obj_remove_event_cb(a->button[i],touch);
    lv_free(a);
}
const struct pocketos_app app_neon_outpost={.id="neon-outpost",.name="Relay Nine",.icon=LV_SYMBOL_PLAY,.create=create,.destroy=destroy};
struct rn_game *neon_app_game(void *priv){return priv?&((struct neon_app *)priv)->game:NULL;}
lv_obj_t *neon_app_button(void *priv,unsigned i){return priv&&i<14?((struct neon_app *)priv)->button[i]:NULL;}
void neon_app_refresh(void *priv){if(priv)refresh(priv);}
