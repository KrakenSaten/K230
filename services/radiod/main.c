/*
 * radiod: PocketOS radio service. Owns the LoRa transceiver, enforces the
 * region guard, keeps airtime statistics and serves radio.* over pocketipc.
 * See docs/api/radio.md.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "airtime.h"
#include "pocketipc/pocketipc.h"
#include "pocketipc/server.h"
#include "pocketlog/pocketlog.h"
#include "radio_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RADIOD_API_VERSION 0
#define HOUR_BUCKETS 60

struct region {
    const char *name;
    double frequency_min_mhz;
    double frequency_max_mhz;
    int max_tx_power_dbm;
};

/* EU868 is a safety net (863-870 MHz, 14 dBm requested output), not
 * regulatory compliance. NONE is for shielded lab use only. */
static const struct region regions[] = {
    { "EU868", 863.0, 870.0, 14 },
    { "NONE", 0.0, 1e9, 22 },
};

static const double lora_bandwidths_khz[] = {
    7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125.0, 250.0, 500.0
};

struct radiod {
    struct radio_backend be;
    struct radio_caps caps;
    const struct region *region;
    const char *state;
    bool verbose;
    char socket_name[64];
    struct pocketipc_server *server;
    /* statistics */
    uint64_t start_mono_ms;
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t rx_crc_errors;
    double tx_airtime_ms;
    double rx_airtime_ms;
    double last_rssi_dbm;
    double last_snr_db;
    double last_frequency_error_hz;
    bool have_last_rx;
    double hour_buckets[HOUR_BUCKETS];
    int64_t bucket_minute;
    uint64_t last_rx_recovery_ms;
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

uint64_t radio_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- statistics ------------------------------------------------------- */

static void buckets_advance(struct radiod *rd)
{
    int64_t minute = (int64_t)(mono_ms() / 60000u);

    while (rd->bucket_minute < minute) {
        rd->bucket_minute++;
        rd->hour_buckets[rd->bucket_minute % HOUR_BUCKETS] = 0.0;
    }
}

static void buckets_add_tx(struct radiod *rd, double airtime_ms)
{
    buckets_advance(rd);
    rd->hour_buckets[rd->bucket_minute % HOUR_BUCKETS] += airtime_ms;
}

static double buckets_last_hour(struct radiod *rd)
{
    double sum = 0.0;
    int i;

    buckets_advance(rd);
    for (i = 0; i < HOUR_BUCKETS; i++) {
        sum += rd->hour_buckets[i];
    }
    return sum;
}

/* ---- helpers ---------------------------------------------------------- */

static int hex_decode(const char *hex, uint8_t *out, size_t max, size_t *len)
{
    size_t n = strlen(hex);
    size_t i;

    if (n == 0 || n % 2 != 0 || n / 2 > max) {
        return -1;
    }
    for (i = 0; i < n; i += 2) {
        unsigned int v;
        char tmp[3] = { hex[i], hex[i + 1], '\0' };
        char *end;

        v = (unsigned int)strtoul(tmp, &end, 16);
        if (*end != '\0') {
            return -1;
        }
        out[i / 2] = (uint8_t)v;
    }
    *len = n / 2;
    return 0;
}

static char *hex_encode(const uint8_t *data, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    char *s = malloc(len * 2 + 1);
    size_t i;

    if (!s) {
        return NULL;
    }
    for (i = 0; i < len; i++) {
        s[i * 2] = digits[data[i] >> 4];
        s[i * 2 + 1] = digits[data[i] & 0xf];
    }
    s[len * 2] = '\0';
    return s;
}

/* Returns 1 if found and numeric, 0 if absent, -1 if wrong type. */
static int get_number(const cJSON *obj, const char *key, double *out)
{
    const cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;

    if (!v) {
        return 0;
    }
    if (!cJSON_IsNumber(v)) {
        return -1;
    }
    *out = v->valuedouble;
    return 1;
}

/* Integer field: 1 if found and integral, 0 if absent, -1 if not a number
 * or not integral (7.9 is rejected, 7 and 7.0 are accepted). */
static int get_int(const cJSON *obj, const char *key, int *out)
{
    double v;
    int r = get_number(obj, key, &v);

    if (r <= 0) {
        return r;
    }
    if (v != floor(v) || v < -2147483648.0 || v > 2147483647.0) {
        return -1;
    }
    *out = (int)v;
    return 1;
}

static int get_bool(const cJSON *obj, const char *key, bool *out)
{
    const cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;

    if (!v) {
        return 0;
    }
    if (!cJSON_IsBool(v)) {
        return -1;
    }
    *out = cJSON_IsTrue(v);
    return 1;
}

static const char *get_string(const cJSON *obj, const char *key)
{
    const cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;

    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static cJSON *profile_to_json(const struct radio_profile *p)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddNumberToObject(o, "frequency_mhz", p->frequency_mhz);
    cJSON_AddNumberToObject(o, "bandwidth_khz", p->bandwidth_khz);
    cJSON_AddNumberToObject(o, "spreading_factor", p->spreading_factor);
    cJSON_AddNumberToObject(o, "coding_rate", p->coding_rate);
    cJSON_AddNumberToObject(o, "sync_word", p->sync_word);
    cJSON_AddNumberToObject(o, "preamble_length", p->preamble_length);
    cJSON_AddNumberToObject(o, "tx_power_dbm", p->tx_power_dbm);
    cJSON_AddBoolToObject(o, "crc", p->crc);
    return o;
}

static bool bandwidth_valid(double bw)
{
    size_t i;

    for (i = 0; i < sizeof(lora_bandwidths_khz) / sizeof(lora_bandwidths_khz[0]); i++) {
        if (fabs(lora_bandwidths_khz[i] - bw) < 0.05) {
            return true;
        }
    }
    return false;
}

/* Merge params into *p and validate. Returns 0, or an error code with msg. */
static int profile_apply(struct radiod *rd, const cJSON *params,
                         struct radio_profile *p, char *msg, size_t n)
{
    double v;
    bool b;
    int r;

