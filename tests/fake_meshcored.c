/*
 * A meshcored that is not meshcored. See fake_meshcored.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fake_meshcored.h"

#include "pocketipc/server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SELF_KEY "5f000000000000000000000000000000000000000000000000000000000000ff"

/* The three snapshots a scripted event can race: see events_after_snapshot. */
#define ANSWERED_NODES 1
#define ANSWERED_CHANNELS 2
#define ANSWERED_MESSAGES 4
#define ANSWERED_ALL (ANSWERED_NODES | ANSWERED_CHANNELS | ANSWERED_MESSAGES)

struct state {
    const struct fake_meshcored_script *script;
    struct pocketipc_server *server;
    int subscribed;
    int events_sent;
    int raw_sent;
    int clients_gone;
    int sent;
    int snapshots_answered;
    int inbox_answered;
    int64_t subscribe_ms;
    int64_t started_ms;
    /* What this service has been asked to forget, so a snapshot after a
     * mesh.node_remove no longer carries the node and one after a
     * mesh.node_reset_path carries it with no route - as the real one's
     * would. */
    char removed[8][65];
    int removed_count;
    char unrouted[8][65];
    int unrouted_count;
    /* Managing the node: the channel table as it now stands (taken from the
     * script on first use, then changed by channel_add / channel_remove),
     * the keys behind it, the name and the path hash size. */
    cJSON *channels;
    char channel_key[8][48];
    char name[32];
    int renamed; /* mesh.set_name was taken: the name is the stored one now */
    int path_hash_bytes;
    /* Repeater control: the round, the session, the next request id. */
    int round;
    int64_t round_until;
    int remote_id;
    int session_active;
    int session_ok;
    char session_key[65];
    int cli_seen;
};

/* ---- repeater control --------------------------------------------------- */

static void remote_log(struct state *st, const char *name, const cJSON *params)
{
    FILE *f;
    char *text;

    if (!st->script->remote_log) {
        return;
    }
    f = fopen(st->script->remote_log, "a");
    if (!f) {
        return;
    }
    text = params ? cJSON_PrintUnformatted(params) : NULL;
    fprintf(f, "%s|%s\n", name, text ? text : "{}");
    free(text);
    fclose(f);
}

static int64_t now_ms(void);

static void add_round(struct state *st, cJSON *o)
{
    cJSON_AddNumberToObject(o, "round", st->round);
    cJSON_AddBoolToObject(o, "open", st->round > 0 && now_ms() < st->round_until);
    cJSON_AddNumberToObject(o, "window_ms", 30000);
    if (st->round > 0) {
        cJSON_AddNumberToObject(o, "started_mono_ms", (double)(st->round_until - 30000));
        cJSON_AddNumberToObject(o, "until_mono_ms", (double)st->round_until);
    }
}

static cJSON *repeater_entry(struct state *st)
{
    cJSON *e = st->script->repeater_json ? cJSON_Parse(st->script->repeater_json) : NULL;

    if (cJSON_IsObject(e)) {
        cJSON_DeleteItemFromObject(e, "round");
        cJSON_DeleteItemFromObject(e, "current");
        cJSON_DeleteItemFromObject(e, "mono_ms");
        cJSON_AddNumberToObject(e, "round", st->round);
        cJSON_AddBoolToObject(e, "current", 1);
        cJSON_AddNumberToObject(e, "mono_ms", (double)now_ms());
    }
    return e;
}

static cJSON *session_obj(struct state *st)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "active", st->session_active);
    if (!st->session_active) {
        return o;
    }
    cJSON_AddStringToObject(o, "node", st->session_key);
    cJSON_AddBoolToObject(o, "known", 1);
    cJSON_AddNumberToObject(o, "type", 2);
    cJSON_AddStringToObject(o, "login", st->session_ok ? "ok" : "timeout");
    if (st->session_ok) {
        cJSON_AddBoolToObject(o, "legacy", 0);
        cJSON_AddBoolToObject(o, "admin", 1);
        cJSON_AddNumberToObject(o, "permissions", 1);
        cJSON_AddNumberToObject(o, "acl", 3);
        cJSON_AddNumberToObject(o, "firmware_level", 2);
        cJSON_AddNumberToObject(o, "repeater_clock", 1790000000.0);
    }
    cJSON_AddNumberToObject(o, "stale_replies", 0);
    cJSON_AddNumberToObject(o, "malformed_replies", 0);
    return o;
}

