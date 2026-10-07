/*
 * meshcored: the radiod connection. See radio_link.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "radio_link.h"

#include "mcd.h"
#include "mcd_util.h"

#include "pocketlog/pocketlog.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* How long a connect attempt may take. radiod's listen backlog can be full
 * of somebody else's abandoned connections, where a blocking connect() waits
 * with no limit at all (unit A, v0.0.4); pocketipc_connect_timeout retries
 * without leaving anything queued. */
#define LINK_CONNECT_TIMEOUT_MS 500

enum link_phase {
    LP_DISCONNECTED = 0,
    LP_ACQUIRE,     /* waiting for the reply to radio.acquire */
    LP_CONFIGURE,   /* ...to radio.configure */
    LP_SUBSCRIBE,   /* ...to radio.subscribe */
    LP_STATUS,      /* ...to the first radio.status */
    LP_READY,
    LP_LEASE_WAIT   /* connected, the radio belongs to someone else */
};

enum req_kind {
    RQ_NONE = 0,
    RQ_ACQUIRE,
    RQ_CONFIGURE,
    RQ_SUBSCRIBE,
    RQ_STATUS,
    RQ_SEND,
    RQ_RELEASE
};

#define LINK_MAX_PENDING 16

struct pending {
    int id;
    enum req_kind kind;
};

struct mcd_radio_link {
    struct mcd *d;
    int fd;
    enum link_phase phase;
    struct pocketipc_reader reader;
    struct mcd_backoff backoff;
    struct mcd_tx_map tx;
    struct pending pending[LINK_MAX_PENDING];
    int next_id;
    bool lease_held;
    uint64_t lease_owner_id;
    /* A refusal that asking again cannot change. The connection is closed and
     * never remade: see link_fail_permanently(). */
    bool permanent;
};

/* ---- request bookkeeping ----------------------------------------------- */

static int pending_add(struct mcd_radio_link *l, int id, enum req_kind kind)
{
    int i;

    for (i = 0; i < LINK_MAX_PENDING; i++) {
        if (l->pending[i].kind == RQ_NONE) {
            l->pending[i].id = id;
            l->pending[i].kind = kind;
            return 0;
        }
    }
    return -1;
}

static enum req_kind pending_take(struct mcd_radio_link *l, int id)
{
    int i;

    for (i = 0; i < LINK_MAX_PENDING; i++) {
        if (l->pending[i].kind != RQ_NONE && l->pending[i].id == id) {
            enum req_kind k = l->pending[i].kind;

            l->pending[i].kind = RQ_NONE;
            l->pending[i].id = 0;
            return k;
        }
    }
    return RQ_NONE;
}

/* Write one request. Returns its id, or -1 (the caller disconnects). params
 * is consumed either way. */
static int link_request(struct mcd_radio_link *l, const char *method, cJSON *params,
                        enum req_kind kind)
{
    cJSON *req;
    char *text;
    int id;
    int rc;

    if (l->fd < 0) {
        cJSON_Delete(params);
        return -1;
    }
    id = l->next_id++;
    if (pending_add(l, id, kind) != 0) {
        cJSON_Delete(params);
        LOG_WARN("radiod: too many requests outstanding; dropping %s", method);
        return -1;
    }
    req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", method);
    if (params) {
        cJSON_AddItemToObject(req, "params", params);
    }
    text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!text) {
        pending_take(l, id);
        return -1;
    }
    rc = pocketipc_write_frame(l->fd, text, strlen(text));
    free(text);
    if (rc < 0) {
        pending_take(l, id);
        return -1;
    }
    return id;
}

/* ---- connection lifecycle ---------------------------------------------- */

/* Close the connection and drop every assumption that came with it. The
 * caller decides what the service state becomes afterwards, because the two
 * reasons for being here are not the same answer: radiod going away is waited
 * out, and a profile radiod will not accept is not. */
