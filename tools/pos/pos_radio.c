/*
 * pos radio: client for radiod over pocketipc. See docs/api/radio.md.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketipc/pocketipc.h"
#include "pos_cli.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int cmd_radio(int argc, char **argv);

/* Shared with pos_system.c (pos system status, pos call). */
int pos_print_json(cJSON *obj)
{
    char *text = cJSON_Print(obj);

    if (!text) {
        return 1;
    }
    puts(text);
    free(text);
    return 0;
}

static int call_and_print(int fd, const char *method, cJSON *params)
{
    int code = 0;
    char err[160];
    cJSON *result = pocketipc_call(fd, method, params, &code, err, sizeof(err));

    if (!result) {
        fprintf(stderr, "%s radio: %s failed (code %d): %s\n", pos_cli_name, method, code, err);
        return 1;
    }
    pos_print_json(result);
    cJSON_Delete(result);
    return 0;
}

/* key=value arguments into a params object; numbers become numbers,
 * true/false become booleans, everything else stays a string. tool names
 * the command in the error message. Shared with pos_system.c. */
cJSON *pos_params_from_kv(const char *tool, int argc, char **argv)
{
    cJSON *params = cJSON_CreateObject();
    int i;

    for (i = 0; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        char *end;
        double v;

        if (!eq) {
            fprintf(stderr, "%s: expected key=value, got %s\n", tool, argv[i]);
            cJSON_Delete(params);
            return NULL;
        }
        *eq = '\0';
        if (strcmp(eq + 1, "true") == 0) {
            cJSON_AddBoolToObject(params, argv[i], 1);
        } else if (strcmp(eq + 1, "false") == 0) {
            cJSON_AddBoolToObject(params, argv[i], 0);
        } else {
            v = strtod(eq + 1, &end);
            if (*end == '\0' && end != eq + 1) {
                cJSON_AddNumberToObject(params, argv[i], v);
            } else {
                cJSON_AddStringToObject(params, argv[i], eq + 1);
            }
        }
    }
    return params;
}

static int radio_listen(int fd, int seconds)
{
    int code = 0;
    char err[160];
    cJSON *r = pocketipc_call(fd, "radio.subscribe", NULL, &code, err, sizeof(err));
    time_t end;

    if (!r) {
        fprintf(stderr, "%s radio: subscribe failed: %s\n", pos_cli_name, err);
        return 1;
    }
    cJSON_Delete(r);
    end = time(NULL) + seconds;
    for (;;) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int timeout = seconds > 0 ? (int)(end - time(NULL)) * 1000 : -1;
        char *text;

        if (seconds > 0 && timeout <= 0) {
            return 0;
        }
        if (poll(&p, 1, timeout) <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        text = pocketipc_read_frame(fd, NULL);
        if (!text) {
            fprintf(stderr, "%s radio: connection closed\n", pos_cli_name);
            return 1;
        }
        puts(text);
        fflush(stdout);
        free(text);
    }
}

/* The lease belongs to the connection that took it, so a command that took
 * one and exited would have given it back before the shell prompt came
 * back. This holds the connection open instead, which is the only shape
 * that means anything from a command line: the radio is held for as long as
 * the command runs, and Ctrl-C gives it back. */
static int radio_acquire(int fd, const char *name)
{
    cJSON *params = cJSON_CreateObject();
    int code = 0;
    char err[160];
    cJSON *r;

    cJSON_AddStringToObject(params, "owner", name && *name ? name : "pos");
    r = pocketipc_call(fd, "radio.acquire", params, &code, err, sizeof(err));
    if (!r) {
        fprintf(stderr, "%s radio: acquire failed (code %d): %s\n", pos_cli_name, code, err);
        return 1;
    }
    pos_print_json(r);
    cJSON_Delete(r);
    fprintf(stderr, "%s radio: holding the radio lease; interrupt to release it\n",
            pos_cli_name);
    for (;;) {
        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };

        if (poll(&p, 1, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 1;
        }
        /* Anything arriving here means the service has gone or is talking to
         * us unbidden; either way the lease is no longer ours to hold. */
        if (p.revents & (POLLIN | POLLHUP | POLLERR)) {
            char *text = pocketipc_read_frame(fd, NULL);

            if (!text) {
                fprintf(stderr, "%s radio: connection closed; the lease is gone\n",
                        pos_cli_name);
                return 1;
            }
            free(text);
        }
    }
}

