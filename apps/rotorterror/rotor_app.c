/* RotorTerror for Doors: responsive cockpit, compound touch and held keys.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "rotor_app.h"
#include "rotor_audio.h"
#include "rotor_store.h"
#include "pocketui.h"
#include "shell_lock.h"
#include "shell_power.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

struct rotor_app {
    struct rt_game game;
    struct rt_frame frame;
    struct rt_audio audio;
    struct rt_input touch;
    lv_obj_t *root,*image,*shade,*kicker,*title,*title2,*copybox,*copy,*footer;
    lv_obj_t *score,*systems,*sector,*ordnance,*notice,*button[RTB_COUNT];
    lv_image_dsc_t image_data;
    lv_timer_t *timer;
    uint32_t last,best[3],recorded;
    int volume;
    bool help,sound,pad,rocket,was_playing,storage_failed,laying_out,audio_pending;
    unsigned selection;
};

static const char *const button_text[RTB_COUNT]={
    "LAUNCH  >","PILOT","ACE","TERROR","SOUND ON","HOW TO FLY","DOORS",
    "RESUME  >","TRY AGAIN  >","FLY + AIM\nhold: rockets","ROCKETS\nHOLD SPACE",
    "PAUSE","NEXT WAVE  >","SHIELD","CANNON","ROCKETS","REPAIR","END RUN"
};

static bool flying(const struct rotor_app *a)
{
    return a->game.phase==RT_FLIGHT && !a->game.paused && !a->help;
}

static void show(lv_obj_t *obj,bool visible)
{
    if(visible)lv_obj_remove_flag(obj,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj,LV_OBJ_FLAG_HIDDEN);
}

static void label(lv_obj_t *obj,const char *text)
{
    if(strcmp(lv_label_get_text(obj),text))lv_label_set_text(obj,text);
}

static void place(lv_obj_t *obj,int x,int y,int w,int h)
{
    lv_obj_set_pos(obj,x,y);
    lv_obj_set_size(obj,w,h);
}

static void position_text(lv_obj_t *obj,int x,int y,int w)
{
    place(obj,x,y,w,LV_SIZE_CONTENT);
}

static void clear_input(struct rotor_app *a)
{
    memset(&a->touch,0,sizeof(a->touch));
    a->pad=a->rocket=false;
    pos_input_game_reset(2);
    pos_input_game_take(NULL,NULL,NULL);
}

static void pause_run(struct rotor_app *a)
{
    if(a->game.phase==RT_FLIGHT)a->game.paused=true;
    clear_input(a);
    a->audio_pending=false;
    rt_audio_suspend(&a->audio);
}

static void play(struct rotor_app *a)
{
    clear_input(a);
    bool lost;pos_input_game_take(NULL,NULL,&lost);
    a->help=false;
    a->game.paused=false;
    a->game.accumulator=0;
    a->last=lv_tick_get();
    a->volume=pocketos_shell_volume_effective();
    /* Flush old counters before connecting: menus never queue combat sounds. */
    rt_audio_poll(&a->audio,a->game.sounds);
    a->audio_pending=a->sound;
    a->was_playing=true;
}

static void save_record(struct rotor_app *a)
{
    unsigned diff=a->game.difficulty;
    if(a->game.score<=a->best[diff] || a->recorded==a->game.score)return;
    a->best[diff]=a->game.score;
    a->recorded=a->game.score;
    a->storage_failed=rt_scores_save(a->best)!=0;
}

