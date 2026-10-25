/* MeshScope: a local, read-only mesh observer. No CoreScope upstream code.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "app.h"
#include "pocketui.h"
#include "pos_input.h"
#include "rift_ipc.h"
#include "rift_basemap.h"
#include "scope_model.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS 6
#define LIVE_MS 8000
struct scope_app;
struct scope_row {
    struct scope_app *app;
    lv_obj_t *button, *label;
    uint32_t uid;
    char key[RIFT_KEY_HEX];
};
struct scope_app {
    struct rift_model model;
    struct rift_rxlog log;
    struct rift_ipc ipc;
    struct rift_map_view view;
    struct rift_basemap *basemap;
    lv_obj_t *root, *status, *body, *map, *note, *attribution;
    lv_obj_t *list, *pager, *page_label, *detail, *back, *map_tools, *bm_button;
    lv_obj_t *tabs[3];
    lv_obj_t *workspace, *rail, *path_title, *signal, *fit_button, *live_button;
    lv_obj_t *detail_actions, *path_button;
    int landscape;
    struct scope_row rows[ROWS];
    lv_timer_t *timer;
    int tab, page, detail_open, fit, drag;
    int64_t repaint_at;
    unsigned revision, model_seq;
    uint32_t anchor;
    char selected[RIFT_KEY_HEX];
    struct rift_rx_entry packet;
    int packet_selected;
    lv_point_t press, last;
};
static void refresh(struct scope_app *a);
static void show(struct scope_app *a);

static lv_obj_t *label(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *o = pocketui_label(parent, text, role);
    lv_obj_set_width(o, LV_PCT(100));
    lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
    return o;
}
static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_style_pad_gap(o, 8, 0);
    return o;
}
static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *data)
{
    lv_obj_t *o = pocketui_button(parent, text, cb, data);
    int width = pocketui_role_text_width(POS_STYLE_BUTTON_LABEL, text) + 32;
    lv_obj_set_width(o, width < 56 ? 56 : width);
    lv_obj_set_height(o, 56);
    pos_style_add(o, POS_STYLE_BUTTON_SECONDARY, 0);
    pos_style_add(o, POS_STYLE_SELECTED, LV_STATE_CHECKED);
    pos_style_add(lv_obj_get_child(o, 0), POS_STYLE_TEXT_PRIMARY, 0);
    pos_input_add_obj(o);
    return o;
}
static void visible(lv_obj_t *o, int yes)
{
    if (yes) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static const struct rift_rx_entry *map_packet(struct scope_app *a)
{
    if (a->packet_selected) return &a->packet;
    int64_t now = rift_mono_ms();
    for (int i = 0; i < a->log.count; ++i) {
        const struct rift_rx_entry *e = rift_rxlog_at(&a->log, i);
        int path[RIFT_RXLOG_PATH_MAX];
        if (e->kind == RIFT_RX_MARK) break; /* never bridge a service gap */
        if (now >= e->mono_ms && now - e->mono_ms < LIVE_MS &&
            scope_path(&a->model, e, path)) return e;
    }
    return NULL;
}
static void dot(lv_layer_t *layer, int x, int y, int r, lv_color_t color)
{
    lv_draw_rect_dsc_t d;
    lv_area_t area = {x-r, y-r, x+r, y+r};
    lv_draw_rect_dsc_init(&d);
    d.bg_color = color; d.bg_opa = LV_OPA_COVER; d.radius = LV_RADIUS_CIRCLE;
    d.border_color = pos_theme_color(POS_COLOR_SURFACE); d.border_width = 2;
    lv_draw_rect(layer, &d, &area);
}
static void map_line(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                     enum pos_color_token role, int width)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color=pos_theme_color(role); d.width=width; d.opa=LV_OPA_COVER;
    d.p1.x=x1; d.p1.y=y1; d.p2.x=x2; d.p2.y=y2;
    lv_draw_line(layer,&d);
}
static void map_grid(struct scope_app *a, lv_layer_t *layer, const lv_area_t *area)
{
    double step=rift_map_grid_step(&a->view),top,bottom,left,right,lat,lon;
    if(step<=0) return;
    rift_map_centre(&a->view,&lat,&lon);
    rift_map_unproject(&a->view,0,0,&top,&left);
    rift_map_unproject(&a->view,a->view.w,a->view.h,&bottom,&right);
    double g=ceil(bottom/step)*step;
    for(int n=0;n<32 && g<=top;++n,g+=step) {
        double x,y; rift_map_project(&a->view,g,lon,&x,&y);
        if(y>=0 && y<a->view.h) map_line(layer,area->x1,area->y1+y,area->x2,area->y1+y,POS_COLOR_LINE,1);
    }
    g=ceil(left/step)*step;
    for(int n=0;n<32 && g<=right;++n,g+=step) {
        double x,y; rift_map_project(&a->view,lat,g,&x,&y);
        if(x>=0 && x<a->view.w) map_line(layer,area->x1+x,area->y1,area->x1+x,area->y2,POS_COLOR_LINE,1);
    }
}
static void map_scale(struct scope_app *a, lv_layer_t *layer, const lv_area_t *area)
{
    int32_t px=0;
    double metres=rift_map_scale(&a->view,120,&px);
    if(px<=0) return;
    int x=area->x1+16,y=area->y2-16;
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color=pos_theme_color(POS_COLOR_SURFACE); bg.bg_opa=LV_OPA_COVER; bg.radius=6;
    int height=lv_font_get_line_height(lv_obj_get_style_text_font(a->map,0));
    lv_area_t panel={x-8,y-height-20,x+140,y+8};
    lv_draw_rect(layer,&bg,&panel);
    map_line(layer,x,y,x+px,y,POS_COLOR_TEXT_SECONDARY,2);
    map_line(layer,x,y-5,x,y+1,POS_COLOR_TEXT_SECONDARY,2);
    map_line(layer,x+px,y-5,x+px,y+1,POS_COLOR_TEXT_SECONDARY,2);
    char text[32];
    snprintf(text,sizeof(text),metres>=1000?"%g km":"%g m",metres>=1000?metres/1000:metres);
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font=lv_obj_get_style_text_font(a->map,0); d.color=pos_theme_color(POS_COLOR_TEXT_SECONDARY); d.text=text;
    lv_area_t label_area={x,y-height-12,x+140,y-8};
    lv_draw_label(layer,&d,&label_area);
}
static void draw(lv_event_t *event)
{
    struct scope_app *a = lv_event_get_user_data(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_content_coords(a->map, &area);
    if(a->view.w<=0 || a->view.h<=0) return;
    rift_basemap_draw(a->basemap, layer, &area, &a->view);
    if(!rift_basemap_on(a->basemap)) map_grid(a,layer,&area);
    const struct rift_rx_entry *e = map_packet(a);
    int path[RIFT_RXLOG_PATH_MAX];
    int n = scope_path(&a->model, e, path);
    /* Only adjacent, uniquely resolved, located hops are connected. A gap
     * is never skipped over, and a one-byte hash is never treated as a key. */
    for (int i = 1; i < n; ++i) {
        if (path[i-1] < 0 || path[i] < 0) continue;
        const struct rift_node *p = &a->model.nodes[path[i-1]], *q = &a->model.nodes[path[i]];
        if (!p->have_location || !q->have_location) continue;
        double x1,y1,x2,y2;
        rift_map_project(&a->view,p->lat,p->lon,&x1,&y1);
        rift_map_project(&a->view,q->lat,q->lon,&x2,&y2);
        /* Avoid giant offscreen coordinates overflowing LVGL's rasteriser. */
        if (fabs(x1)>30000 || fabs(y1)>30000 || fabs(x2)>30000 || fabs(y2)>30000) continue;
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color=pos_theme_color(POS_COLOR_RADIO_RX); d.width=3; d.opa=LV_OPA_COVER;
        d.p1.x=area.x1+x1; d.p1.y=area.y1+y1;
        d.p2.x=area.x1+x2; d.p2.y=area.y1+y2;
        lv_draw_line(layer,&d);
    }
    for (int i=0;i<a->model.node_count;++i) {
        const struct rift_node *node=&a->model.nodes[i];
        if (!node->have_location) continue;
        double x,y;
        rift_map_project(&a->view,node->lat,node->lon,&x,&y);
        if (x<0 || y<0 || x>=a->view.w || y>=a->view.h) continue;
        int active=0;
        for(int j=0;j<n;++j) if(path[j]==i) active=1;
        if(active) dot(layer,area.x1+x,area.y1+y,14,pos_theme_color(POS_COLOR_SURFACE_RAISED));
        dot(layer,area.x1+x,area.y1+y,active?9:6,
            pos_theme_color(active?POS_COLOR_RADIO_RX:POS_COLOR_ACCENT_PRIMARY));
        if (a->model.node_count<=24 || !strcmp(a->selected,node->key)) {
            lv_draw_label_dsc_t d;
            lv_draw_label_dsc_init(&d);
            d.font=lv_obj_get_style_text_font(a->map,0);
            d.color=pos_theme_color(POS_COLOR_TEXT_PRIMARY);
            d.text=node->have_name?node->name:node->hash;
            lv_area_t text={area.x1+x+12,area.y1+y-12,area.x2,area.y1+y+50};
            if(text.x1<text.x2) lv_draw_label(layer,&d,&text);
        }
    }
    map_scale(a,layer,&area);
}
static void map_input(lv_event_t *e)
{
    struct scope_app *a=lv_event_get_user_data(e);
    lv_indev_t *input=lv_indev_active();
    if (!input) return;
    lv_point_t p;
    lv_indev_get_point(input,&p);
    if(lv_event_get_code(e)==LV_EVENT_PRESSED) { a->press=a->last=p; a->drag=0; }
    else if(lv_event_get_code(e)==LV_EVENT_PRESSING) {
        if(abs(p.x-a->press.x)>10 || abs(p.y-a->press.y)>10) a->drag=1;
        if(a->drag) { rift_map_pan(&a->view,p.x-a->last.x,p.y-a->last.y); a->fit=0; lv_obj_remove_state(a->fit_button,LV_STATE_CHECKED); lv_obj_invalidate(a->map); }
        a->last=p;
    } else if(lv_event_get_code(e)==LV_EVENT_RELEASED && !a->drag) {
        lv_area_t area; lv_obj_get_content_coords(a->map,&area);
        int i=rift_map_hit(&a->view,&a->model,p.x-area.x1,p.y-area.y1);
        if(i>=0) {
            snprintf(a->selected,sizeof(a->selected),"%s",a->model.nodes[i].key);
            a->tab=2; a->detail_open=1; show(a);
        }
    }
}
static void tab_click(lv_event_t *e)
{
    struct scope_app *a=lv_event_get_user_data(e);
    for(int i=0;i<3;++i) if(lv_event_get_target(e)==a->tabs[i]) a->tab=i;
    a->page=0; a->anchor=0; a->detail_open=0; show(a);
}
static void row_click(lv_event_t *e)
{
    struct scope_row *r=lv_event_get_user_data(e);
    struct scope_app *a=r->app;
    if(a->tab==1) {
        const struct rift_rx_entry *p=rift_rxlog_find(&a->log,r->uid);
        if(!p) return;
        a->packet=*p; a->packet_selected=1;
    } else snprintf(a->selected,sizeof(a->selected),"%s",r->key);
    a->detail_open=1; show(a);
}
static void older(lv_event_t *e)
{
    struct scope_app *a=lv_event_get_user_data(e);
    if(a->tab==1 && !a->anchor) {
        const struct rift_rx_entry *p=rift_rxlog_at(&a->log,0);
        if(p) a->anchor=p->uid;
    }
    int count=a->tab==1?a->log.count:a->model.node_count;
    if((a->page+1)*ROWS<count) a->page++;
    refresh(a);
}
static void newer(lv_event_t *e)
{
    struct scope_app *a=lv_event_get_user_data(e);
    if(a->page>0) a->page--;
    if(!a->page) a->anchor=0;
    refresh(a);
}
static void live(lv_event_t *e)
{
    struct scope_app *a=lv_event_get_user_data(e);
    a->packet_selected=0; a->page=0; a->anchor=0; a->detail_open=0; show(a);
}
static int back(void *priv)
{
    struct scope_app *a=priv;
    if(a->detail_open) { a->detail_open=0; show(a); return 1; }
    if(a->tab) { a->tab=0; show(a); return 1; }
    return 0;
}
static void back_click(lv_event_t *e) { (void)back(lv_event_get_user_data(e)); }
static void zoom_in(lv_event_t *e) { struct scope_app *a=lv_event_get_user_data(e); a->fit=0; rift_map_zoom(&a->view,1); refresh(a); }
static void zoom_out(lv_event_t *e) { struct scope_app *a=lv_event_get_user_data(e); a->fit=0; rift_map_zoom(&a->view,-1); refresh(a); }
static void fit(lv_event_t *e) { struct scope_app *a=lv_event_get_user_data(e); a->fit=1; a->view.fitted=0; refresh(a); }
static void basemap(lv_event_t *e)
{
    struct scope_app *a=lv_event_get_user_data(e);
    rift_basemap_set(a->basemap,!rift_basemap_on(a->basemap)); refresh(a);
}
static void view_path(lv_event_t *e)
{
    struct scope_app *a=lv_event_get_user_data(e);
    a->tab=0; a->detail_open=0; show(a);
}
/* Text changes never participate in sizing the map. The rail scrolls if its
 * content exceeds the reserved space, including at Large text size. */
static void layout_map(struct scope_app *a)
{
    int landscape=lv_obj_get_width(a->root)>lv_obj_get_height(a->root);
    if(a->landscape==landscape) return;
    a->landscape=landscape;
    lv_obj_set_height(a->status,landscape?28:52);
    lv_obj_set_flex_flow(a->workspace,landscape?LV_FLEX_FLOW_ROW:LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(a->map,landscape?0:LV_PCT(100),landscape?LV_PCT(100):0);
    lv_obj_set_flex_grow(a->map,1);
    lv_obj_set_size(a->rail,landscape?340:LV_PCT(100),landscape?LV_PCT(100):280);
}
static void refresh_detail(struct scope_app *a)
{
    char text[2400];
    if(a->tab==1) {
        rift_rxlog_fmt_detail(&a->packet,text,sizeof(text));
    } else {
        const struct rift_node *n=rift_model_find(&a->model,a->selected);
        if(!n) { lv_label_set_text(a->detail,"Node no longer in the current snapshot."); return; }
        char age[80]="Last heard: unknown", location[96]="Location: not advertised";
        char rssi[48]="RSSI: unknown",snr[48]="SNR: unknown";
        if(n->have_heard) snprintf(age,sizeof(age),"Last heard: %.0f s ago",fmax(0,(rift_mono_ms()-n->heard_mono_ms)/1000.0));
        if(n->have_location) snprintf(location,sizeof(location),"Advertised position: %.5f, %.5f",n->lat,n->lon);
        if(n->have_rssi) snprintf(rssi,sizeof(rssi),"RSSI: %.1f dBm",n->rssi_dbm);
        if(n->have_snr) snprintf(snr,sizeof(snr),"SNR: %.1f dB",n->snr_db);
        snprintf(text,sizeof(text),"%s\n\n%s\n%s\n%s   %s\n%s\n\nObserved flood paths containing this node: %u\n(window of %d log entries)\n\nPrefix matching is provisional; collisions stay unresolved. Signal readings are this receiver's, not measurements of every link.\n\nPublic key:\n%s",
                 n->have_name?n->name:n->hash,a->ipc.fd<0?"Cached data - disconnected":"Current node snapshot",age,rssi,snr,location,
                 scope_node_packets(&a->model,&a->log,(int)(n-a->model.nodes)),a->log.count,n->key);
    }
    lv_label_set_text(a->detail,text);
}
static void refresh(struct scope_app *a)
{
    struct scope_stats st;
    scope_stats(&a->model,&a->log,rift_mono_ms(),&st);
    layout_map(a);
    lv_label_set_text_fmt(a->status,a->landscape?"%s  |  %u RX  |  %u repeats  |  %d nodes":"%s\n%u RX  |  %u repeats  |  %d nodes",
        a->ipc.fd<0?"RECONNECTING":rift_model_state_label(&a->model),
        st.frames,st.duplicates,a->model.node_count);
    if(a->fit) lv_obj_add_state(a->fit_button,LV_STATE_CHECKED);
    else lv_obj_remove_state(a->fit_button,LV_STATE_CHECKED);
    if(!a->packet_selected) lv_obj_add_state(a->live_button,LV_STATE_CHECKED);
    else lv_obj_remove_state(a->live_button,LV_STATE_CHECKED);
    if(a->detail_open) { refresh_detail(a); return; }
    if(a->tab==0) {
        lv_obj_update_layout(a->map);
        int w=lv_obj_get_content_width(a->map), h=lv_obj_get_content_height(a->map);
        if(w>0 && h>0) {
            if(!a->view.fitted || (a->fit && a->model_seq!=a->model.seq)) rift_map_fit(&a->view,&a->model,w,h);
            else rift_map_resize(&a->view,w,h);
        }
        a->model_seq=a->model.seq;
        int hops[RIFT_RXLOG_PATH_MAX], unknown=0,ambiguous=0,unlocated=0;
        const struct rift_rx_entry *p=map_packet(a);
        int n=scope_path(&a->model,p,hops);
        for(int i=0;i<n;++i) {
            unknown+=hops[i]==SCOPE_UNKNOWN; ambiguous+=hops[i]==SCOPE_AMBIGUOUS;
            if(hops[i]>=0) unlocated+=!a->model.nodes[hops[i]].have_location;
        }
        lv_label_set_text(a->path_title,a->packet_selected?"SELECTED PATH":"LIVE PATH");
        if(!st.located) lv_label_set_text(a->note,"Waiting for advertised positions.\nPackets remain available in PACKETS.");
        else if(!p) lv_label_set_text_fmt(a->note,"%u nodes on the map\nWaiting for a flood path.\nDrag to pan; tap a node to inspect.",st.located);
        else if(!n) lv_label_set_text(a->note,"No travelled node path.\nDirect routes are remaining hops; TRACE contains signal samples.");
        else lv_label_set_text_fmt(a->note,"%d hops  /  %d resolved\n%d unknown  /  %d ambiguous\n%d without position\nPaths inferred from node prefixes.",n,n-unknown-ambiguous,unknown,ambiguous,unlocated);
        if(a->log.supported==0) lv_label_set_text(a->signal,"Receive log unavailable");
        else if(a->ipc.fd<0) lv_label_set_text(a->signal,"Cached data - reconnecting");
        else if(p) {
            char rssi[48],snr[48];
            rift_rxlog_fmt_rssi(p,rssi,sizeof(rssi)); rift_rxlog_fmt_snr(p,snr,sizeof(snr));
            lv_label_set_text_fmt(a->signal,"%s  |  %s",rssi,snr);
        } else lv_label_set_text(a->signal,"Listening at this receiver");
        char line[64], detail[96];
        rift_basemap_words(a->basemap,line,sizeof(line),detail,sizeof(detail));
        lv_label_set_text_fmt(a->attribution,"Map data (c) OpenStreetMap contributors\n%s",line);
        visible(a->attribution,rift_basemap_on(a->basemap));
        lv_label_set_text(lv_obj_get_child(a->bm_button,0),rift_basemap_on(a->basemap)?"MAP ON":"MAP OFF");
        lv_obj_invalidate(a->map);
        return;
    }
    int start=a->page*ROWS, count=a->tab==1?a->log.count:a->model.node_count;
    int offset=0;
    if(a->tab==1 && a->anchor) {
        offset=rift_rxlog_position(&a->log,a->log.filter,a->anchor);
        if(offset<0) { a->anchor=0; a->page=0; start=offset=0; }
    }
    count-=offset;
    if(start>=count) { a->page=count?(count-1)/ROWS:0; start=a->page*ROWS; }
    lv_label_set_text_fmt(a->page_label,"%d-%d / %d%s",count?start+1:0,
        start+ROWS<count?start+ROWS:count,count,a->anchor?" (held)":"");
    for(int i=0;i<ROWS;++i) {
        struct scope_row *r=&a->rows[i];
        int k=start+i+offset;
        visible(r->button,start+i<count);
        if(start+i>=count) continue;
        if(a->tab==1) {
            const struct rift_rx_entry *p=rift_rxlog_at(&a->log,k);
            char time[48],state[48],rssi[48],snr[48],hops[48]="Hops: unknown";
            r->uid=p->uid;
            rift_rxlog_fmt_time(p,time,sizeof(time)); rift_rxlog_state_word(p,state,sizeof(state));
            rift_rxlog_fmt_rssi(p,rssi,sizeof(rssi)); rift_rxlog_fmt_snr(p,snr,sizeof(snr));
            if(p->flags&RIFT_RXF_PARSED) snprintf(hops,sizeof(hops),"%s: %u",
                p->flags&RIFT_RXF_PATH_SNR?"TRACE samples":
                (p->route==RIFT_RXRT_DIRECT || p->route==RIFT_RXRT_TRANSPORT_DIRECT)?"Remaining hops":"Hops",p->path_hops);
            if(p->kind==RIFT_RX_MARK) lv_label_set_text_fmt(r->label,"%s  %s",time,p->text);
            else lv_label_set_text_fmt(r->label,"%s  %s  %s  %u B\n%s  %s  |  %s",time,rift_rxlog_type_word(p),state,p->bytes,rssi,snr,hops);
        } else {
            const struct rift_node *n=&a->model.nodes[k];
            snprintf(r->key,sizeof(r->key),"%s",n->key);
            lv_label_set_text_fmt(r->label,"%s  [%.6s]\n%s",n->have_name?n->name:"Unnamed node",n->key,
                n->have_location?"Position advertised - tap for details":"No advertised position - tap for details");
        }
    }
    if(!count) lv_label_set_text(a->page_label,a->tab==1?"Waiting for packets":"No nodes reported");
}
static void show(struct scope_app *a)
{
    int map=a->tab==0;
    visible(a->workspace,map);
    visible(a->list,!map&&!a->detail_open); visible(a->pager,!map&&!a->detail_open);
    visible(a->detail,a->detail_open); visible(a->detail_actions,a->detail_open);
    visible(a->path_button,a->detail_open && a->tab==1);
    if(a->detail_open) lv_obj_add_flag(a->body,LV_OBJ_FLAG_SCROLLABLE);
    else lv_obj_remove_flag(a->body,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_scroll_to_y(a->body,0,LV_ANIM_OFF);
    for(int i=0;i<3;++i) {
        if(i==a->tab) lv_obj_add_state(a->tabs[i],LV_STATE_CHECKED);
        else lv_obj_remove_state(a->tabs[i],LV_STATE_CHECKED);
    }
    refresh(a);
}
static void poll(lv_timer_t *timer)
{
    struct scope_app *a=lv_timer_get_user_data(timer);
    int64_t now=rift_mono_ms();
    rift_ipc_poll(&a->ipc,now);
    if(rift_basemap_pump(a->basemap,&a->view,a->tab==0,now)) lv_obj_invalidate(a->map);
    if(now>=a->repaint_at || a->revision!=a->log.revision) {
        a->repaint_at=now+1000; a->revision=a->log.revision; refresh(a);
    }
}
static void *create(lv_obj_t *root)
{
    struct scope_app *a=calloc(1,sizeof(*a));
    if(!a) return NULL;
    rift_model_init(&a->model); rift_rxlog_init(&a->log);
    rift_ipc_init(&a->ipc,&a->model,NULL); a->ipc.rxlog=&a->log;
    a->ipc.read_only=1;
    a->basemap=rift_basemap_new(); a->fit=1; a->root=root; a->landscape=-1;
    lv_obj_set_flex_flow(root,LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(root,12,0);
    lv_obj_set_style_pad_gap(root,8,0);
    lv_obj_t *tabs=box(root,LV_FLEX_FLOW_ROW);
    static const char *names[]={"MAP","PACKETS","NODES"};
    for(int i=0;i<3;++i) {
        a->tabs[i]=button(tabs,names[i],tab_click,a);
        lv_obj_set_width(a->tabs[i],0); lv_obj_set_flex_grow(a->tabs[i],1);
        pos_style_add(a->tabs[i],POS_STYLE_SELECTED,LV_STATE_CHECKED);
    }
    a->status=label(root,"Connecting",POS_STYLE_CAPTION);
    lv_obj_set_height(a->status,28);
    lv_label_set_long_mode(a->status,LV_LABEL_LONG_DOT);
    a->body=box(root,LV_FLEX_FLOW_COLUMN); lv_obj_set_height(a->body,0); lv_obj_set_flex_grow(a->body,1);
    a->workspace=box(a->body,LV_FLEX_FLOW_ROW);
    lv_obj_set_height(a->workspace,LV_PCT(100));
    lv_obj_remove_flag(a->workspace,LV_OBJ_FLAG_SCROLLABLE);
    a->map=lv_obj_create(a->workspace); pos_style_add(a->map,POS_STYLE_PANEL,0);
    lv_obj_set_size(a->map,LV_PCT(100),LV_PCT(100)); lv_obj_set_style_pad_all(a->map,0,0);
    lv_obj_remove_flag(a->map,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(a->map,draw,LV_EVENT_DRAW_MAIN_END,a);
    lv_obj_add_event_cb(a->map,map_input,LV_EVENT_PRESSED,a);
    lv_obj_add_event_cb(a->map,map_input,LV_EVENT_PRESSING,a);
    lv_obj_add_event_cb(a->map,map_input,LV_EVENT_RELEASED,a);
    pos_theme_watch(a->map);
    a->rail=box(a->workspace,LV_FLEX_FLOW_COLUMN);
    pos_style_add(a->rail,POS_STYLE_SLAB,0);
    lv_obj_set_style_pad_all(a->rail,12,0);
    lv_obj_add_flag(a->rail,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->rail,LV_DIR_VER);
    a->map_tools=box(a->rail,LV_FLEX_FLOW_ROW_WRAP);
    button(a->map_tools,"+",zoom_in,a); button(a->map_tools,"-",zoom_out,a);
    a->fit_button=button(a->map_tools,"FIT",fit,a);
    a->live_button=button(a->map_tools,"LIVE",live,a);
    a->bm_button=button(a->map_tools,"MAP OFF",basemap,a);
    a->path_title=label(a->rail,"LIVE PATH",POS_STYLE_ROW_TITLE);
    a->signal=label(a->rail,"Listening at this receiver",POS_STYLE_CAPTION);
    a->note=label(a->rail,"",POS_STYLE_CAPTION);
    a->attribution=label(a->map,"",POS_STYLE_CAPTION);
    pos_style_add(a->attribution,POS_STYLE_SLAB,0);
    lv_obj_set_width(a->attribution,LV_PCT(60));
    lv_obj_set_style_pad_all(a->attribution,8,0);
    lv_obj_align(a->attribution,LV_ALIGN_BOTTOM_RIGHT,-8,-8);
    lv_obj_remove_flag(a->attribution,LV_OBJ_FLAG_CLICKABLE);
    a->list=box(a->body,LV_FLEX_FLOW_COLUMN); lv_obj_set_height(a->list,LV_PCT(100));
    lv_obj_add_flag(a->list,LV_OBJ_FLAG_SCROLLABLE);
    for(int i=0;i<ROWS;++i) {
        a->rows[i].app=a;
        lv_obj_t *b=button(a->list,"",row_click,&a->rows[i]);
        pos_style_add(b,POS_STYLE_BUTTON_SECONDARY,0);
        pos_style_add(lv_obj_get_child(b,0),POS_STYLE_TEXT_PRIMARY,0);
        a->rows[i].button=b; a->rows[i].label=lv_obj_get_child(b,0);
        lv_obj_set_width(b,LV_PCT(100)); lv_obj_set_height(b,LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(b,72,0); lv_obj_set_style_pad_all(b,12,0);
        lv_obj_set_width(a->rows[i].label,LV_PCT(100));
        lv_label_set_long_mode(a->rows[i].label,LV_LABEL_LONG_WRAP);
        lv_obj_set_flex_flow(b,LV_FLEX_FLOW_COLUMN);
    }
    a->detail=label(a->body,"",POS_STYLE_TEXT_PRIMARY);
    a->detail_actions=box(root,LV_FLEX_FLOW_ROW_WRAP);
    a->back=button(a->detail_actions,"BACK TO LIST",back_click,a);
    a->path_button=button(a->detail_actions,"SHOW PATH",view_path,a);
    a->pager=box(root,LV_FLEX_FLOW_ROW_WRAP);
    button(a->pager,"NEWER",newer,a); button(a->pager,"OLDER",older,a); button(a->pager,"LIVE",live,a);
    a->page_label=label(a->pager,"",POS_STYLE_CAPTION);

    a->timer=lv_timer_create(poll,100,a);
    show(a);
    return a;
}
static void destroy(void *priv)
{
    struct scope_app *a=priv;
    lv_timer_delete(a->timer); rift_basemap_free(a->basemap);
    rift_ipc_close(&a->ipc); free(a);
}
const struct pocketos_app app_meshscope={
    .id="meshscope", .name="MeshScope", .icon=LV_SYMBOL_GPS,
    .create=create, .destroy=destroy, .back=back, .back_slab_in_app=true,
    .chrome=POCKETOS_CHROME_NONE,
};
