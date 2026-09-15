/*
 * Settings view model. See settings_view.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "settings_view.h"

#include <stdio.h>
#include <string.h>

/* "·" is U+00B7, inside the Latin-1 range the product fonts carry. */
#define SEP " \xc2\xb7 "

static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int num(const cJSON *o, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

static int flag(const cJSON *o, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key));
}

static void copy(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

const char *sv_security_label(const char *w)
{
    if (!w) {
        return "Unknown";
    }
    if (strcmp(w, "open") == 0) {
        return "Open";
    }
    if (strcmp(w, "wpa") == 0) {
        return "WPA";
    }
    if (strcmp(w, "wpa2") == 0) {
        return "WPA2";
    }
    if (strcmp(w, "wpa2/wpa3") == 0) {
        return "WPA2/3";
    }
    if (strcmp(w, "wpa3") == 0) {
        return "WPA3";
    }
    if (strcmp(w, "wep") == 0) {
        return "WEP";
    }
    if (strcmp(w, "owe") == 0) {
        return "OWE";
    }
    if (strcmp(w, "enterprise") == 0) {
        return "Enterprise";
    }
    return "Unknown";
}

void sv_wifi_init(struct sv_wifi *w)
{
    memset(w, 0, sizeof(*w));
    copy(w->headline, sizeof(w->headline), "Checking Wi-Fi...");
    w->tone = SV_TONE_MUTED;
}

/* The headline for a state, naming the network where there is one. */
static void headline(struct sv_wifi *w)
{
    const char *s = w->state;
    const char *r = w->reason;
    const char *ssid = w->ssid[0] ? w->ssid : "the network";

    w->tone = SV_TONE_NORMAL;
    if (strcmp(s, "unavailable") == 0) {
        if (strcmp(r, "interface_busy") == 0) {
            copy(w->headline, sizeof(w->headline), "Wi-Fi is in use by another program");
            w->tone = SV_TONE_WARN;
        } else {
            copy(w->headline, sizeof(w->headline), "No Wi-Fi hardware found");
            w->tone = SV_TONE_MUTED;
        }
    } else if (strcmp(s, "off") == 0) {
        copy(w->headline, sizeof(w->headline), "Wi-Fi is off");
        w->tone = SV_TONE_MUTED;
    } else if (strcmp(s, "starting") == 0) {
        copy(w->headline, sizeof(w->headline), "Starting Wi-Fi...");
    } else if (strcmp(s, "disconnected") == 0) {
        copy(w->headline, sizeof(w->headline), "Not connected");
    } else if (strcmp(s, "connecting") == 0) {
        snprintf(w->headline, sizeof(w->headline), "Connecting to %s...", ssid);
    } else if (strcmp(s, "obtaining_ip") == 0) {
        snprintf(w->headline, sizeof(w->headline), "Getting an address from %s...", ssid);
    } else if (strcmp(s, "connected") == 0) {
        snprintf(w->headline, sizeof(w->headline), "Connected to %s", ssid);
        w->tone = SV_TONE_OK;
    } else if (strcmp(s, "failed") == 0) {
        w->tone = SV_TONE_ERROR;
        if (strcmp(r, "auth_failed") == 0) {
            snprintf(w->headline, sizeof(w->headline), "Wrong passphrase for %s", ssid);
        } else if (strcmp(r, "not_found") == 0) {
            snprintf(w->headline, sizeof(w->headline), "%s did not answer", w->ssid[0] ? w->ssid : "The network");
        } else if (strcmp(r, "assoc_failed") == 0) {
            snprintf(w->headline, sizeof(w->headline), "%s refused the connection",
                     w->ssid[0] ? w->ssid : "The network");
        } else if (strcmp(r, "timeout") == 0) {
            snprintf(w->headline, sizeof(w->headline), "Could not connect to %s", ssid);
        } else if (strcmp(r, "dhcp_failed") == 0) {
            snprintf(w->headline, sizeof(w->headline), "Connected to %s, but got no address", ssid);
            w->tone = SV_TONE_WARN;
        } else if (strcmp(r, "supplicant_failed") == 0) {
            copy(w->headline, sizeof(w->headline), "Wi-Fi could not be started");
        } else {
            copy(w->headline, sizeof(w->headline), "Wi-Fi failed");
        }
    } else {
        copy(w->headline, sizeof(w->headline), "Wi-Fi state unknown");
        w->tone = SV_TONE_MUTED;
    }
}

void sv_wifi_apply_status(struct sv_wifi *w, const cJSON *st)
{
    const cJSON *ipv4;
    const cJSON *bars;
    const char *store;

    if (!st || !cJSON_IsObject(st)) {
        /* The last list stays; everything that claims to be live does not. */
        w->reachable = 0;
        w->available = 0;
        w->toggle_enabled = 0;
        w->can_scan = 0;
        w->can_disconnect = 0;
        w->scanning = 0;
        w->list_visible = 0;
        w->state[0] = '\0';
        w->reason[0] = '\0';
        w->detail[0] = '\0';
        w->store_note[0] = '\0';
        copy(w->headline, sizeof(w->headline), "Wi-Fi service is not running");
        w->tone = SV_TONE_ERROR;
        return;
    }
    w->reachable = 1;
    w->available = flag(st, "available");
    w->enabled = flag(st, "enabled");
    copy(w->state, sizeof(w->state), str(st, "state"));
    copy(w->reason, sizeof(w->reason), str(st, "reason"));
    copy(w->ssid, sizeof(w->ssid), str(st, "ssid"));
    headline(w);

    w->detail[0] = '\0';
    ipv4 = cJSON_GetObjectItemCaseSensitive(st, "ipv4");
    bars = cJSON_GetObjectItemCaseSensitive(st, "signal_bars");
    if (cJSON_IsString(ipv4) && cJSON_IsNumber(bars)) {
        snprintf(w->detail, sizeof(w->detail), "%s" SEP "Signal %d/4", ipv4->valuestring, bars->valueint);
    } else if (cJSON_IsString(ipv4)) {
        copy(w->detail, sizeof(w->detail), ipv4->valuestring);
    } else if (cJSON_IsNumber(bars)) {
        snprintf(w->detail, sizeof(w->detail), "Signal %d/4", bars->valueint);
    }

    /* Off can always be turned on while the hardware is there; a board with
     * no interface has nothing to turn on, and the switch says so by being
     * unusable rather than by doing nothing. */
    w->toggle_enabled = w->available || w->enabled;
    w->scanning = flag(st, "scanning");
    w->list_visible = w->available && w->enabled && strcmp(w->state, "off") != 0 &&
                      strcmp(w->state, "starting") != 0 && strcmp(w->state, "unavailable") != 0;
    w->can_scan = w->list_visible && !w->scanning;
    w->can_disconnect = w->list_visible &&
                        (strcmp(w->state, "connected") == 0 || strcmp(w->state, "connecting") == 0 ||
                         strcmp(w->state, "obtaining_ip") == 0 ||
                         (strcmp(w->state, "failed") == 0 && w->ssid[0]));

    store = str(st, "store");
    w->store_note[0] = '\0';
    if (store && strcmp(store, "damaged") == 0) {
        copy(w->store_note, sizeof(w->store_note), "Saved networks could not be read; they were kept aside.");
    } else if (store && strcmp(store, "unreadable") == 0) {
        copy(w->store_note, sizeof(w->store_note), "Saved networks could not be read.");
    } else if (store && strcmp(store, "unwritable") == 0) {
        copy(w->store_note, sizeof(w->store_note), "Networks could not be saved.");
    }
    if (!w->list_visible) {
        w->list_note[0] = '\0';
    }
}

void sv_wifi_apply_networks(struct sv_wifi *w, const cJSON *res)
{
    const cJSON *list;
    const cJSON *e;
    const cJSON *age;
    int hidden;
    int total = 0;

    if (!res || !cJSON_IsObject(res)) {
        return;
    }
    list = cJSON_GetObjectItemCaseSensitive(res, "networks");
    age = cJSON_GetObjectItemCaseSensitive(res, "age_s");
    hidden = num(res, "hidden_count", 0);
    w->scanning = flag(res, "scanning");
    w->net_count = 0;
    cJSON_ArrayForEach(e, list) {
        struct sv_network *n;
        const char *ssid = str(e, "ssid");
        const char *hex = str(e, "ssid_hex");

        total++;
        if (!ssid || !hex || w->net_count >= SV_MAX_NETWORKS) {
            continue;
        }
        n = &w->nets[w->net_count++];
        memset(n, 0, sizeof(*n));
        copy(n->ssid, sizeof(n->ssid), ssid);
        copy(n->ssid_hex, sizeof(n->ssid_hex), hex);
        copy(n->security, sizeof(n->security), str(e, "security"));
        n->bars = num(e, "signal_bars", 0);
        n->saved = flag(e, "saved");
        n->connected = flag(e, "connected");
        n->supported = flag(e, "supported");
        n->needs_passphrase = flag(e, "needs_passphrase");
        if (n->supported) {
            snprintf(n->detail, sizeof(n->detail), "%s" SEP "%d/4", sv_security_label(n->security), n->bars);
        } else {
            snprintf(n->detail, sizeof(n->detail), "%s" SEP "not supported",
                     sv_security_label(n->security));
        }
        copy(n->badge, sizeof(n->badge), n->connected ? "CONNECTED" : n->saved ? "SAVED" : "");
    }
    if (w->scanning && w->net_count == 0) {
        copy(w->list_note, sizeof(w->list_note), "Scanning...");
    } else if (!cJSON_IsNumber(age) && !w->scanning) {
        copy(w->list_note, sizeof(w->list_note), "Tap Scan to look for networks.");
    } else if (w->net_count == 0) {
        copy(w->list_note, sizeof(w->list_note), "No networks found.");
    } else if (total > w->net_count && hidden > 0) {
        snprintf(w->list_note, sizeof(w->list_note), "%d more, and %d hidden, not shown.",
                 total - w->net_count, hidden);
    } else if (total > w->net_count) {
        snprintf(w->list_note, sizeof(w->list_note), "%d weaker networks not shown.", total - w->net_count);
    } else if (hidden > 0) {
        snprintf(w->list_note, sizeof(w->list_note), "%d hidden network%s not shown.", hidden,
                 hidden == 1 ? "" : "s");
    } else {
        w->list_note[0] = '\0';
    }
}

enum sv_join_kind sv_join_kind(const struct sv_network *n)
{
    if (n->connected) {
        return SV_JOIN_CONNECTED;
    }
    if (!n->supported) {
        return SV_JOIN_UNSUPPORTED;
    }
    if (n->saved) {
        return SV_JOIN_SAVED;
    }
    if (!n->needs_passphrase) {
        return strcmp(n->security, "open") == 0 ? SV_JOIN_OPEN : SV_JOIN_UNSUPPORTED;
    }
    return SV_JOIN_PASSPHRASE;
}

void sv_join_text(const struct sv_network *n, char *body, size_t n_body)
{
    switch (sv_join_kind(n)) {
    case SV_JOIN_CONNECTED:
        snprintf(body, n_body, "Connected" SEP "%s" SEP "Signal %d/4", sv_security_label(n->security), n->bars);
        break;
    case SV_JOIN_SAVED:
        snprintf(body, n_body, "Saved network" SEP "%s" SEP "Signal %d/4", sv_security_label(n->security),
                 n->bars);
        break;
    case SV_JOIN_OPEN:
        snprintf(body, n_body,
                 "This network is open. Anything sent over it is not encrypted, and "
                 "anyone nearby can read it.");
        break;
    case SV_JOIN_UNSUPPORTED:
        if (strcmp(n->security, "wpa3") == 0) {
            snprintf(body, n_body, "WPA3-only networks are not supported by this device's Wi-Fi.");
        } else if (strcmp(n->security, "wep") == 0) {
            snprintf(body, n_body, "WEP networks are not supported: WEP is not secure.");
        } else if (strcmp(n->security, "enterprise") == 0) {
            snprintf(body, n_body, "Enterprise (802.1X) networks are not supported.");
        } else {
            snprintf(body, n_body, "%s networks are not supported.", sv_security_label(n->security));
        }
        break;
    case SV_JOIN_PASSPHRASE:
    default:
        snprintf(body, n_body, "%s" SEP "Signal %d/4. Enter the passphrase.", sv_security_label(n->security),
                 n->bars);
        break;
    }
}

int sv_passphrase_check(const char *pass, char *why, size_t n)
{
    size_t len;
    size_t i;

    if (!pass) {
        pass = "";
    }
    for (len = 0; len <= 64 && pass[len]; len++) {
    }
    if (len < 8) {
        snprintf(why, n, "At least 8 characters");
        return -1;
    }
    if (len > 63) {
        snprintf(why, n, "At most 63 characters");
        return -1;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)pass[i];

        if (c < 0x20 || c > 0x7e) {
            snprintf(why, n, "Only letters, digits, spaces and ASCII symbols (no \xc3\xa6\xc3\xb8\xc3\xa5)");
            return -1;
        }
    }
    why[0] = '\0';
    return 0;
}

