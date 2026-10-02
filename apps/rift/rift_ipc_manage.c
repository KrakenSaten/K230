/*
 * RIFT's meshcored client: the requests that manage this node - a channel
 * joined or left, a rename, a path hash size. See rift_ipc.h. Kept apart from
 * rift_ipc.c, which owns the connection, so neither file is everything.
 *
 * None of these transmits. Each is reached only from ACTIVITY's management
 * panels (ui/rift_manage.c), on a reader's press, and leaving a channel only
 * from its confirmation (tests/rift_lint.sh). The method names are the
 * connection's (rift_ipc.c, method_of); this file names none.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_ipc.h"

#include <stdio.h>
#include <string.h>

/* The requests that manage this node. One writer for all four, so the rules
 * are the same: recorded first, one at a time, refused with a reason. params
 * is consumed. */
static int manage_request(struct rift_ipc *c, enum rift_req what, enum rift_action kind,
                          const char *label, int value, cJSON *params)
{
    int64_t now;

    if (!c || !c->model) {
        cJSON_Delete(params);
        return -1;
    }
    now = rift_mono_ms();
    if (rift_model_action_begin(c->model, kind, NULL, label, now) != 0) {
        /* One is already on its way; its caption stays. */
        cJSON_Delete(params);
        return -1;
    }
    c->model->manage_op.value = value;
    if (c->fd < 0) {
        cJSON_Delete(params);
        rift_model_action_failed(c->model, kind, "meshcored is not answering; nothing was changed",
                                 now);
        c->revision++;
        return -1;
    }
    if (!params) {
        rift_model_action_failed(c->model, kind, "out of memory", now);
        c->revision++;
        return -1;
    }
    if (rift_ipc_write(c, what, params, now) != 0) {
        if (rift_model_action_busy(c->model, kind)) {
            rift_model_action_failed(c->model, kind, "the request could not be written", now);
        }
        c->revision++;
        return -1;
    }
    c->revision++;
    return 0;
}

int rift_ipc_channel_add(struct rift_ipc *c, const char *name, const char *key_b64)
{
    cJSON *params;

    if (!name || !key_b64) {
        return -1;
    }
    params = cJSON_CreateObject();
    if (params) {
        /* The key goes into the request and nowhere else: the model records
         * the name it was asked under, never the key. */
        cJSON_AddStringToObject(params, "name", name);
        cJSON_AddStringToObject(params, "key", key_b64);
    }
    return manage_request(c, RIFT_REQ_CHANNEL_ADD, RIFT_ACTION_CHANNEL_ADD, name, -1, params);
}

int rift_ipc_channel_remove(struct rift_ipc *c, int slot, const char *label)
{
    cJSON *params;

    if (slot < 0) {
        return -1;
    }
    params = cJSON_CreateObject();
    if (params) {
        cJSON_AddNumberToObject(params, "channel", slot);
    }
    return manage_request(c, RIFT_REQ_CHANNEL_REMOVE, RIFT_ACTION_CHANNEL_REMOVE, label, slot,
                          params);
}

int rift_ipc_set_name(struct rift_ipc *c, const char *name)
{
    cJSON *params;

    if (!name) {
        return -1;
    }
    params = cJSON_CreateObject();
    if (params) {
        cJSON_AddStringToObject(params, "name", name);
    }
    return manage_request(c, RIFT_REQ_SET_NAME, RIFT_ACTION_RENAME, name, -1, params);
}

int rift_ipc_set_path_hash(struct rift_ipc *c, int bytes)
{
    cJSON *params = cJSON_CreateObject();

    if (params) {
        cJSON_AddNumberToObject(params, "bytes", bytes);
    }
    return manage_request(c, RIFT_REQ_SET_PATH_HASH, RIFT_ACTION_PATH_HASH, NULL, bytes, params);
}

int rift_ipc_request_path_hash(struct rift_ipc *c)
{
    if (!c || c->fd < 0) {
        return -1;
    }
    return rift_ipc_write(c, RIFT_REQ_PATH_HASH, NULL, rift_mono_ms());
}
