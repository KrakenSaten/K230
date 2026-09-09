/*
 * sysd: PocketOS system service. Serves system.* over pocketipc from the
 * facts core/pocketsys collects (docs/api/system.md). Read-only in v0:
 * it opens no device node and changes nothing on the system.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketipc/pocketipc.h"
#include "pocketipc/server.h"
#include "pocketlog/pocketlog.h"
#include "pocketsys.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SYSD_API_VERSION 0
/* CPU utilisation is the busy share of the last sampling interval. */
#define SYSD_CPU_SAMPLE_MS 1000

struct sysd {
    struct pocketipc_server *server;
    struct pocketsys_cpu cpu;
    char socket_name[64];
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static uint64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void on_request(struct pocketipc_server *s, struct pocketipc_client *c, cJSON *req,
                       void *user)
{
    struct sysd *sd = user;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(req, "method");
    const char *method = cJSON_IsString(m) ? m->valuestring : NULL;
    cJSON *result;
    char msg[160];

    if (!method) {
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                              "missing method"));
        return;
    }
    LOG_DEBUG("fd %d %s", pocketipc_client_fd(c), method);
    if (strcmp(method, "system.info") == 0) {
        result = pocketsys_info(pocketlog_version(), pocketlog_build_id());
        cJSON_AddNumberToObject(result, "api_version", SYSD_API_VERSION);
    } else if (strcmp(method, "system.status") == 0) {
        result = pocketsys_status(&sd->cpu);
    } else {
        snprintf(msg, sizeof(msg), "unknown method %s", method);
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_UNKNOWN_METHOD, msg));
        return;
    }
    pocketipc_server_reply(s, c, pocketipc_response(id, result));
}

static int run(struct sysd *sd)
{
    uint64_t last_sample = 0;

    while (!stop_requested) {
        uint64_t now;

        if (pocketipc_server_poll(sd->server, SYSD_CPU_SAMPLE_MS / 4) < 0) {
            LOG_ERROR("poll: %s", strerror(errno));
            return 1;
        }
        now = mono_ms();
        if (now - last_sample >= SYSD_CPU_SAMPLE_MS) {
            if (pocketsys_cpu_sample(&sd->cpu) < 0 && last_sample == 0) {
                LOG_WARN("/proc/stat unreadable: %s; cpu_percent stays null", strerror(errno));
            }
            last_sample = now;
        }
    }
    return 0;
}

static void usage(FILE *out)
{
    fprintf(out,
            "usage: sysd [--socket-name NAME] [--verbose]\n"
            "Serves system.info and system.status (docs/api/system.md).\n"
            "Runtime directory: $POCKETOS_RUNTIME_DIR or %s\n",
            POCKETIPC_DEFAULT_DIR);
}

int main(int argc, char **argv)
{
    struct sysd sd;
    int i;
    int rc;

    memset(&sd, 0, sizeof(sd));
    snprintf(sd.socket_name, sizeof(sd.socket_name), "sysd");
    pocketsys_cpu_init(&sd.cpu);
    pocketlog_init("sysd");
    pocketlog_install_crash_handler();
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket-name") == 0 && i + 1 < argc) {
            snprintf(sd.socket_name, sizeof(sd.socket_name), "%s", argv[++i]);
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

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    sd.server = pocketipc_server_new(sd.socket_name, on_request, &sd);
    if (!sd.server) {
        LOG_ERROR("cannot listen: %s", strerror(errno));
        return 1;
    }
    LOG_INFO("listening on %s", pocketipc_server_path(sd.server));

    rc = run(&sd);
    LOG_INFO("shutting down (rc=%d)", rc);
    pocketipc_server_free(sd.server);
    pocketlog_close();
    return rc;
}
