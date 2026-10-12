/* Helper-only runtime OAuth state; never a shell IPC payload.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef NINJA_AUTH_H
#define NINJA_AUTH_H
#include <stddef.h>
#include <stdint.h>
struct ninja_credentials {
    char access[4096], refresh[4096], base[128], client[160], profile[25];
    int64_t expires, retry_at;
    int attempts;
};
int ninja_credentials_decode(struct ninja_credentials *c, const char *json, size_t n);
int ninja_credentials_valid(const struct ninja_credentials *c);
/* Directory descriptor is also the nonblocking exclusive lock. Hold it through
 * renewal/rotation, import and forget. A killed process releases the lock. */
int ninja_credentials_lock(void);
int ninja_credentials_read(int dir, struct ninja_credentials *c);
int ninja_credentials_write(int dir, const struct ninja_credentials *c);
int ninja_credentials_pending(int dir);
int ninja_credentials_begin(int dir);
int ninja_credentials_finish(int dir);
int ninja_credentials_forget(int dir);
#endif