static void link_close(struct mcd_radio_link *l, const char *reason, bool count_it)
{
    struct mcd *d = l->d;
    uint64_t abandoned[MCD_TX_MAP_SLOTS];
    int n;
    int i;

    if (l->fd >= 0) {
        close(l->fd);
        l->fd = -1;
        if (count_it) {
            d->counters.radiod_disconnects++;
        }
        LOG_WARN("radiod: connection closed (%s)", reason);
    }
    pocketipc_reader_free(&l->reader);
    pocketipc_reader_init(&l->reader);
    memset(l->pending, 0, sizeof(l->pending));
    l->phase = LP_DISCONNECTED;
    l->lease_held = false;
    l->lease_owner_id = 0;
    d->radio_state_known = false;
    d->profile_applied = false;

    /* Every submission that had not completed is resolved as unknown, not as
     * failed. radiod persists no transmit state and a restart delivers no
     * completion for a packet that was in flight (docs/api/radio.md,
     * "Restart"), so whether those bytes went out cannot be known from here.
     * Calling it a failure would invite a retransmission and, with it, the
     * duplicate airtime this distinction exists to avoid. */
    n = mcd_tx_map_abandon_all(&l->tx, abandoned);
    for (i = 0; i < n; i++) {
        d->counters.tx_unknown++;
        mcd_runtime_tx_done(d->rt, abandoned[i], MCD_TX_UNKNOWN);
        mcd_broadcast(d, mcd_event_activity_tx(abandoned[i], -1, "unknown", mcd_mono_ms()));
    }
    /* The protocol core keeps running with no radio: it still expires
     * timeouts, answers clients and holds every node it knows. It simply
     * cannot transmit, and is told so rather than left waiting. */
    mcd_runtime_set_radio_online(d->rt, false);
}

/* radiod went away, or the connection did. Waited out. */
static void link_disconnect(struct mcd_radio_link *l, const char *reason)
{
    link_close(l, reason, true);
    mcd_set_state(l->d, MCD_WAITING_FOR_RADIOD, reason);
    mcd_backoff_failed(&l->backoff, mcd_mono_ms());
}

/* radiod refused something that asking again cannot change - the region guard
 * turning down the profile, or a value it will not take.
 *
 * The important part is what this does NOT do: hold the radio. Entering an
 * error state while keeping the lease and the socket would leave radiod owned
 * by a service that has given up, with nothing able to take the radio back
 * short of killing this process. So the lease is handed back explicitly,
 * the connection is closed - which releases it again on radiod's side even if
 * the request never arrived - and the phase is reset rather than left stale.
 *
 * It is terminal, deliberately. The profile comes from the command line, so
 * reconnecting would apply exactly the same values and be refused exactly the
 * same way, for ever. Whoever changes the profile or the region guard
 * restarts the service; the supervisor makes that one command. Until then
 * mesh.status keeps answering and says why, which is the one useful thing
 * left to do. */
static void link_fail_permanently(struct mcd_radio_link *l, const char *reason)
{
    if (l->fd >= 0 && l->lease_held) {
        /* Best effort, and not waited for: the close below is what really
         * guarantees the release, because radiod drops the lease of a
         * connection that goes away (docs/api/radio.md, "Disconnect"). */
        link_request(l, "radio.release", NULL, RQ_RELEASE);
    }
    LOG_ERROR("radiod refused the radio profile permanently: %s", reason);
    link_close(l, reason, false);
    l->permanent = true;
    mcd_set_state(l->d, MCD_ERROR, reason);
}

static cJSON *profile_params(const struct mcd_profile *p)
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

static void send_acquire(struct mcd_radio_link *l)
{
    cJSON *params = cJSON_CreateObject();

    cJSON_AddStringToObject(params, "owner", MCD_SERVICE_NAME);
    if (link_request(l, "radio.acquire", params, RQ_ACQUIRE) < 0) {
        link_disconnect(l, "the acquire request could not be written");
        return;
    }
    l->phase = LP_ACQUIRE;
}

static void send_configure(struct mcd_radio_link *l)
{
    if (link_request(l, "radio.configure", profile_params(&l->d->cfg.profile),
                     RQ_CONFIGURE) < 0) {
        link_disconnect(l, "the configure request could not be written");
        return;
    }
    l->phase = LP_CONFIGURE;
    mcd_set_state(l->d, MCD_CONFIGURING, "applying the MeshCore profile");
}

static void link_connect(struct mcd_radio_link *l)
{
    struct mcd *d = l->d;
    int fd = pocketipc_connect_timeout(d->cfg.radiod_socket, LINK_CONNECT_TIMEOUT_MS);

    if (fd < 0) {
        mcd_backoff_failed(&l->backoff, mcd_mono_ms());
        if (d->state != MCD_WAITING_FOR_RADIOD) {
            mcd_set_state(d, MCD_WAITING_FOR_RADIOD, strerror(errno));
        }
        return;
    }
    l->fd = fd;
    d->counters.radiod_connects++;
    LOG_INFO("radiod: connected on %s", d->cfg.radiod_socket);
    send_acquire(l);
}

/* ---- replies ----------------------------------------------------------- */

static double num_or(const cJSON *o, const char *key, double fallback, bool *known)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    if (cJSON_IsNumber(v)) {
        if (known) {
            *known = true;
        }
        return v->valuedouble;
    }
    if (known) {
        *known = false;
    }
    return fallback;
}