    if ((r = get_number(params, "frequency_mhz", &v)) < 0) goto bad_type;
    if (r > 0) p->frequency_mhz = v;
    if ((r = get_number(params, "bandwidth_khz", &v)) < 0) goto bad_type;
    if (r > 0) p->bandwidth_khz = v;
    if ((r = get_int(params, "spreading_factor", &p->spreading_factor)) < 0) goto bad_int;
    if ((r = get_int(params, "coding_rate", &p->coding_rate)) < 0) goto bad_int;
    if ((r = get_int(params, "sync_word", &p->sync_word)) < 0) goto bad_int;
    if ((r = get_int(params, "preamble_length", &p->preamble_length)) < 0) goto bad_int;
    if ((r = get_int(params, "tx_power_dbm", &p->tx_power_dbm)) < 0) goto bad_int;
    if ((r = get_bool(params, "crc", &b)) < 0) goto bad_type;
    if (r > 0) p->crc = b;

    if (p->spreading_factor < 5 || p->spreading_factor > 12) {
        snprintf(msg, n, "spreading_factor must be 5..12");
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    if (!bandwidth_valid(p->bandwidth_khz)) {
        snprintf(msg, n, "bandwidth_khz must be a LoRa bandwidth (7.8..500)");
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    if (p->coding_rate < 5 || p->coding_rate > 8) {
        snprintf(msg, n, "coding_rate must be 5..8");
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    if (p->sync_word < 0 || p->sync_word > 0xff) {
        snprintf(msg, n, "sync_word must be 0..255");
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    if (p->preamble_length < 1 || p->preamble_length > 65535) {
        snprintf(msg, n, "preamble_length must be 1..65535");
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    if (p->tx_power_dbm < rd->caps.tx_power_min_dbm ||
        p->tx_power_dbm > rd->caps.tx_power_max_dbm) {
        snprintf(msg, n, "tx_power_dbm must be %d..%d for %s",
                 rd->caps.tx_power_min_dbm, rd->caps.tx_power_max_dbm,
                 rd->be.ops->chip);
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    if (p->frequency_mhz < rd->caps.frequency_min_mhz ||
        p->frequency_mhz > rd->caps.frequency_max_mhz) {
        snprintf(msg, n, "frequency_mhz outside %s range %.1f..%.1f",
                 rd->be.ops->chip, rd->caps.frequency_min_mhz,
                 rd->caps.frequency_max_mhz);
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    if (p->frequency_mhz < rd->region->frequency_min_mhz ||
        p->frequency_mhz > rd->region->frequency_max_mhz) {
        snprintf(msg, n, "frequency_mhz outside region %s (%.3f..%.3f)",
                 rd->region->name, rd->region->frequency_min_mhz,
                 rd->region->frequency_max_mhz);
        return POCKETIPC_ERR_POLICY;
    }
    if (p->tx_power_dbm > rd->region->max_tx_power_dbm) {
        snprintf(msg, n, "tx_power_dbm above region %s limit %d",
                 rd->region->name, rd->region->max_tx_power_dbm);
        return POCKETIPC_ERR_POLICY;
    }
    return 0;

bad_type:
    snprintf(msg, n, "frequency_mhz and bandwidth_khz must be numbers, crc a bool");
    return POCKETIPC_ERR_INVALID_PARAMS;
bad_int:
    snprintf(msg, n, "spreading_factor, coding_rate, sync_word, preamble_length and "
                     "tx_power_dbm must be integers");
    return POCKETIPC_ERR_INVALID_PARAMS;
}

/* ---- events ----------------------------------------------------------- */

static void broadcast(struct radiod *rd, cJSON *msg)
{
    if (rd->server) {
        pocketipc_server_broadcast(rd->server, msg);
    } else {
        cJSON_Delete(msg);
    }
}

static void set_state(struct radiod *rd, const char *state)
{
    cJSON *data;

    if (strcmp(rd->state, state) == 0) {
        return;
    }
    rd->state = state;
    data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "state", state);
    broadcast(rd, pocketipc_event("radio.state", data));
}

/* Report "rx" only when the backend confirms it is receiving (Finding 2).
 * Backends without is_receiving() are assumed to receive whenever idle. */
static void update_rx_state(struct radiod *rd)
{
    if (strcmp(rd->state, "tx") == 0) {
        return;
    }
    if (rd->be.ops->is_receiving && !rd->be.ops->is_receiving(&rd->be)) {
        if (strcmp(rd->state, "error") != 0) {
            LOG_ERROR("transceiver is not in receive mode; state error");
        }
        set_state(rd, "error");
    } else {
        set_state(rd, "rx");
    }
}

/* In state error, ask the backend to re-enter RX at most once per second. */
static void recover_rx(struct radiod *rd)
{
    char err[128] = "";
    uint64_t now;

    if (strcmp(rd->state, "error") != 0 || !rd->be.ops->resume_rx) {
        return;
    }
    now = mono_ms();
    if (now - rd->last_rx_recovery_ms < 1000u) {
        return;
    }
    rd->last_rx_recovery_ms = now;
    if (rd->be.ops->resume_rx(&rd->be, err, sizeof(err)) == 0) {
        LOG_INFO("receive mode recovered");
    } else {
        LOG_WARN("receive recovery failed: %s", err);
    }
    update_rx_state(rd);
}

static void drain_receive(struct radiod *rd)
{
    struct radio_rx_packet pkt;
    int r;

    while ((r = rd->be.ops->receive(&rd->be, &pkt)) != 0) {
        if (r < 0) {
            if (r == -EBADMSG) {
                rd->rx_crc_errors++;
                LOG_DEBUG("rx packet with CRC error dropped");
                continue;
            }
            LOG_WARN("receive failed: %d", r);
            update_rx_state(rd);
            break;
        }
        const struct radio_profile *p = &rd->be.profile;
        double airtime = lora_airtime_ms(p->spreading_factor, p->bandwidth_khz,
                                         p->coding_rate, p->preamble_length,
                                         pkt.len, p->crc, false);
        cJSON *data = cJSON_CreateObject();
        char *hex = hex_encode(pkt.data, pkt.len);

        rd->rx_packets++;
        rd->rx_airtime_ms += airtime > 0 ? airtime : 0;
        rd->last_rssi_dbm = pkt.rssi_dbm;
        rd->last_snr_db = pkt.snr_db;
        rd->last_frequency_error_hz = pkt.frequency_error_hz;
        rd->have_last_rx = true;
        cJSON_AddStringToObject(data, "payload_hex", hex ? hex : "");
        free(hex);
        cJSON_AddNumberToObject(data, "bytes", (double)pkt.len);
        cJSON_AddNumberToObject(data, "rssi_dbm", pkt.rssi_dbm);
        cJSON_AddNumberToObject(data, "snr_db", pkt.snr_db);
        cJSON_AddNumberToObject(data, "frequency_error_hz", pkt.frequency_error_hz);
        cJSON_AddNumberToObject(data, "timestamp_ms", (double)pkt.timestamp_ms);
        cJSON_AddNumberToObject(data, "airtime_ms", airtime);
        broadcast(rd, pocketipc_event("radio.rx", data));
    }
}

/* ---- methods ---------------------------------------------------------- */

static cJSON *m_info(struct radiod *rd)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *caps = cJSON_CreateObject();
    cJSON *mods = cJSON_CreateArray();

    cJSON_AddStringToObject(o, "chip", rd->be.ops->chip);
    cJSON_AddStringToObject(o, "backend", rd->be.ops->name);
    cJSON_AddNumberToObject(o, "api_version", RADIOD_API_VERSION);
    cJSON_AddStringToObject(o, "region", rd->region->name);
    cJSON_AddNumberToObject(caps, "frequency_min_mhz", rd->caps.frequency_min_mhz);
    cJSON_AddNumberToObject(caps, "frequency_max_mhz", rd->caps.frequency_max_mhz);
    cJSON_AddNumberToObject(caps, "tx_power_min_dbm", rd->caps.tx_power_min_dbm);
    cJSON_AddNumberToObject(caps, "tx_power_max_dbm", rd->caps.tx_power_max_dbm);
    cJSON_AddNumberToObject(caps, "max_payload", rd->caps.max_payload);
    cJSON_AddBoolToObject(caps, "cad", rd->caps.cad && rd->be.ops->cad != NULL);
    cJSON_AddItemToArray(mods, cJSON_CreateString("lora"));
    cJSON_AddItemToObject(caps, "modulations", mods);
    cJSON_AddItemToObject(o, "capabilities", caps);
    return o;
}

static cJSON *m_status(struct radiod *rd)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddStringToObject(o, "state", rd->state);
    cJSON_AddItemToObject(o, "profile", profile_to_json(&rd->be.profile));
    cJSON_AddNumberToObject(o, "uptime_s", (double)((mono_ms() - rd->start_mono_ms) / 1000u));
    return o;
}

static cJSON *m_stats(struct radiod *rd)
{
    cJSON *o = cJSON_CreateObject();
    double hour = buckets_last_hour(rd);

    cJSON_AddNumberToObject(o, "tx_packets", (double)rd->tx_packets);
    cJSON_AddNumberToObject(o, "rx_packets", (double)rd->rx_packets);
    cJSON_AddNumberToObject(o, "rx_crc_errors", (double)rd->rx_crc_errors);
    cJSON_AddNumberToObject(o, "tx_airtime_ms", rd->tx_airtime_ms);
    cJSON_AddNumberToObject(o, "rx_airtime_ms", rd->rx_airtime_ms);
    cJSON_AddNumberToObject(o, "tx_airtime_last_hour_ms", hour);
    cJSON_AddNumberToObject(o, "duty_cycle_last_hour_percent", hour / 3600000.0 * 100.0);
    if (rd->have_last_rx) {
        cJSON_AddNumberToObject(o, "last_rssi_dbm", rd->last_rssi_dbm);
        cJSON_AddNumberToObject(o, "last_snr_db", rd->last_snr_db);
        cJSON_AddNumberToObject(o, "last_frequency_error_hz", rd->last_frequency_error_hz);
    }
    return o;
}

static cJSON *m_configure(struct radiod *rd, const cJSON *params, int *code, char *msg, size_t n)
{
    struct radio_profile p = rd->be.profile;
    int rc;

    if (params && !cJSON_IsObject(params)) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(msg, n, "params must be an object");
        return NULL;
    }
    rc = profile_apply(rd, params, &p, msg, n);
    if (rc) {
        *code = rc;
        return NULL;
    }
    if (rd->be.ops->configure(&rd->be, &p, msg, n) < 0) {
        *code = POCKETIPC_ERR_BACKEND;
        return NULL;
    }
    rd->be.profile = p;
    return profile_to_json(&p);
}

static cJSON *m_send(struct radiod *rd, const cJSON *params, int *code, char *msg, size_t n)
{
    const char *hex = get_string(params, "payload_hex");
    uint8_t data[RADIO_MAX_PAYLOAD];
    size_t len;
    double airtime = 0.0;
    cJSON *o;
    cJSON *ev;

    if (!hex || hex_decode(hex, data, (size_t)rd->caps.max_payload, &len) < 0) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(msg, n, "payload_hex must be 1..%d bytes of hex", rd->caps.max_payload);
        return NULL;
    }
    if (params && cJSON_HasObjectItem(params, "timeout_ms")) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(msg, n, "timeout_ms is not supported: radio.send is synchronous in v0");
        return NULL;
    }
    if (strcmp(rd->state, "tx") == 0) {
        *code = POCKETIPC_ERR_BUSY;
        snprintf(msg, n, "transmission in progress");
        return NULL;
    }
    set_state(rd, "tx");
    if (rd->be.ops->send(&rd->be, data, len, &airtime, msg, n) < 0) {
        rd->state = "idle";
        update_rx_state(rd);
        *code = POCKETIPC_ERR_BACKEND;
        return NULL;
    }
    rd->tx_packets++;
    rd->tx_airtime_ms += airtime;
    buckets_add_tx(rd, airtime);
    rd->state = "idle";
    update_rx_state(rd);

    ev = cJSON_CreateObject();
    cJSON_AddNumberToObject(ev, "bytes", (double)len);
    cJSON_AddNumberToObject(ev, "airtime_ms", airtime);
    cJSON_AddNumberToObject(ev, "timestamp_ms", (double)radio_now_ms());
    broadcast(rd, pocketipc_event("radio.tx_done", ev));

    o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "bytes", (double)len);
    cJSON_AddNumberToObject(o, "airtime_ms", airtime);
    return o;
}

static cJSON *m_cad(struct radiod *rd, int *code, char *msg, size_t n)
{
    bool activity = false;
    cJSON *o;

    if (!rd->be.ops->cad) {
        *code = POCKETIPC_ERR_UNSUPPORTED;
        snprintf(msg, n, "CAD not supported by backend %s", rd->be.ops->name);
        return NULL;
    }
    if (rd->be.ops->cad(&rd->be, &activity) < 0) {
        update_rx_state(rd);
        *code = POCKETIPC_ERR_BACKEND;
        snprintf(msg, n, "CAD failed");
        return NULL;
    }
    update_rx_state(rd);
    o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "activity", activity);
    return o;
}