static void layout(struct rotor_app *a)
{
    if(a->laying_out)return;
    a->laying_out=true;
    int w=lv_obj_get_content_width(a->root),h=lv_obj_get_content_height(a->root);
    if(w<200 || h<200) { a->laying_out=false;return; }
    bool wide=w>h;
    lv_area_t bounds;lv_obj_get_coords(a->root,&bounds);
    struct pos_insets safe=pos_display_rect_insets(pocketui_display_geometry(),
        bounds.x1,bounds.y1,bounds.x2,bounds.y2);
    int left=24+safe.left,top=18+safe.top,right=w-24-safe.right,bottom=h-24-safe.bottom;
    int usable=right-left;
    unsigned scale=(unsigned)w*256/RT_W;
    int scene_h=wide?h:(flying(a)?h-320:h-360);
    if(scene_h<240)scene_h=240;
    rt_resize(&a->game,scene_h*256/(int)scale);
    if(a->image_data.header.h!=(unsigned)a->game.height) {
        lv_image_cache_drop(&a->image_data);
        a->image_data.header.h=a->game.height;
        a->image_data.data_size=(uint32_t)(RT_W*a->game.height*2);
        lv_image_set_src(a->image,&a->image_data);
    }
    lv_image_set_scale(a->image,scale);
    lv_image_set_pivot(a->image,0,0);
    int drawn_h=a->game.height*(int)scale/256;
    int image_y=wide?(h-drawn_h)/2:flying(a)?100+(h-320-drawn_h)/2:230;
    lv_obj_set_pos(a->image,0,image_y);
    place(a->shade,0,0,w,h);
    position_text(a->score,left,top,wide?260:usable);
    position_text(a->systems,wide?left+280:left,wide?top:top+44,wide?320:usable);
    position_text(a->sector,wide?right-420:left,wide?top:top+80,wide?280:usable);
    position_text(a->ordnance,wide?left+280:left,wide?top+42:bottom-252,wide?400:usable);
    position_text(a->notice,wide?w/2-220:left,top+(wide?92:130),wide?440:usable);
    place(a->button[RTB_PAUSE],right-108,wide?top:bottom-78,108,72);
    place(a->button[RTB_PAD],left,bottom-(wide?130:202),wide?200:usable-124,wide?130:188);
    place(a->button[RTB_ROCKET],right-112,bottom-(wide?104:202),112,wide?104:112);

    if(a->help) {
        position_text(a->kicker,left,top,usable);
        position_text(a->title,left,top+40,usable);
        int y=top+(wide?110:155);
        place(a->copybox,left,y,usable,bottom-y-98);
        place(a->button[RTB_RESUME],left,bottom-76,wide?320:usable,72);
        a->laying_out=false;
        return;
    }
    if(a->game.phase==RT_HANGAR) {
        position_text(a->kicker,left,top,wide?370:usable);
        position_text(a->title,left,top+38,wide?370:usable);
        place(a->copybox,left,top+(wide?190:108),wide?370:usable,wide?80:62);
        int card_w=wide?usable-400:usable;
        int card_h=wide?(bottom-top-134)/4:116;
        if(wide && card_h>98)card_h=98;
        int first=wide?top:top+205;
        for(int i=0;i<4;i++) {
            place(a->button[RTB_SHIELD+i],left+(wide?400:0),first+i*(card_h+10),card_w,card_h);
        }
        place(a->button[RTB_MENU],left,bottom-76,wide?180:140,72);
        place(a->button[RTB_NEXT],wide?right-340:left+156,bottom-76,wide?340:usable-156,72);
        position_text(a->footer,left,wide?top+280:bottom-115,wide?370:usable);
        a->laying_out=false;
        return;
    }
    bool title=a->game.phase==RT_TITLE;
    int text_w=wide?430:usable;
    position_text(a->kicker,left,top+(wide?5:26),text_w);
    position_text(a->title,left,top+(wide?41:70),text_w);
    position_text(a->title2,left,top+(wide?95:126),text_w);
    int copy_y=top+(wide?157:206);
    place(a->copybox,left,copy_y,text_w,wide?74:100);
    if(title) {
        int row=wide?bottom-206:bottom-260;
        int bw=(text_w-16)/3;
        for(int i=0;i<3;i++)place(a->button[RTB_PILOT+i],left+i*(bw+8),row,bw,64);
        place(a->button[RTB_START],left,wide?bottom-76:bottom-172,wide?320:usable,76);
        position_text(a->footer,left,wide?bottom-130:bottom-305,wide?650:usable);
    } else {
        int y=wide?bottom-164:bottom-268;
        place(a->button[RTB_RESUME],left,y,wide?320:usable,76);
        place(a->button[RTB_RETRY],left,y,wide?320:usable,76);
        place(a->button[RTB_MENU],left,y+88,wide?320:usable,72);
        position_text(a->footer,left,wide?bottom-194:bottom-320,wide?650:usable);
    }
    int nav_y=wide?top:bottom-76;
    int nav_w=wide?136:(usable-16)/3;
    int nav_x=wide?right-3*nav_w-16:left;
    place(a->button[RTB_SOUND],nav_x,nav_y,nav_w,64);
    place(a->button[RTB_HELP],nav_x+nav_w+8,nav_y,nav_w,64);
    place(a->button[RTB_EXIT],nav_x+2*(nav_w+8),nav_y,nav_w,64);
    a->laying_out=false;
}