/* Every number below came out of another process's JSON, and converting a
 * double outside the target type's range is undefined behaviour - not a
 * strange value, an undefined program. cJSON will happily hand over 1e300
 * for a field that should hold a byte count. So the conversions are clamped,
 * once, here, rather than written as casts at a dozen call sites. */
#define MCD_JSON_INT_MAX 9007199254740992.0  /* 2^53, where a double stops counting */

static uint64_t num_u64(const cJSON *o, const char *key, uint64_t fallback)
{
    bool known = false;
    double d = num_or(o, key, 0.0, &known);

    if (!known) {
        return fallback;
    }
    if (!(d > 0.0)) {           /* also catches NaN */
        return 0;
    }
    if (d >= MCD_JSON_INT_MAX) {
        return (uint64_t)MCD_JSON_INT_MAX;
    }
    return (uint64_t)d;
}

static int num_int(const cJSON *o, const char *key, int fallback, int lo, int hi)
{
    bool known = false;
    double d = num_or(o, key, 0.0, &known);

    if (!known || !(d == d)) {  /* absent, or NaN */
        return fallback;
    }
    if (d < (double)lo) {
        return lo;
    }
    if (d > (double)hi) {
        return hi;
    }
    return (int)d;
}

static double num_finite(const cJSON *o, const char *key, double fallback, bool *known)
{
    double d = num_or(o, key, fallback, known);

    /* A profile value that is not a real number is no value at all. */
    if (known && *known && !(d > -1e12 && d < 1e12)) {
        *known = false;
        return fallback;
    }
    return d;
}

static void apply_profile_result(struct mcd *d, const cJSON *result)
{
    struct mcd_profile p = d->cfg.profile;
    bool known;

    if (!cJSON_IsObject(result)) {
        return;
    }
    p.frequency_mhz = num_finite(result, "frequency_mhz", p.frequency_mhz, &known);
    p.bandwidth_khz = num_finite(result, "bandwidth_khz", p.bandwidth_khz, &known);
    p.spreading_factor = num_int(result, "spreading_factor", p.spreading_factor, 0, 32);
    p.coding_rate = num_int(result, "coding_rate", p.coding_rate, 0, 32);
    p.sync_word = num_int(result, "sync_word", p.sync_word, 0, 0xFFFF);
    p.preamble_length = num_int(result, "preamble_length", p.preamble_length, 0, 65535);
    p.tx_power_dbm = num_int(result, "tx_power_dbm", p.tx_power_dbm, -128, 127);
    {
        const cJSON *crc = cJSON_GetObjectItemCaseSensitive(result, "crc");

        if (cJSON_IsBool(crc)) {
            p.crc = cJSON_IsTrue(crc) ? true : false;
        }
    }
    d->applied = p;
    d->profile_applied = true;
    /* What the radio really is, not what we asked for: the airtime the
     * protocol core budgets with comes from here. */
    mcd_runtime_set_profile(d->rt, p.spreading_factor, p.bandwidth_khz, p.coding_rate,
                            p.preamble_length, p.crc);
}

/* ---- what radiod's state word means here --------------------------------
 *
 * radiod reports state `tx` for the whole time a packet is on the air
 * (services/radiod/main.c, tx_accept), and while this service holds the lease
 * every one of those transmits is its own. Reading any non-`rx` state as a
 * loss of radio service therefore made a node report `degraded` for the
 * airtime of every frame it sent - four transmits out of four on unit A, up
 * to 772 ms each, with nothing wrong (docs/hardware/MESHCORED_HARDWARE_GATE.md,
 * finding 1).
 *
 * So `tx` is expected - but only while this service is actually waiting for a
 * transmit of its own. The transmit table is the whole test, and it is the
 * right one: a slot is taken before the radio.send_async request is written
 * and is freed only by the completion, the refusal, the deadline or a
 * disconnect, so it covers exactly the window radiod can be in `tx` on this
 * service's behalf - including the moment before the reply, because radiod
 * announces the state change before it answers the request.
 *
 * A `tx` with nothing outstanding is not ours. Something is driving a radio
 * this service holds the lease on and cannot account for, which is the one
 * thing `degraded` has always meant, so it keeps that answer rather than
 * being waved through with the expected ones.
 */
static bool radio_state_expected(const struct mcd_radio_link *l, const char *state)
{
    if (strcmp(state, "rx") == 0) {
        return true;
    }
    return strcmp(state, "tx") == 0 && mcd_tx_map_outstanding(&l->tx) > 0;
}

