/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "rift_scope.h"
#include "rift_map.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct rift_model m;
static struct rift_rxlog logbook;
static void node(int i,const char *prefix,double lat,double lon)
{
    memset(m.nodes[i].key,'0',64); m.nodes[i].key[64]=0;
    memcpy(m.nodes[i].key,prefix,strlen(prefix));
    m.nodes[i].have_location=1; m.nodes[i].lat=lat; m.nodes[i].lon=lon;
    m.node_count=i+1;
}
int main(void)
{
    rift_model_init(&m); rift_rxlog_init(&logbook);
    node(0,"aa1122",59.91,10.75); node(1,"bb1122",59.95,10.80);
    uint8_t prefix[]={0xaa,0x11,0x22};
    assert(rift_scope_resolve(&m,prefix,1)==0);
    node(2,"aa1133",59.89,10.70);
    assert(rift_scope_resolve(&m,prefix,1)==RIFT_SCOPE_AMBIGUOUS);
    assert(rift_scope_resolve(&m,prefix,2)==RIFT_SCOPE_AMBIGUOUS);
    assert(rift_scope_resolve(&m,prefix,3)==0);
    m.nodes[2].have_location=0; /* Unlocated collisions still count. */
    assert(rift_scope_resolve(&m,prefix,1)==RIFT_SCOPE_AMBIGUOUS);
    prefix[0]=0xcc; assert(rift_scope_resolve(&m,prefix,1)==RIFT_SCOPE_UNKNOWN);
    assert(rift_scope_resolve(&m,prefix,0)==RIFT_SCOPE_UNKNOWN);
    assert(rift_scope_resolve(&m,prefix,4)==RIFT_SCOPE_UNKNOWN);
    struct rift_rx_entry e={.kind=RIFT_RX_FRAME,.flags=RIFT_RXF_PARSED,
        .route=RIFT_RXRT_FLOOD,.path_hash_size=1,.path_hops=3,.path_bytes=3,
        .path={0xbb,0xcc,0xaa}};
    int hops[RIFT_RXLOG_PATH_MAX];
    assert(rift_scope_path(&m,&e,hops)==3);
    assert(hops[0]==1 && hops[1]==RIFT_SCOPE_UNKNOWN && hops[2]==RIFT_SCOPE_AMBIGUOUS);
    e.route=RIFT_RXRT_DIRECT; assert(!rift_scope_path(&m,&e,hops));
    e.route=RIFT_RXRT_TRANSPORT_DIRECT; assert(!rift_scope_path(&m,&e,hops));
    e.route=RIFT_RXRT_FLOOD; e.flags|=RIFT_RXF_PATH_SNR; assert(!rift_scope_path(&m,&e,hops));
    e.flags=RIFT_RXF_PARSED; e.path_bytes=64; assert(!rift_scope_path(&m,&e,hops));
    e.path_bytes=3; e.path_hash_size=3; e.path_hops=1;
    memcpy(e.path,(uint8_t[]){0xaa,0x11,0x22},3);
    assert(rift_scope_path(&m,&e,hops)==1 && hops[0]==0);
    const char *json="{\"v\":1,\"seq\":1,\"mono_ms\":1000,\"bytes\":42,\"verdict\":\"new\",\"type_code\":5,\"route\":\"flood\",\"path_kind\":\"hops\",\"path_hash_size\":1,\"path_hops\":3,\"path_hex\":\"bbbbcc\",\"hash\":\"0123456789abcdef\",\"dup\":1}";
    cJSON *data=cJSON_Parse(json);
    assert(rift_rxlog_apply(&logbook,data,1000,0)==0);
    assert(rift_scope_node_packets(&m,&logbook,1)==1); /* repeated hop, one packet */
    assert(rift_scope_node_packets(&m,&logbook,0)==0);
    cJSON_ReplaceItemInObject(data,"seq",cJSON_CreateNumber(2));
    cJSON_ReplaceItemInObject(data,"dup",cJSON_CreateNumber(2));
    assert(rift_rxlog_apply(&logbook,data,1000,0)==0);
    struct rift_scope_stats st;
    rift_scope_stats(&m,&logbook,2000,&st);
    assert(st.frames==2 && st.duplicates==1 && st.recent==2 && st.located==2);
    rift_scope_stats(&m,&logbook,62000,&st); assert(st.recent==0);
    rift_scope_stats(&m,&logbook,999,&st); assert(st.recent==0);
    rift_rxlog_mark(&logbook,"Disconnected",2000,0);
    rift_scope_stats(&m,&logbook,2000,&st); assert(st.frames==2);
    cJSON_ReplaceItemInObject(data,"v",cJSON_CreateNumber(9));
    assert(rift_rxlog_apply(&logbook,data,1000,0)==-1);
    assert(logbook.malformed==1);
    cJSON_Delete(data);
    struct rift_map_view view={0};
    assert(rift_map_fit(&view,&m,500,700)==2);
    double x,y; rift_map_project(&view,m.nodes[0].lat,m.nodes[0].lon,&x,&y);
    assert(rift_map_hit(&view,&m,x,y)==0);
    rift_map_resize(&view,1100,300);
    rift_map_project(&view,m.nodes[1].lat,m.nodes[1].lon,&x,&y);
    assert(rift_map_hit(&view,&m,x,y)==1);
    /* LIVE: the newest flood path, never one from before a service gap. */
    assert(rift_scope_live(&m,&logbook,2500)==NULL); /* the mark is newest */
    data=cJSON_Parse(json);
    cJSON_ReplaceItemInObject(data,"seq",cJSON_CreateNumber(3));
    cJSON_ReplaceItemInObject(data,"mono_ms",cJSON_CreateNumber(3000));
    assert(rift_rxlog_apply(&logbook,data,3000,0)==0);
    cJSON_Delete(data);
    const struct rift_rx_entry *live=rift_scope_live(&m,&logbook,3500);
    assert(live && live->seq==3);
    assert(rift_scope_live(&m,&logbook,3000+RIFT_SCOPE_LIVE_MS)==NULL);
    assert(rift_scope_live(&m,&logbook,2999)==NULL); /* not from the future */
    /* A TRACE is never the live path, however new. */
    data=cJSON_Parse("{\"v\":1,\"seq\":4,\"mono_ms\":3100,\"bytes\":20,\"verdict\":\"new\",\"type_code\":9,\"route\":\"flood\",\"path_kind\":\"snr\",\"path_hash_size\":1,\"path_hops\":2,\"path_hex\":\"1020\",\"hash\":\"1111111111111111\",\"dup\":1}");
    assert(data && rift_rxlog_apply(&logbook,data,3100,0)==0);
    cJSON_Delete(data);
    live=rift_scope_live(&m,&logbook,3500);
    assert(live && live->seq==3);
    /* Segments only between adjacent resolved, located hops. */
    struct rift_scope_hops h;
    int path[]={0,1,RIFT_SCOPE_UNKNOWN,1,RIFT_SCOPE_AMBIGUOUS,0,2};
    m.nodes[2].have_location=0;
    rift_scope_hops_of(&m,path,7,&h);
    assert(h.hops==7 && h.resolved==5 && h.unknown==1 && h.ambiguous==1 && h.unlocated==1);
    assert(h.segments==1); /* 0-1 only: no bridge over the gaps or to the unlocated node */
    /* SHOW PATH's fit takes only the located hops it is given. */
    int some[]={1,2,RIFT_SCOPE_UNKNOWN,999};
    assert(rift_map_fit_nodes(&view,&m,some,4,400,300)==1);
    rift_map_project(&view,m.nodes[1].lat,m.nodes[1].lon,&x,&y);
    assert(x>199 && x<201 && y>149 && y<151);
    puts("rift_scope_test: route ambiguity, path semantics, live path, segments, window statistics and map fitting passed");
    return 0;
}