/* The mesh.remote event that ends request `id`. */
static void remote_event(struct state *st, int id, const char *kind, const char *outcome,
                         const cJSON *params)
{
    cJSON *data = cJSON_CreateObject();
    cJSON *reply = cJSON_AddObjectToObject(data, "reply");

    cJSON_AddNumberToObject(reply, "request_id", id);
    cJSON_AddStringToObject(reply, "kind", kind);
    cJSON_AddStringToObject(reply, "outcome", outcome);
    cJSON_AddStringToObject(reply, "node", st->session_key);
    cJSON_AddNumberToObject(reply, "mono_ms", (double)now_ms());
    if (strcmp(outcome, "replied") == 0) {
        cJSON_AddNumberToObject(reply, "snr_db", 7.25);
        cJSON_AddNumberToObject(reply, "rssi_dbm", -74);
        if (strcmp(kind, "status") == 0) {
            cJSON *s = cJSON_AddObjectToObject(reply, "status");

            cJSON_AddNumberToObject(s, "battery_mv", 4012);
            cJSON_AddNumberToObject(s, "tx_queue", 0);
            cJSON_AddNumberToObject(s, "noise_floor_dbm", -118);
            cJSON_AddNumberToObject(s, "last_rssi_dbm", -81);
            cJSON_AddNumberToObject(s, "last_snr_db", 6.5);
            cJSON_AddNumberToObject(s, "packets_recv", 15532);
            cJSON_AddNumberToObject(s, "packets_sent", 4410);
            cJSON_AddNumberToObject(s, "air_time_s", 3605);
            cJSON_AddNumberToObject(s, "uptime_s", 360500);
            cJSON_AddNumberToObject(s, "sent_flood", 4000);
            cJSON_AddNumberToObject(s, "sent_direct", 410);
            cJSON_AddNumberToObject(s, "recv_flood", 15000);
            cJSON_AddNumberToObject(s, "recv_direct", 532);
            cJSON_AddNumberToObject(s, "err_events", 0);
            cJSON_AddNumberToObject(s, "direct_dups", 3);
            cJSON_AddNumberToObject(s, "flood_dups", 211);
        } else if (strcmp(kind, "neighbours") == 0) {
            cJSON *nb = cJSON_AddObjectToObject(reply, "neighbours");
            cJSON *arr = cJSON_AddArrayToObject(nb, "entries");
            cJSON *e = cJSON_CreateObject();

            cJSON_AddNumberToObject(nb, "total", 2);
            cJSON_AddStringToObject(e, "prefix", "a19ac21e7d04");
            cJSON_AddStringToObject(e, "name", "OSLO-01");
            cJSON_AddBoolToObject(e, "known", 1);
            cJSON_AddNumberToObject(e, "heard_s_ago", 95);
            cJSON_AddNumberToObject(e, "snr_db", 9.5);
            cJSON_AddItemToArray(arr, e);
            e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "prefix", "77aa00bb11cc");
            cJSON_AddBoolToObject(e, "known", 0);
            cJSON_AddNumberToObject(e, "heard_s_ago", 3700);
            cJSON_AddNumberToObject(e, "snr_db", -4.25);
            cJSON_AddItemToArray(arr, e);
        } else if (strcmp(kind, "owner") == 0) {
            cJSON *o = cJSON_AddObjectToObject(reply, "owner");

            cJSON_AddStringToObject(o, "firmware", "v1.9.0");
            cJSON_AddStringToObject(o, "name", "HYTTA");
            cJSON_AddStringToObject(o, "owner", "bench");
        } else if (strcmp(kind, "cli") == 0) {
            const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(params, "command");
            char text[200];

            snprintf(text, sizeof(text), "-> %s\nOK", cJSON_IsString(cmd) ? cmd->valuestring : "");
            cJSON_AddStringToObject(reply, "text", text);
        }
    }
    cJSON_AddItemToObject(data, "session", session_obj(st));
    pocketipc_server_broadcast(st->server, pocketipc_event("mesh.remote", data));
}

/* Answers a repeater method; returns 0 when name is not one. */
static int remote_method(struct state *st, struct pocketipc_server *s,
                         struct pocketipc_client *c, const cJSON *id, const char *name,
                         const cJSON *params)
{
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(params, "node");
    cJSON *result;
    int rid;

    if (strcmp(name, "mesh.discover") != 0 && strcmp(name, "mesh.discovered") != 0 &&
        strncmp(name, "mesh.remote_", 12) != 0) {
        return 0;
    }
    remote_log(st, name, params);
    if (st->script->no_remote) {
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_UNKNOWN_METHOD,
                                                              "unknown method"));
        return 1;
    }
    result = cJSON_CreateObject();
    if (strcmp(name, "mesh.discover") == 0) {
        int open = st->round > 0 && now_ms() < st->round_until;

        if (!open) {
            st->round++;
            st->round_until = now_ms() + 30000;
        }
        cJSON_AddBoolToObject(result, "started", !open);
        add_round(st, result);
        pocketipc_server_reply(s, c, pocketipc_response(id, result));
        if (!open && st->script->repeater_json && !st->script->remote_silent) {
            cJSON *data = cJSON_CreateObject();

            cJSON_AddStringToObject(data, "reason", "reply");
            add_round(st, data);
            cJSON_AddItemToObject(data, "repeater", repeater_entry(st));
            pocketipc_server_broadcast(st->server, pocketipc_event("mesh.discover", data));
        }
        return 1;
    }
    if (strcmp(name, "mesh.discovered") == 0) {
        cJSON *arr = cJSON_CreateArray();

        add_round(st, result);
        if (st->round > 0 && st->script->repeater_json) {
            cJSON_AddItemToArray(arr, repeater_entry(st));
        }
        cJSON_AddNumberToObject(result, "count", cJSON_GetArraySize(arr));
        cJSON_AddItemToObject(result, "repeaters", arr);
    } else if (strcmp(name, "mesh.remote_session") == 0) {
        cJSON_Delete(result);
        result = session_obj(st);
    } else if (strcmp(name, "mesh.remote_logout") == 0) {
        cJSON_AddBoolToObject(result, "logged_out", st->session_active);
        cJSON_AddBoolToObject(result, "transmitted", 0);
        st->session_active = 0;
        st->session_ok = 0;
        cJSON_AddItemToObject(result, "session", session_obj(st));
    } else {
        const char *kind = strcmp(name, "mesh.remote_login") == 0 ? "login"
                           : strcmp(name, "mesh.remote_cli") == 0 ? "cli"
                                                                  : NULL;
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(params, "kind");

        if (!kind && cJSON_IsString(k)) {
            kind = k->valuestring;
        }
        if (!cJSON_IsString(node) || strlen(node->valuestring) != 64 || !kind) {
            cJSON_Delete(result);
            pocketipc_server_reply(s, c, pocketipc_error_response(
                                             id, POCKETIPC_ERR_INVALID_PARAMS,
                                             "node must be a whole public key, 64 hex characters"));
            return 1;
        }
        if (strcmp(kind, "login") != 0 &&
            (!st->session_ok || strcmp(st->session_key, node->valuestring) != 0)) {
            cJSON_Delete(result);
            pocketipc_server_reply(s, c, pocketipc_error_response(
                                             id, POCKETIPC_ERR_INVALID_PARAMS,
                                             "not logged in to that node"));
            return 1;
        }
        rid = ++st->remote_id;
        cJSON_AddBoolToObject(result, "accepted", 1);
        cJSON_AddNumberToObject(result, "request_id", rid);
        cJSON_AddStringToObject(result, "route", "flood");
        cJSON_AddNumberToObject(result, "wait_ms", 20000);
        if (strcmp(kind, "login") == 0) {
            const cJSON *pw = cJSON_GetObjectItemCaseSensitive(params, "password");

            st->session_active = 1;
            snprintf(st->session_key, sizeof(st->session_key), "%s", node->valuestring);
            st->session_ok = cJSON_IsString(pw) && strcmp(pw->valuestring, "hunter2") == 0;
        }
        pocketipc_server_reply(s, c, pocketipc_response(id, result));
        if (!st->script->remote_silent) {
            int lost = (strcmp(kind, "login") == 0 && !st->session_ok) ||
                       (strcmp(kind, "cli") == 0 && st->script->cli_timeout_first &&
                        st->cli_seen++ == 0);

            remote_event(st, rid, kind, lost ? "timeout" : "replied", params);
        }
        return 1;
    }
    pocketipc_server_reply(s, c, pocketipc_response(id, result));
    return 1;
}

