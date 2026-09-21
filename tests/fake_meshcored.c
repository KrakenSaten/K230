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
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SELF_KEY "5f000000000000000000000000000000000000000000000000000000000000ff"

struct state {
    const struct fake_meshcored_script *script;
    struct pocketipc_server *server;
    int subscribed;
    int events_sent;
    int raw_sent;
    int clients_gone;
    int sent;
    int64_t subscribe_ms;
    int64_t started_ms;
};

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

    cJSON_AddStringToObject(o, "state", st->script->state ? st->script->state : "online");
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
    cJSON_AddBoolToObject(radio, "online", 1);
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

static cJSON *nodes_array(const struct state *st)
{
    cJSON *arr = st->script->nodes_json ? cJSON_Parse(st->script->nodes_json) : NULL;
    cJSON *item;

    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        arr = cJSON_CreateArray();
        return arr;
    }
    cJSON_ArrayForEach (item, arr) {
        relative_to_now(item, "last_heard_mono_ms");
    }
    return arr;
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
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "public_key", SELF_KEY);
        cJSON_AddStringToObject(result, "node_hash", "5f");
        cJSON_AddStringToObject(result, "name", "K230-A");
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
        cJSON *arr = st->script->channels_json ? cJSON_Parse(st->script->channels_json)
                                               : cJSON_CreateArray();

        if (!cJSON_IsArray(arr)) {
            cJSON_Delete(arr);
            arr = cJSON_CreateArray();
        }
        result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "count", cJSON_GetArraySize(arr));
        cJSON_AddNumberToObject(result, "max", 8);
        /* The opposite of mesh.messages, and said for the same reason: a
         * client should not have to reboot to find out. */
        cJSON_AddBoolToObject(result, "persistent", 1);
        cJSON_AddItemToObject(result, "channels", arr);
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

        relative_to_now(data, "mono_ms");
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
        if (st.subscribed && script->events) {
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
    pid_t pid = fork();

    if (pid == 0) {
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
    int status;

    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
}
