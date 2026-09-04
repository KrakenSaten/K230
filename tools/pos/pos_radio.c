/*
 * pos radio: client for radiod over pocketipc. See docs/api/radio.md.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketipc/pocketipc.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int cmd_radio(int argc, char **argv);

static int print_json(cJSON *obj)
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
        fprintf(stderr, "pos radio: %s failed (code %d): %s\n", method, code, err);
        return 1;
    }
    print_json(result);
    cJSON_Delete(result);
    return 0;
}

/* key=value arguments into a params object; numbers become numbers,
 * true/false become booleans, everything else stays a string. */
static cJSON *params_from_kv(int argc, char **argv)
{
    cJSON *params = cJSON_CreateObject();
    int i;

    for (i = 0; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        char *end;
        double v;

        if (!eq) {
            fprintf(stderr, "pos radio: expected key=value, got %s\n", argv[i]);
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
        fprintf(stderr, "pos radio: subscribe failed: %s\n", err);
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
            fprintf(stderr, "pos radio: connection closed\n");
            return 1;
        }
        puts(text);
        fflush(stdout);
        free(text);
    }
}

static int usage(void)
{
    fprintf(stderr,
            "usage: pos radio <command>\n"
            "  info                     chip, backend, capabilities, region\n"
            "  status                   state and current profile\n"
            "  stats                    packet and airtime counters\n"
            "  configure key=value ...  e.g. frequency_mhz=868.1 spreading_factor=7\n"
            "  send <hex>               transmit payload given as hex\n"
            "  cad                      channel activity detection\n"
            "  rssi                     instantaneous channel RSSI\n"
            "  listen [seconds]         print events (radio.rx, radio.tx_done, radio.state)\n"
            "  inject <hex> [rssi] [snr]  mock backend only: simulate a received packet\n");
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
        fprintf(stderr, "pos radio: cannot connect to %s: %s\n", path, strerror(errno));
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
    } else if (strcmp(sub, "configure") == 0) {
        cJSON *params = params_from_kv(argc - 1, argv + 1);

        rc = params ? call_and_print(fd, "radio.configure", params) : 2;
    } else if (strcmp(sub, "send") == 0 && argc >= 2) {
        cJSON *params = cJSON_CreateObject();

        cJSON_AddStringToObject(params, "payload_hex", argv[1]);
        rc = call_and_print(fd, "radio.send", params);
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
    } else if (strcmp(sub, "listen") == 0) {
        rc = radio_listen(fd, argc >= 2 ? atoi(argv[1]) : 0);
    } else {
        rc = usage();
    }
    close(fd);
    return rc;
}