static const char *degraded_reason(const char *state)
{
    if (strcmp(state, "off") == 0) {
        return "the radio is switched off";
    }
    if (strcmp(state, "tx") == 0) {
        return "radiod is transmitting something this service did not submit";
    }
    return "radiod reports the radio is not receiving";
}

static void note_radio_state(struct mcd_radio_link *l, const char *state)
{
    struct mcd *d = l->d;
    bool rx;

    snprintf(d->radio_state, sizeof(d->radio_state), "%s", state);
    d->radio_state_known = true;
    rx = (strcmp(state, "rx") == 0);
    /* The owner switched the radio off (radio.set_enabled). Unlike `error`,
     * radiod refuses every transmit then, so the protocol core is told the
     * radio is not there - the same as a lost connection - rather than left
     * to build adverts and forwards that can only be refused. The link, the
     * lease and the profile are kept; the first `rx` brings it all back. */
    if (strcmp(state, "off") == 0) {
        mcd_runtime_set_radio_online(d->rt, false);
        if (d->state == MCD_ONLINE || d->state == MCD_DEGRADED) {
            mcd_set_state(d, MCD_DEGRADED, degraded_reason(state));
        }
        return;
    }
    /* Asymmetric on purpose. Leaving `online` takes a state that is not one
     * this service asked for; coming back takes a proven `rx` and nothing
     * less. A transmit submitted while the receiver is broken would otherwise
     * put `tx` on the wire and lift the service back to `online` on the
     * strength of its own voice. */
    if (d->state == MCD_ONLINE && !radio_state_expected(l, state)) {
        /* Attached and holding the radio, but it is not listening. The
         * protocol core keeps its transmit path - radiod's own documentation
         * is explicit that a send still works in state error - and stops
         * being told it is in receive mode. */
        mcd_set_state(d, MCD_DEGRADED, degraded_reason(state));
    } else if (d->state == MCD_DEGRADED && rx) {
        /* radiod got the receiver back. The adapter is told as well, not just
         * the service state: a failed transmit completion clears its receive
         * flag, and without this the protocol core would go on believing the
         * radio was deaf for the rest of the connection while radiod said
         * otherwise. */
        mcd_runtime_set_radio_online(d->rt, true);
        mcd_set_state(d, MCD_ONLINE, "the radio is receiving again");
    }
}

