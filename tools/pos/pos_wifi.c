/*
 * pos wifi: client for netd's wifi.* (docs/api/network.md).
 *
 * The passphrase is never taken from the command line, where every process
 * on the machine could read it in /proc: `pos wifi connect` reads it from
 * standard input, with echo off when that is a terminal.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketipc/pocketipc.h"
#include "pos_cli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

int cmd_wifi(int argc, char **argv);

#define CONNECT_WAIT_S 60
#define SCAN_WAIT_S 15

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

static cJSON *wifi_call_quiet(const char *method, cJSON *params, int *code, int quiet);

static cJSON *wifi_call(const char *method, cJSON *params, int *code)
{
    return wifi_call_quiet(method, params, code, 0);
}

/* Right after `pos wifi on` netd answers "starting" (code 5) for a moment;
 * a command typed straight after it waits that out rather than failing.
 * params is consumed on every path, so it is copied for the retries. */
static cJSON *wifi_call_patient(const char *method, cJSON *params, int *code)
{
    time_t end = time(NULL) + 6;
    cJSON *result;

    for (;;) {
        cJSON *copy = params ? cJSON_Duplicate(params, 1) : NULL;
        int last = time(NULL) >= end;

        result = wifi_call_quiet(method, copy, code, !last);
        if (result || *code != POCKETIPC_ERR_BUSY || last) {
            break;
        }
        sleep_ms(300);
    }
    cJSON_Delete(params);
    return result;
}

static cJSON *wifi_call_quiet(const char *method, cJSON *params, int *code, int quiet)
{
    int fd = pocketipc_connect("netd");
    char err[200];
    cJSON *result;

    *code = 0;
    if (fd < 0) {
        char path[256];

        cJSON_Delete(params);
        pocketipc_socket_path("netd", path, sizeof(path));
        fprintf(stderr, "%s wifi: cannot connect to %s: %s\n", pos_cli_name, path, strerror(errno));
        *code = -1;
        return NULL;
    }
    result = pocketipc_call(fd, method, params, code, err, sizeof(err));
    close(fd);
    if (!result && !(quiet && *code == POCKETIPC_ERR_BUSY)) {
        fprintf(stderr, "%s wifi: %s failed (code %d): %s\n", pos_cli_name, method, *code, err);
        if (*code == 0) {
            *code = -1;
        }
    }
    return result;
}

static const char *str_or(const cJSON *o, const char *key, const char *fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsString(v) ? v->valuestring : fallback;
}

static int int_or(const cJSON *o, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

static int bool_of(const cJSON *o, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key));
}

static void print_status(const cJSON *st)
{
    const char *reason = str_or(st, "reason", NULL);
    const char *ssid = str_or(st, "ssid", NULL);

    printf("%-14s %s%s%s%s\n", "state", str_or(st, "state", "?"), reason ? " (" : "",
           reason ? reason : "", reason ? ")" : "");
    printf("%-14s %s\n", "interface", str_or(st, "interface", "-"));
    printf("%-14s %s\n", "enabled", bool_of(st, "enabled") ? "yes" : "no");
    if (ssid) {
        printf("%-14s %s\n", "ssid", ssid);
    }
    if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(st, "signal_dbm"))) {
        printf("%-14s %d dBm (%d/4)\n", "signal", int_or(st, "signal_dbm", 0), int_or(st, "signal_bars", 0));
    }
    if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(st, "frequency_mhz"))) {
        printf("%-14s %d MHz\n", "frequency", int_or(st, "frequency_mhz", 0));
    }
    printf("%-14s %s\n", "ipv4", str_or(st, "ipv4", "-"));
    printf("%-14s %d (store %s)\n", "saved", int_or(st, "saved_count", 0), str_or(st, "store", "?"));
}

static int print_networks(const cJSON *res)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(res, "networks");
    const cJSON *age = cJSON_GetObjectItemCaseSensitive(res, "age_s");
    const cJSON *e;

    if (!cJSON_IsNumber(age)) {
        printf("no scan yet: run %s wifi scan\n", pos_cli_name);
        return 0;
    }
    printf("%-8s %-5s %-11s %-6s %s\n", "signal", "bars", "security", "flags", "ssid");
    cJSON_ArrayForEach(e, list) {
        char flags[8];

        snprintf(flags, sizeof(flags), "%s%s%s", bool_of(e, "connected") ? "C" : "",
                 bool_of(e, "saved") ? "S" : "", bool_of(e, "supported") ? "" : "X");
        printf("%-8d %-5d %-11s %-6s %s\n", int_or(e, "signal_dbm", 0), int_or(e, "signal_bars", 0),
               str_or(e, "security", "?"), flags[0] ? flags : "-", str_or(e, "ssid", "?"));
    }
    printf("(%d s old; %d hidden; C connected, S saved, X not supported)\n", age->valueint,
           int_or(res, "hidden_count", 0));
    return 0;
}


