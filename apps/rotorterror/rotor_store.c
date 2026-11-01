/* Small versioned, checksummed record, atomically replaced after a new best.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "rotor_store.h"
#include "pocketpaths.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t checksum(const unsigned char *p)
{
    uint32_t n=2166136261u;
    for(int i=0;i<16;i++)n=(n^p[i])*16777619u;
    return n;
}

static void put32(unsigned char *p,uint32_t n)
{
    for(int i=0;i<4;i++)p[i]=(unsigned char)(n>>(i*8));
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}

static int paths(char dir[512],char path[540])
{
    if(snprintf(dir,512,"%s/rotorterror",pocketos_state_dir())>=512)return -1;
    snprintf(path,540,"%s/scores.bin",dir);
    return 0;
}

int rt_scores_load(uint32_t best[3])
{
    memset(best,0,3*sizeof(*best));
    char dir[512],path[540];
    if(paths(dir,path))return -1;
    FILE *f=fopen(path,"rb");
    if(!f)return errno==ENOENT?0:-1;
    unsigned char record[21];size_t n=fread(record,1,sizeof(record),f);
    bool failed=ferror(f)!=0;fclose(f);
    if(failed || n!=20 || memcmp(record,"RTR1",4) || get32(record+16)!=checksum(record))return -1;
    for(int i=0;i<3;i++)best[i]=get32(record+4+i*4);
    return 0;
}

int rt_scores_save(const uint32_t best[3])
{
    char dir[512],path[540],temp[560];
    if(paths(dir,path) || pocketos_mkdir_p(dir,0700))return -1;
    snprintf(temp,sizeof(temp),"%s/.scores-XXXXXX",dir);
    int fd=mkstemp(temp);
    if(fd<0)return -1;
    unsigned char record[20];memcpy(record,"RTR1",4);
    for(int i=0;i<3;i++)put32(record+4+i*4,best[i]);
    put32(record+16,checksum(record));
    bool failed=write(fd,record,sizeof(record))!=(ssize_t)sizeof(record);
    if(!failed && fsync(fd))failed=true;
    if(close(fd))failed=true;
    if(!failed && rename(temp,path))failed=true;
    if(failed)unlink(temp);
    return failed?-1:0;
}
