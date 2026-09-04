/*
 * Radio app: live view of radiod (status, profile, statistics) with a test
 * transmit. Uses only the radio.* API over pocketipc.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "pocketui.h"
#include "shell_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct radio_app {
    lv_obj_t *chip;
    lv_obj_t *state;
    lv_obj_t *frequency;
    lv_obj_t *modulation;
    lv_obj_t *power;
    lv_obj_t *packets;
    lv_obj_t *airtime;
    lv_obj_t *last_rx;
    lv_obj_t *message;
};

static const char *str(const cJSON *o, const char *key, const char *fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsString(v) ? v->valuestring : fallback;
}

static double num(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

static void on_send(lv_event_t *e)
{
    struct radio_app *a = lv_event_get_user_data(e);
    cJSON *params = cJSON_CreateObject();
    cJSON *r;
    char err[96];

    cJSON_AddStringToObject(params, "payload_hex", "506f636b65744f5320746573740a"); /* "PocketOS test\n" */
    r = shell_ipc_call("radiod", "radio.send", params, err, sizeof(err));
    if (r) {
        lv_label_set_text_fmt(a->message, "Sent %d bytes, %.1f ms airtime",
                              (int)num(r, "bytes"), num(r, "airtime_ms"));
        cJSON_Delete(r);
    } else {
        lv_label_set_text_fmt(a->message, "Send failed: %s", err);
    }
}

static void on_inject(lv_event_t *e)
{
    struct radio_app *a = lv_event_get_user_data(e);
    cJSON *params = cJSON_CreateObject();
    cJSON *r;
    char err[96];

    cJSON_AddStringToObject(params, "payload_hex", "48656c6c6f");
    cJSON_AddNumberToObject(params, "rssi_dbm", -92.0);
    cJSON_AddNumberToObject(params, "snr_db", 6.5);
    r = shell_ipc_call("radiod", "mock.inject_rx", params, err, sizeof(err));
    if (r) {
        lv_label_set_text(a->message, "Injected a mock packet");
        cJSON_Delete(r);
    } else {
        lv_label_set_text_fmt(a->message, "Inject failed: %s", err);
    }
}

static void *radio_create(lv_obj_t *root)
{
    struct radio_app *a = calloc(1, sizeof(*a));
    lv_obj_t *card;

    if (!a) {
        return NULL;
    }
    card = pocketui_card(root);
    a->chip = pocketui_kv_row(card, "Radio", "-");
    a->state = pocketui_kv_row(card, "State", "-");
    a->frequency = pocketui_kv_row(card, "Frequency", "-");
    a->modulation = pocketui_kv_row(card, "LoRa", "-");
    a->power = pocketui_kv_row(card, "TX power", "-");

    card = pocketui_card(root);
    a->packets = pocketui_kv_row(card, "Packets TX / RX", "-");
    a->airtime = pocketui_kv_row(card, "Airtime last hour", "-");
    a->last_rx = pocketui_kv_row(card, "Last RX", "-");

    pocketui_button(root, "Send test packet", on_send, a);
    pocketui_button(root, "Inject mock RX", on_inject, a);
    a->message = lv_label_create(root);
    lv_label_set_text(a->message, "");
    lv_obj_set_style_text_color(a->message, pocketui_tokens()->text_dim, 0);
    return a;
}

static void radio_tick(void *priv)
{
    struct radio_app *a = priv;
    char err[96];
    cJSON *info = shell_ipc_call("radiod", "radio.info", NULL, err, sizeof(err));
    cJSON *st;
    cJSON *stats;

    if (!info) {
        lv_label_set_text(a->chip, "unavailable");
        lv_label_set_text(a->state, err[0] ? err : "-");
        return;
    }
    lv_label_set_text_fmt(a->chip, "%s (%s)", str(info, "chip", "?"), str(info, "region", "?"));
    cJSON_Delete(info);

    st = shell_ipc_call("radiod", "radio.status", NULL, err, sizeof(err));
    if (st) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(st, "profile");

        lv_label_set_text(a->state, str(st, "state", "?"));
        if (p) {
            lv_label_set_text_fmt(a->frequency, "%.3f MHz", num(p, "frequency_mhz"));
            lv_label_set_text_fmt(a->modulation, "SF%d  BW%.0f  CR4/%d",
                                  (int)num(p, "spreading_factor"), num(p, "bandwidth_khz"),
                                  (int)num(p, "coding_rate"));
            lv_label_set_text_fmt(a->power, "%d dBm", (int)num(p, "tx_power_dbm"));
        }
        cJSON_Delete(st);
    }
    stats = shell_ipc_call("radiod", "radio.stats", NULL, err, sizeof(err));
    if (stats) {
        lv_label_set_text_fmt(a->packets, "%d / %d", (int)num(stats, "tx_packets"),
                              (int)num(stats, "rx_packets"));
        lv_label_set_text_fmt(a->airtime, "%.0f ms (%.2f %%)",
                              num(stats, "tx_airtime_last_hour_ms"),
                              num(stats, "duty_cycle_last_hour_percent"));
        if (cJSON_HasObjectItem(stats, "last_rssi_dbm")) {
            lv_label_set_text_fmt(a->last_rx, "%.0f dBm, SNR %.1f dB",
                                  num(stats, "last_rssi_dbm"), num(stats, "last_snr_db"));
        }
        cJSON_Delete(stats);
    }
}

static void radio_destroy(void *priv)
{
    free(priv);
}

const struct pocketos_app app_radio = {
    .id = "radio",
    .name = "Radio",
    .icon = LV_SYMBOL_WIFI,
    .create = radio_create,
    .tick = radio_tick,
    .destroy = radio_destroy,
};
