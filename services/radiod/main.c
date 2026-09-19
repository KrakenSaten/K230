/*
 * radiod: PocketOS radio service. Owns the LoRa transceiver, enforces the
 * region guard, keeps airtime statistics and serves radio.* over pocketipc.
 * See docs/api/radio.md.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "airtime.h"
#include "lease.h"
#include "pocketipc/pocketipc.h"
#include "pocketipc/server.h"
#include "pocketlog/pocketlog.h"
#include "radio_backend.h"
#include "tx.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RADIOD_API_VERSION 0
#define HOUR_BUCKETS 60
/* Requested output power at start. 2 dBm is the bench-safe value used for
 * the first transmit on real hardware; the operator raises it per session
 * with radio.configure, and a restart returns to this value (or to
 * --tx-power-dbm), never silently to the region maximum. */
#define RADIOD_DEFAULT_TX_POWER_DBM 2

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
    struct radio_tx tx;
    struct radio_lease lease;
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

/* The two clocks live in services/radiod/radio_time.c, so the daemon, the
 * backends and the host tests that link the state machine without this file
 * all read the same ones. */
static uint64_t mono_ms(void)
{
    return radio_mono_ms();
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

/* One hexadecimal digit, or -1. Deliberately not isxdigit(): that is
 * locale-dependent and takes an int that must not be a negative char. */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Exactly two hexadecimal digits per byte. This used to hand each pair to
 * strtoul(), which skips leading whitespace and accepts a sign, so
 * payload_hex "-1" was transmitted as ff, "+a" as 0a and " a" as 0a: text
 * that is not hex at all became bytes on the air, silently and differently
 * from what was asked for. Upper and lower case are both accepted, as they
 * always were. */
static int hex_decode(const char *hex, uint8_t *out, size_t max, size_t *len)
{
    size_t n = strlen(hex);
    size_t i;

    if (n == 0 || n % 2 != 0 || n / 2 > max) {
        return -1;
    }
    for (i = 0; i < n; i += 2) {
        int hi = hex_digit(hex[i]);
        int lo = hex_digit(hex[i + 1]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i / 2] = (uint8_t)((hi << 4) | lo);
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
            break;
        }
        const struct radio_profile *p = &rd->be.profile;
        double airtime = lora_airtime_ms(p->spreading_factor, p->bandwidth_khz,
                                         p->coding_rate, p->preamble_length,
                                         pkt.len, p->crc, false);
        cJSON *data = cJSON_CreateObject();
        char *hex = hex_encode(pkt.data, pkt.len);

        /* A backend that did not stamp the packet gets it stamped here, so
         * no packet reaches a client without a monotonic time. Later than
         * the backend's own reading by the cost of one read, and the right
         * order of magnitude better than a zero a consumer would subtract. */
        if (pkt.mono_ms == 0) {
            pkt.mono_ms = mono_ms();
        }

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
        cJSON_AddNumberToObject(data, "mono_ms", (double)pkt.mono_ms);
        cJSON_AddNumberToObject(data, "airtime_ms", airtime);
        broadcast(rd, pocketipc_event("radio.rx", data));
    }
    /* Whatever ended the drain, the backend re-entered receive - or tried to -
     * while doing it, and only it knows whether that worked. SX1262 re-enters
     * after reading a packet and after a CRC error, and reports the result
     * through is_receiving() alone (backend_sx1262.cpp, sx_receive). This used
     * to run on the one exit that never happens in practice, the unexpected
     * read failure, so a transceiver that stopped receiving while handing over
     * a good packet was reported as "rx" for the rest of the session: the
     * state never became "error", and recover_rx only runs in "error". */
    update_rx_state(rd);
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
    cJSON_AddStringToObject(o, "version", pocketlog_version());
    cJSON_AddStringToObject(o, "build", pocketlog_build_id());
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
    /* Only present when it is true, so an ordinary status is unchanged. The
     * profile above is then the last one this daemon successfully applied,
     * not a description of the transceiver: a configure changed the chip,
     * failed part way, and the previous settings could not be put back
     * either. A bench report that cannot tell those apart is not evidence. */
    if (rd->be.profile_uncertain) {
        cJSON_AddBoolToObject(o, "profile_uncertain", true);
    }
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
    struct radio_profile previous;
    char rollback[160] = "";
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
    previous = rd->be.profile;
    rc = rd->be.ops->configure(&rd->be, &p, msg, n);
    if (rc == 0) {
        rd->be.profile = p;
        rd->be.profile_uncertain = false;
        update_rx_state(rd);
        return profile_to_json(&p);
    }

    /* Configuring a transceiver is several operations in order, and one of
     * them can fail after an earlier one has already changed the chip: on the
     * SX1262, begin() puts the whole radio configuration on the air and
     * setCRC() and startReceive() come after it. So a failed configure does
     * not mean the radio is untouched, and reporting the old profile as
     * though it still described the hardware would put the wrong frequency
     * and spreading factor in every status, every log and every bench report
     * that followed.
     *
     * The previous profile is asked for again. If that succeeds the radio is
     * back where the caller still believes it is and nothing has been lost
     * but the request. If it fails too, neither profile is on the chip and
     * nothing here can say what is: the daemon says so rather than choosing
     * one to report, until a configure succeeds. */
    if (rd->be.ops->configure(&rd->be, &previous, rollback, sizeof(rollback)) == 0) {
        rd->be.profile = previous;
        rd->be.profile_uncertain = false;
        LOG_WARN("configure failed (%s); the previous profile was restored", msg);
    } else {
        rd->be.profile_uncertain = true;
        LOG_ERROR("configure failed (%s) and the previous profile could not be "
                  "restored (%s); the transceiver's settings are unknown", msg, rollback);
    }
    update_rx_state(rd);
    *code = POCKETIPC_ERR_BACKEND;
    return NULL;
}