static void refresh(struct rotor_app *a)
{
    struct rt_game *g=&a->game;
    bool active=flying(a),title=g->phase==RT_TITLE,hangar=g->phase==RT_HANGAR;
    show(a->shade,!active && !title);
    show(a->score,active);show(a->systems,active);show(a->sector,active);show(a->ordnance,active);
    show(a->notice,active && (g->banner_ms>0 || g->combo>1));
    show(a->kicker,!active);show(a->title,!active);show(a->title2,!active && !a->help && !hangar);
    show(a->copybox,!active);show(a->footer,!active && !a->help);
    for(unsigned i=0;i<RTB_COUNT;i++) {
        bool visible=false;
        if(active)visible=i==RTB_PAD || i==RTB_ROCKET || i==RTB_PAUSE;
        else if(a->help)visible=i==RTB_RESUME;
        else if(hangar)visible=i>=RTB_NEXT;
        else if(title)visible=i<=RTB_EXIT;
        else visible=i==RTB_SOUND || i==RTB_HELP || i==RTB_EXIT || i==RTB_MENU ||
            (i==RTB_RESUME && g->phase==RT_FLIGHT) || (i==RTB_RETRY && g->phase==RT_OVER);
        show(a->button[i],visible);
    }
    char text[320];
    snprintf(text,sizeof(text),"SCORE %07u",g->score);label(a->score,text);
    snprintf(text,sizeof(text),"SHIELD %03d / %d   HULL %03d",(int)ceilf(g->shield),g->shield_max,g->hull);label(a->systems,text);
    snprintf(text,sizeof(text),"%s / %02u\n%s",rt_sector(g->wave),g->wave,rt_difficulty(g->difficulty));label(a->sector,text);
    snprintf(text,sizeof(text),"RKT %d/%d   SALVAGE %u\n%s",g->rockets,g->rocket_max,g->credits,a->sound?a->audio.status:"SOUND OFF");label(a->ordnance,text);
    if(g->banner_ms>0 && g->boss_spawned)snprintf(text,sizeof(text),"HEAVY GUNSHIP INBOUND");
    else if(g->banner_ms>0)snprintf(text,sizeof(text),"WAVE %02u  /  %s",g->wave,rt_sector(g->wave));
    else snprintf(text,sizeof(text),"COMBO x%d",g->combo);
    label(a->notice,text);
    label(lv_obj_get_child(a->button[RTB_SOUND],0),a->sound?"SOUND ON":"SOUND OFF");
    for(unsigned i=0;i<3;i++) {
        if(i==a->selection)lv_obj_add_state(a->button[RTB_PILOT+i],LV_STATE_CHECKED);
        else lv_obj_remove_state(a->button[RTB_PILOT+i],LV_STATE_CHECKED);
    }
    if(a->help) {
        label(a->kicker,"KESTREL / FLIGHT MANUAL");label(a->title,"OWN THE HORIZON.");
        label(a->copy,
            "W / S or Up / Down: climb and dive.\n"
            "A / D: move forward and back.\n"
            "Left / Right: tilt the nose to aim.\n"
            "Cannon fires automatically. Hold Space for rockets.\n\n"
            "Touch FLY + AIM: drag vertically to fly and horizontally to aim. "
            "Holding the pad also launches rockets.\n\n"
            "Collect gold salvage to buy upgrades after each wave. "
            "The shield starts recharging 2.5 seconds after damage.\n\n"
            "P / Esc pauses. Leaving ends this run. High scores stay.");
        label(lv_obj_get_child(a->button[RTB_RESUME],0),"BACK  >");
    } else if(hangar) {
        label(a->kicker,"FORWARD BASE / FIELD WORKSHOP");
        label(a->title,lv_obj_get_width(a->root)>lv_obj_get_height(a->root)?"REARM.\nREBUILD.":"REARM. REBUILD.");
        snprintf(text,sizeof(text),"WAVE %02u CLEAR   /   %u SALVAGE",g->wave,g->credits);label(a->copy,text);
        static const char *const names[]={"SHIELD / +30 CAPACITY","CANNON / MORE FIREPOWER","ROCKETS / +1 CAPACITY","REPAIR / RESTORE HULL"};
        for(unsigned i=0;i<4;i++) {
            unsigned price=rt_price(g,(enum rt_upgrade)i);
            if(price)snprintf(text,sizeof(text),"%s\n%s%u SALVAGE",names[i],g->credits<price?"NEED ":"BUY / ",price);
            else snprintf(text,sizeof(text),"%s\n%s",names[i],i==3?"HULL AT 100%":"FULLY UPGRADED");
            label(lv_obj_get_child(a->button[RTB_SHIELD+i],0),text);
            if(!price || g->credits<price)lv_obj_add_state(a->button[RTB_SHIELD+i],LV_STATE_DISABLED);
            else lv_obj_remove_state(a->button[RTB_SHIELD+i],LV_STATE_DISABLED);
        }
        label(a->footer,"Upgrades last for this run. Next boss: every third wave.");
    } else if(title) {
        label(a->kicker,"POCKET GAMES  /  DOORS ORIGINAL");label(a->title,"ROTOR");label(a->title2,"TERROR");
        label(a->copy,"An endless horizon.\nOne very loud helicopter.");
        snprintf(text,sizeof(text),"BEST / %07u    %s\n%s",a->best[a->selection],rt_difficulty(a->selection),
            a->selection==0?"A forgiving first flight.":a->selection==1?"Faster enemies. Sharper reflexes.":"Maximum pressure. Triple score.");
        label(a->footer,text);
    } else if(g->phase==RT_OVER) {
        label(a->kicker,"KESTREL / FLIGHT RECORDER");label(a->title,"SIGNAL");label(a->title2,"LOST.");
        snprintf(text,sizeof(text),"SCORE %07u  /  WAVE %02u\n%u hostiles down. %s",g->score,g->wave,g->kills,g->score>=a->best[g->difficulty]?"A new horizon.":"Fly further next time.");label(a->copy,text);
        snprintf(text,sizeof(text),"%s  /  BEST %07u%s",rt_difficulty(g->difficulty),a->best[g->difficulty],a->storage_failed?" / NOT SAVED":"");label(a->footer,text);
    } else {
        label(a->kicker,"KESTREL / HOLDING PATTERN");label(a->title,"FLIGHT");label(a->title2,"PAUSED.");
        label(a->copy,"Take a breath. The horizon can wait.\nResume when you are ready.");
        snprintf(text,sizeof(text),"WAVE %02u  /  %07u POINTS  /  %u SALVAGE",g->wave,g->score,g->credits);label(a->footer,text);
        label(lv_obj_get_child(a->button[RTB_RESUME],0),"RESUME  >");
    }
    layout(a);
}

