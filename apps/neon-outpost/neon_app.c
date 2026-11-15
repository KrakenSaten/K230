/* Relay Nine: held keyboard actions, single-pointer compound pads, bounded
 * software viewport and foreground audio session. Internal app id retained.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "neon_app.h"
#include "relay_audio.h"
#include "relay_economy.h"
#include "relay_scale.h"
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
    lv_obj_t *root,*image,*hud,*message_box,*message,*button[14],*brand,*eyebrow,*sector,*fx;
    enum rn_state visual_state;uint32_t end_at;
    bool compact_brand,compact_message,warning;
    int scene_x,scene_y,scene_w,scene_h;struct rn_economy economy;unsigned render_us;
    lv_image_dsc_t image_data;lv_timer_t *timer;uint32_t last;
    /* The scene is scaled here (relay_scale.h) and drawn unscaled. lvgl_scale
     * selects LVGL's image transform instead: the fallback when the buffer
     * cannot be allocated, and DOORS_RELAY_LVGL_SCALE for A/B measurement. */
    struct rn_scaler scaler;int view_x,view_y;unsigned view_scale;bool scene_dirty,lvgl_scale;
    int ticks; /* kill-tick state last invalidated */
    struct rn_input touch;int pad;bool help,sound,started;int volume;
    struct rn_stats *stats;uint64_t tick;bool title_shown,fullscreen;
};
static const char *const labels[]={"NAVIGATE\nMOVE / TURN","ENGAGE\nFIRE / MOVE","USE","RELOAD","PAUSE","CONTROLS","RESTART","PLAY","WEAPON","SOUND","VOL -","VOL +","MUTE","BACK"};
static bool panel(struct neon_app *a){return a->help||a->game.paused||a->game.state!=RN_PLAY;}
static void style_toggle(lv_obj_t *obj,enum pos_style_role role,bool value,bool *previous)
{
    if(*previous==value)return;
    *previous=value;
    if(value)pos_style_add(obj,role,0);else lv_obj_remove_style(obj,pos_style(role),0);
}
static void scene_rect(struct neon_app *a,int x,int y,int w,int h)
{
    if(a->scene_x==x&&a->scene_y==y&&a->scene_w==w&&a->scene_h==h)return;
    a->scene_x=x;a->scene_y=y;a->scene_w=w;a->scene_h=h;
    lv_obj_invalidate(a->root);
}
/* The scene's top-left corner and uniform scale (in 1/256); present() draws it. */
static void scene_view(struct neon_app *a,int x,int y,unsigned scale)
{
    if(a->view_x==x&&a->view_y==y&&a->view_scale==scale)return;
    a->view_x=x;a->view_y=y;a->view_scale=scale;a->scene_dirty=true;
}
static void image_source(struct neon_app *a,const uint16_t *pixels,int w,int h)
{
    lv_image_dsc_t *d=&a->image_data;
    if(d->data==(const uint8_t *)pixels&&(int)d->header.w==w&&(int)d->header.h==h)return;
    d->header.w=(uint32_t)w;d->header.h=(uint32_t)h;d->header.stride=(uint32_t)w*2;
    d->data_size=(uint32_t)((size_t)w*h*2);d->data=(const uint8_t *)pixels;
    lv_image_set_src(a->image,d);
}
/* Puts the latest frame on screen at the current view. In the normal path the
 * app scales it into a buffer of exactly the drawn size (filtered, or nearest
 * in economy) and LVGL copies it; nothing is rescaled while the frame and the
 * view are unchanged (title, menus). */
