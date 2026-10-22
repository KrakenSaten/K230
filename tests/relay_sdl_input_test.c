/* Real SDL source down/up watch and unchanged ordinary text source.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "platform.h"
#include "pos_input.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL line %d %s\n",__LINE__,#x);abort();}}while(0)
static void pump(void){for(int i=0;i<100;i+=5){SDL_Delay(5);lv_timer_handler();}}
static void key(Uint32 type,SDL_Scancode scan,SDL_Keycode sym)
{SDL_Event e;memset(&e,0,sizeof(e));e.type=type;e.key.keysym.scancode=scan;e.key.keysym.sym=sym;CHECK(SDL_PushEvent(&e)==1);}
int main(void)
{
    setenv("SDL_VIDEODRIVER","dummy",1);lv_init();struct pos_panel panel={.width=320,.height=240};struct pos_display_geometry geo;pos_display_geometry_init(&geo,&panel,POS_ROTATION_0);
    lv_display_t *d=pocketos_platform_init(&panel,&geo);CHECK(d);pos_input_init();CHECK(pos_input_add_source(pocketos_platform_keyboard()));
    lv_obj_t *owner=lv_obj_create(lv_screen_active());pos_input_add_obj(owner);pos_input_focus(owner);CHECK(pos_input_game_subscribe(owner));
    key(SDL_KEYDOWN,SDL_SCANCODE_W,SDLK_w);key(SDL_KEYDOWN,SDL_SCANCODE_RIGHT,SDLK_RIGHT);key(SDL_KEYDOWN,SDL_SCANCODE_SPACE,SDLK_SPACE);pump();
    unsigned held;bool lost;pos_input_game_take(&held,NULL,&lost);CHECK(!lost);CHECK((held&(POS_GAME_FORWARD|POS_GAME_TURN_RIGHT|POS_GAME_FIRE))==(POS_GAME_FORWARD|POS_GAME_TURN_RIGHT|POS_GAME_FIRE));
    key(SDL_KEYUP,SDL_SCANCODE_W,SDLK_w);key(SDL_KEYUP,SDL_SCANCODE_RIGHT,SDLK_RIGHT);key(SDL_KEYUP,SDL_SCANCODE_SPACE,SDLK_SPACE);pump();pos_input_game_take(&held,NULL,NULL);CHECK(!held);
    key(SDL_KEYDOWN,SDL_SCANCODE_W,SDLK_w);pump();SDL_Event event;memset(&event,0,sizeof(event));event.type=SDL_WINDOWEVENT;event.window.event=SDL_WINDOWEVENT_FOCUS_LOST;CHECK(SDL_PushEvent(&event)==1);pump();pos_input_game_take(&held,NULL,&lost);CHECK(!held&&lost);
    key(SDL_KEYUP,SDL_SCANCODE_W,SDLK_w);pump();pos_input_game_unsubscribe(owner);lv_obj_delete(owner);
    lv_obj_t *field=lv_textarea_create(lv_screen_active());lv_textarea_set_text(field,"");pos_input_add_obj(field);pos_input_focus(field);
    memset(&event,0,sizeof(event));event.type=SDL_TEXTINPUT;strcpy(event.text.text,"w");CHECK(SDL_PushEvent(&event)==1);pump();CHECK(!strcmp(lv_textarea_get_text(field),"w"));
    pos_input_deinit();lv_display_delete(d);lv_deinit();printf("relay_sdl_input_test: %d checks passed\n",checks);return 0;
}
