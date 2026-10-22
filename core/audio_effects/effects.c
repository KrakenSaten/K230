/* Deterministic synthesis: mechanical transients and tonal motor/robot cues.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "effects.h"
#include <math.h>
#include <string.h>
void audio_effects_init(struct audio_effects *m)
{
    memset(m,0,sizeof(*m));const unsigned duration[]={10080,4800,2400,14400,14400,8640,19200,7200};
    for(unsigned k=0;k<AUDIO_EFFECT_KINDS;k++){
        m->length[k]=duration[k];uint32_t seed=0x52454c41u+k*117;
        for(unsigned i=0;i<duration[k];i++){
            seed=seed*1664525u+1013904223u;double noise=((int)(seed>>16)-32768)/32768.0,t=i/48000.0;
            double env=1-(double)i/duration[k],v=0;
            if(k==0)v=(noise*.6+sin(6.283185307*(100*t-60*t*t))*.4)*exp(-t*24);
            if(k==1)v=(noise*.7+sin(t*6.283185307*1400)*.3)*env*env;
            if(k==2)v=noise*exp(-t*140);
            if(k==3)v=noise*env*((i%4800)<550?.9:.05);
            if(k==4)v=sin(6.283185307*(380*t+420*t*t))*env*.6;
            if(k==5)v=(sin(t*6.283185307*190)*.5+noise*.5)*env;
            if(k==6)v=(sin(t*6.283185307*82)+noise*.2)*env*.4;
            if(k==7)v=sin(t*6.283185307*(i<3600?660:880))*env*.55;
            double attack=i<96?i/96.0:1;m->bank[k][i]=(int16_t)(v*attack*6500);
        }
    }
}
void audio_effects_play(struct audio_effects *m,unsigned kind)
{
    if(!m||kind>=AUDIO_EFFECT_KINDS)return;
    int free=-1;for(int i=0;i<4;i++)if(!m->voice[i].active){free=i;break;}
    if(free<0){if(kind==0||kind==2)free=3;else{m->dropped++;return;}}
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