static void present(struct neon_app *a)
{
    if(!a->scene_dirty||!a->view_scale)return;
    a->scene_dirty=false;
    int w=RN_W*(int)a->view_scale/256,h=RN_H*(int)a->view_scale/256;
    if(w<1)w=1;
    if(h<1)h=1;
    if(!a->lvgl_scale&&!rn_scaler_resize(&a->scaler,RN_W,RN_H,w,h)){
        a->lvgl_scale=true;LV_LOG_WARN("relay: no memory for the scaled scene, using LVGL scaling");
    }
    lv_image_cache_drop(&a->image_data);
    if(a->lvgl_scale){
        image_source(a,a->rendered.pixels,RN_W,RN_H);
        lv_image_set_scale(a->image,a->view_scale);
        /* LVGL scales about the centre of the unscaled object. */
        lv_obj_set_pos(a->image,a->view_x-(RN_W-w)/2,a->view_y-(RN_H-h)/2);
    }else{
        uint64_t t=rn_stats_now();
        rn_scale(&a->scaler,a->rendered.pixels,!a->economy.on);
        rn_stats_add(a->stats,RS_SCALE,t);
        image_source(a,a->scaler.pixels,w,h);
        lv_image_set_scale(a->image,LV_SCALE_NONE);
        lv_obj_set_pos(a->image,a->view_x,a->view_y);
    }
    lv_obj_invalidate(a->image);
}
/* Portrait cockpit: the camera is tall, with dedicated instrument and touch
 * bays. Landscape uses side consoles. Scaling remains uniform, with no crop. */