static void draw(struct rotor_app *a)
{
    rt_render(&a->game,&a->frame,a->game.phase==RT_TITLE);
    lv_image_cache_drop(&a->image_data);
    lv_obj_invalidate(a->image);
}

static void action(struct rotor_app *a,unsigned id)
{
    if(id==RTB_START || id==RTB_RETRY) {
        pause_run(a);
        a->recorded=0;
        rt_start(&a->game,a->selection,lv_tick_get()^0x6b657374u);
        a->game.reduced_motion=pocketos_shell_reduced_motion()!=0;
        play(a);
    } else if(id>=RTB_PILOT && id<=RTB_TERROR)a->selection=id-RTB_PILOT;
    else if(id==RTB_HELP) { pause_run(a);a->help=true;lv_obj_scroll_to_y(a->copybox,0,LV_ANIM_OFF); }
    else if(id==RTB_RESUME) {
        if(a->help)a->help=false;
        else play(a);
    } else if(id==RTB_PAUSE)pause_run(a);
    else if(id==RTB_SOUND) {
        a->sound=!a->sound;
        if(!a->sound)rt_audio_suspend(&a->audio);
    } else if(id==RTB_NEXT) { rt_next_wave(&a->game);play(a); }
    else if(id>=RTB_SHIELD && id<=RTB_REPAIR)rt_buy(&a->game,(enum rt_upgrade)(id-RTB_SHIELD));
    else if(id==RTB_MENU) { save_record(a);pause_run(a);a->game.phase=RT_TITLE;a->help=false; }
    else if(id==RTB_EXIT) { save_record(a);pause_run(a);pocketos_shell_go_home(); }
    refresh(a);draw(a);
}