/* ---- transmit ---------------------------------------------------------- */

static const char *tx_result_name(enum radio_tx_result r)
{
    switch (r) {
    case RADIO_TX_OK:                return "ok";
    case RADIO_TX_FAILED:            return "tx_failed";
    case RADIO_TX_RX_RESUME_FAILED:  return "rx_resume_failed";
    }
    return "unknown";
}

/* Every completed transmission arrives here, from either path: statistics,
 * then the receive state, then the event. In that order, because tx_done
 * carries the state the radio ended up in and a client reading "rx" from it
 * must be reading something that has actually been checked. */
static void on_tx_done(const struct radio_tx_done *d, void *user)
{
    struct radiod *rd = user;
    cJSON *ev;

    if (d->transmitted) {
        rd->tx_packets++;
        rd->tx_airtime_ms += d->airtime_ms;
        buckets_add_tx(rd, d->airtime_ms);
    }
    /* Straight assignment, not set_state: "idle" is a step on the way out of
     * transmitting and never a state anyone should see announced. The event
     * that matters is the one update_rx_state sends next. */
    rd->state = "idle";
    update_rx_state(rd);

    if (!d->ok) {
        LOG_WARN("tx %llu: %s (%s)", (unsigned long long)d->tx_id,
                 tx_result_name(d->result), d->error);
    }

    ev = cJSON_CreateObject();
    cJSON_AddNumberToObject(ev, "tx_id", (double)d->tx_id);
    cJSON_AddBoolToObject(ev, "ok", d->ok);
    cJSON_AddStringToObject(ev, "result", tx_result_name(d->result));
    cJSON_AddBoolToObject(ev, "transmitted", d->transmitted);
    cJSON_AddBoolToObject(ev, "rx_resumed", d->rx_resumed);
    cJSON_AddStringToObject(ev, "state", rd->state);
    cJSON_AddNumberToObject(ev, "bytes", (double)d->bytes);
    cJSON_AddNumberToObject(ev, "airtime_ms", d->airtime_ms);
    cJSON_AddNumberToObject(ev, "mono_ms", (double)d->mono_ms);
    cJSON_AddNumberToObject(ev, "timestamp_ms", (double)d->timestamp_ms);
    if (d->error[0]) {
        cJSON_AddStringToObject(ev, "error", d->error);
    }
    broadcast(rd, pocketipc_event("radio.tx_done", ev));
}