static cJSON *channels_now(struct state *st)
{
    if (!st->channels) {
        st->channels = st->script->channels_json ? cJSON_Parse(st->script->channels_json)
                                                 : cJSON_CreateArray();
        if (!cJSON_IsArray(st->channels)) {
            cJSON_Delete(st->channels);
            st->channels = cJSON_CreateArray();
        }
    }
    return st->channels;
}

static void manage_log(struct state *st, const char *name, const cJSON *params)
{
    FILE *f;
    char *text;

    if (!st->script->manage_log) {
        return;
    }
    f = fopen(st->script->manage_log, "a");
    if (!f) {
        return;
    }
    text = params ? cJSON_PrintUnformatted(params) : NULL;
    fprintf(f, "%s|%s\n", name, text ? text : "{}");
    free(text);
    fclose(f);
}

static cJSON *identity_json(struct state *st)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddStringToObject(o, "public_key", SELF_KEY);
    cJSON_AddStringToObject(o, "node_hash", "5f");
    cJSON_AddStringToObject(o, "name", st->name[0] ? st->name : "K230-A");
    cJSON_AddStringToObject(o, "name_source",
                            (st->script->name_pinned && !st->renamed) ? "config" : "stored");
    cJSON_AddNumberToObject(o, "name_max", 31);
    return o;
}

static cJSON *path_hash_json(struct state *st)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *allowed = cJSON_CreateArray();
    int b;

    for (b = 1; b <= 3; b++) {
        cJSON_AddItemToArray(allowed, cJSON_CreateNumber(b));
    }
    cJSON_AddNumberToObject(o, "bytes", st->path_hash_bytes ? st->path_hash_bytes : 1);
    cJSON_AddItemToObject(o, "allowed", allowed);
    cJSON_AddNumberToObject(o, "default", 1);
    return o;
}

static int listed(char (*keys)[65], int count, const char *key)
{
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(keys[i], key) == 0) {
            return 1;
        }
    }
    return 0;
}

static void forget_route(cJSON *node)
{
    cJSON_DeleteItemFromObjectCaseSensitive(node, "hops");
    cJSON_DeleteItemFromObjectCaseSensitive(node, "direct");
    cJSON_DeleteItemFromObjectCaseSensitive(node, "path_hex");
    cJSON_DeleteItemFromObjectCaseSensitive(node, "path_known");
    cJSON_AddBoolToObject(node, "path_known", 0);
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static cJSON *status_json(const struct state *st)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *radio = cJSON_CreateObject();
    cJSON *counters = cJSON_CreateObject();

    cJSON_AddStringToObject(o, "state", st->script->radio_off ? "waiting_for_lease"
                                        : st->script->state ? st->script->state : "online");
    cJSON_AddStringToObject(o, "run_id", st->script->run_id ? st->script->run_id : "fake-run-1");
    cJSON_AddStringToObject(o, "reason", st->script->reason ? st->script->reason : "receiving");
    cJSON_AddNumberToObject(o, "state_since_mono_ms", 100);
    /* Grows with this process, the way meshcored's own does - it is
     * (now - start_ms) / 1000 there. A constant would be a service whose
     * clock had stopped, and a client that tells one run from the next by
     * this field would read the drift as a restart. */
    cJSON_AddNumberToObject(o, "uptime_s",
                            (double)((st->script->uptime_s ? st->script->uptime_s : 42) +
                                     (now_ms() - st->started_ms) / 1000));
    cJSON_AddBoolToObject(radio, "connected", 1);
    cJSON_AddBoolToObject(radio, "lease_held", 1);
    cJSON_AddBoolToObject(radio, "online", !st->script->radio_off);
    cJSON_AddStringToObject(radio, "radio_state", "rx");
    cJSON_AddItemToObject(o, "radio", radio);
    cJSON_AddNumberToObject(counters, "rx_events", 12);
    cJSON_AddNumberToObject(counters, "rx_delivered", 11);
    cJSON_AddNumberToObject(counters, "nodes_unretained", 0);
    cJSON_AddItemToObject(o, "counters", counters);
    cJSON_AddNumberToObject(o, "nodes", 2);
    return o;
}