static float axis(float value)
{
    if(fabsf(value)<.13f)return 0;
    return fmaxf(-1,fminf(1,value));
}

static void touch(lv_event_t *e)
{
    struct rotor_app *a=lv_event_get_user_data(e);
    unsigned id=(unsigned)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    lv_event_code_t code=lv_event_get_code(e);
    if((id==RTB_PAD || id==RTB_ROCKET) && flying(a)) {
        if(code==LV_EVENT_PRESSED) { a->pad=id==RTB_PAD;a->rocket=true; }
        if((code==LV_EVENT_PRESSED || code==LV_EVENT_PRESSING) && id==RTB_PAD && a->pad) {
            lv_indev_t *indev=lv_indev_active();
            if(indev) {
                lv_point_t p;lv_indev_get_point(indev,&p);
                lv_area_t r;lv_obj_get_coords(a->button[id],&r);
                a->touch.lift=axis((p.y-(r.y1+r.y2)*.5f)/((r.y2-r.y1)*.4f));
                a->touch.aim=axis((p.x-(r.x1+r.x2)*.5f)/((r.x2-r.x1)*.4f));
            }
        }
        if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST) {
            a->pad=a->rocket=false;memset(&a->touch,0,sizeof(a->touch));
        }
    } else if(code==LV_EVENT_CLICKED)action(a,id);
}

static void key(lv_event_t *e)
{
    struct rotor_app *a=lv_event_get_user_data(e);
    uint32_t k=lv_event_get_key(e);pos_key_t raw;unsigned mods;
    if(pos_input_raw_decode(k,&raw,&mods))k=raw;
    if(k==LV_KEY_ENTER) {
        if(a->help)action(a,RTB_RESUME);
        else if(a->game.phase==RT_TITLE)action(a,RTB_START);
        else if(a->game.phase==RT_HANGAR)action(a,RTB_NEXT);
        else if(a->game.phase==RT_OVER)action(a,RTB_RETRY);
        else if(a->game.paused)action(a,RTB_RESUME);
    } else if(k=='h' || k=='H')action(a,a->help?RTB_RESUME:RTB_HELP);
    else if(a->game.phase==RT_TITLE && k>='1' && k<='3')action(a,RTB_PILOT+k-'1');
    else if(a->game.phase==RT_HANGAR && k>='1' && k<='4')action(a,RTB_SHIELD+k-'1');
}

static void lost(lv_event_t *e)
{
    struct rotor_app *a=lv_event_get_user_data(e);
    pause_run(a);refresh(a);
}

static void resized(lv_event_t *e)
{
    struct rotor_app *a=lv_event_get_user_data(e);
    layout(a);draw(a);
}

static void frame(lv_timer_t *timer)
{
    struct rotor_app *a=lv_timer_get_user_data(timer);
    uint32_t now=lv_tick_get(),dt=now-a->last;a->last=now;
    unsigned held,pressed;bool input_lost;
    pos_input_game_take(&held,&pressed,&input_lost);
    bool available=pos_input_focused()==a->root && !pos_input_group_redirected() &&
                   !shell_power_screen_off() && !shell_lock_is_locked();
    if(flying(a) && (!available || input_lost || dt>250))pause_run(a);
    if(pressed&POS_GAME_PAUSE) {
        if(a->help)action(a,RTB_RESUME);
        else if(a->game.phase==RT_FLIGHT)action(a,a->game.paused?RTB_RESUME:RTB_PAUSE);
    }
    if(a->game.phase==RT_TITLE && !a->help) {
        if(pressed&POS_GAME_TURN_LEFT)a->selection=(a->selection+2)%3;
        if(pressed&POS_GAME_TURN_RIGHT)a->selection=(a->selection+1)%3;
        if(!a->game.reduced_motion)a->game.steps++;
    }
    if(flying(a)) {
        struct rt_input in=a->touch;
        in.lift+=(held&POS_GAME_BACK?1:0)-(held&POS_GAME_FORWARD?1:0);
        in.move+=(held&POS_GAME_STRAFE_RIGHT?1:0)-(held&POS_GAME_STRAFE_LEFT?1:0);
        in.aim+=(held&POS_GAME_TURN_RIGHT?1:0)-(held&POS_GAME_TURN_LEFT?1:0);
        in.rocket=a->rocket || (held&POS_GAME_FIRE)!=0;
        rt_advance(&a->game,in,dt);
    }
    bool playing=flying(a);
    int volume=pocketos_shell_volume_effective();
    if(a->was_playing && !playing)rt_audio_suspend(&a->audio);
    if(playing && a->sound && (volume!=a->volume || !a->was_playing)) {
        rt_audio_suspend(&a->audio);a->volume=volume;
        a->audio_pending=true;
    }
    rt_audio_poll(&a->audio,a->game.sounds);
    if(playing && a->sound && a->audio_pending && a->audio.pid<=0 && a->audio.recovery<=0) {
        rt_audio_start(&a->audio,volume);a->audio_pending=false;
    }
    a->was_playing=playing;
    if(a->game.phase==RT_OVER)save_record(a);
    refresh(a);
    if(playing || (a->game.phase==RT_TITLE && !a->game.reduced_motion))draw(a);
}