/* One line from stdin into buf. Echo off when stdin is a terminal. Returns
 * 0, or -1 when nothing could be read or the line did not fit. */
static int read_passphrase(const char *ssid, char *buf, size_t n)
{
    struct termios old;
    struct termios quiet;
    int tty = isatty(STDIN_FILENO);
    size_t len;
    int ok;

    if (tty) {
        fprintf(stderr, "Passphrase for %s: ", ssid);
        fflush(stderr);
        if (tcgetattr(STDIN_FILENO, &old) == 0) {
            quiet = old;
            quiet.c_lflag &= ~(tcflag_t)ECHO;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
        } else {
            tty = 0;
        }
    }
    ok = fgets(buf, (int)n, stdin) != NULL;
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        fprintf(stderr, "\n");
    }
    if (!ok) {
        return -1;
    }
    len = strlen(buf);
    if (len > 0 && buf[len - 1] != '\n' && !feof(stdin)) {
        explicit_bzero(buf, n);
        return -1;
    }
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    return 0;
}

static int ssid_params(cJSON *params, const char *ssid, int hex)
{
    cJSON_AddStringToObject(params, hex ? "ssid_hex" : "ssid", ssid);
    return 0;
}

static int cmd_connect(int argc, char **argv)
{
    const char *ssid = NULL;
    const char *hidden_sec = NULL;
    int open_ok = 0;
    int hex = 0;
    char pass[256];
    cJSON *params;
    cJSON *res;
    char last[64] = "";
    int code;
    int i;
    time_t end;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--hidden") == 0 && i + 1 < argc) {
            hidden_sec = argv[++i];
        } else if (strcmp(argv[i], "--open") == 0) {
            open_ok = 1;
        } else if (strcmp(argv[i], "--ssid-hex") == 0) {
            hex = 1;
        } else if (!ssid && argv[i][0] != '-') {
            ssid = argv[i];
        } else {
            ssid = NULL;
            break;
        }
    }
    if (!ssid) {
        fprintf(stderr, "usage: %s wifi connect <ssid> [--ssid-hex] [--open] [--hidden open|wpa|wpa2|wpa3]\n"
                        "  the passphrase is read from standard input, never from the command line\n",
                pos_cli_name);
        return 2;
    }
    params = cJSON_CreateObject();
    ssid_params(params, ssid, hex);
    if (open_ok) {
        cJSON_AddBoolToObject(params, "allow_open", 1);
    }
    if (hidden_sec) {
        cJSON_AddBoolToObject(params, "hidden", 1);
        cJSON_AddStringToObject(params, "security", hidden_sec);
    }
    if (!open_ok) {
        cJSON *item;

        if (read_passphrase(ssid, pass, sizeof(pass)) < 0) {
            fprintf(stderr, "%s wifi: no passphrase read (use --open for an open network)\n", pos_cli_name);
            cJSON_Delete(params);
            return 2;
        }
        /* An empty line joins a saved network with its saved passphrase. */
        if (pass[0]) {
            item = cJSON_CreateString(pass);
            cJSON_AddItemToObject(params, "passphrase", item);
        }
        explicit_bzero(pass, sizeof(pass));
    }
    /* pocketipc_call consumes params; the JSON text holding the passphrase is
     * freed inside it. */
    res = wifi_call_patient("wifi.connect", params, &code);
    if (!res) {
        return 1;
    }
    printf("joining %s (%s)\n", str_or(res, "ssid", ssid), str_or(res, "security", "?"));
    cJSON_Delete(res);

    end = time(NULL) + CONNECT_WAIT_S;
    while (time(NULL) < end) {
        const char *state;

        sleep_ms(500);
        res = wifi_call("wifi.status", NULL, &code);
        if (!res) {
            return 1;
        }
        state = str_or(res, "state", "?");
        if (strcmp(state, last) != 0) {
            snprintf(last, sizeof(last), "%s", state);
            if (strcmp(state, "connected") != 0 && strcmp(state, "failed") != 0) {
                printf("%s\n", state);
                fflush(stdout);
            }
        }
        if (strcmp(state, "connected") == 0) {
            printf("connected to %s, ipv4 %s\n", str_or(res, "ssid", "?"), str_or(res, "ipv4", "-"));
            cJSON_Delete(res);
            return 0;
        }
        if (strcmp(state, "failed") == 0) {
            printf("failed: %s\n", str_or(res, "reason", "unknown"));
            cJSON_Delete(res);
            return 1;
        }
        cJSON_Delete(res);
    }
    printf("still not connected after %d s (state %s)\n", CONNECT_WAIT_S, last);
    return 1;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: %s wifi <command>\n"
            "  status                      state, network, signal, address\n"
            "  on | off                    turn Wi-Fi on or off (remembered)\n"
            "  scan                        scan and list networks\n"
            "  list                        networks from the last scan\n"
            "  connect <ssid> [options]    join; passphrase from stdin (%s wifi connect help)\n"
            "  disconnect                  leave the current network (until the next join or boot)\n"
            "  saved                       networks netd remembers (never their passphrases)\n"
            "  forget <ssid> [--ssid-hex]  forget a saved network\n",
            pos_cli_name, pos_cli_name);
    return 2;
}

