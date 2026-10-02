/*
 * The debug overlay's line. See overlay_model.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "overlay_model.h"

#include <stdio.h>
#include <string.h>

#define DASH "\xE2\x80\x94"
#define DOT " \xC2\xB7 "
#define DEG "\xC2\xB0"
#define DOWN "\xE2\x86\x93"
#define UP "\xE2\x86\x91"

static int number(const cJSON *o, const char *key, double *out)
{
    const cJSON *v = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;

    if (!cJSON_IsNumber(v)) {
        return 0;
    }
    *out = v->valuedouble;
    return 1;
}

static int six_octet_mac(const char *mac)
{
    int i;

    if (!mac || strlen(mac) != 17) {
        return 0;
    }
    for (i = 0; i < 17; i++) {
        char c = mac[i];

        if (i % 3 == 2 ? c != ':' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

int overlay_net_bytes(const cJSON *status, uint64_t *rx, uint64_t *tx)
{
    const cJSON *arr = cJSON_IsObject(status) ? cJSON_GetObjectItemCaseSensitive(status, "network") : NULL;
    const cJSON *e;
    uint64_t r = 0;
    uint64_t t = 0;
    int counted = 0;

    if (!cJSON_IsArray(arr)) {
        return -1;
    }
    cJSON_ArrayForEach(e, arr) {
        const cJSON *mac = cJSON_GetObjectItemCaseSensitive(e, "mac");
        double a;
        double b;

        if (!six_octet_mac(cJSON_IsString(mac) ? mac->valuestring : NULL)) {
            continue;
        }
        if (!number(e, "rx_bytes", &a) || !number(e, "tx_bytes", &b) || a < 0 || b < 0) {
            /* One interface without its counters makes the sum a guess. */
            return -1;
        }
        r += (uint64_t)a;
        t += (uint64_t)b;
        counted++;
    }
    if (!counted) {
        return -1;
    }
    *rx = r;
    *tx = t;
    return 0;
}

static void append(char *out, size_t n, const char *fmt_part)
{
    size_t len = strlen(out);

    snprintf(out + len, n - len, "%s%s", len ? DOT : "", fmt_part);
}

static void rate_text(double kbs, char *out, size_t n)
{
    if (kbs < 10.0) {
        snprintf(out, n, "%.1f", kbs);
    } else {
        snprintf(out, n, "%.0f", kbs);
    }
}

void overlay_model_init(struct overlay_model *m)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->text, sizeof(m->text), "CPU " DASH DOT "RAM " DASH DOT DASH DOT "NET " DASH);
}

void overlay_model_update(struct overlay_model *m, const cJSON *status, const cJSON *radio, uint32_t now_ms)
{
    const cJSON *mem = cJSON_IsObject(status) ? cJSON_GetObjectItemCaseSensitive(status, "memory") : NULL;
    char part[64];
    char line[OVERLAY_TEXT_MAX] = "";
    double d;
    double total;
    double avail;
    uint64_t rx;
    uint64_t tx;

    if (number(status, "cpu_percent", &d)) {
        snprintf(part, sizeof(part), "CPU %.0f%%", d);
    } else {
        snprintf(part, sizeof(part), "CPU " DASH);
    }
    append(line, sizeof(line), part);

    if (number(mem, "total_kb", &total) && number(mem, "available_kb", &avail) && total > 0 && avail >= 0 &&
        avail <= total) {
        snprintf(part, sizeof(part), "RAM %.0f%%", (total - avail) / total * 100.0);
    } else {
        snprintf(part, sizeof(part), "RAM " DASH);
    }
    append(line, sizeof(line), part);

    if (number(status, "temperature_c", &d)) {
        snprintf(part, sizeof(part), "%.0f" DEG "C", d);
    } else {
        snprintf(part, sizeof(part), DASH DEG "C");
    }
    append(line, sizeof(line), part);

    snprintf(part, sizeof(part), "NET " DASH);
    if (status && overlay_net_bytes(status, &rx, &tx) == 0) {
        uint32_t dt = now_ms - m->at_ms;

        /* A counter that went down (a driver reloaded) starts again rather
         * than showing a wrapped figure. */
        if (m->have_net && dt > 0 && rx >= m->rx && tx >= m->tx) {
            double s = dt / 1000.0;
            double down = (double)(rx - m->rx) / 1024.0 / s;
            double upr = (double)(tx - m->tx) / 1024.0 / s;
            const char *unit = "KB/s";
            char a[16];
            char b[16];

            if (down >= 1024.0 || upr >= 1024.0) {
                down /= 1024.0;
                upr /= 1024.0;
                unit = "MB/s";
            }
            rate_text(down, a, sizeof(a));
            rate_text(upr, b, sizeof(b));
            snprintf(part, sizeof(part), "NET " DOWN "%s " UP "%s %s", a, b, unit);
        }
        m->have_net = 1;
        m->rx = rx;
        m->tx = tx;
        m->at_ms = now_ms;
    } else if (status) {
        /* sysd answered without counters: no rate, and none to come from
         * this answer. A missed answer (NULL) keeps the last count, so the
         * next one still measures from it. */
        m->have_net = 0;
    }
    append(line, sizeof(line), part);

    if (number(radio, "rx_packets", &d) && number(radio, "tx_packets", &total)) {
        snprintf(part, sizeof(part), "LORA " DOWN "%.0f " UP "%.0f", d, total);
        append(line, sizeof(line), part);
    }
    snprintf(m->text, sizeof(m->text), "%s", line);
}
