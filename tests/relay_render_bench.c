/* Comparable fixed-scene host CPU observation, not a K230 frame-rate test.
 * Optional PPM output is the actual representative scene at native resolution.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "relay.h"
#include "audio_effects/effects.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
static int compare(const void *a,const void *b){double x=*(const double*)a,y=*(const double*)b;return(x>y)-(x<y);}
int main(int argc,char **argv)
{
    static struct rn_game g;static struct rn_frame f;double times[400];
    if(!rn_restart(&g,true))return 1;
    for(int i=0;i<420;i++){
        g.angle=RN_PI+(i%20-10)*.025;clock_t begin=clock();
        if(!rn_render(&g,&f)||f.cap_hits)return 1;
        if(i>=20)times[i-20]=1000.0*(clock()-begin)/CLOCKS_PER_SEC;
    }
    qsort(times,400,sizeof(*times),compare);
    printf("same slice, 400 frames, 320x200, CPU ms p50 %.3f p95 %.3f p99 %.3f; game %zu frame %zu mixer %zu bytes\n",times[200],times[380],times[396],sizeof(g),sizeof(f),sizeof(struct audio_effects));
    if(argc==2){
        g.angle=RN_PI;rn_advance(&g,(struct rn_input){.fire=true},10);if(!rn_render(&g,&f))return 1;
        FILE *out=fopen(argv[1],"wb");if(!out)return 1;fprintf(out,"P6\n%d %d\n255\n",RN_W,RN_H);
        for(int i=0;i<RN_W*RN_H;i++){uint16_t c=f.pixels[i];unsigned char p[3]={(c>>11)*255/31,((c>>5)&63)*255/63,(c&31)*255/31};fwrite(p,1,3,out);}fclose(out);
    }
    return 0;
}