static void gameplay_layout(struct neon_app *a,int w,int h)
{
    lv_area_t r;lv_obj_get_coords(a->root,&r);
    struct pos_insets safe=pos_display_rect_insets(pocketui_display_geometry(),r.x1,r.y1,r.x2,r.y2);
    int x=16+safe.left,y=12+safe.top,rw=w-x-16-safe.right,bottom=h-12-safe.bottom;
    bool wide=w>h;int bh=80,bw=wide?156:(rw-32)/5;
    int padw=wide?172:(rw-16)/2,padh=wide?154:192;
    int top=wide?72:y+166,available=wide?h-80:bottom-top-padh-bh-92;
    if(available<80)available=80;
    int vw=wide?w-2*(padw+20):w,vh=vw*RN_H/RN_W;
    if(vh>available){vh=available;vw=vh*RN_W/RN_H;}
    unsigned scale=(unsigned)vw*256/RN_W;
    int actual_w=RN_W*(int)scale/256,actual_h=RN_H*(int)scale/256;
    scene_rect(a,(w-actual_w)/2,top,actual_w,actual_h);
    scene_view(a,(w-actual_w)/2,top,scale);
    style_toggle(a->brand,POS_STYLE_GAME_BRAND_COMPACT,wide,&a->compact_brand);
    lv_obj_set_pos(a->brand,x,y);lv_obj_set_width(a->brand,wide?170:rw);
    lv_obj_set_pos(a->eyebrow,x,y+(wide?36:48));lv_obj_set_width(a->eyebrow,wide?172:rw);
    lv_obj_set_pos(a->hud,x,y+(wide?64:79));lv_obj_set_size(a->hud,wide?172:rw,LV_SIZE_CONTENT);
    lv_obj_set_pos(a->sector,wide?w-174:x,wide?y+bh+8:top-27);lv_obj_set_width(a->sector,wide?156:rw);
    lv_text_align_t align=wide?LV_TEXT_ALIGN_RIGHT:LV_TEXT_ALIGN_LEFT;
    if(lv_obj_get_style_text_align(a->sector,LV_PART_MAIN)!=align)lv_obj_set_style_text_align(a->sector,align,0);
    style_toggle(a->message,POS_STYLE_GAME_MESSAGE_COMPACT,wide,&a->compact_message);
    int msgy=wide?8:top+actual_h+20;
    lv_obj_set_pos(a->message_box,wide?(w-360)/2:x,msgy);
    lv_obj_set_size(a->message_box,wide?360:rw,wide?56:64);
    lv_obj_set_width(a->message,LV_PCT(100));lv_obj_set_height(a->message,LV_SIZE_CONTENT);
    for(int i=0;i<14;i++){
        if(lv_obj_has_flag(a->button[i],LV_OBJ_FLAG_HIDDEN))continue;
        int bx,by,bwidth=bw,bheight=bh;
        if(i<2){bwidth=padw;bheight=padh;bx=i?x+rw-padw:x;by=bottom-padh-(wide?0:bh+12);}
        else if(wide){
            bool right=i==8||i==5||i==4;bx=right?x+rw-bw:x;
            by=i==4?y:bottom-padh-12-bh-((i==2||i==8)?bh+8:0);
        }else{
            int slot=i==2?0:i==3?1:i==8?2:i==5?3:4;
            bx=x+slot*(bw+8);by=bottom-bh;
        }
        lv_obj_set_pos(a->button[i],bx,by);lv_obj_set_size(a->button[i],bwidth,bheight);
    }
    present(a);
}
static void layout(struct neon_app *a)
{
    int w=lv_obj_get_content_width(a->root),h=lv_obj_get_content_height(a->root);if(w<64||h<64)return;
    if(!panel(a)){gameplay_layout(a,w,h);return;}
    bool wide=w>h,title=!a->started&&!a->help&&!a->sound;
    style_toggle(a->message,POS_STYLE_GAME_MESSAGE_COMPACT,false,&a->compact_message);
    int left=wide?w*2/3:w,controls=w-left;
    style_toggle(a->brand,POS_STYLE_GAME_BRAND_COMPACT,false,&a->compact_brand);
    lv_obj_set_pos(a->brand,24,20);lv_obj_set_width(a->brand,left-48);
    lv_obj_set_pos(a->eyebrow,24,72);lv_obj_set_width(a->eyebrow,left-48);
    lv_obj_set_pos(a->hud,24,110);lv_obj_set_size(a->hud,left-48,LV_SIZE_CONTENT);
    lv_obj_set_pos(a->sector,24,164);lv_obj_set_width(a->sector,left-48);
    if(lv_obj_get_style_text_align(a->sector,LV_PART_MAIN)!=LV_TEXT_ALIGN_LEFT)lv_obj_set_style_text_align(a->sector,LV_TEXT_ALIGN_LEFT,0);
    int vh=wide?h-360:(title?h/2:h-300),vw=vh*RN_W/RN_H;
    if(vw>left-32){vw=left-32;vh=vw*RN_H/RN_W;}
    if(vw<1)vw=1;
    if(vh<1)vh=1;
    unsigned scale=(unsigned)vw*256/RN_W;int dw=RN_W*(int)scale/256,dh=RN_H*(int)scale/256;
    scene_view(a,(left-dw)/2,190+(vh-dh)/2,scale);
    scene_rect(a,(left-vw)/2,190,vw,vh);
    int msgy=title?190+vh+12:a->game.state!=RN_PLAY&&!wide?368:206;
    lv_obj_set_pos(a->message_box,24,msgy);
    lv_obj_set_size(a->message_box,left-48,(wide?h-12:h-104)-msgy);
    lv_obj_set_width(a->message,LV_PCT(100));lv_obj_set_height(a->message,LV_SIZE_CONTENT);
    int count=0;for(int i=0;i<14;i++)if(!lv_obj_has_flag(a->button[i],LV_OBJ_FLAG_HIDDEN))count++;
    int at=0;
    for(int i=0;i<14;i++){
        if(lv_obj_has_flag(a->button[i],LV_OBJ_FLAG_HIDDEN))continue;
        int bw=wide?controls-24:(w-16*(count+1))/count;
        int bx=wide?left+12:16+at*(bw+16),by=wide?140+at*76:h-88;at++;
        lv_obj_set_pos(a->button[i],bx,by);lv_obj_set_size(a->button[i],bw,72);
    }
    present(a);
}
static void text(lv_obj_t *obj,const char *value){if(strcmp(lv_label_get_text(obj),value))lv_label_set_text(obj,value);}
/* Where the end-of-run emblem is drawn (end_art), in screen coordinates. */
static void end_geometry(struct neon_app *a,int *cx,int *cy,int *radius)
{
    lv_area_t r;lv_obj_get_coords(a->root,&r);int w=lv_obj_get_width(a->root),h=lv_obj_get_height(a->root);
    bool wide=w>h;
    *cx=r.x1+(wide?w*5/6:w/2);*cy=r.y1+(wide?74:275);*radius=wide?34:64;
}
static void refresh(struct neon_app *a)
{
    bool fullscreen=!panel(a);
    if(a->fullscreen!=fullscreen){a->fullscreen=fullscreen;pocketos_shell_set_fullscreen(fullscreen);}
    struct rn_game *g=&a->game;char b[180];
    if(a->visual_state!=g->state){a->visual_state=g->state;a->end_at=lv_tick_get();lv_obj_invalidate(a->fx);}
    /* The 1.4 s expansion redraws the emblem's own square, not the screen:
     * rings reach radius + 32 at 3 px, so radius + 40 holds every frame. */
    if(g->state!=RN_PLAY&&!g->reduced_motion&&lv_tick_get()-a->end_at<1400){
        int cx,cy,radius;end_geometry(a,&cx,&cy,&radius);int m=radius+40;
        lv_area_t box={cx-m,cy-m,cx+m,cy+m};lv_obj_invalidate_area(a->fx,&box);
    }
    /* The portrait kill ticks (cockpit) lie outside every widget that changes
     * in play, so a kill or the relay must redraw their strip itself. */
    int ticks=g->relay?100:g->kills;
    if(ticks!=a->ticks){
        a->ticks=ticks;lv_area_t r;lv_obj_get_coords(a->root,&r);
        int w=lv_obj_get_width(a->root),yy=r.y1+lv_obj_get_height(a->root)-310;
        lv_area_t strip={r.x1+w/2-58,yy-8,r.x1+w/2+54,yy+4};lv_obj_invalidate_area(a->root,&strip);
    }
    int ammo=g->weapon==RN_SHOTGUN?g->loaded:g->mag,reserve=g->weapon==RN_SHOTGUN?g->shells:g->reserve;
    if(!a->started)lv_snprintf(b,sizeof(b),"REACTOR DORMANT\nAWAITING OPERATOR");
    else lv_snprintf(b,sizeof(b),"VITAL %03d   %s %02d / %02d\nSENTINELS %02d / %02d",g->health,g->weapon==RN_SHOTGUN?"B-2":"C-9",ammo,reserve,g->kills,g->enemy_count);
    if(a->started&&lv_obj_get_content_width(a->root)>lv_obj_get_content_height(a->root)&&!panel(a))
        lv_snprintf(b,sizeof(b),"HP %03d  %s\n%02d/%02d   K %02d/%02d",g->health,g->weapon==RN_SHOTGUN?"B-2":"C-9",ammo,reserve,g->kills,g->enemy_count);
    text(a->hud,b);
    lv_snprintf(b,sizeof(b),"%s / %s",!a->started?"STATION 09":rn_room_name(g->sectors[g->sector].room),g->relay?"UPLINK ONLINE":"POWER CRITICAL");
    text(a->sector,b);
    text(a->eyebrow,a->started?a->audio.status:"A SIGNAL IN THE DARK");
    style_toggle(a->hud,POS_STYLE_GAME_HUD_WARNING,g->health<30,&a->warning);
    char sound_text[180];lv_snprintf(sound_text,sizeof(sound_text),"SYSTEM SOUND\nVolume %d%% | %s\nUses DOORS system volume and mute.\n%s",pocketos_shell_volume_get(),pocketos_shell_volume_muted()?"MUTED":"UNMUTED",pocketos_shell_volume_available()?"Changes apply to the next effect session.":"No system sound output is detected.");
    const char *msg=a->help?"CONTROLS\nW/S move; A/D strafe; Left/Right turn.\nHold Space to fire. F use. R reload. Q change weapon.\nP/Esc pause. H controls. Enter resumes.\nTouch NAVIGATE: up/down moves, left/right turns.\nENGAGE uses the same drag axes while shooting.\nRelease stops all pad actions. Stop and tap WEAPON to change guns.\nUSE opens nearby doors, scored panels, the relay and exit.\nRestore the upper relay, then use the EXIT lock.\nLeaving or rotating abandons the run.":a->sound?sound_text:g->state==RN_DEAD?"SIGNAL LOST\nThe station's machines overwhelmed you.\nRESTART to re-establish the link.":g->state==RN_WON?"RELAY RESTORED\nEmergency power stabilised. Exit reached.\nStation 09 is transmitting again.":!a->started?"Emergency power is failing.\nRestore the upper relay. Reach the exit.\nThe maintenance bay may hold useful equipment.\nNo signal gets left behind.":g->paused?"LINK SUSPENDED\nPLAY to resume. Leaving abandons this run.":g->reloading?"RELOADING":g->notice_ms?rn_notice(g):g->relay?"UPLINK ONLINE  /  RETURN TO EXIT":g->sectors[g->sector].room==4?"Power reserves critical. USE the relay console.":g->sectors[g->sector].room==7?"Maintenance bay. Inspect the scored service panel.":"OBJECTIVE  /  RESTORE THE UPPER RELAY";
    text(a->message,msg);
    bool menu=panel(a),title=menu&&!a->started&&!a->help&&!a->sound;
    lv_obj_remove_flag(a->image,LV_OBJ_FLAG_HIDDEN);
    lv_opa_t image_opa=menu&&!title?LV_OPA_20:LV_OPA_COVER;
    lv_opa_t message_opa=menu&&!title?LV_OPA_90:LV_OPA_TRANSP;
    if(lv_obj_get_style_opa(a->image,LV_PART_MAIN)!=image_opa)lv_obj_set_style_opa(a->image,image_opa,0);
    if(lv_obj_get_style_bg_opa(a->message_box,LV_PART_MAIN)!=message_opa)lv_obj_set_style_bg_opa(a->message_box,message_opa,0);
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
    if((title&&!a->title_shown)||!menu){uint64_t t=rn_stats_now();if(title)rn_title(&a->rendered);else {
        rn_render_quality(g,&a->rendered,a->economy.on);
        /* Fed to the economy controller by frame(), with the interval. */
        uint64_t elapsed=rn_stats_now()-t;a->render_us=elapsed>UINT32_MAX?UINT32_MAX:(unsigned)elapsed;
    }rn_stats_add(a->stats,RS_SCENE,t);a->scene_dirty=true;}
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
    a->render_us=0;refresh(a);
    /* Shed optional detail under sustained load, scene or display (LVGL draw
     * and flush show up in the interval); relay_economy.h has the rules. */
    if(playing){
        bool was=a->economy.on;rn_economy_frame(&a->economy,dt,a->render_us);
        if(a->economy.on!=was)lv_image_set_antialias(a->image,!a->economy.on);
    }else rn_economy_idle(&a->economy);
    rn_stats_economy(a->stats,a->economy.on,a->economy.entries);
    rn_stats_frame(a->stats,&a->game,playing,stalled);
}
/* Instrument artwork is local to this app. Fixed geometry is only redrawn
 * when LVGL invalidates its region; no animation objects or extra timers. */
