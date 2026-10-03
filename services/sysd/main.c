/*
 * sysd: PocketOS system service. Serves system.* over pocketipc from the
 * facts core/pocketsys collects and what pos-supervise records
 * (docs/api/system.md). Everything it reports about the machine is
 * read-only; the two things it can change there are system.reboot and
 * system.poweroff, which it does not do itself but asks init to do
 * (services/sysd/sysd_power.c).
 *
 * It is also the one owner of the USB drive: storage.status and
 * storage.eject, and the mount it makes when a FAT drive is plugged in
 * (services/sysd/sysd_storage.c). That is the only device it opens, and only
 * to read the drive's first sectors.
 *
 * And of growing the root filesystem over the rest of the microSD card when
 * the owner asks: storage.expand (services/sysd/sysd_expand.c), which reads
 * the card's partition table and the root superblock to decide, and runs
 * parted, partprobe and resize2fs to do it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketipc/pocketipc.h"
#include "pocketipc/server.h"
#include "pocketlog/pocketlog.h"
#include "pocketpaths.h"
#include "pocketsys.h"
#include "sysd_expand.h"
#include "sysd_logs.h"
#include "sysd_power.h"
#include "sysd_services.h"
#include "sysd_storage.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SYSD_API_VERSION 0
/* CPU utilisation is the busy share of the last sampling interval. */
#define SYSD_CPU_SAMPLE_MS 1000
/* How long a power action waits after its reply was written before it runs.
 * The same budget the transport gives a write to reach the peer
 * (POCKETIPC_SEND_TIMEOUT_MS): the reply is already on the socket by then,
 * this is the client's chance to read it before the machine goes. */
#define SYSD_POWER_DELAY_MS POCKETIPC_SEND_TIMEOUT_MS
/* While an action is pending the loop wakes more often, so the delay above is
 * what decides when it runs rather than the sampler's poll timeout. */
#define SYSD_POWER_POLL_MS 50
/* While an eject runs, how soon its end is noticed. */
#define SYSD_EJECT_POLL_MS 100

struct sysd {
    struct pocketipc_server *server;
    struct pocketsys_cpu cpu;
    char socket_name[64];
    /* At most one power action is ever pending. It is recorded by the
     * request handler only after that request's reply has gone out, and
     * consumed by the main loop; the handler never runs it itself. */
    enum sysd_power_action pending;
    uint64_t pending_at;
    struct sysd_storage storage;
    struct sysd_expand expand;
};

/* storage.* take no parameters; like the power actions, eject does not act on
 * a request it does not fully understand. */
static bool no_params(const cJSON *params)
{
    return !params || cJSON_IsNull(params) || (cJSON_IsObject(params) && cJSON_GetArraySize(params) == 0);
}

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

/* A power action is accepted in three steps, in this order and no other:
 * refuse it if one is already pending, send the reply, and only then record
 * it. Nothing here runs the command. The reply has to be out first because
 * after the machine acts there is no connection left to answer on, and it has
 * to be out *successfully*: a client that has gone away has not been told the
 * machine is about to reboot, so it is not going to. */
static void on_power(struct pocketipc_server *s, struct pocketipc_client *c, const cJSON *id,
                     const cJSON *params, struct sysd *sd, enum sysd_power_action action)
{
    const char *name = sysd_power_name(action);
    cJSON *result;
    char msg[160];

    /* These two methods take no parameters at all. system.info and
     * system.status ignore extras; a method that stops the machine does not
     * act on a request it does not fully understand. */
    if (params && !cJSON_IsNull(params)) {
        if (!cJSON_IsObject(params) || cJSON_GetArraySize(params) != 0) {
            snprintf(msg, sizeof(msg), "system.%s takes no parameters", name);
            pocketipc_server_reply(s, c,
                                   pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS, msg));
            return;
        }
    }
    if (sd->pending != SYSD_POWER_NONE) {
        snprintf(msg, sizeof(msg), "%s already pending", sysd_power_name(sd->pending));
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_BUSY, msg));
        return;
    }
    /* Not in the middle of rewriting the card's partition table or growing
     * the root filesystem. */
    if (sysd_expand_busy(&sd->expand)) {
        snprintf(msg, sizeof(msg), "the storage expansion is running; %s when it has finished", name);
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_BUSY, msg));
        return;
    }
    result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "action", name);
    pocketipc_server_reply(s, c, pocketipc_response(id, result));
    if (pocketipc_client_fd(c) < 0) {
        /* pocketipc_server_reply closes the client when the write fails, so
         * this is how a failed reply is seen. Nobody was told; do nothing. */
        LOG_WARN("%s: reply could not be delivered, not acting", name);
        return;
    }
    sd->pending = action;
    sd->pending_at = mono_ms();
    LOG_INFO("%s accepted, acting in %d ms", name, SYSD_POWER_DELAY_MS);
}

