/*
 * gatewayd: bounding the event stream. See gw_feed.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_feed.h"

#include <string.h>

void gw_feed_init(struct gw_feed *f)
{
    memset(f, 0, sizeof(*f));
}

void gw_feed_reset(struct gw_feed *f)
{
    int i;

    for (i = 0; i < GW_FEED_NODES; i++) {
        cJSON_Delete(f->node[i].held);
    }
    memset(f, 0, sizeof(*f));
}

/* ---- mesh.node ---------------------------------------------------------- */

static bool hex64(const char *s)
{
    size_t i;

    for (i = 0; i < 64; i++) {
        char c = s[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return s[64] == '\0';
}

static struct gw_feed_node *node_slot(struct gw_feed *f, const char *key, int64_t now_ms)
{
    struct gw_feed_node *spare = NULL;
    int i;

    for (i = 0; i < GW_FEED_NODES; i++) {
        struct gw_feed_node *n = &f->node[i];

        if (n->used && n->held == NULL && now_ms - n->last_ms >= GW_NODE_EVENT_MS) {
            n->used = false; /* its time is over: nothing is owed for it */
        }
        if (n->used && strcmp(n->key, key) == 0) {
            return n;
        }
        if (!n->used && spare == NULL) {
            spare = n;
        }
    }
    return spare;
}

cJSON *gw_feed_node(struct gw_feed *f, cJSON *data, int64_t now_ms)
{
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(data, "reason");
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(data, "node");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(node, "public_key");
    struct gw_feed_node *n;

    if (!cJSON_IsString(reason) || !cJSON_IsString(key) || !hex64(key->valuestring)) {
        cJSON_Delete(data);
        return NULL;
    }
    n = node_slot(f, key->valuestring, now_ms);
    if (strcmp(reason->valuestring, "removed") == 0) {
        /* A client takes the node off its list; nothing held may put it
         * back afterwards. */
        if (n != NULL && n->used) {
            cJSON_Delete(n->held);
            memset(n, 0, sizeof(*n));
        }
        return data;
    }
    if (n == NULL) {
        /* More nodes inside one period than the table holds: this one is
         * not sent rather than sent unbounded. mesh.nodes still has it. */
        cJSON_Delete(data);
        return NULL;
    }
    if (!n->used) {
        n->used = true;
        memcpy(n->key, key->valuestring, sizeof(n->key));
        n->last_ms = now_ms;
        n->held = NULL;
        return data;
    }
    cJSON_Delete(n->held);
    n->held = data;
    return NULL;
}

cJSON *gw_feed_node_due(struct gw_feed *f, int64_t now_ms)
{
    int i;

    for (i = 0; i < GW_FEED_NODES; i++) {
        struct gw_feed_node *n = &f->node[i];

        if (n->used && n->held != NULL && now_ms - n->last_ms >= GW_NODE_EVENT_MS) {
            cJSON *out = n->held;

            n->held = NULL;
            n->last_ms = now_ms;
            return out;
        }
    }
    return NULL;
}

/* ---- mesh.activity ------------------------------------------------------ */

/* A payload type as meshcored words it: lower case letters, digits and
 * underscores. Anything else is counted as "other". */
static bool type_word(const char *s)
{
    size_t i;

    for (i = 0; s[i] != '\0'; i++) {
        char c = s[i];

        if (i >= GW_FEED_TYPE_MAX || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return i > 0;
}

static void count_type(struct gw_feed_bin *b, const char *word)
{
    int i;

    if (word == NULL || !type_word(word)) {
        word = "other";
    }
    for (i = 0; i < b->types; i++) {
        if (strcmp(b->type[i].name, word) == 0) {
            b->type[i].n += 1;
            return;
        }
    }
    if (b->types >= GW_FEED_TYPES - 1 && strcmp(word, "other") != 0) {
        count_type(b, "other"); /* the last place is kept for it */
        return;
    }
    if (b->types < GW_FEED_TYPES) {
        memcpy(b->type[b->types].name, word, strlen(word) + 1);
        b->type[b->types].n = 1;
        b->types++;
    }
}

void gw_feed_activity(struct gw_feed *f, const cJSON *data, int64_t now_ms)
{
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(data, "kind");
    const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(data, "bytes");
    struct gw_feed_bin *b = &f->bin;

    if (!cJSON_IsString(kind)) {
        return;
    }
    if (strcmp(kind->valuestring, "rx") == 0) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(data, "payload_type");

        b->rx_frames += 1;
        if (cJSON_IsNumber(bytes) && bytes->valuedouble >= 0 && bytes->valuedouble <= 65535) {
            b->rx_bytes += bytes->valuedouble;
        }
        count_type(b, cJSON_IsString(type) ? type->valuestring : NULL);
    } else if (strcmp(kind->valuestring, "tx") == 0) {
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(data, "result");
        const char *word = cJSON_IsString(result) ? result->valuestring : "";

        /* docs/api/mesh.md: "ok" and "rx_resume_failed" went on the air;
         * "tx_failed" and "refused" did not; "unknown" is exactly that. */
        if (strcmp(word, "ok") == 0 || strcmp(word, "rx_resume_failed") == 0) {
            b->tx_ok += 1;
        } else if (strcmp(word, "tx_failed") == 0 || strcmp(word, "refused") == 0) {
            b->tx_failed += 1;
        } else {
            b->tx_unknown += 1;
        }
    } else {
        return;
    }
    if (!b->any) {
        b->any = true;
        b->start_ms = now_ms;
    }
}

cJSON *gw_feed_activity_due(struct gw_feed *f, int64_t now_ms)
{
    struct gw_feed_bin *b = &f->bin;
    cJSON *d, *types;
    int i;

    if (!b->any || now_ms - b->start_ms < GW_ACTIVITY_BIN_MS) {
        return NULL;
    }
    d = cJSON_CreateObject();
    types = cJSON_CreateObject();
    if (d != NULL && types != NULL) {
        cJSON_AddNumberToObject(d, "start_mono_ms", (double)b->start_ms);
        cJSON_AddNumberToObject(d, "rx_frames", b->rx_frames);
        cJSON_AddNumberToObject(d, "rx_bytes", b->rx_bytes);
        cJSON_AddNumberToObject(d, "tx_ok", b->tx_ok);
        cJSON_AddNumberToObject(d, "tx_failed", b->tx_failed);
        cJSON_AddNumberToObject(d, "tx_unknown", b->tx_unknown);
        for (i = 0; i < b->types; i++) {
            cJSON_AddNumberToObject(types, b->type[i].name, b->type[i].n);
        }
        cJSON_AddItemToObject(d, "by_type", types);
    } else {
        cJSON_Delete(d);
        cJSON_Delete(types);
        d = NULL;
    }
    memset(b, 0, sizeof(*b));
    return d;
}
