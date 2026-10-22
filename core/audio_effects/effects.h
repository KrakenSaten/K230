/* Original short effects, four bounded voices, 48 kHz mono S16.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef AUDIO_EFFECTS_H
#define AUDIO_EFFECTS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define AUDIO_EFFECT_KINDS 8
#define AUDIO_EFFECT_FRAMES 19200
struct audio_effects { int16_t bank[AUDIO_EFFECT_KINDS][AUDIO_EFFECT_FRAMES]; unsigned length[AUDIO_EFFECT_KINDS]; struct {unsigned kind,at;bool active;} voice[4];unsigned dropped,played; };
void audio_effects_init(struct audio_effects *m);
void audio_effects_play(struct audio_effects *m,unsigned kind);
void audio_effects_mix(struct audio_effects *m,int16_t *out,size_t frames);
#endif
