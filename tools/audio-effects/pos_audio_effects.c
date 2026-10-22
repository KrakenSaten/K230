/* Foreground audio.effects service. One client over inherited stdin/stdout,
 * one pocketaudio owner, fixed sample bank/voices and no shell audio work.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "audio_effects/effects.h"
#include "pocketaudio/pocketaudio.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
/* Periods kept queued ahead of the device. One hart at HZ=250 also runs the
 * shell's renderer, so a period written just in time underruns whenever this
 * process is scheduled late; two give 40 ms of slack for ~40-60 ms onset. */
#define LEAD_PERIODS 2
static volatile sig_atomic_t stop;
static void stopped(int signal){(void)signal;stop=1;}
static unsigned long long now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000+t.tv_nsec/1000000;}
static void put(struct pocketaudio_stream *stream,const int16_t *pcm)
{
    size_t offset=0;
    while(stream&&offset<960&&!stop){long n=pocketaudio_write(stream,pcm+offset,960-offset);if(n<=0){stop=1;break;}offset+=(size_t)n;}
}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"recover")){struct pocketaudio_options o={.backend=pocketaudio_alsa_backend()};return pocketaudio_recover(&o,NULL,0)!=0;}
    int volume=argc==2?atoi(argv[1]):50;if(volume<0||volume>100)return 2;
    pid_t parent=getppid();if(prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent)return 2;
    signal(SIGTERM,stopped);signal(SIGINT,stopped);signal(SIGPIPE,SIG_IGN);
    struct audio_effects *m=calloc(1,sizeof(*m));if(!m)return 2;audio_effects_init(m);
    struct pocketaudio_stream *stream=NULL;char err[160];
    struct pocketaudio_options opts={.backend=pocketaudio_alsa_backend(),.volume_percent=volume?volume:1};
    int rc=volume?pocketaudio_open(&stream,POCKETAUDIO_PLAYBACK,&opts,err,sizeof(err)):0;
    if(rc){printf("UNAVAILABLE %d\n",rc);fflush(stdout);free(m);return 1;}
    puts(volume?"READY":"MUTED");fflush(stdout);
    static const int16_t quiet[960];int16_t pcm[960];unsigned char command[256],packet[8];unsigned used=0,xruns=0;bool refill=true;unsigned long long next=now();
    while(!stop){
        struct pollfd p={STDIN_FILENO,POLLIN,0};int wait=(int)(next>now()?next-now():0);if(wait>20)wait=20;
        if(poll(&p,1,wait)<0&&errno!=EINTR)break;
        if(p.revents&(POLLIN|POLLHUP)){
            ssize_t n=read(STDIN_FILENO,command,sizeof(command));if(n<=0)break;
            for(ssize_t i=0;i<n;i++){
                packet[used++]=command[i];if(used<8)continue;used=0;
                uint32_t stamp=(uint32_t)packet[4]|((uint32_t)packet[5]<<8)|((uint32_t)packet[6]<<16)|((uint32_t)packet[7]<<24);
                int32_t age=(int32_t)((uint32_t)now()-stamp);
                if(packet[0]<AUDIO_EFFECT_KINDS && !packet[1]&&!packet[2]&&!packet[3] && age>=-20&&age<=200)audio_effects_play(m,packet[0]);
            }
        }
        unsigned long long t=now();if(t<next)continue;
        /* Paced from the schedule, not from this wake-up, so lateness is made
         * up instead of eating the lead. A stall longer than the queue, or an
         * underrun the backend recovered, starts the lead again. */
        if(stream&&(t>next+20*(LEAD_PERIODS+1)||pocketaudio_xruns(stream)!=xruns))refill=true;
        if(refill){refill=false;next=t;for(int i=0;i<LEAD_PERIODS;i++)put(stream,quiet);xruns=pocketaudio_xruns(stream);}
        audio_effects_mix(m,pcm,960);put(stream,pcm);
        next+=20; /* null/file hosts cannot busy-spin, and silence doesn't grow a queue */
    }
    if(stream)fprintf(stderr,"xruns %u\n",pocketaudio_xruns(stream));
    pocketaudio_close(stream);free(m);return 0;
}