static lv_obj_t *text_object(lv_obj_t *parent,enum pos_style_role font,enum pos_style_role ink)
{
    lv_obj_t *obj=pocketui_label(parent,"",font);
    pos_style_add(obj,ink,0);
    lv_label_set_long_mode(obj,LV_LABEL_LONG_WRAP);
    return obj;
}

static void *create(lv_obj_t *body)
{
    struct rotor_app *a=lv_malloc_zeroed(sizeof(*a));
    if(!a)return NULL;
    rt_init(&a->game,0x524f544fu);
    a->game.reduced_motion=pocketos_shell_reduced_motion()!=0;
    a->sound=true;rt_audio_init(&a->audio);
    a->storage_failed=rt_scores_load(a->best)!=0;
    pocketos_shell_set_fullscreen(true);
    a->root=lv_obj_create(body);lv_obj_remove_style_all(a->root);
    lv_obj_set_width(a->root,LV_PCT(100));lv_obj_set_flex_grow(a->root,1);
    lv_obj_remove_flag(a->root,LV_OBJ_FLAG_SCROLLABLE);
    pos_style_add(a->root,POS_STYLE_ROTOR_BACKDROP,0);
    a->image_data.header.magic=LV_IMAGE_HEADER_MAGIC;
    a->image_data.header.cf=LV_COLOR_FORMAT_RGB565;
    a->image_data.header.w=RT_W;a->image_data.header.h=a->game.height;
    a->image_data.header.stride=RT_W*2;
    a->image_data.data_size=RT_W*a->game.height*2;
    a->image_data.data=(const uint8_t *)a->frame.pixels;
    a->image=lv_image_create(a->root);lv_image_set_src(a->image,&a->image_data);
    lv_image_set_antialias(a->image,false);
    a->shade=lv_obj_create(a->root);lv_obj_remove_style_all(a->shade);
    lv_obj_remove_flag(a->shade,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    pos_style_add(a->shade,POS_STYLE_ROTOR_PANEL,0);
    a->kicker=text_object(a->root,POS_STYLE_CAPTION,POS_STYLE_ROTOR_ACCENT);
    a->title=text_object(a->root,POS_STYLE_HERO_48,POS_STYLE_ROTOR_TEXT);
    a->title2=text_object(a->root,POS_STYLE_HERO_48,POS_STYLE_ROTOR_CORAL);
    a->copybox=lv_obj_create(a->root);lv_obj_remove_style_all(a->copybox);
    lv_obj_set_scroll_dir(a->copybox,LV_DIR_VER);
    a->copy=text_object(a->copybox,POS_STYLE_TEXT_PRIMARY,POS_STYLE_ROTOR_TEXT);
    lv_obj_set_width(a->copy,LV_PCT(100));
    a->footer=text_object(a->root,POS_STYLE_CAPTION,POS_STYLE_ROTOR_MUTED);
    a->score=text_object(a->root,POS_STYLE_VALUE,POS_STYLE_ROTOR_TEXT);
    a->systems=text_object(a->root,POS_STYLE_CAPTION,POS_STYLE_ROTOR_ACCENT);
    a->sector=text_object(a->root,POS_STYLE_CAPTION,POS_STYLE_ROTOR_TEXT);
    a->ordnance=text_object(a->root,POS_STYLE_CAPTION,POS_STYLE_ROTOR_MUTED);
    a->notice=text_object(a->root,POS_STYLE_TITLE,POS_STYLE_ROTOR_TEXT);
    lv_obj_set_style_text_align(a->notice,LV_TEXT_ALIGN_CENTER,0);
    for(unsigned i=0;i<RTB_COUNT;i++) {
        lv_obj_t *b=a->button[i]=lv_button_create(a->root);
        lv_obj_remove_style_all(b);
        lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_flag(b,LV_OBJ_FLAG_PRESS_LOCK);
        bool primary=i==RTB_START || i==RTB_RESUME || i==RTB_RETRY || i==RTB_NEXT;
        pos_style_add(b,primary?POS_STYLE_ROTOR_BUTTON:POS_STYLE_ROTOR_SECONDARY,0);
        pos_style_add(b,POS_STYLE_ROTOR_PRESSED,LV_STATE_PRESSED);
        pos_style_add(b,POS_STYLE_ROTOR_SELECTED,LV_STATE_CHECKED);
        lv_obj_t *t=pocketui_label(b,button_text[i],i>=RTB_SHIELD && i<=RTB_REPAIR?POS_STYLE_TEXT_PRIMARY:POS_STYLE_BUTTON_LABEL);
        pos_style_add(t,primary?POS_STYLE_ROTOR_INK:POS_STYLE_ROTOR_TEXT,0);
        lv_obj_set_style_text_align(t,LV_TEXT_ALIGN_CENTER,0);
        lv_obj_set_width(t,LV_PCT(100));lv_label_set_long_mode(t,LV_LABEL_LONG_WRAP);lv_obj_center(t);
        lv_obj_set_style_opa(b,LV_OPA_50,LV_STATE_DISABLED);
        lv_obj_set_user_data(b,(void *)(uintptr_t)i);
        lv_obj_add_event_cb(b,touch,LV_EVENT_ALL,a);
    }
    lv_obj_add_event_cb(a->root,resized,LV_EVENT_SIZE_CHANGED,a);
    lv_obj_add_event_cb(a->root,key,LV_EVENT_KEY,a);
    lv_obj_add_event_cb(a->root,lost,LV_EVENT_DEFOCUSED,a);
    pos_input_add_obj(a->root);pos_input_focus(a->root);pos_input_set_raw_target(a->root);
    pos_input_game_subscribe(a->root);
    a->last=lv_tick_get();a->timer=lv_timer_create(frame,32,a);
    lv_obj_update_layout(a->root);refresh(a);draw(a);
    return a;
}

static int back(void *priv)
{
    struct rotor_app *a=priv;
    if(a->help) { a->help=false;refresh(a);return 1; }
    if(flying(a)) { pause_run(a);refresh(a);return 1; }
    return 0;
}

static void destroy(void *priv)
{
    struct rotor_app *a=priv;
    if(!a)return;
    lv_timer_delete(a->timer);
    save_record(a);
    rt_audio_destroy(&a->audio);
    pos_input_game_unsubscribe(a->root);pos_input_set_raw_target(NULL);
    lv_image_cache_drop(&a->image_data);lv_image_set_src(a->image,NULL);
    lv_obj_remove_event_cb(a->root,lost);lv_obj_remove_event_cb(a->root,key);lv_obj_remove_event_cb(a->root,resized);
    for(unsigned i=0;i<RTB_COUNT;i++)lv_obj_remove_event_cb(a->button[i],touch);
    lv_free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_rotorterror);
const struct pocketos_app app_rotorterror = {
    .id = "rotorterror",
    .name = "RotorTerror",
    .icon = LV_SYMBOL_PLAY,
    .icon_mask = &pos_app_icon_rotorterror,
    .create = create,
    .destroy = destroy,
    .back = back,
    .chrome = POCKETOS_CHROME_NONE,
};

struct rt_game *rotor_app_game(void *priv) { return priv?&((struct rotor_app *)priv)->game:NULL; }
lv_obj_t *rotor_app_button(void *priv,unsigned index) { return priv && index<RTB_COUNT?((struct rotor_app *)priv)->button[index]:NULL; }
void rotor_app_refresh(void *priv) { if(priv) { refresh(priv);draw(priv); } }
