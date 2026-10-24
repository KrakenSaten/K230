/* Private UI test: production app, with parsed fixtures and no IPC transport.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "../apps/meshscope/scope_app.c"
#include "src/libs/lodepng/lodepng.h"
#include <assert.h>
static void flush(lv_display_t *d,const lv_area_t *area,uint8_t *px)
{ (void)area; (void)px; lv_display_flush_ready(d); }
static void shot(const char *name)
{
    const char *dir=getenv("SHOTS_DIR");
    if(!dir) return;
    lv_refr_now(NULL);
    lv_draw_buf_t *buf=lv_snapshot_take(lv_screen_active(),LV_COLOR_FORMAT_RGB888);
    assert(buf);
    size_t size=(size_t)buf->header.w*buf->header.h*3;
    unsigned char *rgb=malloc(size);
    assert(rgb);
    for(unsigned y=0;y<buf->header.h;++y) for(unsigned x=0;x<buf->header.w;++x) {
        const unsigned char *p=buf->data+y*buf->header.stride+x*3;
        size_t i=((size_t)y*buf->header.w+x)*3;
        rgb[i]=p[2]; rgb[i+1]=p[1]; rgb[i+2]=p[0];
    }
    char path[1024]; snprintf(path,sizeof(path),"%s/%s.png",dir,name);
    assert(lodepng_encode24_file(path,rgb,buf->header.w,buf->header.h)==0);
    free(rgb); lv_draw_buf_destroy(buf);
}
static void click(lv_obj_t *b) { lv_obj_send_event(b,LV_EVENT_CLICKED,NULL); lv_obj_update_layout(lv_screen_active()); }
static void inside(lv_obj_t *inner,lv_obj_t *outer)
{
    lv_area_t a,b; lv_obj_get_coords(inner,&a); lv_obj_get_content_coords(outer,&b);
    assert(a.x1>=b.x1 && a.x2<=b.x2 && a.y1>=b.y1 && a.y2<=b.y2);
}
int main(void)
{
    lv_init();
    lv_display_t *d=lv_display_create(568,1232);
    static unsigned char pixels[1232*64*4];
    lv_display_set_buffers(d,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    pos_input_init(); pocketui_init();
    pos_theme_select_text_size(POS_TEXT_SIZE_LARGE);
    for(int rotated=0;rotated<2;++rotated) {
        int w=rotated?1232:568,h=rotated?568:1232;
        lv_display_set_resolution(d,w,h);
        lv_obj_t *screen=lv_screen_active(); pocketui_style_screen(screen);
        lv_obj_t *root=lv_obj_create(screen); lv_obj_remove_style_all(root);
        lv_obj_set_pos(root,0,72); lv_obj_set_size(root,w,h-72);
        struct scope_app *a=create(root); assert(a); lv_timer_pause(a->timer);
        a->ipc.next_attempt_ms=INT64_MAX;
        const char *nodes="{\"nodes\":[{\"public_key\":\"a100000000000000000000000000000000000000000000000000000000000000\",\"name\":\"OSLO\",\"node_hash\":\"a1\",\"lat\":59.91,\"lon\":10.75},{\"public_key\":\"a200000000000000000000000000000000000000000000000000000000000000\",\"name\":\"NORD\",\"node_hash\":\"a2\",\"lat\":59.96,\"lon\":10.79},{\"public_key\":\"a300000000000000000000000000000000000000000000000000000000000000\",\"name\":\"VEST\",\"node_hash\":\"a3\",\"lat\":59.92,\"lon\":10.68}]}";
        cJSON *json=cJSON_Parse(nodes); assert(rift_model_apply_nodes(&a->model,json)==0); cJSON_Delete(json);
        for(int i=0;i<20;++i) {
            char event[600];
            snprintf(event,sizeof(event),"{\"v\":1,\"seq\":%d,\"mono_ms\":%lld,\"bytes\":42,\"verdict\":\"new\",\"type_code\":5,\"route\":\"flood\",\"path_kind\":\"hops\",\"path_hash_size\":1,\"path_hops\":3,\"path_hex\":\"a1a2a3\",\"hash\":\"1234567890abcdef\",\"dup\":%d,\"rssi_dbm\":-82,\"snr_db\":7.5}",i+1,(long long)rift_mono_ms(),i+1);
            json=cJSON_Parse(event); assert(rift_rxlog_apply(&a->log,json,rift_mono_ms(),0)==0); cJSON_Delete(json);
        }
        refresh(a); lv_obj_update_layout(screen); refresh(a);
        assert(a->model.node_count==3 && a->log.count==20);
        assert(lv_obj_get_height(a->map)>100);
        for(int i=0;i<3;++i) inside(a->tabs[i],root);
        inside(a->map,root); inside(a->note,root);
        shot(rotated?"landscape-map":"portrait-map");
        click(a->tabs[1]); assert(a->tab==1 && !a->detail_open);
        inside(a->pager,root);
        shot(rotated?"landscape-packets":"portrait-packets");
        click(a->rows[0].button); assert(a->detail_open && a->packet_selected);
        uint64_t seq=a->packet.seq;
        rift_rxlog_clear(&a->log); refresh(a); assert(a->packet.seq==seq);
        shot(rotated?"landscape-packet-detail":"portrait-packet-detail");
        assert(back(a)==1 && !a->detail_open);
        click(a->tabs[2]); click(a->rows[0].button);
        assert(a->detail_open && !strcmp(a->selected,a->model.nodes[0].key));
        shot(rotated?"landscape-node":"portrait-node");
        assert(back(a)==1); assert(back(a)==1 && a->tab==0);
        char why[80]; pos_theme_apply("brass","outdoor",why,sizeof(why));
        refresh(a); lv_obj_update_layout(screen); inside(a->note,root);
        shot(rotated?"landscape-outdoor":"portrait-outdoor");
        destroy(a); lv_obj_delete(root);
    }
    pos_input_deinit(); lv_deinit();
    puts("meshscope_ui_test: both orientations at Large, controls, details, selection survival and theme passed");
    return 0;
}
