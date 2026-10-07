/*
 * RIFT's model: this node itself - who it says it is (mesh.identity, and
 * where its name came from) and the one routing setting it reports, the path
 * hash size (mesh.path_hash). The fifth translation unit over struct
 * rift_model; it obeys the rules at the top of rift_model.h.
 *
 * No LVGL and no sockets: host-tested by tests/rift_model_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_model.h"

#include "rift_format.h"
#include "rift_json.h"

#include <stdio.h>
#include <string.h>

int rift_model_apply_identity(struct rift_model *m, const cJSON *result)
{
    const char *key;
    const char *hash;
    const char *name;

    if (!m || !cJSON_IsObject(result)) {
        return -1;
    }
    key = str_of(result, "public_key");
    if (!hex_only(key, 64)) {
        return -1;
    }
    snprintf(m->self_key, sizeof(m->self_key), "%s", key);
    hash = str_of(result, "node_hash");
    if (hex_only(hash, 2)) {
        snprintf(m->self_hash, sizeof(m->self_hash), "%s", hash);
    } else {
        m->self_hash[0] = key[0];
        m->self_hash[1] = key[1];
        m->self_hash[2] = '\0';
    }
    name = str_of(result, "name");
    rift_utf8_copy(m->self_name, sizeof(m->self_name), name ? name : "");
    {
        const char *src = str_of(result, "name_source");
        double d;

        m->self_name_source = !src                         ? RIFT_NAME_SOURCE_UNKNOWN
                              : strcmp(src, "config") == 0  ? RIFT_NAME_SOURCE_CONFIG
                              : strcmp(src, "stored") == 0  ? RIFT_NAME_SOURCE_STORED
                              : strcmp(src, "derived") == 0 ? RIFT_NAME_SOURCE_DERIVED
                                                            : RIFT_NAME_SOURCE_UNKNOWN;
        m->self_name_max = (num_of(result, "name_max", &d) && d >= 1 && d < RIFT_NAME_MAX)
                               ? (int)d
                               : 0;
    }
    m->have_identity = 1;
    return 0;
}

/* The path hash size the service reports (mesh.path_hash, and the answer to
 * mesh.set_path_hash): kept beside the request that changes it. */
int rift_model_apply_path_hash(struct rift_model *m, const cJSON *result)
{
    const cJSON *allowed;
    const cJSON *item;
    unsigned mask = 0;
    double d;

    if (!m || !cJSON_IsObject(result) || !num_of(result, "bytes", &d) || d < 1 || d > 4 ||
        d != (double)(int)d) {
        return -1;
    }
    allowed = cJSON_GetObjectItemCaseSensitive(result, "allowed");
    cJSON_ArrayForEach(item, allowed)
    {
        if (cJSON_IsNumber(item) && item->valuedouble >= 1 && item->valuedouble <= 4) {
            mask |= 1u << (int)item->valuedouble;
        }
    }
    m->path_hash_bytes = (int)d;
    m->path_hash_allowed = mask ? mask : (1u << (int)d);
    m->have_path_hash = 1;
    m->path_hash_unsupported = 0;
    return 0;
}
