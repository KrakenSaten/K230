/*
 * netd: PocketOS network service. v0 serves wifi.* for one wireless
 * interface (docs/api/network.md): it starts and owns wpa_supplicant and the
 * DHCP client for that interface, and keeps the networks a person has joined
 * (docs/decisions/ADR-003-wifi-credentials.md). Ethernet is left to the
 * vendor's ifupdown, as before.
 *
 * Every request answers at once; anything that takes time is reported
 * through wifi.status as it progresses (wifi_mgr.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketipc/pocketipc.h"
#include "pocketipc/server.h"
#include "pocketlog/pocketlog.h"
#include "pocketpaths.h"
#include "wifi_mgr.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

#define NETD_API_VERSION 0

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static long mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- parameters ------------------------------------------------------------ */

static int reply_error(struct pocketipc_server *s, struct pocketipc_client *c, const cJSON *id,
                       int code, const char *msg)
{
    pocketipc_server_reply(s, c, pocketipc_error_response(id, code, msg));
    return -1;
}

/* Every key of params must be in allowed (NULL-terminated). A method that
 * changes the machine does not act on a request it does not fully
 * understand, the rule system.reboot set. */
static int only_keys(const cJSON *params, const char *const allowed[], char *bad, size_t n)
{
    const cJSON *item;

    if (!params || cJSON_IsNull(params)) {
        return 0;
    }
    if (!cJSON_IsObject(params)) {
        snprintf(bad, n, "params must be an object");
        return -1;
    }
    cJSON_ArrayForEach(item, params) {
        size_t i;
        int ok = 0;

        for (i = 0; allowed[i]; i++) {
            if (item->string && strcmp(item->string, allowed[i]) == 0) {
                ok = 1;
            }
        }
        if (!ok) {
            snprintf(bad, n, "unknown parameter %.40s", item->string ? item->string : "?");
            return -1;
        }
    }
    return 0;
}

static const cJSON *param(const cJSON *params, const char *key)
{
    return params && cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
}

/* An optional boolean: absent is 0. Returns 0, or -1 when present but not a
 * boolean. */
static int opt_bool(const cJSON *params, const char *key, int *out)
{
    const cJSON *v = param(params, key);

    *out = 0;
    if (!v) {
        return 0;
    }
    if (!cJSON_IsBool(v)) {
        return -1;
    }
    *out = cJSON_IsTrue(v);
    return 0;
}

/* ssid (UTF-8 text, 1..32 bytes) or ssid_hex (2..64 hex digits), exactly one. */
static int ssid_param(const cJSON *params, struct wifi_ssid *out, char *err, size_t n)
{
    const cJSON *text = param(params, "ssid");
    const cJSON *hex = param(params, "ssid_hex");

    if ((text != NULL) == (hex != NULL)) {
        snprintf(err, n, "exactly one of ssid or ssid_hex is required");
        return -1;
    }
    if (text) {
        if (!cJSON_IsString(text) || wifi_ssid_from_text(text->valuestring, out) < 0) {
            snprintf(err, n, "ssid must be a string of 1..32 bytes");
            return -1;
        }
    } else if (!cJSON_IsString(hex) || wifi_ssid_from_hex(hex->valuestring, out) < 0) {
        snprintf(err, n, "ssid_hex must be 2..64 hex digits");
        return -1;
    }
    if (wifi_ssid_is_hidden(out)) {
        snprintf(err, n, "ssid must not be empty or all zero bytes");
        return -1;
    }
    return 0;
}

/* ---- requests ---------------------------------------------------------------- */

