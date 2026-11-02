/* Nonblocking session commands, bounded queues and parent-bound cleanup.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "rotor_audio.h"
#include "audio_effects/effects.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

_Static_assert(RT_SOUNDS==AUDIO_EFFECT_KINDS,"Effects bank and client must agree");
_Static_assert(RT_GUN==AUDIO_EFFECT_ROTOR_GUN,"Stable effects wire IDs");

static uint64_t now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}

static pid_t spawn(int fd,const char *arg)
{
    const char *path=getenv("DOORS_AUDIO_EFFECTS_HELPER");
    if(!path || !*path)path="/usr/bin/pos-audio-effects";
    pid_t parent=getpid(),pid=fork();
    if(pid)return pid;
    if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent)_exit(127);
    if(fd>=0) { dup2(fd,STDIN_FILENO);dup2(fd,STDOUT_FILENO); }
    else {
        int null=open("/dev/null",O_RDWR);
        if(null>=0) { dup2(null,STDIN_FILENO);dup2(null,STDOUT_FILENO); }
    }
    int null=open("/dev/null",O_WRONLY);
    if(null>=0)dup2(null,STDERR_FILENO);
    long max=sysconf(_SC_OPEN_MAX);
    if(max<0 || max>65536)max=65536;
    for(int i=3;i<max;i++)close(i);
    execl(path,path,arg,(char *)NULL);
    _exit(127);
}

void rt_audio_init(struct rt_audio *a)
{
    memset(a,0,sizeof(*a));
    a->pid=a->recovery=-1;a->fd=-1;
    snprintf(a->status,sizeof(a->status),"SOUND STANDBY");
}

void rt_audio_start(struct rt_audio *a,int volume)
{
    if(a->pid>0 || a->recovery>0)return;
    a->stopping=false;
    if(volume<=0) { snprintf(a->status,sizeof(a->status),"SYSTEM MUTED");return; }
    if(volume>100)volume=100;
    int sv[2];
    if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sv)) {
        snprintf(a->status,sizeof(a->status),"SOUND UNAVAILABLE");return;
    }
    char arg[8];snprintf(arg,sizeof(arg),"%d",volume);
    a->pid=spawn(sv[1],arg);close(sv[1]);
    if(a->pid<0) { close(sv[0]);snprintf(a->status,sizeof(a->status),"SOUND UNAVAILABLE");return; }
    a->fd=sv[0];fcntl(a->fd,F_SETFL,O_NONBLOCK);
    a->started=now();
    snprintf(a->status,sizeof(a->status),"SOUND STARTING");
}

void rt_audio_suspend(struct rt_audio *a)
{
    if(a->fd>=0) { close(a->fd);a->fd=-1; }
    if(a->pid>0 && !a->stopping) {
        kill(a->pid,SIGTERM);a->stopping=true;a->deadline=now()+300;
    }
    snprintf(a->status,sizeof(a->status),"SOUND STANDBY");
}

static void read_status(struct rt_audio *a)
{
    if(a->fd>=0) {
        char text[80];ssize_t n=recv(a->fd,text,sizeof(text)-1,MSG_DONTWAIT);
        if(n>0) {
            text[n]=0;
            snprintf(a->status,sizeof(a->status),"%s",strstr(text,"READY")?"SOUND ON":
                strstr(text,"MUTED")?"SYSTEM MUTED":strstr(text,"UNAVAILABLE -2")?"AUDIO IN USE":"SOUND UNAVAILABLE");
        }
    }
}

void rt_audio_poll(struct rt_audio *a,const unsigned sounds[RT_SOUNDS])
{
    if(a->recovery>0 && waitpid(a->recovery,NULL,WNOHANG)==a->recovery)a->recovery=-1;
    read_status(a);
    unsigned char commands[RT_SOUNDS*8];size_t count=0;
    uint32_t stamp=(uint32_t)now();
    /* Impact and rewards are sent ahead of the low priority ambience. */
    for(unsigned id=RT_GUN;id<RT_SOUNDS;id++) {
        if(sounds[id]==a->sent[id])continue;
        commands[count++]=(unsigned char)id;
        commands[count++]=0;commands[count++]=0;commands[count++]=0;
        for(int b=0;b<4;b++)commands[count++]=(unsigned char)(stamp>>(b*8));
        a->sent[id]=sounds[id];
    }
    if(count && a->fd>=0) {
        /* Address-free local IPC: the connected child is the sole peer. */
        struct iovec io={commands,count};
        struct msghdr message={.msg_iov=&io,.msg_iovlen=1};
        if(sendmsg(a->fd,&message,MSG_NOSIGNAL|MSG_DONTWAIT)!=(ssize_t)count)a->dropped++;
    }
    if(a->pid>0) {
        int status=0;pid_t done=waitpid(a->pid,&status,WNOHANG);
        if(done==a->pid) {
            /* A short-lived refusal can exit between recv and waitpid. Read
             * its final packet before closing, so busy is not lost in a race. */
            read_status(a);
            if(a->fd>=0)close(a->fd);
            a->fd=-1;a->pid=-1;
            if(!a->stopping && strcmp(a->status,"AUDIO IN USE"))snprintf(a->status,sizeof(a->status),"SOUND UNAVAILABLE");
            if(WIFSIGNALED(status))a->recovery=spawn(-1,"recover");
        } else if(a->stopping && now()>a->deadline)kill(a->pid,SIGKILL);
        else if(!a->stopping && now()-a->started>1500 && !strcmp(a->status,"SOUND STARTING")) {
            rt_audio_suspend(a);snprintf(a->status,sizeof(a->status),"SOUND TIMED OUT");
        }
    }
}

void rt_audio_destroy(struct rt_audio *a)
{
    rt_audio_suspend(a);
    uint64_t end=now()+700;
    while((a->pid>0 || a->recovery>0) && now()<end) {
        rt_audio_poll(a,a->sent);
        struct timespec t={0,2000000};nanosleep(&t,NULL);
    }
    if(a->pid>0) { kill(a->pid,SIGKILL);waitpid(a->pid,NULL,0);a->pid=-1; }
    if(a->recovery>0) { kill(a->recovery,SIGKILL);waitpid(a->recovery,NULL,0);a->recovery=-1; }
}
