/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef RELAY_AUDIO_H
#define RELAY_AUDIO_H
#include <sys/types.h>
#include "relay.h"
struct rn_audio {pid_t pid,recovery;int fd;unsigned sent[RN_SOUNDS],dropped;unsigned long long started;char status[64];};
void rn_audio_init(struct rn_audio *a);
void rn_audio_poll(struct rn_audio *a,const unsigned sounds[RN_SOUNDS]);
void rn_audio_start(struct rn_audio *a,int volume);
void rn_audio_stop(struct rn_audio *a);
#endif