static void on_request(struct pocketipc_server *s, struct pocketipc_client *c, cJSON *req,
                       void *user)
{
    static const char *const no_keys[] = { NULL };
    static const char *const enabled_keys[] = { "enabled", NULL };
    static const char *const connect_keys[] = { "ssid", "ssid_hex", "passphrase", "hidden",
                                                "security", "allow_open", NULL };
    static const char *const forget_keys[] = { "ssid", "ssid_hex", NULL };
    struct wifi_mgr *m = user;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *mth = cJSON_GetObjectItemCaseSensitive(req, "method");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    const char *method = cJSON_IsString(mth) ? mth->valuestring : NULL;
    long now = mono_ms();
    cJSON *result = NULL;
    char err[160];
    int code = 0;

    err[0] = '\0';
    if (!method) {
        reply_error(s, c, id, POCKETIPC_ERR_INVALID_PARAMS, "missing method");
        return;
    }
    /* The method name only: a request can carry a passphrase. */
    LOG_DEBUG("fd %d %s", pocketipc_client_fd(c), method);

    if (strcmp(method, "wifi.status") == 0) {
        code = wifi_mgr_status(m, &result, err, sizeof(err));
    } else if (strcmp(method, "wifi.networks") == 0) {
        code = wifi_mgr_networks(m, now, &result, err, sizeof(err));
    } else if (strcmp(method, "wifi.saved") == 0) {
        code = wifi_mgr_saved(m, &result, err, sizeof(err));
    } else if (strcmp(method, "wifi.scan") == 0 || strcmp(method, "wifi.disconnect") == 0) {
        if (only_keys(params, no_keys, err, sizeof(err)) < 0) {
            code = POCKETIPC_ERR_INVALID_PARAMS;
        } else if (strcmp(method, "wifi.scan") == 0) {
            code = wifi_mgr_scan(m, now, &result, err, sizeof(err));
        } else {
            code = wifi_mgr_disconnect(m, now, &result, err, sizeof(err));
        }
    } else if (strcmp(method, "wifi.set_enabled") == 0) {
        const cJSON *on = param(params, "enabled");

        if (only_keys(params, enabled_keys, err, sizeof(err)) < 0) {
            code = POCKETIPC_ERR_INVALID_PARAMS;
        } else if (!cJSON_IsBool(on)) {
            snprintf(err, sizeof(err), "enabled must be true or false");
            code = POCKETIPC_ERR_INVALID_PARAMS;
        } else {
            code = wifi_mgr_set_enabled(m, cJSON_IsTrue(on), now, &result, err, sizeof(err));
        }
    } else if (strcmp(method, "wifi.connect") == 0) {
        struct wifi_ssid ssid;
        const cJSON *pass = param(params, "passphrase");
        const cJSON *sec = param(params, "security");
        int hidden;
        int allow_open;

        if (only_keys(params, connect_keys, err, sizeof(err)) < 0 ||
            ssid_param(params, &ssid, err, sizeof(err)) < 0) {
            code = POCKETIPC_ERR_INVALID_PARAMS;
        } else if (pass && !cJSON_IsString(pass)) {
            snprintf(err, sizeof(err), "passphrase must be a string");
            code = POCKETIPC_ERR_INVALID_PARAMS;
        } else if (sec && !cJSON_IsString(sec)) {
            snprintf(err, sizeof(err), "security must be a string");
            code = POCKETIPC_ERR_INVALID_PARAMS;
        } else if (opt_bool(params, "hidden", &hidden) < 0 ||
                   opt_bool(params, "allow_open", &allow_open) < 0) {
            snprintf(err, sizeof(err), "hidden and allow_open must be true or false");
            code = POCKETIPC_ERR_INVALID_PARAMS;
        } else if (sec && !hidden) {
            snprintf(err, sizeof(err), "security is only given for a hidden network");
            code = POCKETIPC_ERR_INVALID_PARAMS;
        } else {
            code = wifi_mgr_connect(m, &ssid, pass ? pass->valuestring : NULL, hidden,
                                    sec ? sec->valuestring : NULL, allow_open, now, &result, err,
                                    sizeof(err));
        }
        /* The request is freed by the server after this; its copy of the
         * passphrase is cleared first. */
        if (cJSON_IsString(pass) && pass->valuestring) {
            explicit_bzero(pass->valuestring, strlen(pass->valuestring));
        }
    } else if (strcmp(method, "wifi.forget") == 0) {
        struct wifi_ssid ssid;

        if (only_keys(params, forget_keys, err, sizeof(err)) < 0 ||
            ssid_param(params, &ssid, err, sizeof(err)) < 0) {
            code = POCKETIPC_ERR_INVALID_PARAMS;
        } else {
            code = wifi_mgr_forget(m, &ssid, &result, err, sizeof(err));
        }
    } else {
        snprintf(err, sizeof(err), "unknown method %.60s", method);
        code = POCKETIPC_ERR_UNKNOWN_METHOD;
    }

    if (code) {
        cJSON_Delete(result);
        reply_error(s, c, id, code, err[0] ? err : "request failed");
        return;
    }
    pocketipc_server_reply(s, c, pocketipc_response(id, result));
}