int cmd_wifi(int argc, char **argv)
{
    const char *sub = argc > 0 ? argv[0] : "status";
    cJSON *res;
    int code;

    if (strcmp(sub, "status") == 0) {
        res = wifi_call("wifi.status", NULL, &code);
        if (!res) {
            return 1;
        }
        print_status(res);
        cJSON_Delete(res);
        return 0;
    }
    if (strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0) {
        cJSON *params = cJSON_CreateObject();

        cJSON_AddBoolToObject(params, "enabled", strcmp(sub, "on") == 0);
        res = wifi_call("wifi.set_enabled", params, &code);
        if (!res) {
            return 1;
        }
        printf("Wi-Fi %s (state %s)\n", bool_of(res, "enabled") ? "on" : "off", str_or(res, "state", "?"));
        cJSON_Delete(res);
        return 0;
    }
    if (strcmp(sub, "scan") == 0) {
        time_t end = time(NULL) + SCAN_WAIT_S;

        res = wifi_call_patient("wifi.scan", NULL, &code);
        if (!res) {
            return 1;
        }
        cJSON_Delete(res);
        for (;;) {
            sleep_ms(300);
            res = wifi_call("wifi.networks", NULL, &code);
            if (!res) {
                return 1;
            }
            if (!bool_of(res, "scanning") || time(NULL) >= end) {
                break;
            }
            cJSON_Delete(res);
        }
        if (bool_of(res, "scan_failed")) {
            fprintf(stderr, "%s wifi: the scan did not complete; showing the previous results\n", pos_cli_name);
        }
        print_networks(res);
        cJSON_Delete(res);
        return 0;
    }
    if (strcmp(sub, "list") == 0) {
        res = wifi_call("wifi.networks", NULL, &code);
        if (!res) {
            return 1;
        }
        print_networks(res);
        cJSON_Delete(res);
        return 0;
    }
    if (strcmp(sub, "connect") == 0) {
        return cmd_connect(argc - 1, argv + 1);
    }
    if (strcmp(sub, "disconnect") == 0) {
        res = wifi_call("wifi.disconnect", NULL, &code);
        if (!res) {
            return 1;
        }
        printf("disconnected (state %s)\n", str_or(res, "state", "?"));
        cJSON_Delete(res);
        return 0;
    }
    if (strcmp(sub, "saved") == 0) {
        const cJSON *e;

        res = wifi_call("wifi.saved", NULL, &code);
        if (!res) {
            return 1;
        }
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(res, "networks")) {
            printf("%-11s %s%s\n", str_or(e, "security", "?"), str_or(e, "ssid", "?"),
                   bool_of(e, "hidden") ? " (hidden)" : "");
        }
        cJSON_Delete(res);
        return 0;
    }
    if (strcmp(sub, "forget") == 0 && argc >= 2) {
        cJSON *params = cJSON_CreateObject();
        int hex = argc >= 3 && strcmp(argv[2], "--ssid-hex") == 0;

        ssid_params(params, argv[1], hex);
        res = wifi_call("wifi.forget", params, &code);
        if (!res) {
            return 1;
        }
        printf("forgot %s\n", argv[1]);
        cJSON_Delete(res);
        return 0;
    }
    return usage();
}