/* A monotonic stamp in a fixture cannot be written as an absolute number:
 * CLOCK_MONOTONIC is the host's uptime, so a fixed value would be an age of
 * however long this machine has been running. A negative value is read as
 * "this long ago", which is what a fixture actually means and what makes a
 * screenshot of the ages reproducible. */
static void relative_to_now(cJSON *o, const char *field)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, field);
    double at;

    if (!cJSON_IsNumber(v) || v->valuedouble >= 0) {
        return;
    }
    /* The result may come out negative, which no real monotonic clock ever
     * reports: a host that booted two minutes ago has no timestamp four
     * hours old. It is left negative rather than clamped to zero, because
     * every reader of this field subtracts it from now, and clamping would
     * quietly turn "four hours ago" into "as long as this machine has been
     * up" - a wrong age, which is the one thing these fixtures exist to
     * check the client does not show. */
    at = (double)now_ms() + v->valuedouble;
    cJSON_SetNumberValue(v, at);
}

static cJSON *nodes_array(struct state *st)
{
    cJSON *arr = st->script->nodes_json ? cJSON_Parse(st->script->nodes_json) : NULL;
    cJSON *item;
    int i;

    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        arr = cJSON_CreateArray();
        return arr;
    }
    for (i = cJSON_GetArraySize(arr) - 1; i >= 0; i--) {
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, i),
                                                           "public_key");

        if (cJSON_IsString(key) && listed(st->removed, st->removed_count, key->valuestring)) {
            cJSON_DeleteItemFromArray(arr, i);
        }
    }
    cJSON_ArrayForEach (item, arr) {
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "public_key");

        relative_to_now(item, "last_heard_mono_ms");
        if (cJSON_IsString(key) && listed(st->unrouted, st->unrouted_count, key->valuestring)) {
            forget_route(item);
        }
    }
    return arr;
}

/* The node this service holds under exactly this whole key, or NULL. */
static cJSON *node_by_key(struct state *st, const char *key)
{
    cJSON *arr = nodes_array(st);
    cJSON *found = NULL;
    const cJSON *item;

    if (key && strlen(key) == 64) {
        cJSON_ArrayForEach (item, arr) {
            const cJSON *k = cJSON_GetObjectItemCaseSensitive(item, "public_key");

            if (cJSON_IsString(k) && strcmp(k->valuestring, key) == 0) {
                found = cJSON_Duplicate(item, 1);
                break;
            }
        }
    }
    cJSON_Delete(arr);
    return found;
}

