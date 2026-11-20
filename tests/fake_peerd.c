/*
 * A peerd that is not peerd. See fake_peerd.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fake_peerd.h"

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

#define KEPT_MAX 4

struct state {
    const struct fake_peerd_script *script;
    struct pocketipc_server *server;
    int subscribed;
    int inbox_answered;
    int events_sent;
    int visible;
    char kept[KEPT_MAX][65];
    int kept_count;
    int probes;                 /* how many have been asked; the last one is listed */
    char probe_address[32];
};

static const char *name_of(const struct fake_peerd_script *script)
{
    return script->service ? script->service : "peerd";
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void relative_to_now(cJSON *o, const char *field)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, field);

    if (cJSON_IsNumber(v) && v->valuedouble < 0) {
        cJSON_SetNumberValue(v, (double)now_ms() + v->valuedouble);
    }
}

static void log_request(const struct state *st, const char *name, const cJSON *params)
{
    FILE *f;
    char *text;

    if (!st->script->log || !(f = fopen(st->script->log, "a"))) {
        return;
    }
    text = params ? cJSON_PrintUnformatted(params) : NULL;
    fprintf(f, "%s|%s\n", name, text ? text : "{}");
    free(text);
    fclose(f);
}

static int kept_find(const struct state *st, const char *key)
{
    int i;

    for (i = 0; i < st->kept_count; i++) {
        if (strcmp(st->kept[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

static void fail(struct pocketipc_server *s, struct pocketipc_client *c, const cJSON *id, int code,
                 const char *why)
{
    pocketipc_server_reply(s, c, pocketipc_error_response(id, code, why));
}

static void on_request(struct pocketipc_server *s, struct pocketipc_client *c, cJSON *req,
                       void *user)
{
    struct state *st = user;
    const struct fake_peerd_script *sc = st->script;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    const char *name = cJSON_IsString(method) && method->valuestring ? method->valuestring : "";
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(params, "key");
    int ready = !sc->state || strcmp(sc->state, "ready") == 0;
    cJSON *result = cJSON_CreateObject();

    log_request(st, name, params);
    if (strcmp(name, "peer.status") == 0) {
        cJSON_AddStringToObject(result, "state", sc->state ? sc->state : "ready");
        cJSON_AddStringToObject(result, "run_id", sc->run_id ? sc->run_id : "00000000000000a1");
        cJSON_AddNumberToObject(result, "port", 47230);
        cJSON_AddBoolToObject(result, "socket", ready && (st->visible || st->kept_count > 0));
        cJSON_AddBoolToObject(result, "kept_fault", 0);
        if (ready) {
            cJSON *ifs = cJSON_AddArrayToObject(result, "interfaces");

            cJSON_AddBoolToObject(result, "visible", st->visible);
            cJSON_AddNumberToObject(result, "epoch", 7);
            if (!sc->no_interfaces) {
                cJSON *one = cJSON_CreateObject();

                cJSON_AddStringToObject(one, "address", "192.168.1.23");
                cJSON_AddStringToObject(one, "broadcast", "192.168.1.255");
                cJSON_AddItemToArray(ifs, one);
            }
        }
    } else if (strcmp(name, "peer.subscribe") == 0) {
        pocketipc_client_set_subscribed(c, true);
        st->subscribed = 1;
        cJSON_AddBoolToObject(result, "subscribed", 1);
    } else if (strcmp(name, "peer.unsubscribe") == 0) {
        pocketipc_client_set_subscribed(c, false);
        cJSON_AddBoolToObject(result, "subscribed", 0);
    } else if (!ready) {
        /* As the real one: closed by a fault, everything else fails naming it. */
        cJSON_Delete(result);
        fail(s, c, id, POCKETIPC_ERR_BACKEND, sc->state);
        return;
    } else if (strcmp(name, "peer.identity") == 0) {
        cJSON_AddStringToObject(result, "key", FAKE_PEERD_SELF);
        cJSON_AddStringToObject(result, "fingerprint", "5e00-0000");
        cJSON_AddStringToObject(result, "name", "K230-A");
    } else if (strcmp(name, "peer.set_visible") == 0) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, "visible");

        if (!cJSON_IsBool(v)) {
            cJSON_Delete(result);
            fail(s, c, id, POCKETIPC_ERR_INVALID_PARAMS, "visible must be true or false");
            return;
        }
        st->visible = cJSON_IsTrue(v);
        cJSON_AddBoolToObject(result, "visible", st->visible);
        cJSON_AddNumberToObject(result, "lease_ms", st->visible ? 30000 : 0);
    } else if (strcmp(name, "peer.nodes") == 0) {
        cJSON *arr = sc->nodes_json ? cJSON_Parse(sc->nodes_json) : NULL;

        if (!cJSON_IsArray(arr)) {
            cJSON_Delete(arr);
            arr = cJSON_CreateArray();
        }
        cJSON_AddNumberToObject(result, "count", cJSON_GetArraySize(arr));
        cJSON_AddItemToObject(result, "nodes", arr);
    } else if (strcmp(name, "peer.app_inbox") == 0) {
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(params, "port");
        const cJSON *after = cJSON_GetObjectItemCaseSensitive(params, "after_id");
        cJSON *all = sc->inbox_json ? cJSON_Parse(sc->inbox_json) : NULL;
        cJSON *arr = cJSON_CreateArray();
        cJSON *item;

        cJSON_ArrayForEach (item, all) {
            const cJSON *ip = cJSON_GetObjectItemCaseSensitive(item, "port");
            const cJSON *iid = cJSON_GetObjectItemCaseSensitive(item, "id");

            if (cJSON_IsNumber(port) && cJSON_IsNumber(ip) && ip->valueint == port->valueint &&
                cJSON_IsNumber(iid) &&
                iid->valuedouble > (cJSON_IsNumber(after) ? after->valuedouble : 0)) {
                cJSON *copy = cJSON_Duplicate(item, 1);

                relative_to_now(copy, "mono_ms");
                cJSON_AddItemToArray(arr, copy);
            }
        }
        cJSON_Delete(all);
        cJSON_AddNumberToObject(result, "count", cJSON_GetArraySize(arr));
        cJSON_AddItemToObject(result, "datagrams", arr);
        st->inbox_answered = 1;
    } else if (strcmp(name, "peer.app_send") == 0) {
        const cJSON *to = cJSON_GetObjectItemCaseSensitive(params, "to");
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(params, "port");
        const cJSON *payload = cJSON_GetObjectItemCaseSensitive(params, "payload_hex");

        if (!cJSON_IsString(to) || strlen(to->valuestring) != 64 || !cJSON_IsNumber(port) ||
            port->valueint < 1 || port->valueint > 15 || !cJSON_IsString(payload) ||
            strlen(payload->valuestring) == 0 || strlen(payload->valuestring) % 2 != 0) {
            cJSON_Delete(result);
            fail(s, c, id, POCKETIPC_ERR_INVALID_PARAMS, "bad app datagram");
            return;
        }
        if (sc->refuse_send) {
            cJSON_Delete(result);
            fail(s, c, id, POCKETIPC_ERR_BUSY, "no address for that node yet");
            return;
        }
        cJSON_AddBoolToObject(result, "accepted", 1);
        cJSON_AddStringToObject(result, "route", "lan");
        cJSON_AddNumberToObject(result, "est_timeout_ms", 500);
        cJSON_AddNumberToObject(result, "bytes", (double)(strlen(payload->valuestring) / 2));
    } else if (strcmp(name, "peer.kept") == 0) {
        cJSON *arr = cJSON_AddArrayToObject(result, "kept");
        int i;

        for (i = 0; i < st->kept_count; i++) {
            cJSON *one = cJSON_CreateObject();

            cJSON_AddStringToObject(one, "key", st->kept[i]);
            cJSON_AddBoolToObject(one, "floor", 0);
            cJSON_AddItemToArray(arr, one);
        }
        cJSON_AddNumberToObject(result, "count", st->kept_count);
        cJSON_AddBoolToObject(result, "kept_fault", 0);
    } else if (strcmp(name, "peer.keep") == 0 || strcmp(name, "peer.release") == 0) {
        int keep = strcmp(name, "peer.keep") == 0;
        int at;

        if (!cJSON_IsString(key) || strlen(key->valuestring) != 64 ||
            strcmp(key->valuestring, FAKE_PEERD_SELF) == 0) {
            cJSON_Delete(result);
            fail(s, c, id, POCKETIPC_ERR_INVALID_PARAMS, "not a usable key");
            return;
        }
        at = kept_find(st, key->valuestring);
        if (keep && sc->refuse_keep) {
            cJSON_Delete(result);
            fail(s, c, id, POCKETIPC_ERR_BACKEND, "the kept list could not be written");
            return;
        }
        if (keep && at < 0 && st->kept_count == KEPT_MAX) {
            cJSON_Delete(result);
            fail(s, c, id, 3, "four peers are kept already");
            return;
        }
        if (!keep && at < 0) {
            cJSON_Delete(result);
            fail(s, c, id, POCKETIPC_ERR_INVALID_PARAMS, "that key is not kept");
            return;
        }
        if (keep && at < 0) {
            snprintf(st->kept[st->kept_count++], sizeof(st->kept[0]), "%s", key->valuestring);
        } else if (!keep) {
            memmove(st->kept[at], st->kept[at + 1],
                    (size_t)(st->kept_count - at - 1) * sizeof(st->kept[0]));
            st->kept_count--;
        }
        cJSON_AddBoolToObject(result, keep ? "kept" : "released", 1);
    } else if (strcmp(name, "peer.probe") == 0) {
        const cJSON *addr = cJSON_GetObjectItemCaseSensitive(params, "address");

        if (!cJSON_IsString(addr)) {
            cJSON_Delete(result);
            fail(s, c, id, POCKETIPC_ERR_INVALID_PARAMS, "address must be dotted IPv4");
            return;
        }
        if (!st->visible) {
            cJSON_Delete(result);
            fail(s, c, id, 3, "not visible");
            return;
        }
        st->probes++;
        snprintf(st->probe_address, sizeof(st->probe_address), "%s", addr->valuestring);
        cJSON_AddNumberToObject(result, "probe_id", st->probes);
    } else if (strcmp(name, "peer.probes") == 0) {
        cJSON *arr = cJSON_AddArrayToObject(result, "probes");

        if (st->probes > 0) {
            cJSON *one = cJSON_CreateObject();
            int answered = sc->probe_address && strcmp(sc->probe_address, st->probe_address) == 0;

            cJSON_AddNumberToObject(one, "probe_id", st->probes);
            cJSON_AddStringToObject(one, "address", st->probe_address);
            cJSON_AddNumberToObject(one, "port", 47230);
            cJSON_AddStringToObject(one, "state", sc->probe_pending ? "pending"
                                                  : answered ? "answered" : "no_answer");
            if (answered && !sc->probe_pending) {
                cJSON_AddStringToObject(one, "key", sc->probe_key);
            }
            cJSON_AddItemToArray(arr, one);
        }
    } else {
        cJSON_Delete(result);
        fail(s, c, id, POCKETIPC_ERR_UNKNOWN_METHOD, "unknown method");
        return;
    }
    pocketipc_server_reply(s, c, pocketipc_response(id, result));
}

static void on_disconnect(struct pocketipc_server *s, uint64_t client_id, void *user)
{
    struct state *st = user;

    (void)s;
    (void)client_id;
    st->subscribed = 0;
    st->inbox_answered = 0;
}

int fake_peerd_run(const struct fake_peerd_script *script)
{
    struct state st;
    int64_t started = now_ms();
    const char *k = script->kept;

    memset(&st, 0, sizeof(st));
    st.script = script;
    while (k && strlen(k) >= 64 && st.kept_count < KEPT_MAX) {
        memcpy(st.kept[st.kept_count], k, 64);
        st.kept[st.kept_count++][64] = '\0';
        k += 64;
        if (*k == ',') {
            k++;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    st.server = pocketipc_server_new(name_of(script), on_request, &st);
    if (!st.server) {
        return 1;
    }
    pocketipc_server_set_on_disconnect(st.server, on_disconnect, &st);
    for (;;) {
        pocketipc_server_poll(st.server, 10);
        if (st.subscribed && st.inbox_answered && script->events &&
            script->events[st.events_sent]) {
            cJSON *data = cJSON_Parse(script->events[st.events_sent++]);

            if (data) {
                relative_to_now(data, "mono_ms");
            }
            pocketipc_server_broadcast(st.server, pocketipc_event("peer.app", data));
        }
        if (script->life_ms && now_ms() - started >= script->life_ms) {
            break;
        }
    }
    pocketipc_server_free(st.server);
    return 0;
}

pid_t fake_peerd_spawn(const struct fake_peerd_script *script)
{
    pid_t parent = getpid();
    pid_t pid = fork();

    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) {
            _exit(0);
        }
        _exit(fake_peerd_run(script));
    }
    return pid;
}

int fake_peerd_wait_ready(const struct fake_peerd_script *script, int timeout_ms)
{
    char path[512];
    int waited = 0;

    if (pocketipc_socket_path(name_of(script), path, sizeof(path)) != 0) {
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

void fake_peerd_stop(const struct fake_peerd_script *script, pid_t pid)
{
    char path[512];
    int status;

    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
    /* Killed, it never removed its socket; the next one's wait_ready must
     * not find this one's. */
    if (pocketipc_socket_path(name_of(script), path, sizeof(path)) == 0) {
        unlink(path);
    }
}