cJSON *sv_connect_params(const struct sv_network *n, enum sv_join_kind kind, const char *passphrase)
{
    cJSON *p;

    if (kind != SV_JOIN_PASSPHRASE && kind != SV_JOIN_SAVED && kind != SV_JOIN_OPEN) {
        return NULL;
    }
    if (kind == SV_JOIN_PASSPHRASE && !passphrase) {
        return NULL;
    }
    p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "ssid_hex", n->ssid_hex);
    if (kind == SV_JOIN_PASSPHRASE) {
        cJSON_AddStringToObject(p, "passphrase", passphrase);
    } else if (kind == SV_JOIN_OPEN) {
        cJSON_AddBoolToObject(p, "allow_open", 1);
    }
    return p;
}

cJSON *sv_ssid_params(const struct sv_network *n)
{
    cJSON *p = cJSON_CreateObject();

    cJSON_AddStringToObject(p, "ssid_hex", n->ssid_hex);
    return p;
}

/* ---- brightness ---------------------------------------------------------- */

void sv_brightness_apply(struct sv_brightness *b, int percent, int min, int max)
{
    memset(b, 0, sizeof(*b));
    b->percent = percent;
    if (percent < 0) {
        copy(b->value, sizeof(b->value), "--");
        copy(b->note, sizeof(b->note), "This display has no brightness control.");
        return;
    }
    b->supported = 1;
    snprintf(b->value, sizeof(b->value), "%d %%", percent);
    b->can_down = percent > min;
    b->can_up = percent < max;
}