static void line(lv_layer_t *layer,int x1,int y1,int x2,int y2,enum pos_game_ink color,int width)
{
    lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.color=pos_game_color(color);d.width=width;d.opa=LV_OPA_COVER;
    d.p1.x=x1;d.p1.y=y1;d.p2.x=x2;d.p2.y=y2;lv_draw_line(layer,&d);
}
static void arc(lv_layer_t *layer,int x,int y,int radius,int begin,int end,enum pos_game_ink color,int width)
{
    lv_draw_arc_dsc_t d;lv_draw_arc_dsc_init(&d);d.color=pos_game_color(color);d.width=width;d.opa=LV_OPA_COVER;
    d.center=(lv_point_t){x,y};d.radius=(uint16_t)radius;d.start_angle=begin;d.end_angle=end;lv_draw_arc(layer,&d);
}
static void cockpit(lv_event_t *e)
{
    struct neon_app *a=lv_event_get_user_data(e);lv_layer_t *layer=lv_event_get_layer(e);lv_area_t r;lv_obj_get_coords(a->root,&r);
    int w=lv_obj_get_width(a->root),h=lv_obj_get_height(a->root),ox=r.x1,oy=r.y1;
    /* Authored rails and broken arcs surround the optical chamber. */
    int x=ox+a->scene_x,y=oy+a->scene_y,sw=a->scene_w,sh=a->scene_h;
    for(int side=0;side<2;side++){
        int xx=side?x+sw+4:x-5;
        line(layer,xx,y+26,xx,y+sh-26,POS_GAME_FRAME,1);
        line(layer,xx,y+sh/3,xx,y+sh/3+45,POS_GAME_CURRENT,2);
        for(int j=0;j<4;j++)line(layer,xx-3,y+8+j*5,xx+3,y+8+j*5,POS_GAME_FRAME_LIT,1);
    }
    line(layer,ox+24,oy+86,ox+w-24,oy+86,POS_GAME_FRAME,1);
    if(panel(a)){
        arc(layer,ox+w-52,oy+50,20,25,302,POS_GAME_OPTIC_DIM,2);
        arc(layer,ox+w-52,oy+50,14,200,345,POS_GAME_OPTIC,2);
        return;
    }
    if(w<h){
        int yy=oy+h-310;
        line(layer,ox+24,yy,ox+w-24,yy,POS_GAME_FRAME,1);
        for(int j=0;j<9;j++){
            int xx=ox+w/2-54+j*13;
            line(layer,xx,yy-5,xx,yy+1,a->game.relay?POS_GAME_SUCCESS:j<a->game.kills?POS_GAME_CURRENT:POS_GAME_FRAME,3);
        }
    }
}
static void pad_art(lv_event_t *e)
{
    lv_obj_t *obj=lv_event_get_target(e);int id=(int)(intptr_t)lv_obj_get_user_data(obj);if(id>1)return;
    lv_layer_t *layer=lv_event_get_layer(e);lv_area_t r;lv_obj_get_coords(obj,&r);
    int cx=(r.x1+r.x2)/2,cy=(r.y1+r.y2)/2,rad=lv_obj_get_height(obj)/2-9;
    enum pos_game_ink c=id?POS_GAME_VIOLET:POS_GAME_CURRENT;
    bool down=lv_obj_has_state(obj,LV_STATE_PRESSED);
    arc(layer,cx,cy,rad,25,155,down?c:POS_GAME_FRAME_LIT,2);
    arc(layer,cx,cy,rad,205,335,down?c:POS_GAME_FRAME_LIT,2);
    arc(layer,cx,cy,rad-7,8,78,c,2);arc(layer,cx,cy,rad-7,188,258,c,2);
    line(layer,cx-5,cy-rad+20,cx,cy-rad+15,c,2);line(layer,cx,cy-rad+15,cx+5,cy-rad+20,c,2);
    line(layer,cx-rad+19,cy-5,cx-rad+14,cy,c,2);line(layer,cx-rad+14,cy,cx-rad+19,cy+5,c,2);
    line(layer,cx+rad-19,cy-5,cx+rad-14,cy,c,2);line(layer,cx+rad-14,cy,cx+rad-19,cy+5,c,2);
    line(layer,cx-5,cy+rad-20,cx,cy+rad-15,c,2);line(layer,cx,cy+rad-15,cx+5,cy+rad-20,c,2);
}
static void end_art(lv_event_t *e)
{
    struct neon_app *a=lv_event_get_user_data(e);if(a->game.state==RN_PLAY)return;
    int cx,cy,radius;
    bool won=a->game.state==RN_WON;
    end_geometry(a,&cx,&cy,&radius);
    uint32_t elapsed=lv_tick_get()-a->end_at;if(elapsed>1400)elapsed=1400;
    double t=a->game.reduced_motion?1.0:elapsed/1400.0;
    int expansion=(int)(32*t*(2-t));enum pos_game_ink color=won?POS_GAME_SUCCESS:POS_GAME_DANGER;
    lv_layer_t *layer=lv_event_get_layer(e);
    for(int i=0;i<3;i++)arc(layer,cx,cy,radius+expansion,i*120+8,i*120+(won?105:69),color,won?2:3);
    arc(layer,cx,cy,radius-9,20,won?336:165,won?POS_GAME_SUCCESS_DIM:POS_GAME_DANGER_DIM,1);
    for(int i=0;i<9;i++){
        double angle=i*RN_PI*2/9-RN_PI/2;int inner=radius-20,outer=inner+8;
        line(layer,cx+(int)(cos(angle)*inner),cy+(int)(sin(angle)*inner),cx+(int)(cos(angle)*outer),cy+(int)(sin(angle)*outer),color,3);
    }
    if(won){line(layer,cx-15,cy,cx-3,cy+12,color,3);line(layer,cx-3,cy+12,cx+21,cy-16,color,3);}
    else {line(layer,cx-13,cy-13,cx+13,cy+13,color,3);line(layer,cx+13,cy-13,cx-13,cy+13,color,3);}
}
static void *create(lv_obj_t *body)
{
    struct neon_app *a=lv_malloc_zeroed(sizeof(*a));if(!a)return NULL;
    rn_economy_init(&a->economy);
    bool slice=getenv("DOORS_RELAY_SLICE")!=NULL;
    if(!rn_restart(&a->game,slice)){lv_free(a);return NULL;}a->game.reduced_motion=pocketos_shell_reduced_motion()!=0;
    rn_audio_init(&a->audio);a->help=false;a->pad=-1;rn_pause(&a->game,true);
    a->root=lv_obj_create(body);lv_obj_remove_style_all(a->root);lv_obj_set_width(a->root,LV_PCT(100));lv_obj_set_flex_grow(a->root,1);lv_obj_remove_flag(a->root,LV_OBJ_FLAG_SCROLLABLE);
    pos_style_add(a->root,POS_STYLE_GAME_SCENE,0);
    lv_obj_add_event_cb(a->root,cockpit,LV_EVENT_DRAW_MAIN,a);
    a->image_data.header.magic=LV_IMAGE_HEADER_MAGIC;a->image_data.header.cf=LV_COLOR_FORMAT_RGB565;a->image_data.header.w=RN_W;a->image_data.header.h=RN_H;a->image_data.header.stride=RN_W*2;a->image_data.data_size=sizeof(a->rendered.pixels);a->image_data.data=(uint8_t *)a->rendered.pixels;
    a->image=lv_image_create(a->root);lv_image_set_src(a->image,&a->image_data);lv_image_set_antialias(a->image,true);
    a->lvgl_scale=getenv("DOORS_RELAY_LVGL_SCALE")!=NULL;
    a->fx=lv_obj_create(a->root);lv_obj_remove_style_all(a->fx);
    lv_obj_set_size(a->fx,LV_PCT(100),LV_PCT(100));
    lv_obj_remove_flag(a->fx,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(a->fx,end_art,LV_EVENT_DRAW_MAIN,a);
    a->brand=pocketui_label(a->root,"RELAY NINE",POS_STYLE_TITLE);pos_style_add(a->brand,POS_STYLE_GAME_BRAND,0);
    a->eyebrow=pocketui_label(a->root,"",POS_STYLE_CAPTION);pos_style_add(a->eyebrow,POS_STYLE_GAME_CAPTION,0);
    a->sector=pocketui_label(a->root,"",POS_STYLE_CAPTION);pos_style_add(a->sector,POS_STYLE_GAME_SECTOR,0);
    a->hud=pocketui_label(a->root,"",POS_STYLE_TEXT_PRIMARY);pos_style_add(a->hud,POS_STYLE_GAME_HUD_TEXT,0);
    pos_style_add(a->hud,POS_STYLE_GAME_SCENE,0);lv_obj_set_style_bg_opa(a->hud,LV_OPA_TRANSP,0);
    a->message_box=lv_obj_create(a->root);lv_obj_remove_style_all(a->message_box);lv_obj_set_scroll_dir(a->message_box,LV_DIR_VER);
    pos_style_add(a->message_box,POS_STYLE_GAME_SCENE,0);lv_obj_set_style_bg_opa(a->message_box,LV_OPA_TRANSP,0);
    a->message=pocketui_label(a->message_box,"",POS_STYLE_TEXT_PRIMARY);pos_style_add(a->message,POS_STYLE_GAME_MESSAGE,0);lv_obj_set_style_text_align(a->message,LV_TEXT_ALIGN_LEFT,0);
    for(int i=0;i<14;i++){
        a->button[i]=lv_button_create(a->root);lv_obj_remove_style_all(a->button[i]);lv_obj_remove_flag(a->button[i],LV_OBJ_FLAG_CLICK_FOCUSABLE|LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(a->button[i],LV_OBJ_FLAG_PRESS_LOCK);pos_style_add(a->button[i],POS_STYLE_BUTTON_SECONDARY,0);pos_style_add(a->button[i],POS_STYLE_BUTTON_PRIMARY_PRESSED,LV_STATE_PRESSED);
        pos_style_add(a->button[i],POS_STYLE_GAME_BUTTON,0);
        if(i<2)pos_style_add(a->button[i],POS_STYLE_GAME_PAD,0);
        pos_style_add(a->button[i],POS_STYLE_GAME_PRESSED,LV_STATE_PRESSED);
        if(i<2)lv_obj_add_event_cb(a->button[i],pad_art,LV_EVENT_DRAW_MAIN,a);
        lv_obj_t *label=pocketui_label(a->button[i],labels[i],POS_STYLE_BUTTON_LABEL);pos_style_add(label,POS_STYLE_GAME_BUTTON_LABEL,0);lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_width(label,LV_PCT(100));lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);lv_obj_center(label);
        lv_obj_set_user_data(a->button[i],(void *)(intptr_t)i);lv_obj_add_event_cb(a->button[i],touch,LV_EVENT_ALL,a);
    }
    lv_obj_add_event_cb(a->root,resized,LV_EVENT_SIZE_CHANGED,a);lv_obj_add_event_cb(a->root,key,LV_EVENT_KEY,a);lv_obj_add_event_cb(a->root,lost,LV_EVENT_DEFOCUSED,a);
    pos_input_add_obj(a->root);pos_input_focus(a->root);pos_input_set_raw_target(a->root);pos_input_game_subscribe(a->root);
    a->stats=rn_stats_open();a->last=lv_tick_get();a->timer=lv_timer_create(frame,30,a);lv_obj_update_layout(a->root);refresh(a);return a;
}
static void destroy(void *priv)
{
    struct neon_app *a=priv;if(!a)return;lv_timer_delete(a->timer);rn_audio_stop(&a->audio);rn_stats_close(a->stats);pos_input_game_unsubscribe(a->root);pos_input_set_raw_target(NULL);
    lv_image_cache_drop(&a->image_data);lv_image_set_src(a->image,NULL);rn_scaler_free(&a->scaler);
    lv_obj_remove_event_cb(a->fx,end_art);
    lv_obj_remove_event_cb(a->root,cockpit);
    lv_obj_remove_event_cb(a->root,lost);lv_obj_remove_event_cb(a->root,key);lv_obj_remove_event_cb(a->root,resized);
    for(int i=0;i<14;i++){lv_obj_remove_event_cb(a->button[i],touch);lv_obj_remove_event_cb(a->button[i],pad_art);}
    lv_free(a);
}
const struct pocketos_app app_neon_outpost={.id="neon-outpost",.name="Relay Nine",.icon=LV_SYMBOL_PLAY,.create=create,.destroy=destroy};
struct rn_game *neon_app_game(void *priv){return priv?&((struct neon_app *)priv)->game:NULL;}
lv_obj_t *neon_app_button(void *priv,unsigned i){return priv&&i<14?((struct neon_app *)priv)->button[i]:NULL;}
void neon_app_refresh(void *priv){if(priv)refresh(priv);}

lv_obj_t *neon_app_image(void *priv){return priv?((struct neon_app *)priv)->image:NULL;}