/* Shared front half of radio.send and radio.send_async: decode, check, hand
 * to the state machine. Returns 0 with *tx_id set, or an error code. */
static int tx_accept(struct radiod *rd, const cJSON *params, bool async,
                     uint64_t client_id, uint64_t *tx_id, size_t *bytes, double *airtime_ms,
                     int *code, char *msg, size_t n)
{
    const char *hex = get_string(params, "payload_hex");
    uint8_t data[RADIO_MAX_PAYLOAD];
    size_t len;
    int rc;

    if (!hex || hex_decode(hex, data, (size_t)rd->caps.max_payload, &len) < 0) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(msg, n, "payload_hex must be 1..%d bytes of hex", rd->caps.max_payload);
        return -1;
    }
    if (params && cJSON_HasObjectItem(params, "timeout_ms")) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(msg, n, "timeout_ms is not supported: use radio.send_async and "
                         "the radio.tx_done event");
        return -1;
    }
    rc = radio_tx_submit(&rd->tx, data, len, client_id, async, tx_id);
    if (rc == -EBUSY) {
        *code = POCKETIPC_ERR_BUSY;
        snprintf(msg, n, "transmission in progress (tx_id %llu)",
                 (unsigned long long)radio_tx_active_id(&rd->tx));
        return -1;
    }
    if (rc < 0) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(msg, n, "payload_hex must be 1..%d bytes of hex", rd->caps.max_payload);
        return -1;
    }
    if (bytes) {
        *bytes = len;
    }
    if (airtime_ms) {
        *airtime_ms = rd->tx.airtime_ms;
    }
    set_state(rd, "tx");
    return 0;
}

/* The synchronous transmit, kept because every caller written against v0
 * uses it. It is the same state machine as radio.send_async with the wait
 * moved inside the request: submit, drive to completion here, answer with
 * the outcome. One transmit path, two ways of waiting for it. */
static cJSON *m_send(struct radiod *rd, const cJSON *params, uint64_t client_id,
                     int *code, char *msg, size_t n)
{
    const struct radio_tx_done *d;
    uint64_t tx_id = 0;
    cJSON *o;

    if (tx_accept(rd, params, false, client_id, &tx_id, NULL, NULL, code, msg, n) < 0) {
        return NULL;
    }
    radio_tx_run(&rd->tx);
    d = radio_tx_last(&rd->tx);
    if (!d || d->tx_id != tx_id) {
        /* Cannot happen: radio_tx_run only returns once the active job has
         * completed, and completion records itself before anything else can
         * submit. Refusing to invent an answer costs one branch. */
        *code = POCKETIPC_ERR_BACKEND;
        snprintf(msg, n, "transmit %llu produced no result", (unsigned long long)tx_id);
        return NULL;
    }
    if (!d->transmitted) {
        *code = POCKETIPC_ERR_BACKEND;
        snprintf(msg, n, "%s", d->error[0] ? d->error : "transmit failed");
        return NULL;
    }
    /* The v0 result, unchanged, plus the id the completion event carries.
     * A failed re-entry into receive is not reported as a failed send here:
     * the packet went out, and that is what this call answers. It is in the
     * radio.tx_done event, in radio.status, and in the radio.state event
     * that has already been sent. */
    o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "bytes", (double)d->bytes);
    cJSON_AddNumberToObject(o, "airtime_ms", d->airtime_ms);
    cJSON_AddNumberToObject(o, "tx_id", (double)d->tx_id);
    return o;
}

