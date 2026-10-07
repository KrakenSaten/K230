/*
 * Reading one JSON field at a time, the way this app has to read them.
 *
 * These live in a header because both halves of the model - the nodes and
 * the messages - parse the same API and must apply the same rule to it:
 *
 *   a value that is not known is absent, and absent is not zero.
 *
 * An absent field, a null, a string where a number belongs and an object
 * where a string belongs all mean "not reported", which is a different
 * answer from 0 (docs/api/mesh.md). Every accessor here says whether it
 * found a value rather than returning a fallback that cannot be told apart
 * from a measurement.
 *
 * static inline rather than a fourth translation unit: they are four small
 * readers over cJSON, and giving them a .c of their own would spread the
 * model over more files than it has parts.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_JSON_H
#define RIFT_JSON_H

#include <cjson/cJSON.h>

#include <stddef.h>

static inline const char *str_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

/* A number, only when it is one. Returns whether there was one. */
static inline int num_of(const cJSON *o, const char *key, double *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    if (!v || !cJSON_IsNumber(v)) {
        return 0;
    }
    *out = v->valuedouble;
    return 1;
}

static inline int bool_of(const cJSON *o, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    if (!v || !cJSON_IsBool(v)) {
        return fallback;
    }
    return cJSON_IsTrue(v) ? 1 : 0;
}

/* Hex, and only hex. want_len 0 means "any non-empty length". A public key
 * that is not 64 hex characters is not a public key, and a node or a
 * message named by one is refused rather than held under a key nothing can
 * be matched on. */
static inline int hex_only(const char *s, size_t want_len)
{
    size_t i;

    if (!s) {
        return 0;
    }
    for (i = 0; s[i]; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
              (s[i] >= 'A' && s[i] <= 'F'))) {
            return 0;
        }
    }
    return want_len == 0 ? (i > 0) : (i == want_len);
}

#endif
