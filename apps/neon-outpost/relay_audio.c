/* Nonblocking foreground audio.effects client. No ALSA or hardware in app.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "relay_audio.h"
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
static unsigned long long now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000+t.tv_nsec/1000000;}
static const char *helper(void){const char *v=getenv("DOORS_AUDIO_EFFECTS_HELPER");return v&&*v?v:"/usr/bin/pos-audio-effects";}
void rn_audio_init(struct rn_audio *a){memset(a,0,sizeof(*a));a->pid=a->recovery=-1;a->fd=-1;snprintf(a->status,sizeof(a->status),"Audio paused");}
static pid_t spawn(int fd,const char *arg)
{
    pid_t p=fork();if(p)return p;
    prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()==1)_exit(127);
    if(fd>=0){dup2(fd,0);dup2(fd,1);}else{int null=open("/dev/null",O_RDWR);if(null>=0){dup2(null,0);dup2(null,1);}}
    int null=open("/dev/null",O_WRONLY);if(null>=0)dup2(null,2);
    long max=sysconf(_SC_OPEN_MAX);if(max<0||max>65536)max=65536;
    for(int i=3;i<max;i++)close(i);
    execl(helper(),helper(),arg,(char *)NULL);_exit(127);
}
void rn_audio_start(struct rn_audio *a,int volume)
{
    if(a->pid>0)return;
    if(a->recovery>0){if(waitpid(a->recovery,NULL,WNOHANG)==0)return;a->recovery=-1;}
    int sv[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sv)){snprintf(a->status,sizeof(a->status),"Audio unavailable");return;}
    char arg[8];snprintf(arg,sizeof(arg),"%d",volume);pid_t p=spawn(sv[1],arg);close(sv[1]);
    if(p<0){close(sv[0]);snprintf(a->status,sizeof(a->status),"Audio unavailable");return;}a->pid=p;a->fd=sv[0];fcntl(a->fd,F_SETFL,O_NONBLOCK);a->started=now();snprintf(a->status,sizeof(a->status),"Audio starting");
}
void rn_audio_stop(struct rn_audio *a)
{
    if(a->fd>=0){close(a->fd);a->fd=-1;}
    if(a->pid>0){kill(a->pid,SIGTERM);int status=0;unsigned long long end=now()+300;
        while(waitpid(a->pid,&status,WNOHANG)==0&&now()<end){struct timespec t={0,2000000};nanosleep(&t,NULL);}
        if(waitpid(a->pid,&status,WNOHANG)==0){kill(a->pid,SIGKILL);waitpid(a->pid,&status,0);}
        if(WIFSIGNALED(status)){a->recovery=spawn(-1,"recover");}
        a->pid=-1;
    }
    if(a->recovery>0){int status;unsigned long long end=now()+300;while(waitpid(a->recovery,&status,WNOHANG)==0&&now()<end){struct timespec t={0,2000000};nanosleep(&t,NULL);}if(waitpid(a->recovery,&status,WNOHANG)==0){kill(a->recovery,SIGKILL);waitpid(a->recovery,&status,0);}a->recovery=-1;}
    snprintf(a->status,sizeof(a->status),"Audio paused");
}
void rn_audio_poll(struct rn_audio *a,const unsigned sounds[RN_SOUNDS])
{
    if(a->fd>=0){char b[64];ssize_t n=recv(a->fd,b,sizeof(b)-1,MSG_DONTWAIT);if(n>0){b[n]=0;if(strstr(b,"READY"))snprintf(a->status,sizeof(a->status),"Sound on");else if(strstr(b,"MUTED"))snprintf(a->status,sizeof(a->status),"Sound muted");else if(strstr(b,"UNAVAILABLE -2"))snprintf(a->status,sizeof(a->status),"Audio in use");else snprintf(a->status,sizeof(a->status),"Audio unavailable");}}
    unsigned char commands[RN_SOUNDS*8];size_t count=0;
    uint32_t stamp=(uint32_t)now();
    for(int k=0;k<RN_SOUNDS;k++){if(sounds[k]!=a->sent[k]){if(count+8<=sizeof(commands)){commands[count++]=(unsigned char)k;commands[count++]=0;commands[count++]=0;commands[count++]=0;for(int byte=0;byte<4;byte++)commands[count++]=(unsigned char)(stamp>>(byte*8));}a->sent[k]=sounds[k];}}
    if(count&&a->fd>=0&&send(a->fd,commands,count,MSG_DONTWAIT|MSG_NOSIGNAL)!=(ssize_t)count)a->dropped++;
    if(a->pid>0){int status;pid_t p=waitpid(a->pid,&status,WNOHANG);if(p==a->pid){if(a->fd>=0)close(a->fd);a->fd=-1;a->pid=-1;if(strcmp(a->status,"Audio in use"))snprintf(a->status,sizeof(a->status),"Audio unavailable");if(WIFSIGNALED(status))a->recovery=spawn(-1,"recover");}
        else if(now()-a->started>1500&&!strcmp(a->status,"Audio starting")){rn_audio_stop(a);snprintf(a->status,sizeof(a->status),"Audio timed out");}}
}