static cJSON *m_send_async(struct radiod *rd, const cJSON *params, uint64_t client_id,
                           int *code, char *msg, size_t n)
{
    uint64_t tx_id = 0;
    size_t bytes = 0;
    double airtime_ms = 0.0;
    cJSON *o;

    if (tx_accept(rd, params, true, client_id, &tx_id, &bytes, &airtime_ms,
                  code, msg, n) < 0) {
        return NULL;
    }
    /* Accepted, not completed. Nothing here says the packet went out; the
     * radio has not been touched yet. The transmission is started from the
     * service loop, after this reply has been written, so a client that
     * times the two can see the acceptance arrive first. */
    o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "accepted", true);
    cJSON_AddNumberToObject(o, "tx_id", (double)tx_id);
    cJSON_AddNumberToObject(o, "bytes", (double)bytes);
    /* The airtime this profile will take for this length, computed from the
     * Semtech formula before the packet goes out. The completion event
     * repeats it, from the backend where the backend measures one. */
    cJSON_AddNumberToObject(o, "airtime_ms", airtime_ms);
    return o;
}

/* ---- lease -------------------------------------------------------------- */

static cJSON *lease_json(struct radiod *rd, uint64_t client_id)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "held", radio_lease_held(&rd->lease));
    cJSON_AddBoolToObject(o, "mine", radio_lease_is_owner(&rd->lease, client_id));
    if (radio_lease_held(&rd->lease)) {
        cJSON_AddStringToObject(o, "owner", radio_lease_owner(&rd->lease));
        cJSON_AddNumberToObject(o, "owner_id", (double)radio_lease_owner_id(&rd->lease));
        cJSON_AddNumberToObject(o, "since_mono_ms", (double)radio_lease_since(&rd->lease));
    }
    return o;
}

static void lease_changed(struct radiod *rd, const char *reason)
{
    cJSON *ev = cJSON_CreateObject();

    cJSON_AddBoolToObject(ev, "held", radio_lease_held(&rd->lease));
    if (radio_lease_held(&rd->lease)) {
        cJSON_AddStringToObject(ev, "owner", radio_lease_owner(&rd->lease));
        cJSON_AddNumberToObject(ev, "owner_id", (double)radio_lease_owner_id(&rd->lease));
    }
    cJSON_AddStringToObject(ev, "reason", reason);
    cJSON_AddNumberToObject(ev, "mono_ms", (double)mono_ms());
    broadcast(rd, pocketipc_event("radio.lease", ev));
}

static cJSON *m_acquire(struct radiod *rd, const cJSON *params, uint64_t client_id,
                        int *code, char *msg, size_t n)
{
    const char *owner = get_string(params, "owner");
    bool was_held = radio_lease_held(&rd->lease);
    uint64_t owner_id = 0;
    int rc;

    if (params && !cJSON_IsObject(params)) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(msg, n, "params must be an object");
        return NULL;
    }
    if (owner && strlen(owner) >= RADIO_LEASE_OWNER_MAX) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(msg, n, "owner must be shorter than %d characters", RADIO_LEASE_OWNER_MAX);
        return NULL;
    }
    rc = radio_lease_acquire(&rd->lease, client_id, owner, mono_ms(), &owner_id);
    if (rc == -EBUSY) {
        *code = POCKETIPC_ERR_BUSY;
        snprintf(msg, n, "the radio is leased by %s (owner_id %llu)",
                 radio_lease_owner(&rd->lease),
                 (unsigned long long)radio_lease_owner_id(&rd->lease));
        return NULL;
    }
    if (rc < 0) {
        *code = POCKETIPC_ERR_BACKEND;
        snprintf(msg, n, "the lease could not be taken");
        return NULL;
    }
    if (!was_held) {
        LOG_INFO("radio leased by %s (owner_id %llu)", radio_lease_owner(&rd->lease),
                 (unsigned long long)owner_id);
        lease_changed(rd, "acquired");
    }
    return lease_json(rd, client_id);
}

static cJSON *m_release(struct radiod *rd, uint64_t client_id, int *code, char *msg, size_t n)
{
    if (radio_lease_release(&rd->lease, client_id) < 0) {
        *code = POCKETIPC_ERR_POLICY;
        if (radio_lease_held(&rd->lease)) {
            snprintf(msg, n, "the radio is leased by %s, not by you",
                     radio_lease_owner(&rd->lease));
        } else {
            snprintf(msg, n, "the radio is not leased");
        }
        return NULL;
    }
    LOG_INFO("radio lease released");
    lease_changed(rd, "released");
    return lease_json(rd, client_id);
}