/* ---- main -------------------------------------------------------------------- */

static void usage(FILE *out)
{
    fprintf(out,
            "usage: netd [--interface NAME] [--socket-name NAME] [--connect-timeout-s N]\n"
            "            [--dhcp-timeout-s N] [--verbose]\n"
            "Serves wifi.* for one wireless interface (docs/api/network.md).\n"
            "Store: $POCKETOS_STATE_DIR/netd (default %s/netd), runtime: $POCKETOS_RUNTIME_DIR/netd.\n",
            POCKETOS_STATE_DIR_DEFAULT);
}

static int seconds_arg(const char *s, int *out_ms)
{
    char *end;
    long v = strtol(s, &end, 10);

    if (end == s || *end != '\0' || v < 1 || v > 600) {
        return -1;
    }
    *out_ms = (int)v * 1000;
    return 0;
}

int main(int argc, char **argv)
{
    static struct wifi_mgr mgr;
    struct wifi_mgr_config cfg;
    struct pocketipc_server *server;
    char socket_name[64] = "netd";
    char store_dir[POCKETOS_PATH_MAX];
    char run_dir[POCKETOS_PATH_MAX];
    const char *iface = "wlan0";
    long last_step = 0;
    int i;

    memset(&cfg, 0, sizeof(cfg));
    pocketlog_init("netd");
    pocketlog_install_crash_handler();
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--interface") == 0 && i + 1 < argc) {
            iface = argv[++i];
        } else if (strcmp(argv[i], "--socket-name") == 0 && i + 1 < argc) {
            snprintf(socket_name, sizeof(socket_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--connect-timeout-s") == 0 && i + 1 < argc) {
            if (seconds_arg(argv[++i], &cfg.connect_timeout_ms) < 0) {
                usage(stderr);
                return 2;
            }
        } else if (strcmp(argv[i], "--dhcp-timeout-s") == 0 && i + 1 < argc) {
            if (seconds_arg(argv[++i], &cfg.dhcp_timeout_ms) < 0) {
                usage(stderr);
                return 2;
            }
        } else if (strcmp(argv[i], "--verbose") == 0) {
            pocketlog_set_level(POCKETLOG_DEBUG);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (netd_iface_name_valid(iface) < 0) {
        LOG_ERROR("invalid interface name");
        return 2;
    }
    if (snprintf(store_dir, sizeof(store_dir), "%s/netd", pocketos_state_dir()) >= (int)sizeof(store_dir) ||
        snprintf(run_dir, sizeof(run_dir), "%s/netd", pocketos_runtime_dir()) >= (int)sizeof(run_dir)) {
        LOG_ERROR("state or runtime directory path too long");
        return 2;
    }
    cfg.iface = iface;
    cfg.store_dir = store_dir;
    cfg.run_dir = run_dir;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (wifi_mgr_init(&mgr, &cfg) < 0) {
        LOG_ERROR("cannot initialise Wi-Fi: %s", strerror(errno));
        return 1;
    }
    server = pocketipc_server_new(socket_name, on_request, &mgr);
    if (!server) {
        LOG_ERROR("cannot listen: %s", strerror(errno));
        return 1;
    }
    LOG_INFO("listening on %s (api %d), interface %s", pocketipc_server_path(server),
             NETD_API_VERSION, iface);

    while (!stop_requested) {
        int ready = 0;
        int status;
        pid_t pid;
        long now;

        if (pocketipc_server_poll_fd(server, NETD_STEP_MS, wifi_mgr_event_fd(&mgr), &ready) < 0 &&
            errno != EINTR) {
            LOG_ERROR("poll: %s", strerror(errno));
            break;
        }
        now = mono_ms();
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            wifi_mgr_child_exited(&mgr, pid, status, now);
        }
        if (ready || now - last_step >= NETD_STEP_MS) {
            wifi_mgr_step(&mgr, now);
            last_step = now;
        }
    }
    LOG_INFO("shutting down");
    wifi_mgr_shutdown(&mgr);
    pocketipc_server_free(server);
    pocketlog_close();
    return 0;
}
