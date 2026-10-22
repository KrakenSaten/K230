/* Deterministic synthesis: mechanical transients and tonal motor/robot cues.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "effects.h"
#include <math.h>
#include <string.h>
void audio_effects_init(struct audio_effects *m)
{
    memset(m,0,sizeof(*m));const unsigned duration[]={10080,4800,2400,14400,14400,8640,19200,7200,19200,19200};
    for(unsigned k=0;k<AUDIO_EFFECT_KINDS;k++){
        m->length[k]=duration[k];uint32_t seed=0x52454c41u+k*117;
        for(unsigned i=0;i<duration[k];i++){
            seed=seed*1664525u+1013904223u;double noise=((int)(seed>>16)-32768)/32768.0,t=i/48000.0;
            double env=1-(double)i/duration[k],v=0;
            if(k==0)v=(noise*.42+sin(6.283185307*(135*t-90*t*t))*.5)*exp(-t*27);
            if(k==1)v=(noise*.35+sin(t*6.283185307*940)*.55)*exp(-t*55);
            if(k==2)v=(noise*.45+sin(t*6.283185307*1200)*.3)*exp(-t*160);
            if(k==3)v=noise*env*((i%4800)<380?.75:.015)+sin(t*6.283185307*220)*exp(-t*45)*.2;
            if(k==4)v=(sin(6.283185307*(300*t+650*t*t))*.4+sin(t*6.283185307*75)*.15)*env;
            if(k==5)v=(sin(t*6.283185307*155)*.55+noise*.35)*exp(-t*18);
            if(k==6)v=(sin(t*6.283185307*79)*.45+sin(t*6.283185307*158)*.15+noise*.08)*env;
            if(k==7)v=sin(t*6.283185307*(i<3600?523.25:783.99))*env*.45;
            if(k==8)v=(noise*.68+sin(6.283185307*(65*t-25*t*t))*.55)*exp(-t*14);
            if(k==9){double click=(i%8400)/48000.0;v=(noise*.65+sin(click*6.283185307*430)*.25)*exp(-click*90)*env;}
            double attack=i<96?i/96.0:1;m->bank[k][i]=(int16_t)(v*attack*6500);
        }
    }
}
void audio_effects_play(struct audio_effects *m,unsigned kind)
{
    if(!m||kind>=AUDIO_EFFECT_KINDS)return;
    int free=-1;for(int i=0;i<4;i++)if(!m->voice[i].active){free=i;break;}
    if(free<0){if(kind==0||kind==2||kind==8)free=3;else{m->dropped++;return;}}
    m->voice[free].active=true;m->voice[free].kind=kind;m->voice[free].at=0;m->played++;
}
void audio_effects_mix(struct audio_effects *m,int16_t *out,size_t frames)
{
    if(frames>960)frames=960;
    for(size_t j=0;j<frames;j++){
        int sum=0;for(int i=0;i<4;i++)if(m->voice[i].active){unsigned k=m->voice[i].kind;sum+=m->bank[k][m->voice[i].at++];if(m->voice[i].at>=m->length[k])m->voice[i].active=false;}
        out[j]=(int16_t)(sum>8192?8192:sum< -8192?-8192:sum);
    }
}