/* ---- channel ------------------------------------------------------------ */

/* What can be said about the channel right now, with every value paired
 * with whether it is known at all. A backend that measures nothing produces
 * a perfectly valid answer here: all the *_known flags false. That is the
 * point - "unknown" is an answer, and it is the honest one for a caller
 * deciding whether the air is clear. */
static cJSON *m_channel(struct radiod *rd, int *code, char *msg, size_t n)
{
    struct radio_channel ch;
    bool transmitting = radio_tx_active(&rd->tx);
    cJSON *o;

    memset(&ch, 0, sizeof(ch));
    if (transmitting) {
        /* The radio is the one making the noise. Anything read from it now
         * describes our own transmission, not the channel. */
    } else if (rd->be.ops->channel) {
        if (rd->be.ops->channel(&rd->be, &ch) < 0) {
            *code = POCKETIPC_ERR_BACKEND;
            snprintf(msg, n, "channel read failed");
            return NULL;
        }
    } else if (rd->be.ops->rssi) {
        double dbm = 0.0;

        if (rd->be.ops->rssi(&rd->be, &dbm) == 0) {
            ch.rssi_known = true;
            ch.rssi_dbm = dbm;
        }
    }

    o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "mono_ms", (double)mono_ms());
    cJSON_AddBoolToObject(o, "transmitting", transmitting);
    cJSON_AddBoolToObject(o, "rssi_known", ch.rssi_known);
    if (ch.rssi_known) {
        cJSON_AddNumberToObject(o, "rssi_dbm", ch.rssi_dbm);
    }
    cJSON_AddBoolToObject(o, "noise_known", ch.noise_known);
    if (ch.noise_known) {
        cJSON_AddNumberToObject(o, "noise_dbm", ch.noise_dbm);
    }
    cJSON_AddBoolToObject(o, "activity_known", ch.activity_known);
    if (ch.activity_known) {
        cJSON_AddBoolToObject(o, "busy", ch.busy);
    }
    /* Whether radio.cad can be asked at all. Not the same question as
     * activity_known, and deliberately reported separately: CAD detects a
     * LoRa preamble on the configured modulation and takes the radio off
     * receive to do it, which is not what "is the channel busy" means. */
    cJSON_AddBoolToObject(o, "cad_supported", rd->caps.cad && rd->be.ops->cad != NULL);
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
    pkt.mono_ms = radio_mono_ms();
    /* Test-only override, so a suite can place a packet at a monotonic time
     * it would otherwise have to wait 49 days of uptime to reach. It exists
     * on the mock injection path and nowhere else; no real packet can carry
     * a time the daemon did not read from the clock itself. */
    if (get_number(params, "mono_ms", &v) > 0 && v >= 0) {
        pkt.mono_ms = (uint64_t)v;
    }
    if (rd->be.ops->inject_rx(&rd->be, &pkt) < 0) {
        *code = POCKETIPC_ERR_BUSY;
        snprintf(msg, n, "mock receive queue full");
        return NULL;
    }
    return cJSON_CreateObject();
}

/* Operations that use the radio, as opposed to describing it. While a lease
 * is held only its owner may ask for these; while nobody holds one they are
 * open to everybody, which is what every caller written before the lease
 * existed expects. Reads stay open either way: a diagnostic that cannot run
 * because a daemon owns the radio is a diagnostic nobody can use when they
 * most need it. */
static bool method_needs_lease(const char *method)
{
    return strcmp(method, "radio.configure") == 0 ||
           strcmp(method, "radio.send") == 0 ||
           strcmp(method, "radio.send_async") == 0 ||
           strcmp(method, "radio.cad") == 0;
}

/* Operations that cannot share the radio with a transmission in flight.
 * Before the asynchronous path existed this could not arise - the daemon was
 * inside the blocking send and answered nothing - so nothing guarded it.
 * Now a configure could land between tx_begin and tx_poll and change the
 * frequency of a packet already going out. */
