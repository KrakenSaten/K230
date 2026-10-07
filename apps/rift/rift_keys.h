/*
 * The keys a reader can join a channel with, made or checked on this device
 * and handed straight to mesh.channel_add. No LVGL, no file, no socket:
 * host-tested by tests/rift_model_test.c.
 *
 * A MeshCore channel IS its pre-shared key (docs/api/mesh.md, "What a channel
 * is, and is not"). There are three ways to have one, the three the T-Deck's
 * RIFT offers (vendor/RIFT examples/companion_radio/ui-rift/UITask.cpp,
 * finishChannel):
 *
 *   HASHTAG  a public topic: the key is the first 16 bytes of SHA-256 over
 *            the name with its leading '#' (MyMesh::addGroupChannelHashtag;
 *            test vector "#test" -> 9cd8fcf22a47333b591d96a2b848b73f). Anyone
 *            who knows the name can read it - that is what it is for.
 *   PRIVATE  a new random 16-byte key, made here, which the reader has to
 *            share for anybody else to read the channel.
 *   KEY      a key somebody shared: base64 of 16 or 32 bytes.
 *
 * What this module does NOT do is keep one. A key exists in a caller's
 * buffer for as long as it takes to write the request, and the service is
 * the only thing that stores it (channels.v1, mode 0600). Nothing here is
 * written to the reader's preferences, the model or a log.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_KEYS_H
#define RIFT_KEYS_H

#include <stddef.h>
#include <stdint.h>

/* base64 of 32 bytes is 44 characters; and the NUL. */
#define RIFT_KEY_B64_MAX 48
/* A channel name the service keeps: 1 to 31 bytes (mesh.channel_add). */
#define RIFT_CHANNEL_NAME_BYTES 31

/* SHA-256 of len bytes into out[32]. */
void rift_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* Standard base64 (RFC 4648, with padding) of len bytes. Returns the length
 * written, or 0 when out is too small. */
size_t rift_base64_encode(const uint8_t *in, size_t len, char *out, size_t out_len);

/* The canonical name of a hashtag channel: the reader's name with one
 * leading '#' (added when missing, never doubled), trimmed of spaces at
 * either end. Returns 0, or -1 with why when it is empty or longer than the
 * service keeps once the '#' is on it. */
int rift_hashtag_name(const char *typed, char *out, size_t out_len, char *why, size_t why_len);

/* The key of a hashtag channel, as base64, from its canonical name. */
int rift_hashtag_key(const char *canonical, char *b64, size_t b64_len);

/* A new random 16-byte key, as base64, from the kernel's random source.
 * Returns 0, or -1 when there was none to be had: a key made from anything
 * weaker would be a channel anybody could guess. */
int rift_random_key(char *b64, size_t b64_len);

/* Check a key a reader typed or pasted before it is sent: standard base64
 * with its padding, decoding to exactly 16 or 32 bytes, not all zero, and a
 * 32-byte key not with an all-zero upper half (the service refuses those as
 * ambiguous). Spaces at either end are allowed and ignored. Returns 0 and
 * writes the trimmed key to out, or -1 with why. The service checks again;
 * this is so the reader hears it before anything is asked. */
int rift_key_check(const char *typed, char *out, size_t out_len, char *why, size_t why_len);

/* A channel name the service will take: 1 to 31 bytes, no control
 * characters, not only spaces. Returns 0, or -1 with why. */
int rift_channel_name_check(const char *name, char *why, size_t why_len);

#endif