static void on_request(struct pocketipc_server *s, struct pocketipc_client *c, cJSON *req,
                       void *user)
{
    struct sysd *sd = user;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(req, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
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
        /* The machine's own facts, then what the supervisor says about the
         * services on it: two sources, joined here rather than in core. */
        result = pocketsys_status(&sd->cpu);
        sysd_services_add(result);
    } else if (strcmp(method, "system.logs") == 0) {
        struct sysd_logs_query q;

        if (sysd_logs_parse_query(params, &q, msg, sizeof(msg)) < 0) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS, msg));
            return;
        }
        result = sysd_logs_query(pocketos_log_dir(), &q);
    } else if (strcmp(method, "system.crashes") == 0) {
        if (params && !cJSON_IsNull(params) &&
            !(cJSON_IsObject(params) && cJSON_GetArraySize(params) == 0)) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                                  "system.crashes takes no params"));
            return;
        }
        result = sysd_crashes_list(pocketos_log_dir());
    } else if (strcmp(method, "system.reboot") == 0) {
        on_power(s, c, id, params, sd, SYSD_POWER_REBOOT);
        return;
    } else if (strcmp(method, "system.poweroff") == 0) {
        on_power(s, c, id, params, sd, SYSD_POWER_POWEROFF);
        return;
    } else if (strcmp(method, "storage.status") == 0) {
        result = sysd_storage_status(&sd->storage);
        cJSON_AddItemToObject(result, "internal", sysd_expand_status(&sd->expand));
    } else if (strcmp(method, "storage.expand") == 0) {
        int r;

        if (!no_params(params)) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                                  "storage.expand takes no parameters"));
            return;
        }
        r = sysd_expand_start(&sd->expand, msg, sizeof(msg));
        if (r < 0) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, r == -2   ? POCKETIPC_ERR_BUSY
                                                                      : r == -1 ? POCKETIPC_ERR_POLICY
                                                                                : POCKETIPC_ERR_BACKEND,
                                                                  msg));
            return;
        }
        /* Accepted, not finished: storage.status says when it is. */
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "state", sysd_expand_state_name(SYSD_EXPAND_RUNNING));
    } else if (strcmp(method, "storage.eject") == 0) {
        int r;

        if (!no_params(params)) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                                  "storage.eject takes no parameters"));
            return;
        }
        r = sysd_storage_eject(&sd->storage, msg, sizeof(msg));
        if (r < 0) {
            pocketipc_server_reply(s, c, pocketipc_error_response(id, r == -2   ? POCKETIPC_ERR_BUSY
                                                                      : r == -1 ? POCKETIPC_ERR_POLICY
                                                                                : POCKETIPC_ERR_BACKEND,
                                                                  msg));
            return;
        }
        /* Accepted, not finished: storage.status says when it is. */
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "state", sysd_storage_state_name(sd->storage.state));
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
        int uevent = 0;
        int timeout = sd->pending != SYSD_POWER_NONE ? SYSD_POWER_POLL_MS
                      : sysd_storage_busy(&sd->storage) || sysd_expand_busy(&sd->expand) ? SYSD_EJECT_POLL_MS
                                                                                          : SYSD_CPU_SAMPLE_MS / 4;

        if (pocketipc_server_poll_fd(sd->server, timeout, sd->storage.uevent_fd, &uevent) < 0) {
            LOG_ERROR("poll: %s", strerror(errno));
            return 1;
        }
        if (uevent) {
            sysd_storage_on_uevent(&sd->storage);
        }
        if (sysd_storage_busy(&sd->storage)) {
            sysd_storage_scan(&sd->storage); /* collects a finished eject */
        }
        sysd_expand_reap(&sd->expand);
        now = mono_ms();
        /* The pending action is consumed before it is run, so it runs at most
         * once whatever the command does or how long it takes.
         *
         * stop_requested is tested here and not only by the loop condition
         * above: a signal can arrive while poll is blocked, in the very
         * iteration the delay expires, and without this the action would go
         * ahead anyway. Being told to stop outranks an action not yet
         * started, and dropping it is the safe way to lose that race - the
         * client was told the action was accepted, not that it had happened,
         * and nothing rebooting the machine after sysd was asked to go away
         * is the behaviour anyone wants. */
        if (!stop_requested && sd->pending != SYSD_POWER_NONE &&
            now - sd->pending_at >= SYSD_POWER_DELAY_MS) {
            enum sysd_power_action action = sd->pending;

            sd->pending = SYSD_POWER_NONE;
            if (sysd_power_run(action) < 0) {
                LOG_ERROR("%s did not run; sysd keeps serving", sysd_power_name(action));
            }
            continue;
        }
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
            "Serves system.info, system.status, system.logs, system.crashes,\n"
            "system.reboot, system.poweroff, storage.status, storage.eject and storage.expand\n"
            "(docs/api/system.md).\n"
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
    /* A drive already plugged in sent its events before anyone listened: look
     * once now. The drive stays mounted when sysd stops; the next sysd finds
     * the mount and keeps it, and shutdown's umount -a unmounts it. */
    sysd_storage_init(&sd.storage, &sysd_storage_real_paths, &sysd_storage_real_ops);
    sysd_storage_listen(&sd.storage);
    sysd_storage_scan(&sd.storage);
    /* An expansion that needed a restart is finished here, and only one the
     * owner asked for: there is no first-boot resize. */
    {
        static char pending[512];
        static char log[512];
        struct sysd_expand_paths xp = { "/sys/class/block", "/dev", "/proc/cmdline", pending, log };

        snprintf(pending, sizeof(pending), "%s/storage-expand.pending", pocketos_state_dir());
        snprintf(log, sizeof(log), "%s/storage-expand.log", pocketos_log_dir());
        sysd_expand_init(&sd.expand, &xp, &sysd_expand_real_ops);
        sysd_expand_startup(&sd.expand);
    }

    rc = run(&sd);
    LOG_INFO("shutting down (rc=%d)", rc);
    sysd_storage_close(&sd.storage);
    pocketipc_server_free(sd.server);
    pocketlog_close();
    return rc;
}
