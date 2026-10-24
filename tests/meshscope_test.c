/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "scope_model.h"
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
    assert(scope_resolve(&m,prefix,1)==0);
    node(2,"aa1133",59.89,10.70);
    assert(scope_resolve(&m,prefix,1)==SCOPE_AMBIGUOUS);
    assert(scope_resolve(&m,prefix,2)==SCOPE_AMBIGUOUS);
    assert(scope_resolve(&m,prefix,3)==0);
    m.nodes[2].have_location=0; /* Unlocated collisions still count. */
    assert(scope_resolve(&m,prefix,1)==SCOPE_AMBIGUOUS);
    prefix[0]=0xcc; assert(scope_resolve(&m,prefix,1)==SCOPE_UNKNOWN);
    assert(scope_resolve(&m,prefix,0)==SCOPE_UNKNOWN);
    assert(scope_resolve(&m,prefix,4)==SCOPE_UNKNOWN);
    struct rift_rx_entry e={.kind=RIFT_RX_FRAME,.flags=RIFT_RXF_PARSED,
        .route=RIFT_RXRT_FLOOD,.path_hash_size=1,.path_hops=3,.path_bytes=3,
        .path={0xbb,0xcc,0xaa}};
    int hops[RIFT_RXLOG_PATH_MAX];
    assert(scope_path(&m,&e,hops)==3);
    assert(hops[0]==1 && hops[1]==SCOPE_UNKNOWN && hops[2]==SCOPE_AMBIGUOUS);
    e.route=RIFT_RXRT_DIRECT; assert(!scope_path(&m,&e,hops));
    e.route=RIFT_RXRT_TRANSPORT_DIRECT; assert(!scope_path(&m,&e,hops));
    e.route=RIFT_RXRT_FLOOD; e.flags|=RIFT_RXF_PATH_SNR; assert(!scope_path(&m,&e,hops));
    e.flags=RIFT_RXF_PARSED; e.path_bytes=64; assert(!scope_path(&m,&e,hops));
    e.path_bytes=3; e.path_hash_size=3; e.path_hops=1;
    memcpy(e.path,(uint8_t[]){0xaa,0x11,0x22},3);
    assert(scope_path(&m,&e,hops)==1 && hops[0]==0);
    const char *json="{\"v\":1,\"seq\":1,\"mono_ms\":1000,\"bytes\":42,\"verdict\":\"new\",\"type_code\":5,\"route\":\"flood\",\"path_kind\":\"hops\",\"path_hash_size\":1,\"path_hops\":3,\"path_hex\":\"bbbbcc\",\"hash\":\"0123456789abcdef\",\"dup\":1}";
    cJSON *data=cJSON_Parse(json);
    assert(rift_rxlog_apply(&logbook,data,1000,0)==0);
    assert(scope_node_packets(&m,&logbook,1)==1); /* repeated hop, one packet */
    assert(scope_node_packets(&m,&logbook,0)==0);
    cJSON_ReplaceItemInObject(data,"seq",cJSON_CreateNumber(2));
    cJSON_ReplaceItemInObject(data,"dup",cJSON_CreateNumber(2));
    assert(rift_rxlog_apply(&logbook,data,1000,0)==0);
    struct scope_stats st;
    scope_stats(&m,&logbook,2000,&st);
    assert(st.frames==2 && st.duplicates==1 && st.recent==2 && st.located==2);
    scope_stats(&m,&logbook,62000,&st); assert(st.recent==0);
    scope_stats(&m,&logbook,999,&st); assert(st.recent==0);
    rift_rxlog_mark(&logbook,"Disconnected",2000,0);
    scope_stats(&m,&logbook,2000,&st); assert(st.frames==2);
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
    puts("meshscope_test: route ambiguity, path semantics, window statistics and map hit testing passed");
    return 0;
}
