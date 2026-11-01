/* Foreground audio.effects client; the game never opens an audio device.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef ROTOR_AUDIO_H
#define ROTOR_AUDIO_H
#include "rotor.h"
#include <sys/types.h>
struct rt_audio {
    pid_t pid, recovery;
    int fd;
    unsigned sent[RT_SOUNDS], dropped;
    uint64_t started, deadline;
    bool stopping;
    char status[48];
};
void rt_audio_init(struct rt_audio *a);
void rt_audio_start(struct rt_audio *a,int volume);
void rt_audio_poll(struct rt_audio *a,const unsigned sounds[RT_SOUNDS]);
void rt_audio_suspend(struct rt_audio *a);
void rt_audio_destroy(struct rt_audio *a);
#endif
