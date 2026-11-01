/* RotorTerror client against the actual service, using ALSA null on a host.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "rotor_audio.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do { checks++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);abort();} } while(0)
static void pump(struct rt_audio *a,unsigned *sounds,int ms)
{
    for(int i=0;i<ms;i+=5) { rt_audio_poll(a,sounds);struct timespec t={0,5000000};nanosleep(&t,NULL); }
}
int main(int argc,char **argv)
{
    CHECK(argc==2);setenv("DOORS_AUDIO_EFFECTS_HELPER",argv[1],1);
    char dir[]="/tmp/rotor-audio-XXXXXX";CHECK(mkdtemp(dir));
    setenv("POCKETOS_RUNTIME_DIR",dir,1);setenv("POCKETOS_AUDIO_BOARD","generic",1);
    setenv("POCKETOS_AUDIO_PCM","null",1);
    char cfg[200];snprintf(cfg,sizeof(cfg),"%s/alsa.conf",dir);FILE *f=fopen(cfg,"w");CHECK(f);
    fputs("pcm.null { type null }\n",f);fclose(f);setenv("ALSA_CONFIG_PATH",cfg,1);
    struct rt_audio a;unsigned sounds[RT_SOUNDS]={0};rt_audio_init(&a);
    rt_audio_start(&a,45);pump(&a,sounds,160);CHECK(!strcmp(a.status,"SOUND ON") && a.pid>0);
    for(int k=RT_GUN;k<RT_SOUNDS;k++)sounds[k]++;
    pump(&a,sounds,80);
    if(a.sent[RT_BOOM]!=1 || a.dropped)fprintf(stderr,"sent %u dropped %u pid %d status %s\n",a.sent[RT_BOOM],a.dropped,(int)a.pid,a.status);
    CHECK(a.sent[RT_BOOM]==1 && a.dropped==0);
    struct rt_audio busy;rt_audio_init(&busy);rt_audio_start(&busy,45);pump(&busy,sounds,160);
    if(strcmp(busy.status,"AUDIO IN USE"))fprintf(stderr,"busy client: %s pid %d\n",busy.status,(int)busy.pid);
    CHECK(!strcmp(busy.status,"AUDIO IN USE"));rt_audio_destroy(&busy);
    rt_audio_suspend(&a);pump(&a,sounds,100);CHECK(a.pid<0 && a.fd<0);
    rt_audio_start(&a,0);CHECK(a.pid<0 && !strcmp(a.status,"SYSTEM MUTED"));
    rt_audio_start(&a,45);pump(&a,sounds,120);CHECK(a.pid>0);
    CHECK(!kill(a.pid,SIGKILL));pump(&a,sounds,200);CHECK(a.pid<0 && a.recovery<0);
    rt_audio_start(&a,45);pump(&a,sounds,120);CHECK(!strcmp(a.status,"SOUND ON"));
    rt_audio_destroy(&a);CHECK(a.pid<0 && a.recovery<0 && a.fd<0);
    errno=0;CHECK(waitpid(-1,NULL,WNOHANG)==-1 && errno==ECHILD);
    char path[200];snprintf(path,sizeof(path),"%s/audio.lock",dir);unlink(path);
    snprintf(path,sizeof(path),"%s/audio.recovery",dir);unlink(path);unlink(cfg);rmdir(dir);
    printf("rotor_audio_client_test: %u checks passed; real service, ALSA null, no device\n",checks);
    return 0;
}