static void on_request(struct pocketipc_server *s, struct pocketipc_client *c, cJSON *req,
                       void *user)
{
    struct state *st = user;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
    const char *name = (cJSON_IsString(method) && method->valuestring) ? method->valuestring : "";
    cJSON *result = NULL;

    if (st->script->method_log) {
        FILE *f = fopen(st->script->method_log, "a");

        if (f) {
            fprintf(f, "%s\n", name);
            fclose(f);
        }
    }
    if (remote_method(st, s, c, id, name, cJSON_GetObjectItemCaseSensitive(req, "params"))) {
        return;
    }
    if (strcmp(name, "mesh.info") == 0) {
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "service", "meshcored");
        cJSON_AddNumberToObject(result, "api_version", 0);
        cJSON_AddStringToObject(result, "version", "0.0.10");
        cJSON_AddStringToObject(result, "build", "fake");
        cJSON_AddStringToObject(result, "protocol", "meshcore");
    } else if (strcmp(name, "mesh.status") == 0) {
        result = status_json(st);
    } else if (strcmp(name, "mesh.identity") == 0) {
        result = identity_json(st);
    } else if (strcmp(name, "mesh.path_hash") == 0 && !st->script->no_path_hash) {
        result = path_hash_json(st);
    } else if (strcmp(name, "mesh.channel_add") == 0 ||
               strcmp(name, "mesh.channel_remove") == 0 || strcmp(name, "mesh.set_name") == 0 ||
               strcmp(name, "mesh.set_path_hash") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
        cJSON *table = channels_now(st);

        manage_log(st, name, params);
        if (st->script->manage_silent) {
            return;
        }
        if (strcmp(name, "mesh.channel_add") == 0) {
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(params, "name");
            const cJSON *key = cJSON_GetObjectItemCaseSensitive(params, "key");
            size_t klen = cJSON_IsString(key) ? strlen(key->valuestring) : 0;
            int used[8] = { 0 };
            const cJSON *item;
            cJSON *ch;
            cJSON *data;
            int slot = -1;
            int i;

            if (!cJSON_IsString(nm) || !nm->valuestring[0] || strlen(nm->valuestring) > 31) {
                pocketipc_server_reply(s, c, pocketipc_error_response(
                                                 id, POCKETIPC_ERR_INVALID_PARAMS,
                                                 "name must be 1 to 31 bytes"));
                return;
            }
            if (klen != 24 && klen != 44) {
                pocketipc_server_reply(s, c, pocketipc_error_response(
                                                 id, POCKETIPC_ERR_INVALID_PARAMS,
                                                 "key must be standard base64 decoding to "
                                                 "exactly 16 or 32 non-zero bytes"));
                return;
            }
            cJSON_ArrayForEach (item, table) {
                const cJSON *sl = cJSON_GetObjectItemCaseSensitive(item, "channel");

                if (cJSON_IsNumber(sl) && sl->valuedouble >= 0 && sl->valuedouble < 8) {
                    used[(int)sl->valuedouble] = 1;
                }
            }
            for (i = 0; i < 8; i++) {
                if (used[i] && strcmp(st->channel_key[i], key->valuestring) == 0) {
                    pocketipc_server_reply(s, c, pocketipc_error_response(
                                                     id, POCKETIPC_ERR_INVALID_PARAMS,
                                                     "that key is already a channel on this node"));
                    return;
                }
            }
            for (i = 0; i < 8 && slot < 0; i++) {
                if (!used[i]) {
                    slot = i;
                }
            }
            if (slot < 0) {
                pocketipc_server_reply(s, c, pocketipc_error_response(
                                                 id, POCKETIPC_ERR_BUSY,
                                                 "all 8 channel slots are taken"));
                return;
            }
            snprintf(st->channel_key[slot], sizeof(st->channel_key[slot]), "%s", key->valuestring);
            ch = cJSON_CreateObject();
            cJSON_AddNumberToObject(ch, "channel", slot);
            cJSON_AddStringToObject(ch, "name", nm->valuestring);
            cJSON_AddStringToObject(ch, "channel_hash", "a5");
            cJSON_AddNumberToObject(ch, "key_bits", klen == 24 ? 128 : 256);
            /* As the service does: the Public channel is known by its key. */
            if (strcmp(key->valuestring, "izOH6cXN6mrJ5e26oRXNcg==") == 0) {
                cJSON_AddStringToObject(ch, "well_known", "public");
            }
            cJSON_AddNumberToObject(ch, "text_limit", 147);
            cJSON_AddBoolToObject(ch, "ack_expected", 0);
            cJSON_AddItemToArray(table, cJSON_Duplicate(ch, 1));
            data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "reason", "added");
            cJSON_AddItemToObject(data, "channel", cJSON_Duplicate(ch, 1));
            pocketipc_server_broadcast(s, pocketipc_event("mesh.channel", data));
            result = ch;
        } else if (strcmp(name, "mesh.channel_remove") == 0) {
            const cJSON *sl = cJSON_GetObjectItemCaseSensitive(params, "channel");
            cJSON *gone = NULL;
            cJSON *data;
            int at = 0;
            const cJSON *item;

            cJSON_ArrayForEach (item, table) {
                const cJSON *x = cJSON_GetObjectItemCaseSensitive(item, "channel");

                if (cJSON_IsNumber(sl) && cJSON_IsNumber(x) && x->valuedouble == sl->valuedouble) {
                    gone = cJSON_DetachItemFromArray(table, at);
                    break;
                }
                at++;
            }
            if (!gone) {
                pocketipc_server_reply(s, c, pocketipc_error_response(
                                                 id, POCKETIPC_ERR_INVALID_PARAMS,
                                                 "no channel in that slot"));
                return;
            }
            data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "reason", "removed");
            cJSON_AddItemToObject(data, "channel", gone);
            pocketipc_server_broadcast(s, pocketipc_event("mesh.channel", data));
            result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "removed", 1);
            cJSON_AddNumberToObject(result, "channel", sl->valuedouble);
            cJSON_AddBoolToObject(result, "key_forgotten", 1);
        } else if (strcmp(name, "mesh.set_name") == 0) {
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(params, "name");

            if (!cJSON_IsString(nm) || !nm->valuestring[0] || strlen(nm->valuestring) > 31) {
                pocketipc_server_reply(s, c, pocketipc_error_response(
                                                 id, POCKETIPC_ERR_INVALID_PARAMS,
                                                 "name must be 1 to 31 bytes"));
                return;
            }
            snprintf(st->name, sizeof(st->name), "%s", nm->valuestring);
            st->renamed = 1;
            result = identity_json(st);
            cJSON_AddBoolToObject(result, "persisted", !st->script->rename_unsaved);
        } else {
            const cJSON *b = cJSON_GetObjectItemCaseSensitive(params, "bytes");

            if (!cJSON_IsNumber(b) || b->valuedouble < 1 || b->valuedouble > 3 ||
                b->valuedouble != (double)(int)b->valuedouble) {
                pocketipc_server_reply(s, c, pocketipc_error_response(
                                                 id, POCKETIPC_ERR_INVALID_PARAMS,
                                                 "bytes must be 1, 2 or 3"));
                return;
            }
            st->path_hash_bytes = (int)b->valuedouble;
            result = path_hash_json(st);
            cJSON_AddBoolToObject(result, "persisted", 1);
        }
    } else if (strcmp(name, "mesh.nodes") == 0) {
        cJSON *arr;

        if (st->script->refuse_nodes) {
            pocketipc_server_reply(s, c,
                                   pocketipc_error_response(id, POCKETIPC_ERR_BACKEND,
                                                            "the node table could not be read"));
            return;
        }
        arr = nodes_array(st);
        result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "count", cJSON_GetArraySize(arr));
        cJSON_AddItemToObject(result, "nodes", arr);
        st->snapshots_answered |= ANSWERED_NODES;
    } else if (strcmp(name, "mesh.node") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
        const cJSON *node = cJSON_GetObjectItemCaseSensitive(params, "node");
        cJSON *arr = nodes_array(st);
        const cJSON *item;

        result = NULL;
        cJSON_ArrayForEach (item, arr) {
            const cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "public_key");

            if (cJSON_IsString(key) && cJSON_IsString(node) &&
                strncmp(key->valuestring, node->valuestring, strlen(node->valuestring)) == 0) {
                result = cJSON_Duplicate(item, 1);
                break;
            }
        }
        cJSON_Delete(arr);
        if (!result) {
            pocketipc_server_reply(s, c,
                                   pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                            "no node matches that prefix"));
            return;
        }
    } else if (strcmp(name, "mesh.channels") == 0) {
        cJSON *arr = cJSON_Duplicate(channels_now(st), 1);

        result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "count", cJSON_GetArraySize(arr));
        cJSON_AddNumberToObject(result, "max", 8);
        /* The opposite of mesh.messages, and said for the same reason: a
         * client should not have to reboot to find out. */
        cJSON_AddBoolToObject(result, "persistent", 1);
        cJSON_AddItemToObject(result, "channels", arr);
        st->snapshots_answered |= ANSWERED_CHANNELS;
    } else if (strcmp(name, "mesh.messages") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
        const cJSON *limit = cJSON_GetObjectItemCaseSensitive(params, "limit");
        cJSON *arr = st->script->messages_json ? cJSON_Parse(st->script->messages_json)
                                               : cJSON_CreateArray();
        int total;

        if (!cJSON_IsArray(arr)) {
            cJSON_Delete(arr);
            arr = cJSON_CreateArray();
        }
        total = cJSON_GetArraySize(arr);
        {
            cJSON *item;

            /* Same rule as a node's last_heard: a negative stamp in a
             * fixture means "this long ago", so the ages on screen do not
             * depend on how long this host has been up. */
            cJSON_ArrayForEach (item, arr) {
                relative_to_now(item, "mono_ms");
                relative_to_now(item, "ack_mono_ms");
            }
        }
        /* With a limit, the newest that many, still oldest first - which is
         * what the real service does, and the thing a client that asked for
         * ten and got the oldest ten would get wrong. */
        if (cJSON_IsNumber(limit) && limit->valuedouble >= 0) {
            while (cJSON_GetArraySize(arr) > (int)limit->valuedouble) {
                cJSON_DeleteItemFromArray(arr, 0);
            }
        }
        result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "count", cJSON_GetArraySize(arr));
        cJSON_AddNumberToObject(result, "total", total);
        cJSON_AddBoolToObject(result, "persistent", 0);
        cJSON_AddItemToObject(result, "messages", arr);
        st->snapshots_answered |= ANSWERED_MESSAGES;
    } else if (strcmp(name, "mesh.send") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
        const cJSON *to = cJSON_GetObjectItemCaseSensitive(params, "to");
        const cJSON *chan = cJSON_GetObjectItemCaseSensitive(params, "channel");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(params, "text");

        /* The real service refuses both together rather than preferring one
         * (docs/api/mesh.md), and a client that wrote both would otherwise
         * be tested against something more forgiving than what it will meet. */
        if (to != NULL && chan != NULL) {
            pocketipc_server_reply(s, c,
                                   pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                            "give either to or channel"));
            return;
        }
        if (st->script->send_log && cJSON_IsString(text)) {
            FILE *f = fopen(st->script->send_log, "a");

            if (f) {
                if (cJSON_IsNumber(chan)) {
                    fprintf(f, "#%d|%s\n", (int)chan->valuedouble, text->valuestring);
                } else if (cJSON_IsString(to)) {
                    fprintf(f, "%s|%s\n", to->valuestring, text->valuestring);
                }
                fclose(f);
            }
        }
        if (st->script->refuse_send) {
            pocketipc_server_reply(s, c,
                                   pocketipc_error_response(id, POCKETIPC_ERR_BUSY,
                                                            "the radio is not available"));
            return;
        }
        st->sent++;
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "accepted", 1);
        cJSON_AddNumberToObject(result, "message_id", 1000 + st->sent);
        cJSON_AddStringToObject(result, "route", "flood");
        if (cJSON_IsNumber(chan)) {
            /* No ack_timeout_ms for a channel: there is no ACK to time out. */
            cJSON_AddNumberToObject(result, "channel", chan->valuedouble);
            cJSON_AddBoolToObject(result, "ack_expected", 0);
        } else {
            cJSON_AddNumberToObject(result, "ack_timeout_ms", 30000);
            cJSON_AddBoolToObject(result, "ack_expected", 1);
        }
        pocketipc_server_reply(s, c, pocketipc_response(id, result));
        /* The real service raises mesh.message for a message it has taken,
         * and again when its state changes. Scripted silence is the other
         * case a client must survive: an id it was given and never heard
         * about again. */
        if (!st->script->send_is_silent && cJSON_IsString(to) && cJSON_IsString(text)) {
            cJSON *data = cJSON_CreateObject();
            cJSON *msg = cJSON_CreateObject();

            cJSON_AddNumberToObject(msg, "id", 1000 + st->sent);
            cJSON_AddStringToObject(msg, "direction", "out");
            cJSON_AddStringToObject(msg, "peer_public_key", to->valuestring);
            cJSON_AddStringToObject(msg, "text", text->valuestring);
            cJSON_AddStringToObject(msg, "state", "sent_flood");
            cJSON_AddNumberToObject(msg, "mono_ms", (double)now_ms());
            cJSON_AddItemToObject(data, "message", msg);
            pocketipc_server_broadcast(s, pocketipc_event("mesh.message", data));
        }
        return;
    } else if (strcmp(name, "mesh.advert") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
        const cJSON *zero = cJSON_GetObjectItemCaseSensitive(params, "zero_hop");
        int zero_hop = cJSON_IsTrue(zero);

        if (zero != NULL && !cJSON_IsBool(zero)) {
            pocketipc_server_reply(s, c,
                                   pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                            "zero_hop must be true or false"));
            return;
        }
        if (st->script->advert_log) {
            FILE *f = fopen(st->script->advert_log, "a");

            if (f) {
                fprintf(f, "%s\n", zero_hop ? "zero_hop" : "flood");
                fclose(f);
            }
        }
        if (st->script->refuse_advert) {
            pocketipc_server_reply(s, c,
                                   pocketipc_error_response(id, POCKETIPC_ERR_BUSY,
                                                            "the radio is not available"));
            return;
        }
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "accepted", 1);
        cJSON_AddStringToObject(result, "route", zero_hop ? "zero_hop" : "flood");
    } else if (strcmp(name, "mesh.node_remove") == 0 ||
               strcmp(name, "mesh.node_reset_path") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(params, "node");
        int remove = strcmp(name, "mesh.node_remove") == 0;
        cJSON *node;
        cJSON *data;

        if (st->script->node_ops_silent) {
            return;
        }
        node = cJSON_IsString(key) ? node_by_key(st, key->valuestring) : NULL;
        if (!node) {
            pocketipc_server_reply(s, c,
                                   pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                            "no node with that public key is "
                                                            "held"));
            return;
        }
        if (remove) {
            if (st->removed_count < 8) {
                snprintf(st->removed[st->removed_count++], sizeof(st->removed[0]), "%s",
                         key->valuestring);
            }
            result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "removed", 1);
            cJSON_AddItemToObject(result, "node", cJSON_Duplicate(node, 1));
        } else {
            if (st->unrouted_count < 8) {
                snprintf(st->unrouted[st->unrouted_count++], sizeof(st->unrouted[0]), "%s",
                         key->valuestring);
            }
            forget_route(node);
            result = cJSON_Duplicate(node, 1);
        }
        /* The event every subscriber gets, BEFORE the answer: the real
         * service raises it from inside the runtime call that does the work
         * (forgetNode, resetPath) and replies once that call has returned. */
        data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "reason", remove ? "removed" : "path");
        cJSON_AddItemToObject(data, "node", node);
        pocketipc_server_broadcast(s, pocketipc_event("mesh.node", data));
        pocketipc_server_reply(s, c, pocketipc_response(id, result));
        return;
    } else if (strcmp(name, "mesh.app_send") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
        const cJSON *to = cJSON_GetObjectItemCaseSensitive(params, "to");
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(params, "port");
        const cJSON *payload = cJSON_GetObjectItemCaseSensitive(params, "payload_hex");

        /* The same refusals as the real service, so a client is not tested
         * against something more forgiving than what it will meet. */
        if (!cJSON_IsString(to) || strlen(to->valuestring) != 64 || !cJSON_IsNumber(port) ||
            port->valueint < 1 || port->valueint > 15 || !cJSON_IsString(payload) ||
            strlen(payload->valuestring) == 0 || strlen(payload->valuestring) % 2 != 0 ||
            strlen(payload->valuestring) > 320) {
            pocketipc_server_reply(s, c,
                                   pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                            "bad app datagram"));
            return;
        }
        if (st->script->app_log) {
            FILE *f = fopen(st->script->app_log, "a");

            if (f) {
                fprintf(f, "%s|%d|%s\n", to->valuestring, port->valueint, payload->valuestring);
                fclose(f);
            }
        }
        if (st->script->refuse_app_send) {
            pocketipc_server_reply(s, c,
                                   pocketipc_error_response(id, POCKETIPC_ERR_BUSY,
                                                            "the radio is not available"));
            return;
        }
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "accepted", 1);
        cJSON_AddStringToObject(result, "route", "flood");
        cJSON_AddNumberToObject(result, "est_timeout_ms",
                                st->script->app_est_timeout_ms ? st->script->app_est_timeout_ms
                                                               : 9000);
        cJSON_AddNumberToObject(result, "bytes", (double)(strlen(payload->valuestring) / 2));
    } else if (strcmp(name, "mesh.app_inbox") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(params, "port");
        const cJSON *after = cJSON_GetObjectItemCaseSensitive(params, "after_id");
        cJSON *all = st->script->app_inbox_json ? cJSON_Parse(st->script->app_inbox_json) : NULL;
        cJSON *arr = cJSON_CreateArray();
        cJSON *item;

        if (!cJSON_IsNumber(port) || port->valueint < 1 || port->valueint > 15) {
            cJSON_Delete(all);
            cJSON_Delete(arr);
            pocketipc_server_reply(s, c,
                                   pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                            "port must be 1 to 15"));
            return;
        }
        cJSON_ArrayForEach (item, all) {
            const cJSON *ip = cJSON_GetObjectItemCaseSensitive(item, "port");
            const cJSON *iid = cJSON_GetObjectItemCaseSensitive(item, "id");

            if (cJSON_IsNumber(ip) && ip->valueint == port->valueint && cJSON_IsNumber(iid) &&
                iid->valuedouble > (cJSON_IsNumber(after) ? after->valuedouble : 0)) {
                cJSON *copy = cJSON_Duplicate(item, 1);

                relative_to_now(copy, "mono_ms");
                cJSON_AddItemToArray(arr, copy);
            }
        }
        cJSON_Delete(all);
        result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "count", cJSON_GetArraySize(arr));
        cJSON_AddItemToObject(result, "datagrams", arr);
        st->inbox_answered = 1;
    } else if (strcmp(name, "mesh.subscribe") == 0) {
        pocketipc_client_set_subscribed(c, true);
        st->subscribed = 1;
        st->subscribe_ms = now_ms();
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "subscribed", 1);
    } else if (strcmp(name, "mesh.unsubscribe") == 0) {
        pocketipc_client_set_subscribed(c, false);
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "subscribed", 0);
    } else {
        pocketipc_server_reply(s, c,
                               pocketipc_error_response(id, POCKETIPC_ERR_UNKNOWN_METHOD,
                                                        "unknown method"));
        return;
    }
    pocketipc_server_reply(s, c, pocketipc_response(id, result));
}