static void on_reply(struct mcd_radio_link *l, enum req_kind kind, const cJSON *msg)
{
    struct mcd *d = l->d;
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(msg, "result");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(msg, "error");
    int code = 0;
    const char *emsg = "";
    int id = 0;
    {
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(msg, "id");

        if (cJSON_IsNumber(jid)) {
            id = (int)jid->valuedouble;
        }
    }
    if (cJSON_IsObject(error)) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(error, "code");
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(error, "message");

        code = cJSON_IsNumber(c) ? (int)c->valuedouble : 0;
        emsg = cJSON_IsString(m) ? m->valuestring : "";
    }

    switch (kind) {
    case RQ_ACQUIRE:
        if (code != 0) {
            /* Error 5 is "somebody else holds it", which is not a fault and
             * is never answered by taking it. The lease exists so that one
             * protocol daemon can hold a configured radio; waiting is the
             * whole point of asking. */
            d->counters.lease_refused++;
            l->phase = LP_LEASE_WAIT;
            mcd_set_state(d, MCD_WAITING_FOR_LEASE, emsg[0] ? emsg : "the radio is held elsewhere");
            mcd_backoff_failed(&l->backoff, mcd_mono_ms());
            return;
        }
        l->lease_held = true;
        l->lease_owner_id = num_u64(result, "owner_id", 0);
        d->counters.lease_acquired++;
        LOG_INFO("radiod: radio lease acquired (owner_id %llu)",
                 (unsigned long long)l->lease_owner_id);
        send_configure(l);
        return;

    case RQ_CONFIGURE:
        if (code != 0) {
            if (code == POCKETIPC_ERR_POLICY || code == POCKETIPC_ERR_INVALID_PARAMS) {
                /* The region guard or a range check refused the profile.
                 * Asking again with the same values would be refused again -
                 * and the lease is given back rather than held by a service
                 * that has stopped trying. */
                link_fail_permanently(l, emsg[0] ? emsg : "the radio profile was refused");
                return;
            }
            LOG_WARN("radiod: configure failed (%d) %s; retrying", code, emsg);
            l->phase = LP_LEASE_WAIT;
            mcd_set_state(d, MCD_DEGRADED, emsg[0] ? emsg : "the profile could not be applied");
            mcd_backoff_failed(&l->backoff, mcd_mono_ms());
            return;
        }
        apply_profile_result(d, result);
        if (link_request(l, "radio.subscribe", NULL, RQ_SUBSCRIBE) < 0) {
            link_disconnect(l, "the subscribe request could not be written");
            return;
        }
        l->phase = LP_SUBSCRIBE;
        return;

    case RQ_SUBSCRIBE:
        if (code != 0) {
            link_disconnect(l, "radiod refused the event subscription");
            return;
        }
        if (link_request(l, "radio.status", NULL, RQ_STATUS) < 0) {
            link_disconnect(l, "the status request could not be written");
            return;
        }
        l->phase = LP_STATUS;
        return;

    case RQ_STATUS:
        if (code == 0 && cJSON_IsObject(result)) {
            const cJSON *st = cJSON_GetObjectItemCaseSensitive(result, "state");
            const cJSON *pr = cJSON_GetObjectItemCaseSensitive(result, "profile");

            if (cJSON_IsObject(pr)) {
                apply_profile_result(d, pr);
            }
            if (cJSON_IsString(st)) {
                snprintf(d->radio_state, sizeof(d->radio_state), "%s", st->valuestring);
                d->radio_state_known = true;
            }
        }
        l->phase = LP_READY;
        mcd_backoff_reset(&l->backoff);
        /* Connected to a radio the owner has switched off: attached, lease
         * held, profile given to radiod for later, and nothing to transmit
         * with until the first `rx` (note_radio_state). */
        mcd_runtime_set_radio_online(d->rt, !(d->radio_state_known &&
                                              strcmp(d->radio_state, "off") == 0));
        /* The same rule as note_radio_state, so the first status answer and
         * every later event agree about what a state word means. Nothing is
         * outstanding on a connection this new, so a `tx` here is somebody
         * else's and is degraded. */
        if (d->radio_state_known && !radio_state_expected(l, d->radio_state)) {
            mcd_set_state(d, MCD_DEGRADED, degraded_reason(d->radio_state));
        } else {
            mcd_set_state(d, MCD_ONLINE, "the radio is configured and listening");
        }
        return;

    case RQ_SEND: {
        uint64_t submit_id;

        if (code != 0) {
            submit_id = mcd_tx_map_refused(&l->tx, id);
            d->counters.tx_refused++;
            if (submit_id != 0) {
                LOG_WARN("radiod refused a transmit (%d) %s", code, emsg);
                mcd_runtime_tx_done(d->rt, submit_id, MCD_TX_FAILED);
                mcd_broadcast(d, mcd_event_activity_tx(submit_id, -1, "refused", mcd_mono_ms()));
            }
            return;
        }
        {
            uint64_t tx_id = num_u64(result, "tx_id", 0);
            bool airtime_known = false;
            double airtime = num_finite(result, "airtime_ms", 0.0, &airtime_known);

            /* An airtime radiod did not give, or gave as something that is
             * not a real number, is no airtime: the completion deadline then
             * falls back to its own floor rather than to a window computed
             * from nonsense. */
            if (!airtime_known) {
                airtime = 0.0;
            }

            submit_id = mcd_tx_map_accepted(&l->tx, id, tx_id, airtime, mcd_mono_ms());
            if (submit_id == 0) {
                LOG_WARN("radiod accepted a transmit this service was not waiting for");
                return;
            }
            d->counters.tx_accepted++;
        }
        return;
    }

    case RQ_RELEASE:
    case RQ_NONE:
    default:
        return;
    }
}

/* ---- events ------------------------------------------------------------ */

