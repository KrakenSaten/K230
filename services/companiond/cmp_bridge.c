/*
 * companiond's bridge between the MeshCore companion protocol and mesh.*.
 * See cmp_bridge.h for what it keeps and why, and docs/services/COMPANIOND.md
 * for the command table.
 *
 * Every reply layout is upstream's (meshcore-dev/MeshCore 81dbae59,
 * examples/companion_radio/MyMesh.cpp); the line numbers quoted below are in
 * that file at that commit.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmp_bridge.h"

#include "pocketlog/pocketlog.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* MeshCore's well-known Public channel key (upstream PUBLIC_GROUP_PSK,
 * "izOH6cXN6mrJ5e26oRXNcg=="). It is public by design, so it is the one
 * channel secret this bridge can hand an app: meshcored reports no key at all
 * (docs/api/mesh.md, mesh.channels) and identifies this one by
 * well_known: "public". */
static const uint8_t PUBLIC_KEY[16] = { 0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                        0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72 };

/* SX1262 maximum output (docs/api/radio.md, DOCUMENTED). Reported as
 * SELF_INFO's max_tx_power; meshcored does not set the power, radiod does. */
#define CMP_MAX_TX_POWER_DBM 22

#define MODEL_NAME "LILYGO T-Display K230 (Doors)"

/* ---- small JSON readers -------------------------------------------------- */

static bool jnum(const cJSON *o, const char *k, double *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    if (!cJSON_IsNumber(v)) {
        return false;
    }
    *out = v->valuedouble;
    return true;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

static bool jtrue(const cJSON *o, const char *k)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k));
}

/* Copy at most max bytes of s, whole characters only, NUL-padded to field. */
static void put_name(uint8_t *field, size_t field_len, const char *s, size_t max)
{
    size_t n = s ? cmp_utf8_prefix(s, strlen(s), max) : 0;

    memset(field, 0, field_len);
    if (n) {
        memcpy(field, s, n);
    }
}

/* ---- output -------------------------------------------------------------- */

static void to_app(struct cmp_bridge *b, const uint8_t *p, size_t len)
{
    if (b->app_up) {
        b->io.to_app(b->io.user, p, len);
    }
}

static void reply_ok(struct cmp_bridge *b)
{
    uint8_t f[1] = { CMP_RESP_OK };

    to_app(b, f, 1);
}

static void reply_err(struct cmp_bridge *b, uint8_t code)
{
    uint8_t f[2] = { CMP_RESP_ERR, code };

    if (code != CMP_ERR_UNSUPPORTED_CMD) {
        b->n_refused++;
    }
    to_app(b, f, 2);
}

/* ---- meshcored requests --------------------------------------------------- */

static uint32_t mesh_call(struct cmp_bridge *b, const char *method, cJSON *params)
{
    uint32_t id = ++b->next_id;

    if (id == 0) {
        id = ++b->next_id;
    }
    if (!b->mesh_up || b->io.mesh_call(b->io.user, id, method, params) != 0) {
        b->mesh_reset_wanted = true;
        return 0;
    }
    return id;
}

/* Start waiting for one reply to a command. On a write failure the command
 * is answered at once, as one meshcored could not be asked. */
static void wait_for(struct cmp_bridge *b, enum cmp_wait w, const char *method, cJSON *params)
{
    uint32_t id = mesh_call(b, method, params);

    if (id == 0) {
        b->wait = CMP_WAIT_NONE;
        reply_err(b, CMP_ERR_BAD_STATE);
        return;
    }
    b->wait = w;
    b->wait_id = id;
    b->wait_since = b->io.mono_ms(b->io.user);
}

/* ---- send records ---------------------------------------------------------- */

static void forget_run(struct cmp_bridge *b, const char *why)
{
    int i;
    int n = 0;

    for (i = 0; i < CMP_SEND_RECORDS; i++) {
        if (b->sends[i].used) {
            n++;
        }
    }
    memset(b->sends, 0, sizeof(b->sends));
    memset(b->seen, 0, sizeof(b->seen));
    b->seen_at = 0;
    if (n) {
        LOG_INFO("forgot %d sent message(s): %s", n, why);
    }
}