static void on_disconnect(struct pocketipc_server *s, uint64_t client_id, void *user)
{
    struct state *st = user;

    (void)s;
    (void)client_id;
    st->clients_gone++;
    st->subscribed = 0;
}

/* "<event>|<json>" -> one broadcast frame. */
static void raise_event(struct state *st, const char *spec)
{
    const char *bar = strchr(spec, '|');
    char name[48];
    cJSON *data;
    size_t n;

    if (!bar) {
        return;
    }
    n = (size_t)(bar - spec);
    if (n >= sizeof(name)) {
        n = sizeof(name) - 1;
    }
    memcpy(name, spec, n);
    name[n] = '\0';
    data = cJSON_Parse(bar + 1);
    if (data) {
        cJSON *node = cJSON_GetObjectItemCaseSensitive(data, "node");
        cJSON *dg = cJSON_GetObjectItemCaseSensitive(data, "datagram");

        relative_to_now(data, "mono_ms");
        if (cJSON_IsObject(dg)) {
            relative_to_now(dg, "mono_ms");
        }
        if (cJSON_IsObject(node)) {
            relative_to_now(node, "last_heard_mono_ms");
        }
    }
    pocketipc_server_broadcast(st->server, pocketipc_event(name, data));
}

int fake_meshcored_run(const struct fake_meshcored_script *script)
{
    struct state st;
    int64_t started = now_ms();

    memset(&st, 0, sizeof(st));
    st.script = script;
    st.started_ms = started;
    signal(SIGPIPE, SIG_IGN);
    st.server = pocketipc_server_new("meshcored", on_request, &st);
    if (!st.server) {
        return 1;
    }
    pocketipc_server_set_on_disconnect(st.server, on_disconnect, &st);
    for (;;) {
        pocketipc_server_poll(st.server, 10);
        /* Replies are written as each request is handled, so an event raised
         * here, after the last snapshot's reply went out, reaches the client
         * after it too. */
        if (st.subscribed && script->events &&
            (!script->events_after_snapshot || st.snapshots_answered == ANSWERED_ALL) &&
            (!script->events_after_inbox || st.inbox_answered)) {
            /* One per pass, so the client gets them as separate frames and
             * a burst is still a burst. */
            if (script->events[st.events_sent]) {
                raise_event(&st, script->events[st.events_sent]);
                st.events_sent++;
            } else if (script->junk_frame && !st.raw_sent) {
                pocketipc_server_broadcast(st.server, cJSON_Parse(script->junk_frame));
                st.raw_sent = 1;
            }
        }
        if (script->serve_clients && st.clients_gone >= script->serve_clients) {
            break;
        }
        if (script->life_ms && now_ms() - started >= script->life_ms) {
            break;
        }
    }
    pocketipc_server_free(st.server);
    return 0;
}

pid_t fake_meshcored_spawn(const struct fake_meshcored_script *script)
{
    pid_t parent = getpid();
    pid_t pid = fork();

    if (pid == 0) {
        /* A script's life_ms is a backstop, and a test gives it a long one so
         * that no service ends under a check still waiting for it. A test that
         * dies before stopping it must not leave it running that long. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) {
            _exit(0);
        }
        _exit(fake_meshcored_run(script));
    }
    return pid;
}

int fake_meshcored_wait_ready(int timeout_ms)
{
    char path[512];
    int waited = 0;

    if (pocketipc_socket_path("meshcored", path, sizeof(path)) != 0) {
        return 0;
    }
    while (waited < timeout_ms) {
        struct stat sb;

        if (stat(path, &sb) == 0) {
            return 1;
        }
        usleep(5000);
        waited += 5;
    }
    return 0;
}

void fake_meshcored_stop(pid_t pid)
{
    char path[512];
    int status;

    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
    /* Killed, it never removed its socket. Left there, the next service's
     * fake_meshcored_wait_ready would find it at once and return before that
     * service was listening, and a client that connected then would be
     * refused and back off. */
    if (pocketipc_socket_path("meshcored", path, sizeof(path)) == 0) {
        unlink(path);
    }
}