static void on_rx(struct mcd_radio_link *l, const cJSON *data)
{
    struct mcd *d = l->d;
    const cJSON *hex = cJSON_GetObjectItemCaseSensitive(data, "payload_hex");
    uint8_t bytes[MCD_MAX_FRAME];
    struct mcd_rx_meta meta;
    int len;

    d->counters.rx_events++;
    if (!cJSON_IsString(hex) || hex->valuestring == NULL) {
        d->counters.rx_rejected++;
        LOG_WARN("radio.rx without a payload_hex string; dropped");
        return;
    }
    /* Strictly: every character a hex digit, an even count, and a length
     * that fits a LoRa frame. MeshCore's own decoder would accept a string
     * of the right length containing anything and quietly substitute zero
     * bytes (mcd_util.h), which is how a corrupt event becomes a plausible
     * packet. */
    len = mcd_hex_decode(hex->valuestring, bytes, sizeof(bytes));
    if (len <= 0) {
        d->counters.rx_rejected++;
        LOG_WARN("radio.rx with a payload that is not %s; dropped",
                 len == 0 ? "a packet at all" : "valid hex, or is too long");
        return;
    }

    memset(&meta, 0, sizeof(meta));
    {
        const cJSON *stamp = cJSON_GetObjectItemCaseSensitive(data, "mono_ms");

        /* radiod stamps every packet, including for a backend that did not
         * (docs/api/radio.md, "Time"). An event without one is not from a
         * radiod this service understands; it is stamped here rather than
         * given a zero that a consumer would subtract. */
        meta.mono_ms = cJSON_IsNumber(stamp) ? num_u64(data, "mono_ms", 0) : mcd_mono_ms();
        /* The three telemetry values keep their own known flags, and a value
         * that is not a real number is not a measurement: it stays unknown
         * rather than being reported as something nobody read. */
        meta.rssi_dbm = num_finite(data, "rssi_dbm", 0, &meta.rssi_known);
        meta.snr_db = num_finite(data, "snr_db", 0, &meta.snr_known);
        meta.freq_error_hz = num_finite(data, "frequency_error_hz", 0,
                                        &meta.freq_error_known);
    }

    if (!mcd_runtime_deliver_rx(d->rt, bytes, len, &meta)) {
        d->counters.rx_dropped++;
        return;
    }
    d->counters.rx_delivered++;
}

static void on_tx_done(struct mcd_radio_link *l, const cJSON *data)
{
    struct mcd *d = l->d;
    uint64_t tx_id = num_u64(data, "tx_id", 0);
    uint64_t submit_id = mcd_tx_map_completed(&l->tx, tx_id);
    const cJSON *jtransmitted = cJSON_GetObjectItemCaseSensitive(data, "transmitted");
    const cJSON *jresumed = cJSON_GetObjectItemCaseSensitive(data, "rx_resumed");
    const cJSON *jok = cJSON_GetObjectItemCaseSensitive(data, "ok");
    const cJSON *jstate = cJSON_GetObjectItemCaseSensitive(data, "state");
    enum mcd_tx_outcome outcome;
    bool transmitted;
    bool resumed;
    int bytes;

    if (submit_id == 0) {
        /* A completion for a tx_id this service does not hold: a duplicate,
         * or one left over from a transmit the dispatcher has already given
         * up on. Counted and dropped, never attributed to whatever transmit
         * happens to be outstanding now. */
        d->counters.tx_done_unmatched++;
        return;
    }
    bytes = num_int(data, "bytes", -1, -1, MCD_MAX_FRAME);

    /* Read the fields, not the presence of the event. radio.tx_done is sent
     * for every accepted transmit now, including failures, and `ok` is
     * `transmitted && rx_resumed` - so a packet that went out perfectly well
     * and left the radio unable to receive arrives with ok false. */
    if (cJSON_IsBool(jtransmitted)) {
        transmitted = cJSON_IsTrue(jtransmitted) ? true : false;
    } else {
        transmitted = cJSON_IsTrue(jok) ? true : false;
    }
    resumed = cJSON_IsBool(jresumed) ? (cJSON_IsTrue(jresumed) ? true : false) : transmitted;

    if (!transmitted) {
        outcome = MCD_TX_FAILED;
        d->counters.tx_failed++;
    } else if (!resumed) {
        outcome = MCD_TX_RX_RESUME_FAILED;
        d->counters.tx_rx_resume_failed++;
    } else {
        outcome = MCD_TX_OK;
        d->counters.tx_ok++;
    }
    /* After the slot is freed, so the state radiod ended up in is judged
     * with this transmit already accounted for: a completion that says `tx`
     * is not this service's transmit any more, and a completion that says
     * anything but `rx` is the degraded case it has always been. */
    if (cJSON_IsString(jstate)) {
        note_radio_state(l, jstate->valuestring);
    }
    mcd_runtime_tx_done(d->rt, submit_id, outcome);
    mcd_broadcast(d, mcd_event_activity_tx(submit_id, bytes, mcd_tx_outcome_name(outcome),
                                           mcd_mono_ms()));
}

