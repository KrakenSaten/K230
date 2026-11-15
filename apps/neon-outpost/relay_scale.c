/* Relay Nine's scene scaler; relay_scale.h describes the sampling.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "relay_scale.h"
#include <stdlib.h>
#include <string.h>
/* RGB565 spread over 32 bits as 00000GGGGGG00000RRRRR000000BBBBB: each field
 * has room above it for a 5-bit weight, so one multiply scales all three. */
#define RN_SPREAD_MASK 0x07E0F81Fu
#define RN_SPREAD_HALF 0x02008010u /* 16 in each field: rounds the >>5 */
#define RN_SCALE_MAX 4096
static inline uint32_t spread(uint16_t c){return ((uint32_t)c|(uint32_t)c<<16)&RN_SPREAD_MASK;}
static inline uint16_t fold(uint32_t v){return (uint16_t)(v|v>>16);}
static inline uint32_t mix(uint32_t a,uint32_t b,unsigned f){return ((a*(32-f)+b*f+RN_SPREAD_HALF)>>5)&RN_SPREAD_MASK;}
static size_t up8(size_t n){return (n+7)&~(size_t)7;}
size_t rn_scaler_bytes(int sw,int w,int h)
{
    size_t W=(size_t)w,H=(size_t)h;
    return up8(W*H*2)+2*up8(W*4)+up8(((size_t)sw+1)*4)+2*up8(W*2)+2*up8(H*2)+up8(W)+up8(H);
}
void rn_scaler_free(struct rn_scaler *s)
{
    free(s->pixels);memset(s,0,sizeof(*s));
}
/* Pixel-centre mapping of target index i (of n) onto a source of m, in 1/32
 * source pixels and clamped to the source; the last source pixel has no
 * right/lower neighbour, so its weight is 0 there. With both sizes at most
 * RN_SCALE_MAX (4096), every product stays below 2^31. */
static void map(int m,int n,uint16_t *first,uint8_t *weight,uint16_t *nearest)
{
    int near=0;
    for(int i=0;i<n;i++){
        int num=((2*i+1)*m-n)*32;
        int at=num<=0?0:(num+n)/(2*n);
        int base=at>>5,f=at&31;
        if(base>=m-1){base=m-1;f=0;}
        first[i]=(uint16_t)base;weight[i]=(uint8_t)f;
        /* floor((2i+1)m/2n), clamped, kept as a running index: the K230
         * toolchain's -O3 vectoriser warns about its own temporaries on the
         * plain division. */
        while(near<m-1&&2*(near+1)*n<=(2*i+1)*m)near++;
        nearest[i]=(uint16_t)near;
    }
}
bool rn_scaler_resize(struct rn_scaler *s,int sw,int sh,int w,int h)
{
    if(s->pixels&&s->sw==sw&&s->sh==sh&&s->w==w&&s->h==h)return true;
    rn_scaler_free(s);
    if(sw<1||sh<1||w<1||h<1||sw>RN_SCALE_MAX||sh>RN_SCALE_MAX||w>RN_SCALE_MAX||h>RN_SCALE_MAX)return false;
    uint8_t *p=malloc(rn_scaler_bytes(sw,w,h));if(!p)return false;
    size_t W=(size_t)w,H=(size_t)h;
    s->pixels=(uint16_t *)p;p+=up8(W*H*2);
    s->row[0]=(uint32_t *)p;p+=up8(W*4);s->row[1]=(uint32_t *)p;p+=up8(W*4);
    s->spread=(uint32_t *)p;p+=up8(((size_t)sw+1)*4);
    s->x0=(uint16_t *)p;p+=up8(W*2);s->xn=(uint16_t *)p;p+=up8(W*2);
    s->y0=(uint16_t *)p;p+=up8(H*2);s->yn=(uint16_t *)p;p+=up8(H*2);
    s->fx=p;p+=up8(W);s->fy=p;
    s->sw=sw;s->sh=sh;s->w=w;s->h=h;
    map(sw,w,s->x0,s->fx,s->xn);map(sh,h,s->y0,s->fy,s->yn);
    return true;
}
/* Source row r filtered horizontally into a row slot other than keep. */
static int filtered_row(struct rn_scaler *s,const uint16_t *src,int r,int keep)
{
    if(s->row_y[0]==r)return 0;
    if(s->row_y[1]==r)return 1;
    int slot=keep==0?1:0;
    const uint16_t *line=src+(size_t)r*s->sw;uint32_t *sp=s->spread,*out=s->row[slot];
    for(int i=0;i<s->sw;i++)sp[i]=spread(line[i]);
    sp[s->sw]=sp[s->sw-1];
    for(int x=0;x<s->w;x++){unsigned i=s->x0[x];out[x]=mix(sp[i],sp[i+1],s->fx[x]);}
    s->row_y[slot]=r;return slot;
}
void rn_scale(struct rn_scaler *s,const uint16_t *src,bool filtered)
{
    if(!s->pixels||!src)return;
    int w=s->w;
    if(!filtered){
        int previous=-1;
        for(int y=0;y<s->h;y++){
            uint16_t *dst=s->pixels+(size_t)y*w;int r=s->yn[y];
            if(r==previous){memcpy(dst,dst-w,(size_t)w*2);continue;}
            const uint16_t *line=src+(size_t)r*s->sw;
            for(int x=0;x<w;x++)dst[x]=line[s->xn[x]];
            previous=r;
        }
        return;
    }
    s->row_y[0]=s->row_y[1]=-1;
    for(int y=0;y<s->h;y++){
        uint16_t *dst=s->pixels+(size_t)y*w;int r=s->y0[y];unsigned f=s->fy[y];
        int a=filtered_row(s,src,r,-1);
        const uint32_t *top=s->row[a];
        if(!f){for(int x=0;x<w;x++)dst[x]=fold(top[x]);continue;}
        const uint32_t *bottom=s->row[filtered_row(s,src,r+1<s->sh?r+1:r,a)];
        for(int x=0;x<w;x++)dst[x]=fold(mix(top[x],bottom[x],f));
    }
}