static int find_send(const struct cmp_bridge *b, bool channel, uint8_t chan, const uint8_t *prefix,
                     uint32_t ts, const uint8_t *text, size_t text_len)
{
    int i;

    for (i = 0; i < CMP_SEND_RECORDS; i++) {
        const struct cmp_send_rec *r = &b->sends[i];

        if (!r->used || r->is_channel != channel || r->app_timestamp != ts ||
            r->text_len != text_len || memcmp(r->text, text, text_len) != 0) {
            continue;
        }
        if (channel ? r->channel == chan : memcmp(r->prefix, prefix, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static int new_send(struct cmp_bridge *b)
{
    int i;
    int oldest = 0;

    for (i = 0; i < CMP_SEND_RECORDS; i++) {
        if (!b->sends[i].used) {
            oldest = i;
            break;
        }
        if (b->sends[i].age < b->sends[oldest].age) {
            oldest = i;
        }
    }
    memset(&b->sends[oldest], 0, sizeof(b->sends[oldest]));
    b->sends[oldest].age = ++b->send_age;
    return oldest;
}

static uint32_t new_tag(struct cmp_bridge *b)
{
    uint32_t t = b->next_tag++;

    if (t == 0) {
        t = b->next_tag++;
    }
    return t;
}

static void reply_sent(struct cmp_bridge *b, const struct cmp_send_rec *r)
{
    uint8_t f[10];

    f[0] = CMP_RESP_SENT;
    f[1] = r->flood ? 1 : 0;
    cmp_put_u32(f + 2, r->tag);
    cmp_put_u32(f + 6, r->timeout_ms);
    to_app(b, f, sizeof(f));
}

/* PUSH_CODE_SEND_CONFIRMED (MyMesh.cpp:425): the tag the app was given and
 * the round trip in milliseconds. */
static void push_confirmed(struct cmp_bridge *b, struct cmp_send_rec *r)
{
    uint8_t f[9];
    uint64_t trip = r->ack_mono_ms > r->sent_mono_ms ? r->ack_mono_ms - r->sent_mono_ms : 0;

    f[0] = CMP_PUSH_SEND_CONFIRMED;
    cmp_put_u32(f + 1, r->tag);
    cmp_put_u32(f + 5, trip > UINT32_MAX ? UINT32_MAX : (uint32_t)trip);
    to_app(b, f, sizeof(f));
    r->confirm_pending = false;
    b->n_confirmed++;
}

/* ---- the received-message queue ------------------------------------------- */

static bool seen_id(struct cmp_bridge *b, uint32_t id)
{
    int i;

    for (i = 0; i < CMP_SEEN_IDS; i++) {
        if (b->seen[i] == id) {
            return true;
        }
    }
    b->seen[b->seen_at] = id;
    b->seen_at = (b->seen_at + 1) % CMP_SEEN_IDS;
    return false;
}

static struct cmp_queued *queue_slot(struct cmp_bridge *b)
{
    int i;

    if (b->q_len < CMP_QUEUE_LEN) {
        return &b->queue[(b->q_head + b->q_len++) % CMP_QUEUE_LEN];
    }
    /* Full. The oldest channel message goes, as upstream's addToOfflineQueue
     * does; failing that the oldest direct one, and either way it is said. */
    for (i = 0; i < b->q_len; i++) {
        int at = (b->q_head + i) % CMP_QUEUE_LEN;

        if (b->queue[at].is_channel) {
            break;
        }
    }
    if (i == b->q_len) {
        i = 0;
    }
    LOG_WARN("message queue full (%d): dropped the oldest %s message", CMP_QUEUE_LEN,
             b->queue[(b->q_head + i) % CMP_QUEUE_LEN].is_channel ? "channel" : "direct");
    b->n_evicted++;
    for (; i < b->q_len - 1; i++) {
        b->queue[(b->q_head + i) % CMP_QUEUE_LEN] = b->queue[(b->q_head + i + 1) % CMP_QUEUE_LEN];
    }
    return &b->queue[(b->q_head + b->q_len - 1) % CMP_QUEUE_LEN];
}

static void queue_message(struct cmp_bridge *b, const cJSON *m)
{
    const char *kind = jstr(m, "kind");
    const char *text = jstr(m, "text");
    double id, ts, snr, slot;
    struct cmp_queued q;
    uint8_t wake = CMP_PUSH_MSG_WAITING;

    if (!kind || !text || !jnum(m, "id", &id) || !jnum(m, "timestamp", &ts)) {
        return;
    }
    memset(&q, 0, sizeof(q));
    if (strcmp(kind, "channel") == 0) {
        if (!jnum(m, "channel", &slot) || slot < 0 || slot > 255) {
            return;
        }
        q.is_channel = 1;
        q.channel = (uint8_t)slot;
    } else if (strcmp(kind, "direct") == 0) {
        uint8_t key[CMP_PUB_KEY_SIZE];
        const char *hex = jstr(m, "peer_public_key");

        if (!hex || cmp_unhex(hex, key, sizeof(key)) != 0) {
            return;
        }
        memcpy(q.prefix, key, 6);
    } else {
        return;
    }
    if (seen_id(b, (uint32_t)id)) {
        return;
    }
    q.timestamp = (uint32_t)ts;
    /* SNR is reported only when radiod measured it; the frame has no
     * "unknown", so an unmeasured one goes as 0 (docs/services/COMPANIOND.md). */
    if (jnum(m, "snr_db", &snr)) {
        double x4 = snr * 4.0;

        q.snr_x4 = (int8_t)(x4 > 127 ? 127 : x4 < -128 ? -128 : lround(x4));
    }
    {
        size_t n = cmp_utf8_prefix(text, strlen(text), CMP_MAX_FRAME);

        memcpy(q.text, text, n);
        q.text[n] = '\0';
    }
    *queue_slot(b) = q;
    b->n_queued++;
    /* The tickle, as upstream's queueMessage sends it whenever an app is
     * connected; with none the message waits for the next SYNC. */
    to_app(b, &wake, 1);
}

/* RESP_CODE_*_MSG_RECV(_V3), as queueMessage (MyMesh.cpp:440) and
 * onChannelMessageRecv (:558) build them. The path length byte is 0xFF:
 * meshcored does not report the route a received message took, and 0xFF is
 * what the firmware writes for anything it does not count as a flood hop
 * count. */
static size_t build_recv(const struct cmp_bridge *b, const struct cmp_queued *q, uint8_t *f)
{
    size_t i = 0;
    size_t tlen;

    if (b->app_target_ver >= 3) {
        f[i++] = q->is_channel ? CMP_RESP_CHANNEL_MSG_RECV_V3 : CMP_RESP_CONTACT_MSG_RECV_V3;
        f[i++] = (uint8_t)q->snr_x4;
        f[i++] = 0;
        f[i++] = 0;
    } else {
        f[i++] = q->is_channel ? CMP_RESP_CHANNEL_MSG_RECV : CMP_RESP_CONTACT_MSG_RECV;
    }
    if (q->is_channel) {
        f[i++] = q->channel;
    } else {
        memcpy(f + i, q->prefix, 6);
        i += 6;
    }
    f[i++] = CMP_OUT_PATH_UNKNOWN;
    f[i++] = CMP_TXT_TYPE_PLAIN;
    cmp_put_u32(f + i, q->timestamp);
    i += 4;
    tlen = cmp_utf8_prefix(q->text, strlen(q->text), CMP_MAX_FRAME - i);
    memcpy(f + i, q->text, tlen);
    return i + tlen;
}

/* ---- frames built from meshcored's answers --------------------------------- */

/* RESP_CODE_CONTACT, writeContactRespFrame (MyMesh.cpp:171): 148 bytes. */
static size_t build_contact(struct cmp_bridge *b, uint8_t code, const cJSON *node, uint8_t *f,
                            uint32_t *lastmod_out)
{
    const char *key = jstr(node, "public_key");
    const char *path = jstr(node, "path_hex");
    double type = 0, hops = 0, adv = 0, lat, lon, heard;
    uint8_t path_len = CMP_OUT_PATH_UNKNOWN;
    uint32_t lastmod = 0;
    size_t i = 0;

    f[i++] = code;
    if (!key || cmp_unhex(key, f + i, CMP_PUB_KEY_SIZE) != 0) {
        return 0;
    }
    i += CMP_PUB_KEY_SIZE;
    jnum(node, "type", &type);
    f[i++] = (uint8_t)type;
    f[i++] = 0; /* flags: favourites and the like are the app's own */
    memset(f + i + 1, 0, CMP_MAX_PATH_SIZE);
    /* out_path_len is MeshCore's packed byte: relay count in the low six
     * bits, hash size - 1 in the top two (Packet.h); 0xFF is no route. */
    if (jtrue(node, "path_known") && jnum(node, "hops", &hops) && hops >= 0 && hops <= 63) {
        size_t bytes = path ? strlen(path) / 2 : 0;
        int n = (int)hops;

        if (n == 0) {
            path_len = 0;
        } else if (bytes % (size_t)n == 0 && bytes / (size_t)n >= 1 && bytes / (size_t)n <= 3 &&
                   bytes <= CMP_MAX_PATH_SIZE && cmp_unhex(path, f + i + 1, bytes) == 0) {
            path_len = (uint8_t)(((bytes / (size_t)n - 1) << 6) | (size_t)n);
        } else {
            memset(f + i + 1, 0, CMP_MAX_PATH_SIZE);
        }
    }
    f[i++] = path_len;
    i += CMP_MAX_PATH_SIZE;
    put_name(f + i, 32, jstr(node, "name"), 31);
    i += 32;
    jnum(node, "last_advert_timestamp", &adv);
    cmp_put_u32(f + i, (uint32_t)adv);
    i += 4;
    cmp_put_u32(f + i, jnum(node, "lat", &lat) ? (uint32_t)(int32_t)lround(lat * 1e6) : 0);
    i += 4;
    cmp_put_u32(f + i, jnum(node, "lon", &lon) ? (uint32_t)(int32_t)lround(lon * 1e6) : 0);
    i += 4;
    /* lastmod is upstream's clock reading at the contact's last change. The
     * nearest fact meshcored has is when this node last heard it, by the
     * monotonic clock; it is put on the system clock here. Not heard in
     * this run of meshcored: 0. */
    if (jnum(node, "last_heard_mono_ms", &heard) && heard >= 0) {
        uint64_t now = b->io.mono_ms(b->io.user);
        uint32_t wall = b->io.wall_s(b->io.user);
        uint64_t ago = now > (uint64_t)heard ? (now - (uint64_t)heard) / 1000 : 0;

        lastmod = wall > ago ? (uint32_t)(wall - ago) : 0;
    }
    cmp_put_u32(f + i, lastmod);
    i += 4;
    if (lastmod_out) {
        *lastmod_out = lastmod;
    }
    return i;
}

/* RESP_CODE_DEVICE_INFO (MyMesh.cpp:1056). */
static void reply_device_info(struct cmp_bridge *b, uint8_t path_hash_mode)
{
    uint8_t f[82];
    size_t i = 0;
    char ver[40];
    char date[12];

    memset(f, 0, sizeof(f));
    f[i++] = CMP_RESP_DEVICE_INFO;
    f[i++] = CMP_FIRMWARE_VER_CODE;
    /* MAX_CONTACTS / 2 in a byte. meshcored holds 1000, which a byte cannot
     * say; 255 (510) is the most it can. */
    f[i++] = 255;
    f[i++] = b->dq_max_channels;
    cmp_put_u32(f + i, 0); /* BLE PIN: there is no BLE here */
    i += 4;
    snprintf(date, sizeof(date), "%s", __DATE__);
    put_name(f + i, 12, date, 11);
    i += 12;
    put_name(f + i, 40, MODEL_NAME, 39);
    i += 40;
    snprintf(ver, sizeof(ver), "Doors %s", b->version[0] ? b->version : POCKETOS_VERSION);
    put_name(f + i, 20, ver, 19);
    i += 20;
    f[i++] = 0; /* client repeat: meshcored forwards nothing (mesh.md, "Not in v0") */
    f[i++] = path_hash_mode;
    to_app(b, f, i);
}

/* RESP_CODE_SELF_INFO (MyMesh.cpp:1077). The radio values are meshcored's
 * configured profile (mesh.info requested_profile), which is what upstream
 * reports too: its _prefs, not a measurement. */
static void reply_self_info(struct cmp_bridge *b, const cJSON *identity)
{
    uint8_t f[CMP_MAX_FRAME];
    const char *key = jstr(identity, "public_key");
    const char *name = jstr(identity, "name");
    size_t i = 0;
    size_t n;
    int k;

    if (!key || !b->have_profile) {
        reply_err(b, CMP_ERR_BAD_STATE);
        return;
    }
    f[i++] = CMP_RESP_SELF_INFO;
    f[i++] = 1; /* ADV_TYPE_CHAT */
    f[i++] = (uint8_t)(int8_t)b->tx_power_dbm;
    f[i++] = CMP_MAX_TX_POWER_DBM;
    if (cmp_unhex(key, f + i, CMP_PUB_KEY_SIZE) != 0) {
        reply_err(b, CMP_ERR_BAD_STATE);
        return;
    }
    i += CMP_PUB_KEY_SIZE;
    cmp_put_u32(f + i, 0); /* this node's adverts carry no location */
    i += 4;
    cmp_put_u32(f + i, 0);
    i += 4;
    f[i++] = 0; /* multi_acks */
    f[i++] = 0; /* advert_loc_policy: none */
    f[i++] = 0; /* telemetry modes */
    f[i++] = 0; /* manual_add_contacts: meshcored keeps every advert it hears */
    cmp_put_u32(f + i, (uint32_t)lround(b->freq_mhz * 1000.0));
    i += 4;
    cmp_put_u32(f + i, (uint32_t)lround(b->bw_khz * 1000.0));
    i += 4;
    f[i++] = (uint8_t)b->sf;
    f[i++] = (uint8_t)b->cr;
    n = name ? cmp_utf8_prefix(name, strlen(name), 31) : 0;
    memcpy(f + i, name, n);
    i += n;
    to_app(b, f, i);
    b->app_started = true;
    /* ACKs that arrived while no app was listening, now that one is. */
    for (k = 0; k < CMP_SEND_RECORDS; k++) {
        if (b->sends[k].used && b->sends[k].confirm_pending) {
            push_confirmed(b, &b->sends[k]);
        }
    }
}

/* RESP_CODE_CHANNEL_INFO (MyMesh.cpp:1752): slot, 32-byte name, 16-byte
 * secret. An empty slot is answered with an empty name and a zero secret, as
 * upstream answers one; a slot past the table is NOT_FOUND, which is how an
 * app finds the end of it.
 *
 * meshcored never reports a channel key (docs/api/mesh.md: "The key is not
 * here, and no method reports it"). The Public channel's key is public and is
 * sent; for every other channel the secret field is sent as zeros. That is a
 * withheld value, not the key: see docs/services/COMPANIOND.md. */
static void reply_channel(struct cmp_bridge *b, const cJSON *result, uint8_t slot)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(result, "channels");
    const cJSON *c;
    double max = 0;
    uint8_t f[50];

    if (!jnum(result, "max", &max) || slot >= max) {
        reply_err(b, CMP_ERR_NOT_FOUND);
        return;
    }
    memset(f, 0, sizeof(f));
    f[0] = CMP_RESP_CHANNEL_INFO;
    f[1] = slot;
    cJSON_ArrayForEach (c, arr) {
        double s;
        const char *wk = jstr(c, "well_known");

        if (!jnum(c, "channel", &s) || (int)s != slot) {
            continue;
        }
        put_name(f + 2, 32, jstr(c, "name"), 31);
        if (wk && strcmp(wk, "public") == 0) {
            memcpy(f + 34, PUBLIC_KEY, 16);
        }
        break;
    }
    to_app(b, f, sizeof(f));
}

/* ---- commands -------------------------------------------------------------- */

static void cmd_sync_next(struct cmp_bridge *b)
{
    uint8_t f[CMP_MAX_FRAME];

    if (b->q_len == 0) {
        f[0] = CMP_RESP_NO_MORE_MESSAGES;
        to_app(b, f, 1);
        return;
    }
    to_app(b, f, build_recv(b, &b->queue[b->q_head], f));
    b->q_head = (b->q_head + 1) % CMP_QUEUE_LEN;
    b->q_len--;
    b->n_synced++;
}

/* CMD_SEND_TXT_MSG (MyMesh.cpp:1132): type, attempt, timestamp, 6-byte key
 * prefix, text. */
static void cmd_send_dm(struct cmp_bridge *b, const uint8_t *c, size_t len)
{
    uint8_t txt_type = c[1];
    uint8_t attempt = c[2];
    uint32_t ts = cmp_get_u32(c + 3);
    const uint8_t *prefix = c + 7;
    const uint8_t *text = c + 13;
    size_t tlen = len - 13;
    int r;

    /* Only plain text. CLI data to repeaters is upstream's too, and is not
     * part of this bridge. */
    if (txt_type != CMP_TXT_TYPE_PLAIN) {
        reply_err(b, CMP_ERR_UNSUPPORTED_CMD);
        return;
    }
    if (tlen == 0 || tlen > CMP_TEXT_MAX || !cmp_text_ok(text, tlen)) {
        reply_err(b, CMP_ERR_ILLEGAL_ARG);
        return;
    }
    r = find_send(b, false, 0, prefix, ts, text, tlen);
    if (r >= 0 && attempt <= b->sends[r].attempt) {
        /* The same frame again: after a reconnect the app could not know its
         * first one was taken. Answered from the record; nothing is sent. */
        b->n_dup_suppressed++;
        LOG_INFO("direct message %u attempt %u again: answered, not resent",
                 b->sends[r].message_id, attempt);
        reply_sent(b, &b->sends[r]);
        return;
    }
    if (!b->mesh_ready) {
        reply_err(b, CMP_ERR_BAD_STATE);
        return;
    }
    if (r >= 0) {
        /* The app's own retry, attempt one higher: meshcored's resend, which
         * keeps the text and the timestamp and raises the attempt
         * (docs/api/mesh.md, mesh.send "resend"). */
        cJSON *p = cJSON_CreateObject();

        cJSON_AddNumberToObject(p, "resend", b->sends[r].message_id);
        b->wait_rec = r;
        wait_for(b, CMP_WAIT_RESEND_DM, "mesh.send", p);
        return;
    }
    {
        cJSON *p = cJSON_CreateObject();
        char hex[13];
        char body[CMP_TEXT_MAX + 1];
        struct cmp_send_rec *rec;

        r = new_send(b);
        rec = &b->sends[r];
        rec->is_channel = false;
        memcpy(rec->prefix, prefix, 6);
        rec->app_timestamp = ts;
        rec->attempt = attempt;
        rec->text_len = (uint16_t)tlen;
        memcpy(rec->text, text, tlen);
        memcpy(body, text, tlen);
        body[tlen] = '\0';
        cmp_hex(prefix, 6, hex);
        cJSON_AddStringToObject(p, "to", hex);
        cJSON_AddStringToObject(p, "text", body);
        b->wait_rec = r;
        wait_for(b, CMP_WAIT_SEND_DM, "mesh.send", p);
        if (b->wait == CMP_WAIT_NONE) {
            rec->used = false;
        }
    }
}

/* CMD_SEND_CHANNEL_TXT_MSG (MyMesh.cpp:1182): type, slot, timestamp, text. */
static void cmd_send_channel(struct cmp_bridge *b, const uint8_t *c, size_t len)
{
    size_t tlen = len - 7;

    if (c[1] != CMP_TXT_TYPE_PLAIN) {
        reply_err(b, CMP_ERR_UNSUPPORTED_CMD);
        return;
    }
    if (tlen == 0 || tlen > CMP_TEXT_MAX || !cmp_text_ok(c + 7, tlen)) {
        reply_err(b, CMP_ERR_ILLEGAL_ARG);
        return;
    }
    if (find_send(b, true, c[2], NULL, cmp_get_u32(c + 3), c + 7, tlen) >= 0) {
        b->n_dup_suppressed++;
        LOG_INFO("channel message to slot %u again: answered, not resent", c[2]);
        reply_ok(b);
        return;
    }
    if (!b->mesh_ready) {
        reply_err(b, CMP_ERR_BAD_STATE);
        return;
    }
    /* The table first: a slot that holds nothing is NOT_FOUND and a text
     * over the channel's own limit is ILLEGAL_ARG, which mesh.send's one
     * error code would not tell apart. */
    wait_for(b, CMP_WAIT_CHAN_LOOKUP, "mesh.channels", NULL);
}

void cmp_bridge_app_frame(struct cmp_bridge *b, const uint8_t *c, size_t len)
{
    if (len == 0 || len > CMP_MAX_FRAME || b->wait != CMP_WAIT_NONE) {
        return;
    }
    memcpy(b->cmd, c, len);
    b->cmd_len = len;
    switch (c[0]) {
    case CMP_CMD_DEVICE_QUERY:
        if (len < 2) {
            break;
        }
        b->app_target_ver = c[1];
        if (!b->mesh_ready) {
            reply_err(b, CMP_ERR_BAD_STATE);
        } else {
            wait_for(b, CMP_WAIT_DQ_CHANNELS, "mesh.channels", NULL);
        }
        return;
    case CMP_CMD_APP_START:
        if (len < 8) {
            break;
        }
        /* As upstream: an APP_START ends any contact listing left running. */
        cJSON_Delete(b->iter_nodes);
        b->iter_nodes = NULL;
        if (!b->mesh_ready) {
            reply_err(b, CMP_ERR_BAD_STATE);
        } else {
            wait_for(b, CMP_WAIT_SELF_INFO, "mesh.identity", NULL);
        }
        return;
    case CMP_CMD_SEND_TXT_MSG:
        if (len < 14) {
            break;
        }
        cmd_send_dm(b, c, len);
        return;
    case CMP_CMD_SEND_CHANNEL_TXT_MSG:
        if (len < 7) {
            break;
        }
        cmd_send_channel(b, c, len);
        return;
    case CMP_CMD_GET_CONTACTS:
        if (b->iter_nodes) {
            reply_err(b, CMP_ERR_BAD_STATE); /* as upstream: one listing at a time */
        } else if (!b->mesh_ready) {
            reply_err(b, CMP_ERR_BAD_STATE);
        } else {
            wait_for(b, CMP_WAIT_CONTACTS, "mesh.nodes", NULL);
        }
        return;
    case CMP_CMD_GET_CONTACT_BY_KEY:
        if (len < 1 + CMP_PUB_KEY_SIZE) {
            break;
        }
        if (!b->mesh_ready) {
            reply_err(b, CMP_ERR_BAD_STATE);
        } else {
            cJSON *p = cJSON_CreateObject();
            char hex[2 * CMP_PUB_KEY_SIZE + 1];

            cmp_hex(c + 1, CMP_PUB_KEY_SIZE, hex);
            cJSON_AddStringToObject(p, "node", hex);
            wait_for(b, CMP_WAIT_CONTACT_BY_KEY, "mesh.node", p);
        }
        return;
    case CMP_CMD_GET_CHANNEL:
        if (len < 2) {
            break;
        }
        if (!b->mesh_ready) {
            reply_err(b, CMP_ERR_BAD_STATE);
        } else {
            wait_for(b, CMP_WAIT_CHANNEL, "mesh.channels", NULL);
        }
        return;
    case CMP_CMD_SYNC_NEXT_MESSAGE:
        cmd_sync_next(b);
        return;
    case CMP_CMD_GET_DEVICE_TIME: {
        uint8_t f[5];

        f[0] = CMP_RESP_CURR_TIME;
        cmp_put_u32(f + 1, b->io.wall_s(b->io.user));
        to_app(b, f, sizeof(f));
        return;
    }
    case CMP_CMD_SET_DEVICE_TIME:
        /* The app sets the clock as it connects. This one belongs to Doors
         * (NTP, the system's own settings) and is not the app's to change,
         * so the command is refused rather than answered OK. */
        LOG_INFO("refused SET_DEVICE_TIME: the clock is not changed from a companion app");
        reply_err(b, CMP_ERR_UNSUPPORTED_CMD);
        return;
    case CMP_CMD_EXPORT_PRIVATE_KEY:
    case CMP_CMD_IMPORT_PRIVATE_KEY: {
        /* What upstream answers when key export/import is compiled out. */
        uint8_t f[1] = { CMP_RESP_DISABLED };

        to_app(b, f, 1);
        return;
    }
    case CMP_CMD_GET_DEFAULT_FLOOD_SCOPE: {
        /* meshcored writes no transport codes: every flood is unscoped,
         * which upstream says with the one-byte reply (MyMesh.cpp:2007). */
        uint8_t f[1] = { CMP_RESP_DEFAULT_FLOOD_SCOPE };

        to_app(b, f, 1);
        return;
    }
    default:
        break;
    }
    /* Everything else - every command that would change the clock, the
     * radio, the identity, a key, a contact, a channel or a setting, and
     * every one not implemented - is answered as upstream answers a command
     * it does not support. Never OK. */
    LOG_DEBUG("unsupported command %u (%zu bytes)", c[0], len);
    reply_err(b, CMP_ERR_UNSUPPORTED_CMD);
}

/* ---- meshcored replies ------------------------------------------------------ */

static uint8_t mesh_error_to_cmp(int code)
{
    switch (code) {
    case 2:
        return CMP_ERR_NOT_FOUND;
    case 5:
    case 4:
        return CMP_ERR_TABLE_FULL; /* upstream: "unable to send" */
    default:
        return CMP_ERR_BAD_STATE;
    }
}

static void on_send_dm(struct cmp_bridge *b, const cJSON *res, int code, bool resend)
{
    struct cmp_send_rec *r = &b->sends[b->wait_rec];
    const char *route = jstr(res, "route");
    double id, timeout = 0;

    if (!res || !jnum(res, "message_id", &id)) {
        if (!resend) {
            r->used = false;
        }
        /* A resend meshcored will not do is a message it is still waiting on,
         * or one already delivered: the state is wrong, not the recipient. */
        reply_err(b, resend && code == 2 ? CMP_ERR_BAD_STATE : mesh_error_to_cmp(code));
        return;
    }
    jnum(res, "ack_timeout_ms", &timeout);
    r->used = true;
    r->message_id = (uint32_t)id;
    r->tag = new_tag(b);
    r->timeout_ms = (uint32_t)timeout;
    r->flood = !route || strcmp(route, "flood") == 0;
    r->sent_mono_ms = b->io.mono_ms(b->io.user);
    r->acked = false;
    r->confirm_pending = false;
    if (resend) {
        r->attempt = b->cmd[2];
        b->n_resent++;
    } else {
        b->n_sent_dm++;
    }
    reply_sent(b, r);
}

static void on_chan_lookup(struct cmp_bridge *b, const cJSON *res)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(res, "channels");
    const cJSON *c;
    uint8_t slot = b->cmd[2];
    size_t tlen = b->cmd_len - 7;

    cJSON_ArrayForEach (c, arr) {
        double s, limit;

        if (!jnum(c, "channel", &s) || (int)s != slot) {
            continue;
        }
        if (jnum(c, "text_limit", &limit) && tlen > limit) {
            reply_err(b, CMP_ERR_ILLEGAL_ARG);
            return;
        }
        {
            cJSON *p = cJSON_CreateObject();
            char body[CMP_TEXT_MAX + 1];

            memcpy(body, b->cmd + 7, tlen);
            body[tlen] = '\0';
            cJSON_AddNumberToObject(p, "channel", slot);
            cJSON_AddStringToObject(p, "text", body);
            wait_for(b, CMP_WAIT_SEND_CHAN, "mesh.send", p);
        }
        return;
    }
    reply_err(b, CMP_ERR_NOT_FOUND);
}

static void on_send_chan(struct cmp_bridge *b, const cJSON *res, int code)
{
    struct cmp_send_rec *r;
    size_t tlen = b->cmd_len - 7;

    if (!res || !jtrue(res, "accepted")) {
        reply_err(b, code == 2 ? CMP_ERR_ILLEGAL_ARG : mesh_error_to_cmp(code));
        return;
    }
    r = &b->sends[new_send(b)];
    r->used = true;
    r->is_channel = true;
    r->channel = b->cmd[2];
    r->app_timestamp = cmp_get_u32(b->cmd + 3);
    r->text_len = (uint16_t)tlen;
    memcpy(r->text, b->cmd + 7, tlen);
    b->n_sent_chan++;
    /* Upstream answers OK and nothing follows (MyMesh.cpp:2212): a channel
     * message is an unacknowledged flood. */
    reply_ok(b);
}

static void on_reply(struct cmp_bridge *b, const cJSON *res, int code)
{
    enum cmp_wait w = b->wait;

    b->wait = CMP_WAIT_NONE;
    switch (w) {
    case CMP_WAIT_DQ_CHANNELS: {
        double max = 0;

        if (!res || !jnum(res, "max", &max)) {
            reply_err(b, CMP_ERR_BAD_STATE);
            return;
        }
        b->dq_max_channels = (uint8_t)(max > 255 ? 255 : max < 0 ? 0 : max);
        wait_for(b, CMP_WAIT_DQ_PATH_HASH, "mesh.path_hash", NULL);
        return;
    }
    case CMP_WAIT_DQ_PATH_HASH: {
        double bytes = 1;

        /* A meshcored older than mesh.path_hash only ever sent 1-byte
         * hashes: mode 0 is the truth for it. */
        if (res) {
            jnum(res, "bytes", &bytes);
        } else if (code != 1) {
            reply_err(b, CMP_ERR_BAD_STATE);
            return;
        }
        reply_device_info(b, (uint8_t)(bytes >= 1 && bytes <= 3 ? bytes - 1 : 0));
        return;
    }
    case CMP_WAIT_SELF_INFO:
        if (!res) {
            reply_err(b, CMP_ERR_BAD_STATE);
            return;
        }
        reply_self_info(b, res);
        return;
    case CMP_WAIT_CONTACTS: {
        const cJSON *nodes = res ? cJSON_GetObjectItemCaseSensitive(res, "nodes") : NULL;
        uint8_t f[5];

        if (!cJSON_IsArray(nodes)) {
            reply_err(b, CMP_ERR_BAD_STATE);
            return;
        }
        /* The whole list, whatever 'since' the app gave: meshcored keeps no
         * modification time to filter on, and sending a contact the app
         * already has is harmless where leaving one out is not. */
        f[0] = CMP_RESP_CONTACTS_START;
        cmp_put_u32(f + 1, (uint32_t)cJSON_GetArraySize(nodes));
        to_app(b, f, sizeof(f));
        b->iter_nodes = cJSON_Duplicate(nodes, 1);
        b->iter_at = 0;
        b->iter_lastmod = 0;
        return;
    }
    case CMP_WAIT_CONTACT_BY_KEY: {
        uint8_t f[CMP_MAX_FRAME];
        size_t n = res ? build_contact(b, CMP_RESP_CONTACT, res, f, NULL) : 0;

        if (n == 0) {
            reply_err(b, CMP_ERR_NOT_FOUND);
        } else {
            to_app(b, f, n);
        }
        return;
    }
    case CMP_WAIT_CHANNEL:
        if (!res) {
            reply_err(b, CMP_ERR_BAD_STATE);
            return;
        }
        reply_channel(b, res, b->cmd[1]);
        return;
    case CMP_WAIT_SEND_DM:
        on_send_dm(b, res, code, false);
        return;
    case CMP_WAIT_RESEND_DM:
        on_send_dm(b, res, code, true);
        return;
    case CMP_WAIT_CHAN_LOOKUP:
        if (!res) {
            reply_err(b, CMP_ERR_BAD_STATE);
            return;
        }
        on_chan_lookup(b, res);
        return;
    case CMP_WAIT_SEND_CHAN:
        on_send_chan(b, res, code);
        return;
    case CMP_WAIT_NONE:
        return;
    }
}

/* ---- meshcored events ------------------------------------------------------- */

static void on_message_event(struct cmp_bridge *b, const cJSON *m)
{
    const char *dir = jstr(m, "direction");
    const char *kind = jstr(m, "kind");
    const char *state = jstr(m, "state");
    double id;
    int i;

    if (!dir || !jnum(m, "id", &id)) {
        return;
    }
    if (strcmp(dir, "in") == 0) {
        queue_message(b, m);
        return;
    }
    /* Outgoing. Only a direct message this app sent can be confirmed, and
     * only by its ACK; no_ack is reported by nothing, as upstream reports
     * nothing (the app's own timer runs out). */
    if (!state || strcmp(state, "acked") != 0 || (kind && strcmp(kind, "direct") != 0)) {
        return;
    }
    for (i = 0; i < CMP_SEND_RECORDS; i++) {
        struct cmp_send_rec *r = &b->sends[i];
        double ack_mono = 0;

        if (!r->used || r->is_channel || r->message_id != (uint32_t)id || r->acked) {
            continue;
        }
        r->acked = true;
        if (!jnum(m, "ack_mono_ms", &ack_mono) || ack_mono < 0) {
            ack_mono = (double)b->io.mono_ms(b->io.user);
        }
        r->ack_mono_ms = (uint64_t)ack_mono;
        if (b->app_up && b->app_started) {
            push_confirmed(b, r);
        } else {
            /* Held until an app has started on a connection (upstream would
             * have written it to nobody), with the real round trip. */
            r->confirm_pending = true;
        }
        return;
    }
}

static void on_node_event(struct cmp_bridge *b, const cJSON *data)
{
    const char *reason = jstr(data, "reason");
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(data, "node");
    const char *key = jstr(node, "public_key");
    uint8_t f[1 + CMP_PUB_KEY_SIZE];

    if (!reason || !key || cmp_unhex(key, f + 1, CMP_PUB_KEY_SIZE) != 0) {
        return;
    }
    if (strcmp(reason, "discovered") == 0) {
        f[0] = CMP_PUSH_ADVERT;
    } else if (strcmp(reason, "path") == 0) {
        f[0] = CMP_PUSH_PATH_UPDATED;
    } else if (strcmp(reason, "removed") == 0) {
        f[0] = CMP_PUSH_CONTACT_DELETED;
    } else {
        return;
    }
    to_app(b, f, sizeof(f));
}

void cmp_bridge_mesh_frame(struct cmp_bridge *b, const cJSON *msg)
{
    const char *ev = jstr(msg, "event");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(msg, "data");
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(msg, "result");
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(msg, "error");
    double idd;
    uint32_t id;
    int code = 0;

    if (ev) {
        if (strcmp(ev, "mesh.message") == 0) {
            on_message_event(b, cJSON_GetObjectItemCaseSensitive(data, "message"));
        } else if (strcmp(ev, "mesh.node") == 0) {
            on_node_event(b, data);
        }
        return;
    }
    if (!jnum(msg, "id", &idd)) {
        return;
    }
    id = (uint32_t)idd;
    if (err) {
        double c = 0;

        jnum(err, "code", &c);
        code = (int)c;
        res = NULL;
    } else if (!cJSON_IsObject(res)) {
        res = NULL;
    }
    if (id == b->id_status) {
        const char *run = jstr(res, "run_id");

        b->id_status = 0;
        if (run) {
            if (b->run_id[0] && strcmp(b->run_id, run) != 0) {
                /* A new run of meshcored: its message ids start again at 1,
                 * so nothing kept about the old run can be matched. The
                 * queue keeps what it holds; those messages were received. */
                forget_run(b, "meshcored restarted");
            }
            snprintf(b->run_id, sizeof(b->run_id), "%s", run);
        }
    } else if (id == b->id_info) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(res, "requested_profile");
        double f, bw, sf, cr, tx;
        const char *ver = jstr(res, "version");

        b->id_info = 0;
        if (jnum(p, "frequency_mhz", &f) && jnum(p, "bandwidth_khz", &bw) &&
            jnum(p, "spreading_factor", &sf) && jnum(p, "coding_rate", &cr) &&
            jnum(p, "tx_power_dbm", &tx)) {
            b->have_profile = true;
            b->freq_mhz = f;
            b->bw_khz = bw;
            b->sf = (int)sf;
            b->cr = (int)cr;
            b->tx_power_dbm = (int)tx;
        }
        if (ver) {
            snprintf(b->version, sizeof(b->version), "%s", ver);
        }
    } else if (id == b->id_subscribe) {
        b->id_subscribe = 0;
        if (!res) {
            LOG_WARN("meshcored refused the subscription (code %d)", code);
            b->mesh_reset_wanted = true;
        }
    } else if (b->wait != CMP_WAIT_NONE && id == b->wait_id) {
        on_reply(b, res, code);
        return;
    }
    if (!b->mesh_ready && b->mesh_up && b->id_status == 0 && b->id_info == 0 &&
        b->id_subscribe == 0) {
        b->mesh_ready = true;
        LOG_INFO("meshcored ready (run %s, profile %s)", b->run_id[0] ? b->run_id : "?",
                 b->have_profile ? "known" : "unknown");
    }
}

/* ---- lifecycle ------------------------------------------------------------- */

void cmp_bridge_init(struct cmp_bridge *b, const struct cmp_io *io, uint32_t tag_seed)
{
    memset(b, 0, sizeof(*b));
    b->io = *io;
    b->next_tag = tag_seed ? tag_seed : 1;
}

void cmp_bridge_free(struct cmp_bridge *b)
{
    cJSON_Delete(b->iter_nodes);
    b->iter_nodes = NULL;
}

void cmp_bridge_mesh_up(struct cmp_bridge *b)
{
    b->mesh_up = true;
    b->mesh_ready = false;
    b->mesh_reset_wanted = false;
    /* In this order on purpose: meshcored answers in order, so the run id
     * is known before the first event can arrive, and the subscription is
     * plain - no receive log is asked for. */
    b->id_status = mesh_call(b, "mesh.status", NULL);
    b->id_info = mesh_call(b, "mesh.info", NULL);
    b->id_subscribe = mesh_call(b, "mesh.subscribe", NULL);
}

void cmp_bridge_mesh_down(struct cmp_bridge *b)
{
    b->mesh_up = false;
    b->mesh_ready = false;
    b->id_status = b->id_info = b->id_subscribe = 0;
    if (b->wait != CMP_WAIT_NONE) {
        enum cmp_wait w = b->wait;

        b->wait = CMP_WAIT_NONE;
        if (w == CMP_WAIT_SEND_DM) {
            b->sends[b->wait_rec].used = false;
        }
        reply_err(b, CMP_ERR_BAD_STATE);
    }
}

void cmp_bridge_app_up(struct cmp_bridge *b)
{
    b->app_up = true;
    b->app_started = false;
    b->app_target_ver = 0;
}

void cmp_bridge_app_down(struct cmp_bridge *b)
{
    b->app_up = false;
    b->app_started = false;
    cJSON_Delete(b->iter_nodes);
    b->iter_nodes = NULL;
    /* A command still waiting for meshcored is let finish; its answer has
     * nowhere to go, and a send it made is recorded all the same, so the
     * app's repeat of it after reconnecting is recognised. */
}

bool cmp_bridge_busy(const struct cmp_bridge *b)
{
    return b->wait != CMP_WAIT_NONE;
}

void cmp_bridge_tick(struct cmp_bridge *b, int room)
{
    if (b->wait != CMP_WAIT_NONE &&
        b->io.mono_ms(b->io.user) - b->wait_since > CMP_MESH_TIMEOUT_MS) {
        LOG_WARN("meshcored did not answer within %d ms; reconnecting", CMP_MESH_TIMEOUT_MS);
        b->mesh_reset_wanted = true;
        cmp_bridge_mesh_down(b);
    }
    while (b->iter_nodes && room-- > 0) {
        const cJSON *node = cJSON_GetArrayItem(b->iter_nodes, b->iter_at);
        uint8_t f[CMP_MAX_FRAME];

        if (node) {
            uint32_t lastmod = 0;
            size_t n = build_contact(b, CMP_RESP_CONTACT, node, f, &lastmod);

            b->iter_at++;
            if (n) {
                to_app(b, f, n);
                if (lastmod > b->iter_lastmod) {
                    b->iter_lastmod = lastmod;
                }
            }
            continue;
        }
        f[0] = CMP_RESP_END_OF_CONTACTS;
        cmp_put_u32(f + 1, b->iter_lastmod);
        to_app(b, f, 5);
        cJSON_Delete(b->iter_nodes);
        b->iter_nodes = NULL;
    }
}