static void on_lease_event(struct mcd_radio_link *l, const cJSON *data)
{
    struct mcd *d = l->d;
    const cJSON *held = cJSON_GetObjectItemCaseSensitive(data, "held");
    uint64_t owner;
    bool is_held = cJSON_IsTrue(held) ? true : false;

    if (!l->lease_held) {
        return;  /* somebody else's lease coming and going is not our news */
    }
    owner = num_u64(data, "owner_id", 0);
    if (is_held && owner == l->lease_owner_id) {
        return;  /* still ours */
    }
    /* radiod does not hand a held lease to another client, so this should
     * not happen; if it ever does, the answer is to stop behaving as the
     * owner rather than to keep transmitting on somebody else's radio. */
    LOG_WARN("radiod: the radio lease is no longer ours");
    d->counters.lease_lost++;
    l->lease_held = false;
    l->lease_owner_id = 0;
    mcd_runtime_set_radio_online(d->rt, false);
    l->phase = LP_LEASE_WAIT;
    mcd_set_state(d, MCD_WAITING_FOR_LEASE, "the radio lease was lost");
    mcd_backoff_failed(&l->backoff, mcd_mono_ms());
}

static void on_event(struct mcd_radio_link *l, const char *name, const cJSON *data)
{
    if (strcmp(name, "radio.rx") == 0) {
        on_rx(l, data);
    } else if (strcmp(name, "radio.tx_done") == 0) {
        on_tx_done(l, data);
    } else if (strcmp(name, "radio.state") == 0) {
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(data, "state");

        if (cJSON_IsString(st)) {
            note_radio_state(l, st->valuestring);
        }
    } else if (strcmp(name, "radio.lease") == 0) {
        on_lease_event(l, data);
    }
}

/* ---- the loop hooks ---------------------------------------------------- */

struct mcd_radio_link *mcd_link_new(struct mcd *d)
{
    struct mcd_radio_link *l = calloc(1, sizeof(*l));

    if (!l) {
        return NULL;
    }
    l->d = d;
    l->fd = -1;
    l->phase = LP_DISCONNECTED;
    l->next_id = 1;
    pocketipc_reader_init(&l->reader);
    mcd_backoff_reset(&l->backoff);
    mcd_tx_map_init(&l->tx);
    return l;
}

void mcd_link_free(struct mcd_radio_link *l)
{
    if (!l) {
        return;
    }
    if (l->fd >= 0) {
        close(l->fd);
    }
    pocketipc_reader_free(&l->reader);
    free(l);
}

int mcd_link_fd(const struct mcd_radio_link *l)
{
    return l->fd;
}

bool mcd_link_ready(const struct mcd_radio_link *l)
{
    return l->phase == LP_READY;
}

bool mcd_link_connected(const struct mcd_radio_link *l)
{
    return l->fd >= 0;
}

bool mcd_link_lease_held(const struct mcd_radio_link *l)
{
    return l->lease_held;
}

uint64_t mcd_link_lease_owner_id(const struct mcd_radio_link *l)
{
    return l->lease_owner_id;
}

/* How much of radiod's stream one turn of the loop will take. Whatever is
 * left stays in the socket buffer and the next poll reports it readable
 * again, so nothing is lost - but a radiod that never stops talking cannot
 * keep this service from answering its own clients in between. */
#define LINK_READ_CHUNKS_PER_TURN 32

void mcd_link_readable(struct mcd_radio_link *l)
{
    uint8_t buf[4096];
    int chunks = 0;

    if (l->fd < 0) {
        return;
    }
    while (chunks++ < LINK_READ_CHUNKS_PER_TURN) {
        ssize_t n = recv(l->fd, buf, sizeof(buf), 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            link_disconnect(l, strerror(errno));
            return;
        }
        if (n == 0) {
            link_disconnect(l, "radiod closed the connection");
            return;
        }
        if (pocketipc_reader_feed(&l->reader, buf, (size_t)n) != 0) {
            link_disconnect(l, "radiod sent an oversized frame");
            return;
        }
        for (;;) {
            int bad = 0;
            cJSON *msg = pocketipc_reader_next(&l->reader, &bad);

            if (bad) {
                link_disconnect(l, "radiod sent a frame that is not JSON");
                return;
            }
            if (!msg) {
                break;
            }
            {
                const cJSON *ev = cJSON_GetObjectItemCaseSensitive(msg, "event");
                const cJSON *jid = cJSON_GetObjectItemCaseSensitive(msg, "id");

                if (cJSON_IsString(ev)) {
                    const cJSON *data = cJSON_GetObjectItemCaseSensitive(msg, "data");

                    on_event(l, ev->valuestring, data);
                } else if (cJSON_IsNumber(jid)) {
                    enum req_kind kind = pending_take(l, (int)jid->valuedouble);

                    if (kind != RQ_NONE) {
                        on_reply(l, kind, msg);
                    }
                }
            }
            cJSON_Delete(msg);
            /* on_reply/on_event may have dropped the connection. */
            if (l->fd < 0) {
                return;
            }
        }
    }
}