int sv_brightness_step(int percent, int direction, int min, int max, int step)
{
    int next;

    if (step <= 0) {
        step = 10;
    }
    if (percent < min) {
        return min;
    }
    if (direction > 0) {
        next = (percent / step + 1) * step;
    } else if (direction < 0) {
        next = percent % step ? (percent / step) * step : percent - step;
    } else {
        next = percent;
    }
    if (next < min) {
        next = min;
    }
    if (next > max) {
        next = max;
    }
    return next;
}

void sv_rotation_apply(struct sv_rotation *r, int mode, int mode_valid, int landscape, int next_landscape,
                       int restart_required, int keyboard_present)
{
    const char *prefix = mode_valid ? "" : "The stored rotation was not recognised, so Automatic is used. ";

    memset(r, 0, sizeof(*r));
    r->selected = mode >= 0 && mode < SV_ROTATION_MODES ? mode : 0;
    if (restart_required) {
        snprintf(r->note, sizeof(r->note), "%sShowing %s now. %s takes effect when the Doors shell restarts.",
                 prefix, landscape ? "landscape" : "portrait", next_landscape ? "Landscape" : "Portrait");
    } else if (r->selected == 0) {
        snprintf(r->note, sizeof(r->note), "%s%s", prefix,
                 keyboard_present ? "Landscape, because a keyboard is attached."
                                  : "Portrait: no keyboard detected. Automatic turns to landscape with a keyboard.");
    } else {
        snprintf(r->note, sizeof(r->note), "%s%s, whatever the keyboard.", prefix,
                 r->selected == 2 ? "Landscape" : "Portrait");
    }
}