static cJSON *m_rssi(struct radiod *rd, int *code, char *msg, size_t n)
{
    double dbm = 0.0;
    cJSON *o;

    if (!rd->be.ops->rssi) {
        *code = POCKETIPC_ERR_UNSUPPORTED;
        snprintf(msg, n, "RSSI not supported by backend %s", rd->be.ops->name);
        return NULL;
    }
    if (rd->be.ops->rssi(&rd->be, &dbm) < 0) {
        *code = POCKETIPC_ERR_BACKEND;
        snprintf(msg, n, "RSSI read failed");
        return NULL;
    }
    o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "rssi_dbm", dbm);
    return o;
}

static cJSON *m_inject(struct radiod *rd, const cJSON *params, int *code, char *msg, size_t n)
{
    const char *hex = get_string(params, "payload_hex");
    struct radio_rx_packet pkt;
    double v;

    if (!rd->be.ops->inject_rx) {
        *code = POCKETIPC_ERR_UNSUPPORTED;
        snprintf(msg, n, "mock.inject_rx only exists on the mock backend");
        return NULL;
    }
    memset(&pkt, 0, sizeof(pkt));
    if (!hex || hex_decode(hex, pkt.data, sizeof(pkt.data), &pkt.len) < 0) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(msg, n, "payload_hex must be 1..255 bytes of hex");
        return NULL;
    }
    pkt.rssi_dbm = get_number(params, "rssi_dbm", &v) > 0 ? v : -90.0;
    pkt.snr_db = get_number(params, "snr_db", &v) > 0 ? v : 7.5;
    pkt.frequency_error_hz = 0.0;
    pkt.timestamp_ms = radio_now_ms();
    if (rd->be.ops->inject_rx(&rd->be, &pkt) < 0) {
        *code = POCKETIPC_ERR_BUSY;
        snprintf(msg, n, "mock receive queue full");
        return NULL;
    }
    return cJSON_CreateObject();
}