/* Resolve any submission whose completion never came. See tx_map.h, "the
 * completion deadline": without this one lost radio.tx_done is permanent,
 * because the slot it holds refuses every later submission and the node goes
 * quiet for the rest of the session with nothing to say why. */
static void expire_transmits(struct mcd_radio_link *l, uint64_t now_ms)
{
    struct mcd *d = l->d;
    uint64_t expired[MCD_TX_MAP_SLOTS];
    int n = mcd_tx_map_expire(&l->tx, now_ms, expired);
    int i;

    for (i = 0; i < n; i++) {
        d->counters.tx_unknown++;
        LOG_WARN("radiod sent no completion for a transmit within its deadline; "
                 "the outcome is unknown and it is not sent again");
        mcd_runtime_tx_done(d->rt, expired[i], MCD_TX_UNKNOWN);
        mcd_broadcast(d, mcd_event_activity_tx(expired[i], -1, "unknown", now_ms));
    }
    /* The expectation goes with the submission. A `tx` this service excused
     * because it was waiting for its own completion stops being excusable
     * when that completion never comes: radiod's last word still stands, and
     * a radio left transmitting with nothing outstanding is not a radio this
     * service can report as online. It only ever degrades here - coming back
     * still takes a `rx` from radiod itself. */
    if (n > 0 && d->state == MCD_ONLINE && d->radio_state_known &&
        !radio_state_expected(l, d->radio_state)) {
        mcd_set_state(d, MCD_DEGRADED, degraded_reason(d->radio_state));
    }
}

void mcd_link_step(struct mcd_radio_link *l, uint64_t now_ms)
{
    expire_transmits(l, now_ms);
    if (l->permanent) {
        /* A refused profile. The radio has been given back and nothing is
         * reconnected: the same values would be refused the same way. Keyed
         * on the link's own flag rather than on the service state, so a state
         * change elsewhere can never quietly restart the attempt. */
        return;
    }
    switch (l->phase) {
    case LP_DISCONNECTED:
        if (mcd_backoff_due(&l->backoff, now_ms)) {
            link_connect(l);
        }
        return;
    case LP_LEASE_WAIT:
        if (mcd_backoff_due(&l->backoff, now_ms)) {
            if (l->lease_held) {
                send_configure(l);
            } else {
                send_acquire(l);
            }
        }
        return;
    default:
        return;
    }
}

int mcd_link_submit_tx(struct mcd_radio_link *l, const uint8_t *bytes, int len,
                       uint64_t *submit_id)
{
    struct mcd *d = l->d;
    char hex[MCD_MAX_FRAME * 2 + 1];
    cJSON *params;
    uint64_t id;
    int req_id;

    if (l->phase != LP_READY || l->fd < 0) {
        return -1;
    }
    if (len <= 0 || len > MCD_MAX_FRAME) {
        return -1;
    }
    /* One at a time, the same rule radiod has. Submitting a second frame
     * would be answered with BUSY, so the refusal is made here, where the
     * protocol core can be told immediately that the send did not start. */
    if (mcd_tx_map_outstanding(&l->tx) > 0) {
        return -1;
    }
    if (!mcd_hex_encode(bytes, (size_t)len, hex, sizeof(hex))) {
        return -1;
    }
    /* The slot is taken before the request is written, so a reply cannot
     * arrive for a submission the table does not know about. The id it is
     * keyed by is the one link_request is about to use. */
    req_id = l->next_id;
    id = mcd_tx_map_submit(&l->tx, req_id, len, mcd_mono_ms());
    if (id == 0) {
        return -1;
    }
    params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "payload_hex", hex);
    if (link_request(l, "radio.send_async", params, RQ_SEND) < 0) {
        /* Nothing went out, and the slot is released before the disconnect
         * so that this frame is not also reported as an unknown outcome: the
         * protocol core is told the send did not start, which is the truth
         * and the one answer that cannot cause a duplicate transmission. */
        mcd_tx_map_refused(&l->tx, req_id);
        link_disconnect(l, "the transmit request could not be written");
        return -1;
    }
    d->counters.tx_submitted++;
    *submit_id = id;
    return 0;
}

void mcd_link_shutdown(struct mcd_radio_link *l)
{
    if (l->fd < 0) {
        return;
    }
    /* Give the radio back rather than leaving radiod to notice the socket
     * closing. Best effort: the reply is not waited for, because the next
     * thing this process does is exit, and radiod releases the lease on the
     * disconnect either way. */
    if (l->lease_held) {
        link_request(l, "radio.release", NULL, RQ_RELEASE);
    }
    close(l->fd);
    l->fd = -1;
    l->phase = LP_DISCONNECTED;
    l->lease_held = false;
}