static int usage(void)
{
    fprintf(stderr,
            "usage: %s radio <command>\n"
            "  info                     chip, backend, capabilities, region\n"
            "  status                   state and current profile\n"
            "  stats                    packet and airtime counters\n"
            "  configure key=value ...  e.g. frequency_mhz=868.1 spreading_factor=7\n"
            "  send <hex>               transmit payload given as hex, waiting for it\n"
            "  send-async <hex>         accept a transmit and return its tx_id at once\n"
            "  cad                      channel activity detection (takes the radio off receive)\n"
            "  rssi                     instantaneous channel RSSI\n"
            "  channel                  what the backend can say about the channel\n"
            "  lease                    who owns the radio, if anybody\n"
            "  acquire [name]           take the radio lease (held until this exits)\n"
            "  release                  give the radio lease back\n"
            "  listen [seconds]         print events (radio.rx, radio.tx_done,\n"
            "                           radio.state, radio.lease)\n"
            "  inject <hex> [rssi] [snr]  mock backend only: simulate a received packet\n"
            "  mock <key>=<int>         mock backend only: debug knob, e.g. rx_failing=1\n",
            pos_cli_name);
    return 2;
}

int cmd_radio(int argc, char **argv)
{
    const char *sub = argc > 0 ? argv[0] : "";
    int fd;
    int rc;

    if (argc < 1 || strcmp(sub, "help") == 0) {
        return usage();
    }
    fd = pocketipc_connect("radiod");
    if (fd < 0) {
        char path[256];

        pocketipc_socket_path("radiod", path, sizeof(path));
        fprintf(stderr, "%s radio: cannot connect to %s: %s\n", pos_cli_name, path, strerror(errno));
        return 1;
    }

    if (strcmp(sub, "info") == 0) {
        rc = call_and_print(fd, "radio.info", NULL);
    } else if (strcmp(sub, "status") == 0) {
        rc = call_and_print(fd, "radio.status", NULL);
    } else if (strcmp(sub, "stats") == 0) {
        rc = call_and_print(fd, "radio.stats", NULL);
    } else if (strcmp(sub, "cad") == 0) {
        rc = call_and_print(fd, "radio.cad", NULL);
    } else if (strcmp(sub, "rssi") == 0) {
        rc = call_and_print(fd, "radio.rssi", NULL);
    } else if (strcmp(sub, "channel") == 0) {
        rc = call_and_print(fd, "radio.channel", NULL);
    } else if (strcmp(sub, "lease") == 0) {
        rc = call_and_print(fd, "radio.lease", NULL);
    } else if (strcmp(sub, "release") == 0) {
        rc = call_and_print(fd, "radio.release", NULL);
    } else if (strcmp(sub, "configure") == 0) {
        char tool[32];
        cJSON *params;

        snprintf(tool, sizeof(tool), "%s radio", pos_cli_name);
        params = pos_params_from_kv(tool, argc - 1, argv + 1);

        rc = params ? call_and_print(fd, "radio.configure", params) : 2;
    } else if (strcmp(sub, "send") == 0 && argc >= 2) {
        cJSON *params = cJSON_CreateObject();

        cJSON_AddStringToObject(params, "payload_hex", argv[1]);
        rc = call_and_print(fd, "radio.send", params);
    } else if (strcmp(sub, "send-async") == 0 && argc >= 2) {
        cJSON *params = cJSON_CreateObject();

        cJSON_AddStringToObject(params, "payload_hex", argv[1]);
        /* Prints the acceptance and the tx_id, not the outcome: the packet
         * has not gone out yet. `pos radio listen` shows the completion. */
        rc = call_and_print(fd, "radio.send_async", params);
    } else if (strcmp(sub, "acquire") == 0) {
        rc = radio_acquire(fd, argc >= 2 ? argv[1] : NULL);
    } else if (strcmp(sub, "inject") == 0 && argc >= 2) {
        cJSON *params = cJSON_CreateObject();

        cJSON_AddStringToObject(params, "payload_hex", argv[1]);
        if (argc >= 3) {
            cJSON_AddNumberToObject(params, "rssi_dbm", atof(argv[2]));
        }
        if (argc >= 4) {
            cJSON_AddNumberToObject(params, "snr_db", atof(argv[3]));
        }
        rc = call_and_print(fd, "mock.inject_rx", params);
    } else if (strcmp(sub, "mock") == 0 && argc >= 2 && strchr(argv[1], '=')) {
        cJSON *params = cJSON_CreateObject();
        char *eq = strchr(argv[1], '=');

        *eq = '\0';
        cJSON_AddStringToObject(params, "key", argv[1]);
        cJSON_AddNumberToObject(params, "value", atoi(eq + 1));
        rc = call_and_print(fd, "mock.set", params);
    } else if (strcmp(sub, "listen") == 0) {
        rc = radio_listen(fd, argc >= 2 ? atoi(argv[1]) : 0);
    } else {
        rc = usage();
    }
    close(fd);
    return rc;
}