static void on_request(struct pocketipc_server *s, struct pocketipc_client *c, cJSON *req,
                       void *user)
{
    struct radiod *rd = user;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const char *method = get_string(req, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    cJSON *result = NULL;
    int code = 0;
    char msg[160] = "";

    if (!method) {
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                    "missing method"));
        return;
    }
    LOG_DEBUG("fd %d %s", pocketipc_client_fd(c), method);
    if (strcmp(method, "radio.info") == 0) {
        result = m_info(rd);
    } else if (strcmp(method, "radio.status") == 0) {
        result = m_status(rd);
    } else if (strcmp(method, "radio.stats") == 0) {
        result = m_stats(rd);
    } else if (strcmp(method, "radio.configure") == 0) {
        result = m_configure(rd, params, &code, msg, sizeof(msg));
    } else if (strcmp(method, "radio.send") == 0) {
        result = m_send(rd, params, &code, msg, sizeof(msg));
    } else if (strcmp(method, "radio.cad") == 0) {
        result = m_cad(rd, &code, msg, sizeof(msg));
    } else if (strcmp(method, "radio.rssi") == 0) {
        result = m_rssi(rd, &code, msg, sizeof(msg));
    } else if (strcmp(method, "radio.subscribe") == 0) {
        pocketipc_client_set_subscribed(c, true);
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "subscribed", true);
    } else if (strcmp(method, "radio.unsubscribe") == 0) {
        pocketipc_client_set_subscribed(c, false);
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "subscribed", false);
    } else if (strcmp(method, "mock.inject_rx") == 0) {
        result = m_inject(rd, params, &code, msg, sizeof(msg));
    } else if (strcmp(method, "mock.set") == 0) {
        const char *key = get_string(params, "key");
        int value = 0;

        if (!rd->be.ops->debug_set) {
            code = POCKETIPC_ERR_UNSUPPORTED;
            snprintf(msg, sizeof(msg), "mock.set only exists on test backends");
        } else if (!key || get_int(params, "value", &value) <= 0) {
            code = POCKETIPC_ERR_INVALID_PARAMS;
            snprintf(msg, sizeof(msg), "key (string) and value (integer) required");
        } else if (rd->be.ops->debug_set(&rd->be, key, value) < 0) {
            code = POCKETIPC_ERR_INVALID_PARAMS;
            snprintf(msg, sizeof(msg), "unknown key %s", key);
        } else {
            update_rx_state(rd);
            result = cJSON_CreateObject();
        }
    } else {
        code = POCKETIPC_ERR_UNKNOWN_METHOD;
        snprintf(msg, sizeof(msg), "unknown method %s", method);
    }

    if (result) {
        pocketipc_server_reply(s, c, pocketipc_response(id, result));
    } else {
        pocketipc_server_reply(s, c, pocketipc_error_response(id, code ? code : POCKETIPC_ERR_BACKEND,
                                                    msg[0] ? msg : "failed"));
    }
}

