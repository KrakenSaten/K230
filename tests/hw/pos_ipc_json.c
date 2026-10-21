/*
 * pos-ipc-json: one pocketipc request with raw JSON params, for hardware
 * tests that need parameters `pos call` cannot express (arrays, null, an
 * empty array). Not installed in an image; built by name and copied to a
 * unit for a test (tests/hw/mesh_explicit_path_rf.sh).
 *
 *   pos-ipc-json [-s service] [-t timeout_ms] [-l seconds] <method> [json]
 *
 * Prints one line of compact JSON on stdout: the result object, or
 * {"error":{"code":N,"message":"..."}} for an error response. With -l the
 * connection stays open after the answer and every frame that arrives in
 * the next `seconds` (events such as mesh.rx after mesh.subscribe) is
 * printed as one more line.
 *
 * Exit status: 0 result, 3 error response, 2 usage or bad JSON params,
 * 1 transport failure.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketipc/pocketipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void print_line(const cJSON *obj)
{
    char *text = cJSON_PrintUnformatted(obj);

    if (text) {
        puts(text);
        free(text);
    }
    fflush(stdout);
}

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int usage(void)
{
    fprintf(stderr, "usage: pos-ipc-json [-s service] [-t timeout_ms] [-l seconds] "
                    "<method> [json-object]\n"
                    "  e.g. pos-ipc-json mesh.path_preview '{\"to\":\"<64 hex>\",\"via\":[]}'\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *service = "meshcored";
    int timeout_ms = 15000;
    int listen_s = 0;
    cJSON *params = NULL;
    cJSON *result;
    int code = 0;
    char err[200];
    int opt;
    int fd;

    while ((opt = getopt(argc, argv, "s:t:l:")) != -1) {
        switch (opt) {
        case 's': service = optarg; break;
        case 't': timeout_ms = atoi(optarg); break;
        case 'l': listen_s = atoi(optarg); break;
        default: return usage();
        }
    }
    if (optind >= argc || argc - optind > 2 || timeout_ms < 0 || listen_s < 0) {
        return usage();
    }
    if (argc - optind == 2) {
        params = cJSON_Parse(argv[optind + 1]);
        if (!cJSON_IsObject(params)) {
            fprintf(stderr, "pos-ipc-json: params must be one JSON object\n");
            cJSON_Delete(params);
            return 2;
        }
    }
    fd = pocketipc_connect_timeout(service, timeout_ms);
    if (fd < 0) {
        cJSON_Delete(params);
        fprintf(stderr, "pos-ipc-json: cannot connect to %s: %s\n", service, strerror(errno));
        return 1;
    }
    result = pocketipc_call_timeout(fd, argv[optind], params, timeout_ms, &code, err, sizeof(err));
    if (!result && code == 0) {
        close(fd);
        fprintf(stderr, "pos-ipc-json: %s: %s\n", argv[optind], err);
        return 1;
    }
    if (!result) {
        cJSON *o = cJSON_CreateObject();
        cJSON *e = cJSON_AddObjectToObject(o, "error");

        cJSON_AddNumberToObject(e, "code", code);
        cJSON_AddStringToObject(e, "message", err);
        print_line(o);
        cJSON_Delete(o);
        close(fd);
        return 3;
    }
    print_line(result);
    cJSON_Delete(result);
    if (listen_s > 0) {
        long long end = now_ms() + (long long)listen_s * 1000;

        for (;;) {
            long long left = end - now_ms();
            size_t len = 0;
            char *text;

            if (left <= 0) {
                break;
            }
            text = pocketipc_read_frame_timeout(fd, &len, (int)left);
            if (!text) {
                if (errno == ETIMEDOUT || errno == EAGAIN) {
                    break;
                }
                close(fd);
                fprintf(stderr, "pos-ipc-json: connection ended while listening\n");
                return 1;
            }
            puts(text);
            fflush(stdout);
            free(text);
        }
    }
    close(fd);
    return 0;
}