static bool method_conflicts_with_tx(const char *method)
{
    return strcmp(method, "radio.configure") == 0 ||
           strcmp(method, "radio.cad") == 0;
}

static void on_request(struct pocketipc_server *s, struct pocketipc_client *c, cJSON *req,
                       void *user)
{
    struct radiod *rd = user;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const char *method = get_string(req, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    uint64_t client_id = pocketipc_client_id(c);
    cJSON *result = NULL;
    int code = 0;
    char msg[160] = "";

    if (!method) {
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                    "missing method"));
        return;
    }
    LOG_DEBUG("fd %d client %llu %s", pocketipc_client_fd(c),
              (unsigned long long)client_id, method);

    if (method_needs_lease(method) && !radio_lease_permits(&rd->lease, client_id)) {
        snprintf(msg, sizeof(msg), "%s needs the radio lease, held by %s (owner_id %llu)",
                 method, radio_lease_owner(&rd->lease),
                 (unsigned long long)radio_lease_owner_id(&rd->lease));
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_POLICY, msg));
        return;
    }
    if (method_conflicts_with_tx(method) && radio_tx_active(&rd->tx)) {
        snprintf(msg, sizeof(msg), "%s cannot run while tx_id %llu is on the air",
                 method, (unsigned long long)radio_tx_active_id(&rd->tx));
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_BUSY, msg));
        return;
    }

    if (strcmp(method, "radio.info") == 0) {
        result = m_info(rd);
    } else if (strcmp(method, "radio.status") == 0) {
        result = m_status(rd);
    } else if (strcmp(method, "radio.stats") == 0) {
        result = m_stats(rd);
    } else if (strcmp(method, "radio.configure") == 0) {
        result = m_configure(rd, params, &code, msg, sizeof(msg));
    } else if (strcmp(method, "radio.send") == 0) {
        result = m_send(rd, params, client_id, &code, msg, sizeof(msg));
    } else if (strcmp(method, "radio.send_async") == 0) {
        result = m_send_async(rd, params, client_id, &code, msg, sizeof(msg));
    } else if (strcmp(method, "radio.acquire") == 0) {
        result = m_acquire(rd, params, client_id, &code, msg, sizeof(msg));
    } else if (strcmp(method, "radio.release") == 0) {
        result = m_release(rd, client_id, &code, msg, sizeof(msg));
    } else if (strcmp(method, "radio.lease") == 0) {
        result = lease_json(rd, client_id);
    } else if (strcmp(method, "radio.channel") == 0) {
        result = m_channel(rd, &code, msg, sizeof(msg));
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

/* A client has gone. Two pieces of per-connection state outlive it if
 * nobody clears them, and both of them are the kind that goes wrong
 * quietly: a lease nobody can ever take again, and a transmission whose
 * submitter is being waited on. */
static void on_disconnect(struct pocketipc_server *s, uint64_t client_id, void *user)
{
    struct radiod *rd = user;

    (void)s;
    if (radio_tx_forget_client(&rd->tx, client_id)) {
        /* Not cancelled. The packet is already going out and stopping half
         * way would leave the transceiver keyed with nothing to bring it
         * back. It finishes, it is counted, and its completion goes to
         * whoever is still subscribed - possibly nobody. */
        LOG_WARN("tx %llu: the client that submitted it disconnected; "
                 "the transmit runs to completion",
                 (unsigned long long)radio_tx_active_id(&rd->tx));
    }
    if (radio_lease_client_gone(&rd->lease, client_id)) {
        LOG_INFO("radio lease released: the owner disconnected");
        lease_changed(rd, "client_gone");
    }
}

/* ---- main loop -------------------------------------------------------- */

static int run(struct radiod *rd)
{
    while (!stop_requested) {
        int radio_fd = rd->be.ops->poll_fd ? rd->be.ops->poll_fd(&rd->be) : -1;
        int radio_ready = 0;
        /* A transmission in flight is polled far more often than clients
         * are, so a packet's completion is reported when it happens rather
         * than up to a fifth of a second later. The loop only runs at this
         * rate while something is actually on the air - and an accepted
         * transmission that has not started yet does not wait at all: its
         * client has been answered and the packet should go out now. */
        int timeout = 200;

        if (radio_tx_pending_start(&rd->tx)) {
            timeout = 0;
        } else if (radio_tx_active(&rd->tx)) {
            timeout = RADIO_TX_POLL_MS;
        }

        if (pocketipc_server_poll_fd(rd->server, timeout, radio_fd, &radio_ready) < 0) {
            LOG_ERROR("poll: %s", strerror(errno));
            return 1;
        }
        radio_tx_step(&rd->tx);
        /* Not while transmitting. The transceiver belongs to the transmit
         * until it says otherwise, and calling receive() or trying to
         * recover receive mode underneath it would be reading a radio that
         * is not listening and reporting a state it is not in. */
        if (!radio_tx_active(&rd->tx)) {
            drain_receive(rd);
            recover_rx(rd);
        }
    }
    /* A packet already on the air is finished rather than abandoned: the
     * statistics count it, the completion event goes out, and the backend
     * gets its chance to leave the transceiver in a sane state before
     * shutdown() takes it down. Bounded by the airtime. */
    if (radio_tx_active(&rd->tx)) {
        LOG_INFO("stopping: finishing tx %llu first",
                 (unsigned long long)radio_tx_active_id(&rd->tx));
        radio_tx_run(&rd->tx);
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
            "              [--tx-power-dbm N] [--socket-name NAME] [--verbose]\n"
            "Defaults: backend mock, region EU868, tx power %d dBm (the profile\n"
            "is not persisted: every start returns to these values).\n"
            "Runtime directory: $POCKETOS_RUNTIME_DIR or %s\n",
            RADIOD_DEFAULT_TX_POWER_DBM, POCKETIPC_DEFAULT_DIR);
}

int main(int argc, char **argv)
{
    struct radiod rd;
    const char *backend = "mock";
    const char *region = "EU868";
    int tx_power_dbm = RADIOD_DEFAULT_TX_POWER_DBM;
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
        } else if (strcmp(argv[i], "--tx-power-dbm") == 0 && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);

            if (*argv[i] == '\0' || *end != '\0' || v < -100 || v > 100) {
                LOG_ERROR("--tx-power-dbm needs an integer, got '%s'", argv[i]);
                return 2;
            }
            tx_power_dbm = (int)v;
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
    radio_tx_init(&rd.tx, &rd.be, on_tx_done, &rd);
    /* No lease survives a restart, and nothing tries to reconstruct one. A
     * restart closes every connection there was, so the daemon cannot tell
     * which of the clients that come back is the one that held it - and a
     * lease handed to the wrong one is worse than no lease. The protocol
     * daemon asks again when it reconnects. */
    radio_lease_init(&rd.lease);

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
    rd.be.profile.tx_power_dbm = tx_power_dbm;
    rd.be.profile.crc = true;
    if (strcmp(rd.region->name, "NONE") == 0) {
        rd.be.profile.frequency_mhz = 868.0;
    }
    /* The start-up profile goes through the same validation as
     * radio.configure, so a bad --tx-power-dbm (outside the chip's range or
     * above the region cap) is refused before the radio is touched. */
    {
        struct radio_profile check = rd.be.profile;
        int prc = profile_apply(&rd, NULL, &check, err, sizeof(err));

        if (prc != 0) {
            LOG_ERROR("start-up profile rejected: %s", err);
            rd.be.ops->shutdown(&rd.be);
            return 2;
        }
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
    pocketipc_server_set_on_disconnect(rd.server, on_disconnect, &rd);
    LOG_INFO("listening on %s backend=%s region=%s", pocketipc_server_path(rd.server),
             rd.be.ops->name, rd.region->name);

    rc = run(&rd);
    LOG_INFO("shutting down (rc=%d)", rc);

    pocketipc_server_free(rd.server);
    rd.be.ops->shutdown(&rd.be);
    pocketlog_close();
    return rc;
}