/* ---- main loop -------------------------------------------------------- */

static int run(struct radiod *rd)
{
    while (!stop_requested) {
        int radio_fd = rd->be.ops->poll_fd ? rd->be.ops->poll_fd(&rd->be) : -1;
        int radio_ready = 0;

        if (pocketipc_server_poll_fd(rd->server, 200, radio_fd, &radio_ready) < 0) {
            LOG_ERROR("poll: %s", strerror(errno));
            return 1;
        }
        drain_receive(rd);
        recover_rx(rd);
    }
    return 0;
}

static const struct region *find_region(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        if (strcmp(regions[i].name, name) == 0) {
            return &regions[i];
        }
    }
    return NULL;
}

static void usage(FILE *out)
{
    fprintf(out,
            "usage: radiod [--backend mock|sx1262] [--region EU868|NONE]\n"
            "              [--socket-name NAME] [--verbose]\n"
            "Runtime directory: $POCKETOS_RUNTIME_DIR or %s\n",
            POCKETIPC_DEFAULT_DIR);
}

int main(int argc, char **argv)
{
    struct radiod rd;
    const char *backend = "mock";
    const char *region = "EU868";
    char err[160] = "";
    int i;
    int rc;

    memset(&rd, 0, sizeof(rd));
    snprintf(rd.socket_name, sizeof(rd.socket_name), "radiod");
    pocketlog_init("radiod");
    pocketlog_install_crash_handler();
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (strcmp(argv[i], "--region") == 0 && i + 1 < argc) {
            region = argv[++i];
        } else if (strcmp(argv[i], "--socket-name") == 0 && i + 1 < argc) {
            snprintf(rd.socket_name, sizeof(rd.socket_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            rd.verbose = true;
            pocketlog_set_level(POCKETLOG_DEBUG);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }

    if (strcmp(backend, "mock") == 0) {
        rd.be.ops = &radio_backend_mock_ops;
    } else if (strcmp(backend, "sx1262") == 0) {
#ifdef POCKETOS_HAVE_SX1262
        rd.be.ops = &radio_backend_sx1262_ops;
#else
        LOG_ERROR("backend sx1262 was not compiled in (ENABLE_SX1262=1)");
        return 2;
#endif
    } else {
        LOG_ERROR("unknown backend %s", backend);
        return 2;
    }
    rd.region = find_region(region);
    if (!rd.region) {
        LOG_ERROR("unknown region %s", region);
        return 2;
    }
    rd.state = "off";
    rd.start_mono_ms = mono_ms();
    rd.bucket_minute = (int64_t)(rd.start_mono_ms / 60000u);

    if (rd.be.ops->init(&rd.be, err, sizeof(err)) < 0) {
        LOG_ERROR("backend init failed: %s", err);
        return 1;
    }
    rd.be.ops->get_caps(&rd.be, &rd.caps);

    /* EU868 defaults, see docs/api/radio.md. */
    rd.be.profile.frequency_mhz = 869.525;
    rd.be.profile.bandwidth_khz = 125.0;
    rd.be.profile.spreading_factor = 9;
    rd.be.profile.coding_rate = 5;
    rd.be.profile.sync_word = 0x12;
    rd.be.profile.preamble_length = 8;
    rd.be.profile.tx_power_dbm = 14;
    rd.be.profile.crc = true;
    if (strcmp(rd.region->name, "NONE") == 0) {
        rd.be.profile.frequency_mhz = 868.0;
    }
    if (rd.be.ops->configure(&rd.be, &rd.be.profile, err, sizeof(err)) < 0) {
        LOG_ERROR("initial configure failed: %s", err);
        rd.be.ops->shutdown(&rd.be);
        return 1;
    }
    rd.state = "rx";

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    rd.server = pocketipc_server_new(rd.socket_name, on_request, &rd);
    if (!rd.server) {
        LOG_ERROR("cannot listen: %s", strerror(errno));
        rd.be.ops->shutdown(&rd.be);
        return 1;
    }
    LOG_INFO("listening on %s backend=%s region=%s", pocketipc_server_path(rd.server),
             rd.be.ops->name, rd.region->name);

    rc = run(&rd);
    LOG_INFO("shutting down (rc=%d)", rc);

    pocketipc_server_free(rd.server);
    rd.be.ops->shutdown(&rd.be);
    pocketlog_close();
    return rc;
}
