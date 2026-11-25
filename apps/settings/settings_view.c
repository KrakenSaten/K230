/*
 * Settings view model. See settings_view.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "settings_view.h"

#include "power_policy.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

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
                       int applying, int keyboard_present)
{
    const char *prefix = mode_valid ? "" : "The stored rotation was not recognised, so Automatic is used. ";

    memset(r, 0, sizeof(*r));
    r->selected = mode >= 0 && mode < SV_ROTATION_MODES ? mode : 0;
    if (applying) {
        /* The display is turned by opening it again, so Doors restarts itself
         * and comes back on the launcher: the screen going dark and the app
         * closing are the cost, and saying so beforehand is the difference
         * between a feature and a fault (DS §2). */
        (void)landscape;
        snprintf(r->note, sizeof(r->note), "%sTurning to %s now. The screen goes dark for a moment and "
                                           "Doors opens on the launcher.",
                 prefix, next_landscape ? "landscape" : "portrait");
    } else if (r->selected == 0) {
        snprintf(r->note, sizeof(r->note), "%s%s", prefix,
                 keyboard_present ? "Landscape, because a keyboard is attached."
                                  : "Portrait: no keyboard detected. Automatic turns to landscape with a keyboard.");
    } else {
        snprintf(r->note, sizeof(r->note), "%s%s, whatever the keyboard.", prefix,
                 r->selected == 2 ? "Landscape" : "Portrait");
    }
}

void sv_rotation_external(struct sv_rotation *r, int mode_valid, int keyboard_present)
{
    const char *held = r->selected == 2   ? "in landscape."
                       : r->selected == 1 ? "in portrait."
                       : keyboard_present ? "in landscape (keyboard attached)."
                                          : "in portrait (no keyboard).";

    /* The monitor is never turned (shell_display.h); the mode says how the
     * unit stands, which is how the camera's picture is turned upright. A
     * stored value that was not a mode is still said first, as on the panel,
     * in fewer words so that the note fits. */
    if (!mode_valid) {
        snprintf(r->note, sizeof(r->note),
                 "Stored rotation not recognised: Automatic. HDMI: camera picture turned for the unit held %s",
                 keyboard_present ? "in landscape." : "in portrait.");
        return;
    }
    snprintf(r->note, sizeof(r->note), "HDMI: the screen does not turn. Camera picture turned for the unit held %s",
             held);
}

/* ---- sound ---------------------------------------------------------------- */

void sv_volume_apply(struct sv_volume *v, int percent, int muted, int available, int min, int max)
{
    memset(v, 0, sizeof(*v));
    v->available = available != 0;
    v->muted = muted != 0;
    snprintf(v->value, sizeof(v->value), "%d %%", percent);
    v->can_down = percent > min;
    v->can_up = percent < max;
    if (!v->available) {
        /* The level is still kept and can still be set: a card that appears
         * later plays at it. Saying why nothing is heard is the point. */
        copy(v->note, sizeof(v->note), "No sound card was found, so nothing plays at any volume.");
        copy(v->summary, sizeof(v->summary), "No sound output");
    } else if (v->muted) {
        copy(v->note, sizeof(v->note), "Muted: nothing plays until sound is turned back on.");
        snprintf(v->summary, sizeof(v->summary), "Muted (volume %d %%)", percent);
    } else {
        snprintf(v->summary, sizeof(v->summary), "Volume %d %%", percent);
    }
}

/* ---- the keyboard base ------------------------------------------------------ */

void sv_keyboard_text(int keyboard, int light, char *state, size_t n_state, char *summary, size_t n_summary)
{
    const char *word = keyboard == 2 ? "ATTACHED" : keyboard == 1 ? "NOT ATTACHED" : "UNKNOWN";

    copy(state, n_state, word);
    if (keyboard != 2) {
        snprintf(summary, n_summary, "%s", keyboard == 1 ? "Keyboard base not attached" : "Keyboard base unknown");
    } else if (light < 0) {
        snprintf(summary, n_summary, "Keyboard base attached");
    } else if (light == 0) {
        snprintf(summary, n_summary, "Keyboard base attached" SEP "light off");
    } else {
        snprintf(summary, n_summary, "Keyboard base attached" SEP "light %d %%", light);
    }
}

/* ---- Power & Sleep ------------------------------------------------------------ */

void sv_timer_apply(struct sv_timer *t, int which, int seconds)
{
    enum power_timer pt = which == POWER_TIMER_LOCK ? POWER_TIMER_LOCK : POWER_TIMER_SCREEN;
    int i = power_option_index(pt, seconds);

    memset(t, 0, sizeof(*t));
    power_option_label(seconds, t->value, sizeof(t->value));
    /* A value that is not an option (it cannot be stored, but the shell is
     * not this file) still steps: to the first option either way. */
    t->can_down = i != 0;
    t->can_up = i != power_option_count(pt) - 1;
}

void sv_power_summary(int screen_off_s, int lock_s, char *out, size_t n)
{
    char a[16];
    char b[16];

    power_option_label(screen_off_s, a, sizeof(a));
    power_option_label(lock_s, b, sizeof(b));
    if (screen_off_s <= 0 && lock_s <= 0) {
        snprintf(out, n, "Screen stays on, no automatic lock");
    } else if (lock_s <= 0) {
        snprintf(out, n, "Screen off after %s", a);
    } else if (screen_off_s <= 0) {
        snprintf(out, n, "Lock after %s", b);
    } else {
        snprintf(out, n, "Screen off after %s" SEP "lock after %s", a, b);
    }
}

/* ---- Power & Sleep: the battery ------------------------------------------- */

enum { BAT_STATUS, BAT_VOLTAGE, BAT_CURRENT, BAT_LEVEL, BAT_UPDATED };

static const char *battery_status_word(const char *st)
{
    if (st && strcmp(st, "charging") == 0) {
        return "Charging";
    }
    if (st && strcmp(st, "discharging") == 0) {
        return "On battery";
    }
    if (st && strcmp(st, "full") == 0) {
        return "Full";
    }
    if (st && strcmp(st, "not_charging") == 0) {
        return "Not charging";
    }
    return NULL;
}

void sv_battery_apply(struct sv_battery *b, const cJSON *status)
{
    static const char *const labels[SV_BATTERY_ROWS] = {"Status", "Voltage", "Current", "Level",
                                                         "Updated"};
    const cJSON *power;
    const cJSON *bat;
    const cJSON *v;
    const char *reading;
    const char *w;
    int i;

    for (i = 0; i < SV_BATTERY_ROWS; i++) {
        b->label[i] = labels[i];
        copy(b->value[i], sizeof(b->value[i]), "-");
    }
    if (!status) {
        copy(b->value[BAT_STATUS], sizeof(b->value[0]), "System service not answering");
        return;
    }
    power = cJSON_GetObjectItemCaseSensitive(status, "power");
    bat = cJSON_GetObjectItemCaseSensitive(power, "battery");
    if (!cJSON_IsObject(power)) {
        copy(b->value[BAT_STATUS], sizeof(b->value[0]), "Unknown");
        return;
    }
    if (!cJSON_IsObject(bat)) {
        copy(b->value[BAT_STATUS], sizeof(b->value[0]),
             str(power, "source") && strcmp(str(power, "source"), "external") == 0
                 ? "No battery, external power"
                 : "No battery found");
        return;
    }
    reading = str(bat, "reading");
    if (reading && strcmp(reading, "ok") != 0) {
        copy(b->value[BAT_STATUS], sizeof(b->value[0]),
             strcmp(reading, "stale") == 0       ? "No recent reading"
             : strcmp(reading, "stopped") == 0   ? "Not reading (keyboard protection)"
             : strcmp(reading, "no-answer") == 0 ? "Gauge not answering"
                                                 : "Unknown");
        v = cJSON_GetObjectItemCaseSensitive(bat, "age_s");
        if (strcmp(reading, "stale") == 0 && cJSON_IsNumber(v)) {
            snprintf(b->value[BAT_UPDATED], sizeof(b->value[0]), "%d s ago (too old to show)",
                     (int)v->valuedouble);
        }
        return;
    }
    if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(bat, "present"))) {
        copy(b->value[BAT_STATUS], sizeof(b->value[0]), "No battery");
        return;
    }
    w = battery_status_word(str(bat, "status"));
    copy(b->value[BAT_STATUS], sizeof(b->value[0]), w ? w : "Unknown");
    v = cJSON_GetObjectItemCaseSensitive(bat, "voltage_v");
    if (cJSON_IsNumber(v) && v->valuedouble > 0) {
        snprintf(b->value[BAT_VOLTAGE], sizeof(b->value[0]), "%.2f V", v->valuedouble);
    }
    v = cJSON_GetObjectItemCaseSensitive(bat, "current_a");
    if (cJSON_IsNumber(v)) {
        /* The sign as the gauge gives it: plus into the battery. */
        snprintf(b->value[BAT_CURRENT], sizeof(b->value[0]), "%+.2f A%s", v->valuedouble,
                 v->valuedouble < 0 ? " (out of the battery)"
                 : v->valuedouble > 0 ? " (into the battery)"
                                      : "");
    }
    v = cJSON_GetObjectItemCaseSensitive(bat, "capacity_percent");
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 100) {
        snprintf(b->value[BAT_LEVEL], sizeof(b->value[0]), "%d %%", (int)v->valuedouble);
    } else {
        copy(b->value[BAT_LEVEL], sizeof(b->value[0]), "Unknown");
    }
    v = cJSON_GetObjectItemCaseSensitive(bat, "age_s");
    if (cJSON_IsNumber(v)) {
        snprintf(b->value[BAT_UPDATED], sizeof(b->value[0]), "%d s ago", (int)v->valuedouble);
    }
}

/* One part of a compact line, after SEP when the line has one already. */
static void battery_part(char *out, size_t n, const char *prefix, const char *value)
{
    size_t len = strlen(out);

    if (len + 1 < n) {
        snprintf(out + len, n - len, "%s%s%s", len ? SEP : "", prefix, value);
    }
}

void sv_battery_compact(const struct sv_battery *b, char *first, size_t first_n, char *second, size_t second_n)
{
    char current[sizeof(b->value[0])];
    char *paren;

    copy(first, first_n, b->value[BAT_STATUS]);
    copy(second, second_n, "");
    if (strcmp(b->value[BAT_VOLTAGE], "-") != 0) {
        battery_part(first, first_n, "", b->value[BAT_VOLTAGE]);
    }
    if (strcmp(b->value[BAT_CURRENT], "-") != 0) {
        /* The signed amps alone: the status beside them says which way. */
        copy(current, sizeof(current), b->value[BAT_CURRENT]);
        paren = strstr(current, " (");
        if (paren) {
            *paren = '\0';
        }
        battery_part(first, first_n, "", current);
    }
    if (strcmp(b->value[BAT_LEVEL], "-") != 0) {
        battery_part(second, second_n, "level ",
                     strcmp(b->value[BAT_LEVEL], "Unknown") == 0 ? "unknown" : b->value[BAT_LEVEL]);
    }
    if (strcmp(b->value[BAT_UPDATED], "-") != 0) {
        battery_part(second, second_n, "updated ", b->value[BAT_UPDATED]);
    }
    if (second[0] >= 'a' && second[0] <= 'z') {
        second[0] = (char)(second[0] - 'a' + 'A');
    }
}

/* ---- Companion ----------------------------------------------------------- */

/* A phone's name is what the phone said at pairing: drawn only as printable
 * ASCII and the Latin-1 letters the product fonts carry (two bytes of UTF-8,
 * U+00A0 to U+00FF); anything else is a '?'. */
static void phone_name(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    size_t i = 0;

    while (src && src[i]) {
        unsigned char ch = (unsigned char)src[i];
        unsigned char next = (unsigned char)src[i + 1];
        int latin1 = (ch == 0xc2 && next >= 0xa0 && next <= 0xbf) || (ch == 0xc3 && next >= 0x80 && next <= 0xbf);

        /* Room for this character whole, and the end. */
        if (o + (latin1 ? 2u : 1u) >= n) {
            break;
        }
        if (ch >= 0x20 && ch < 0x7f) {
            dst[o++] = (char)ch;
            i++;
        } else if (latin1) {
            dst[o++] = (char)ch;
            dst[o++] = (char)next;
            i += 2;
        } else {
            dst[o++] = '?';
            i++;
            /* The rest of one character is one '?', not one each. */
            while (((unsigned char)src[i] & 0xc0) == 0x80) {
                i++;
            }
        }
    }
    dst[o] = '\0';
    /* No name, or one of spaces alone: a row needs a title. */
    if (strspn(dst, " ") == o) {
        copy(dst, n, "Unnamed phone");
    }
}

static int is_key(const char *k)
{
    size_t i;

    if (!k || strlen(k) != SV_PHONE_KEY - 1) {
        return 0;
    }
    for (i = 0; k[i]; i++) {
        if (!((k[i] >= '0' && k[i] <= '9') || (k[i] >= 'a' && k[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

/* The first eight bytes of a key in fours and capitals, as a phone shows
 * "This phone's key" (Fingerprint.display in the app): "1A2B 3C4D 5E6F 7A8B". */
static void key_groups(const char *key, char *out, size_t n)
{
    size_t o = 0;
    int i;

    if (n == 0) {
        return;
    }
    for (i = 0; key && i < 16 && key[i] && o + 2 < n; i++) {
        char ch = key[i];

        if (i > 0 && i % 4 == 0) {
            out[o++] = ' ';
        }
        out[o++] = ch >= 'a' && ch <= 'f' ? (char)(ch - 'a' + 'A') : ch;
    }
    out[o] = '\0';
}

static int holds(const cJSON *scopes, const char *scope)
{
    const cJSON *el;

    cJSON_ArrayForEach(el, scopes) {
        if (cJSON_IsString(el) && strcmp(el->valuestring, scope) == 0) {
            return 1;
        }
    }
    return 0;
}

void sv_companion_apply(struct sv_companion *c, const cJSON *status, const cJSON *clients)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(clients, "clients");
    const cJSON *el;
    const char *why;
    int allowed = 0;
    int i;

    memset(c, 0, sizeof(*c));
    if (!cJSON_IsObject(status) || !cJSON_IsArray(list)) {
        copy(c->headline, sizeof(c->headline), "Not running");
        c->tone = SV_TONE_MUTED;
        copy(c->note, sizeof(c->note), "The Companion gateway is not running on this unit, so no phone can connect.");
        copy(c->summary, sizeof(c->summary), "Not running");
        return;
    }
    c->reachable = 1;
    cJSON_ArrayForEach(el, list) {
        struct sv_phone *p = &c->phones[c->count];
        const char *key = str(el, "fingerprint");
        size_t o = 0;

        if (c->count >= SV_MAX_PHONES) {
            break;
        }
        if (!is_key(key)) {
            continue;
        }
        copy(p->key, sizeof(p->key), key);
        phone_name(p->name, sizeof(p->name), str(el, "name"));
        p->connected = flag(el, "connected");
        p->vision = holds(cJSON_GetObjectItemCaseSensitive(el, "scopes"), "vision");
        p->scopes = sv_scopes_from_json(cJSON_GetObjectItemCaseSensitive(el, "scopes"));
        sv_scopes_words(p->scopes, p->perms, sizeof(p->perms));
        {
            const cJSON *w = cJSON_GetObjectItemCaseSensitive(el, "paired_wall_s");

            p->paired_wall_s = cJSON_IsNumber(w) && w->valuedouble > 0 ? (long long)w->valuedouble : 0;
        }
        key_groups(key, p->key_show, sizeof(p->key_show));
        /* The first eight bytes in fours and capitals, as the phone shows
         * "This phone's key" (Fingerprint.display in the app). */
        o += (size_t)snprintf(p->detail + o, sizeof(p->detail) - o, "Key");
        for (i = 0; i < 16; i++) {
            char ch = key[i];

            if (i % 4 == 0) {
                p->detail[o++] = ' ';
            }
            p->detail[o++] = ch >= 'a' && ch <= 'f' ? (char)(ch - 'a' + 'A') : ch;
        }
        snprintf(p->detail + o, sizeof(p->detail) - o, "%s", p->connected ? SEP "connected" : "");
        allowed += p->vision;
        c->count++;
    }
    c->listening = flag(status, "listening");
    c->max = num(clients, "max", SV_MAX_PHONES);
    copy(c->closed, sizeof(c->closed), str(status, "closed"));
    if (is_key(str(status, "device_id"))) {
        key_groups(str(status, "device_id"), c->device_key, sizeof(c->device_key));
    }
    if (flag(status, "listening")) {
        const char *addr = str(status, "address");

        if (addr) {
            snprintf(c->headline, sizeof(c->headline), "On" SEP "%.40s:%d", addr, num(status, "port", 0));
        } else {
            copy(c->headline, sizeof(c->headline), "On");
        }
        c->tone = SV_TONE_OK;
    } else {
        why = str(status, "closed");
        c->tone = SV_TONE_MUTED;
        if (why && strcmp(why, "no_clients") == 0) {
            copy(c->headline, sizeof(c->headline), "Off: no phone is paired");
        } else if (why && strcmp(why, "password_required") == 0) {
            copy(c->headline, sizeof(c->headline), "Off: this unit has no password set");
        } else if (why && strcmp(why, "no_address") == 0) {
            copy(c->headline, sizeof(c->headline), "Off: no network");
        } else if (why && strcmp(why, "public_address") == 0) {
            copy(c->headline, sizeof(c->headline), "Off: not on a private network");
        } else if (why && (strcmp(why, "state_fault") == 0 || strcmp(why, "identity_fault") == 0)) {
            copy(c->headline, sizeof(c->headline), "Fault: its stored state cannot be used");
            c->tone = SV_TONE_ERROR;
        } else {
            copy(c->headline, sizeof(c->headline), "Off");
        }
    }
    if (c->count == 0) {
        copy(c->note, sizeof(c->note), "No phone is paired with this unit.");
        copy(c->summary, sizeof(c->summary), "No phone paired");
    } else if (allowed == 0) {
        snprintf(c->summary, sizeof(c->summary), "%d phone%s" SEP "Vision not shown to any", c->count,
                 c->count == 1 ? "" : "s");
    } else {
        snprintf(c->summary, sizeof(c->summary), "%d phone%s" SEP "Vision shown to %d", c->count,
                 c->count == 1 ? "" : "s", allowed);
    }
}

cJSON *sv_companion_vision_params(const struct sv_phone *p, int on)
{
    cJSON *o;

    if (!p || !is_key(p->key)) {
        return NULL;
    }
    o = cJSON_CreateObject();
    if (o) {
        cJSON_AddStringToObject(o, "client", p->key);
        cJSON_AddStringToObject(o, "scope", "vision");
        cJSON_AddBoolToObject(o, "on", on != 0);
    }
    return o;
}

void sv_companion_signature(const struct sv_companion *c, char *out, size_t n)
{
    size_t o = 0;
    int i;

    if (n == 0) {
        return;
    }
    o += (size_t)snprintf(out, n, "%d:%d", c->reachable, c->count);
    for (i = 0; i < c->count && o < n; i++) {
        o += (size_t)snprintf(out + o, n - o, "|%s/%s", c->phones[i].key, c->phones[i].name);
    }
}

/* ---- Companion 2.0: the switch, the password, the permissions, pairing ------ *
 *
 * ADR-019 amendment 2. sysd's companion.status says whether the gateway is
 * installed and switched on, whether root has a password, what SSH allows,
 * and how the last change it made went; gatewayd's gateway.status says why
 * it does or does not listen. Every sentence a person reads about it is made
 * here, so tests/settings_view_test.c can hold each one to what it promises:
 * what happened, and what to do. */

const struct sv_scope_info sv_scope_table[SV_SCOPES] = {
    { "status", SV_SCOPE_STATUS, "Device status", "Battery, Wi-Fi, radio and mesh service state.", 0 },
    { "mesh_read", SV_SCOPE_MESH_READ, "Read RIFT", "Nodes, channels, messages and activity.", 0 },
    { "mesh_send", SV_SCOPE_MESH_SEND, "Send RIFT messages", "Sends as this node, on the air. Needs Read RIFT.",
      SV_SCOPE_MESH_READ },
    { "capture_view", SV_SCOPE_CAPTURE_VIEW, "PacketShark figures", "Capture counters only, never packets.", 0 },
    { "capture_control", SV_SCOPE_CAPTURE_CONTROL, "PacketShark control",
      "HOLD and STOP; START still needs a tap here. Needs PacketShark figures.", SV_SCOPE_CAPTURE_VIEW },
    { "vision", SV_SCOPE_VISION, "Vision picture",
      "The camera's picture whenever Vision is open here. Vision names the phone and has STOP.", 0 },
};

/* Short words for the line under a phone, in the table's order. */
static const char *const scope_words[SV_SCOPES] = { "status", "RIFT", "sending", "PacketShark", "control", "Vision" };

unsigned sv_scopes_from_json(const cJSON *list)
{
    const cJSON *el;
    unsigned s = 0;
    int i;

    cJSON_ArrayForEach(el, list) {
        for (i = 0; cJSON_IsString(el) && i < SV_SCOPES; i++) {
            if (strcmp(el->valuestring, sv_scope_table[i].name) == 0) {
                s |= sv_scope_table[i].bit;
            }
        }
    }
    return s;
}

cJSON *sv_scopes_json(unsigned scopes)
{
    cJSON *a = cJSON_CreateArray();
    int i;

    for (i = 0; a && i < SV_SCOPES; i++) {
        if (scopes & sv_scope_table[i].bit) {
            cJSON_AddItemToArray(a, cJSON_CreateString(sv_scope_table[i].name));
        }
    }
    return a;
}

unsigned sv_scope_toggle(unsigned scopes, unsigned bit)
{
    int i;

    if (scopes & bit) {
        scopes &= ~bit;
        /* What needs it goes with it. */
        for (i = 0; i < SV_SCOPES; i++) {
            if (sv_scope_table[i].needs & bit) {
                scopes &= ~sv_scope_table[i].bit;
            }
        }
    } else {
        scopes |= bit;
        /* And what it needs comes with it. */
        for (i = 0; i < SV_SCOPES; i++) {
            if (sv_scope_table[i].bit == bit) {
                scopes |= sv_scope_table[i].needs;
            }
        }
    }
    return scopes;
}

void sv_scopes_words(unsigned scopes, char *out, size_t n)
{
    size_t o = 0;
    int i;

    if (n == 0) {
        return;
    }
    out[0] = '\0';
    for (i = 0; i < SV_SCOPES; i++) {
        if ((scopes & sv_scope_table[i].bit) && o < n) {
            o += (size_t)snprintf(out + o, n - o, "%s%s", o ? ", " : "", scope_words[i]);
        }
    }
    if (!out[0]) {
        copy(out, n, "nothing");
    }
    /* A capital for the start of the line. */
    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] = (char)(out[0] - 'a' + 'A');
    }
}

cJSON *sv_companion_set_scopes_params(const struct sv_phone *p, unsigned scopes)
{
    cJSON *o;

    if (!p || !is_key(p->key)) {
        return NULL;
    }
    o = cJSON_CreateObject();
    if (o) {
        cJSON_AddStringToObject(o, "client", p->key);
        cJSON_AddItemToObject(o, "scopes", sv_scopes_json(scopes));
    }
    return o;
}

cJSON *sv_companion_revoke_params(const struct sv_phone *p)
{
    cJSON *o;

    if (!p || !is_key(p->key)) {
        return NULL;
    }
    o = cJSON_CreateObject();
    if (o) {
        cJSON_AddStringToObject(o, "client", p->key);
    }
    return o;
}

const struct sv_phone *sv_companion_find(const struct sv_companion *c, const char *key)
{
    int i;

    for (i = 0; key && key[0] && i < c->count; i++) {
        if (strcmp(c->phones[i].key, key) == 0) {
            return &c->phones[i];
        }
    }
    return NULL;
}

void sv_phone_paired_text(const struct sv_phone *p, char *out, size_t n)
{
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    time_t t;
    struct tm tm;

    if (!p || p->paired_wall_s <= 0) {
        copy(out, n, "");
        return;
    }
    t = (time_t)p->paired_wall_s;
    if (!localtime_r(&t, &tm)) {
        copy(out, n, "");
        return;
    }
    snprintf(out, n, "Paired %d %s %d", tm.tm_mday, months[tm.tm_mon % 12], tm.tm_year + 1900);
}

/* ---- the words for a refusal ---------------------------------------------------- */

static const struct {
    const char *word;
    const char *text;
} error_texts[] = {
    { "not_installed", "This DOORS image has no Companion gateway. Update DOORS to use DOORS Companion." },
    { "password_required", "Set a device password first (below). The gateway never runs on a unit without one." },
    { "ssh_unknown", "SSH's settings cannot be checked here: /etc/ssh/sshd_config includes other files. "
                     "Nothing was changed. Simplify that file, then try again." },
    { "ssh_failed", "SSH could not be set to accept root with a key only, so nothing was changed. "
                    "Restart the unit, then try again." },
    { "write_failed", "A setting could not be saved: the card may be full or read-only. Nothing was changed." },
    { "start_failed", "The gateway did not start, so it is off again. Try again; if it keeps failing, restart "
                      "the unit (companion.log in the log folder says why)." },
    { "no_answer", "The gateway started but did not answer in time, so it is off again. Try again; restart the "
                   "unit if it keeps happening." },
    { "stop_failed", "The gateway did not stop. It will not start again when the unit restarts: restart it now "
                     "to be sure it is off." },
    { "busy", "Another change is still running. Wait a moment, then try again." },
    { "invalid_password", "That password cannot be used." },
    { "password_layout", "This unit keeps root's password in an unusual way, so it cannot be set here. Nothing "
                         "was changed." },
    { "password_failed", "The password could not be saved, so it is unchanged. The card may be full or "
                         "read-only." },
    /* gatewayd's own refusals of a pairing window */
    { "client list full", "8 phones are paired, the most there can be. Remove one first." },
    { "no_address", "This unit has no Wi-Fi address. Connect to Wi-Fi in Settings > Network, then try again." },
    { "public_address", "This network is not a private one (home, office or phone hotspot), and the gateway only "
                        "listens on private networks." },
    { "identity_fault", "The gateway's stored key cannot be used, so it stays closed. Nothing was changed." },
    { "state_fault", "The gateway's list of phones cannot be read, so it stays closed. Nothing was changed." },
    { "gateway state fault", "The gateway's stored state cannot be used, so it stays closed. Nothing was changed." },
    { "listen_failed", "The gateway could not open its port (TCP 47231). Restart the unit, then try again." },
    { "a pairing window is open", "A pairing code is already showing. Wait two minutes, then try again." },
};

const char *sv_companion_error_text(const char *word, const char *fallback)
{
    size_t i;

    for (i = 0; word && i < sizeof(error_texts) / sizeof(error_texts[0]); i++) {
        if (strcmp(error_texts[i].word, word) == 0) {
            return error_texts[i].text;
        }
    }
    return fallback;
}

void sv_companion_error_word(const char *message, char *out, size_t n)
{
    size_t len = message ? strcspn(message, ":") : 0;

    if (n == 0) {
        return;
    }
    if (!message || !message[len]) {
        /* No word in front: the whole message is the word (gatewayd's). */
        copy(out, n, message);
        return;
    }
    snprintf(out, n, "%.*s", (int)len, message);
}

/* ---- sysd folded in ------------------------------------------------------------ */

static int is(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

void sv_companion_system(struct sv_companion *c, const cJSON *sys)
{
    const char *why = c->closed;
    int failed;

    c->hint[0] = '\0';
    c->hint_tone = SV_TONE_MUTED;
    c->pw_note[0] = '\0';
    c->pw_note_tone = SV_TONE_MUTED;
    c->sys_known = cJSON_IsObject(sys);
    if (!c->sys_known) {
        /* An older sysd, or none: the gateway's own words stand, and there
         * is no switch to offer. */
        c->switch_can = 0;
        c->switch_on = c->reachable;
        copy(c->switch_label, sizeof(c->switch_label), c->reachable ? "ON" : "OFF");
        copy(c->hint, sizeof(c->hint), "The system service does not answer, so the gateway cannot be switched here.");
        c->hint_tone = SV_TONE_WARN;
        copy(c->pw_headline, sizeof(c->pw_headline), "Unknown");
        c->pw_tone = SV_TONE_MUTED;
        c->pw_ssh[0] = '\0';
        copy(c->pw_button, sizeof(c->pw_button), "SET PASSWORD");
        c->pw_can = 0;
        c->pair_can = 0;
        copy(c->pair_hint, sizeof(c->pair_hint), "Pairing needs the gateway on.");
        return;
    }
    c->installed = flag(sys, "installed");
    c->enabled = flag(sys, "enabled");
    c->password_set = flag(sys, "password_set");
    copy(c->ssh, sizeof(c->ssh), str(sys, "ssh"));
    copy(c->job, sizeof(c->job), str(sys, "action"));
    copy(c->job_state, sizeof(c->job_state), str(sys, "state"));
    copy(c->job_error, sizeof(c->job_error), str(sys, "error"));
    c->busy = is(c->job_state, "running");
    failed = is(c->job_state, "failed");

    /* ---- the switch and the gateway's headline ---- */
    c->switch_on = c->enabled;
    c->switch_can = !c->busy && c->installed && (c->enabled || c->password_set);
    copy(c->switch_label, sizeof(c->switch_label), c->enabled ? "ON" : "OFF");
    if (c->busy && is(c->job, "enable")) {
        copy(c->switch_label, sizeof(c->switch_label), "STARTING");
        c->switch_on = 1;
        copy(c->headline, sizeof(c->headline), "Turning on...");
        c->tone = SV_TONE_NORMAL;
    } else if (c->busy && is(c->job, "disable")) {
        copy(c->switch_label, sizeof(c->switch_label), "STOPPING");
        c->switch_on = 0;
        copy(c->headline, sizeof(c->headline), "Turning off...");
        c->tone = SV_TONE_NORMAL;
    } else if (!c->installed) {
        copy(c->headline, sizeof(c->headline), "Not installed on this unit");
        c->tone = SV_TONE_MUTED;
        copy(c->hint, sizeof(c->hint), sv_companion_error_text("not_installed", ""));
    } else if (!c->enabled) {
        copy(c->headline, sizeof(c->headline), "Off");
        c->tone = SV_TONE_MUTED;
        if (!c->password_set) {
            copy(c->hint, sizeof(c->hint), "Set a device password below to turn the gateway on.");
            c->hint_tone = SV_TONE_WARN;
        } else {
            copy(c->hint, sizeof(c->hint), "No phone can reach this unit while it is off.");
        }
    } else if (!c->reachable) {
        copy(c->headline, sizeof(c->headline), "On, but not answering");
        c->tone = SV_TONE_ERROR;
        copy(c->hint, sizeof(c->hint),
             "The gateway is switched on but does not answer. Turn it off and on again; restart the unit if that "
             "does not help.");
        c->hint_tone = SV_TONE_ERROR;
    } else if (c->listening) {
        /* apply()'s "On · <address>:<port>" stands. */
        copy(c->hint, sizeof(c->hint), "Paired phones on this network can connect.");
    } else if (is(why, "no_clients")) {
        copy(c->headline, sizeof(c->headline), "On: waiting for a phone to be paired");
        c->tone = SV_TONE_NORMAL;
        copy(c->hint, sizeof(c->hint), "Nothing listens until a phone is paired. Pair one below.");
    } else if (is(why, "password_required")) {
        copy(c->headline, sizeof(c->headline), "On, but closed: no device password");
        c->tone = SV_TONE_WARN;
        copy(c->hint, sizeof(c->hint), "Set a device password below; the gateway opens by itself once it is set.");
        c->hint_tone = SV_TONE_WARN;
    } else if (is(why, "no_address")) {
        copy(c->headline, sizeof(c->headline), "On, but closed: no Wi-Fi address");
        c->tone = SV_TONE_WARN;
        copy(c->hint, sizeof(c->hint), "Connect to Wi-Fi in Settings > Network; the gateway opens by itself.");
        c->hint_tone = SV_TONE_WARN;
    } else if (is(why, "public_address")) {
        copy(c->headline, sizeof(c->headline), "On, but closed: not a private network");
        c->tone = SV_TONE_WARN;
        copy(c->hint, sizeof(c->hint), sv_companion_error_text("public_address", ""));
        c->hint_tone = SV_TONE_WARN;
    } else if (is(why, "identity_fault") || is(why, "state_fault")) {
        copy(c->headline, sizeof(c->headline), "Fault: its stored state cannot be used");
        c->tone = SV_TONE_ERROR;
        copy(c->hint, sizeof(c->hint),
             is(why, "identity_fault")
                 ? "The gateway's stored key is missing or damaged, so it stays closed and nothing was replaced. "
                   "Its files are in /var/lib/pocketos/gatewayd (docs/services/GATEWAYD.md, State)."
                 : "The gateway's list of phones is damaged, so it stays closed and nothing was replaced. Its "
                   "files are in /var/lib/pocketos/gatewayd (docs/services/GATEWAYD.md, State).");
        c->hint_tone = SV_TONE_ERROR;
    } else {
        copy(c->headline, sizeof(c->headline), "On, but closed");
        c->tone = SV_TONE_WARN;
        copy(c->hint, sizeof(c->hint), sv_companion_error_text(why, "The gateway could not open; it tries again."));
        c->hint_tone = SV_TONE_WARN;
    }
    /* A switch change that failed says so over whatever the state says. */
    if (failed && (is(c->job, "enable") || is(c->job, "disable"))) {
        copy(c->hint, sizeof(c->hint),
             sv_companion_error_text(c->job_error, "The change did not finish. Try again; restart the unit if it "
                                                   "keeps failing."));
        c->hint_tone = SV_TONE_ERROR;
    }

    /* ---- the device password ---- */
    if (c->busy && is(c->job, "password")) {
        copy(c->pw_headline, sizeof(c->pw_headline), "Setting the password...");
        c->pw_tone = SV_TONE_NORMAL;
    } else if (c->password_set) {
        copy(c->pw_headline, sizeof(c->pw_headline), "Set");
        c->pw_tone = SV_TONE_OK;
    } else {
        copy(c->pw_headline, sizeof(c->pw_headline), "Not set");
        c->pw_tone = SV_TONE_WARN;
    }
    copy(c->pw_button, sizeof(c->pw_button), c->password_set ? "CHANGE PASSWORD" : "SET PASSWORD");
    c->pw_can = !c->busy && !is(c->ssh, "unknown");
    c->pw_ssh_tone = SV_TONE_MUTED;
    if (is(c->ssh, "hardened")) {
        copy(c->pw_ssh, sizeof(c->pw_ssh), "SSH: root signs in with a key only.");
    } else if (is(c->ssh, "open")) {
        copy(c->pw_ssh, sizeof(c->pw_ssh),
             "SSH: root can still sign in with a password. Setting the password here, or turning the gateway on, "
             "changes that to a key only.");
        c->pw_ssh_tone = SV_TONE_WARN;
    } else if (is(c->ssh, "absent")) {
        copy(c->pw_ssh, sizeof(c->pw_ssh), "SSH: no SSH server on this unit.");
    } else {
        copy(c->pw_ssh, sizeof(c->pw_ssh), sv_companion_error_text("ssh_unknown", ""));
        c->pw_ssh_tone = SV_TONE_ERROR;
    }
    if (is(c->job, "password") && failed) {
        copy(c->pw_note, sizeof(c->pw_note),
             sv_companion_error_text(c->job_error, "The password was not set. Try again."));
        c->pw_note_tone = SV_TONE_ERROR;
    } else if (is(c->job, "password") && is(c->job_state, "done")) {
        copy(c->pw_note, sizeof(c->pw_note), "Password set. The serial console asks for it from now on.");
        c->pw_note_tone = SV_TONE_OK;
    }

    /* ---- pairing ---- */
    c->pair_can = 0;
    if (!c->enabled || c->busy) {
        copy(c->pair_hint, sizeof(c->pair_hint), "Turn the gateway on to pair a phone.");
    } else if (!c->reachable) {
        copy(c->pair_hint, sizeof(c->pair_hint), "The gateway does not answer.");
    } else if (!c->password_set) {
        copy(c->pair_hint, sizeof(c->pair_hint), "Set a device password first.");
    } else if (c->count >= (c->max > 0 ? c->max : SV_MAX_PHONES)) {
        copy(c->pair_hint, sizeof(c->pair_hint), "8 phones are paired, the most there can be. Remove one first.");
    } else if (is(why, "identity_fault") || is(why, "state_fault")) {
        copy(c->pair_hint, sizeof(c->pair_hint), "Not while the gateway's stored state cannot be used.");
    } else {
        c->pair_can = 1;
        c->pair_hint[0] = '\0';
    }

    /* The list is gatewayd's: with it off there is none to show. */
    if (!c->reachable) {
        copy(c->note, sizeof(c->note),
             c->enabled && !c->busy ? "The gateway does not answer, so its phones cannot be listed."
                                    : "The paired phones are listed while the gateway is on.");
    }

    /* ---- the list's line ---- */
    if (!c->installed) {
        copy(c->summary, sizeof(c->summary), "Not installed");
    } else if (!c->enabled) {
        copy(c->summary, sizeof(c->summary), c->password_set ? "Off" : "Off" SEP "no device password");
    } else if (c->count == 0) {
        copy(c->summary, sizeof(c->summary), "On" SEP "no phone paired");
    } else {
        int on = 0;
        int i;

        for (i = 0; i < c->count; i++) {
            on += c->phones[i].connected;
        }
        snprintf(c->summary, sizeof(c->summary), "On" SEP "%d phone%s paired%s", c->count, c->count == 1 ? "" : "s",
                 on ? SEP "connected" : "");
    }
}

/* ---- pairing --------------------------------------------------------------------- */

static void countdown(struct sv_pair *p)
{
    long long s = (p->left_ms + 999) / 1000;

    if (s < 0) {
        s = 0;
    }
    snprintf(p->countdown, sizeof(p->countdown), "Closes in %lld:%02lld", s / 60, s % 60);
}

void sv_pair_wipe(struct sv_pair *p)
{
    explicit_bzero(p->uri, sizeof(p->uri));
}

static void pair_say(struct sv_pair *p, enum sv_pair_phase phase, enum sv_tone tone, const char *headline,
                     const char *text)
{
    p->phase = phase;
    p->tone = tone;
    copy(p->headline, sizeof(p->headline), headline);
    copy(p->text, sizeof(p->text), text);
    if (phase == SV_PAIR_DONE || phase == SV_PAIR_CLOSED || phase == SV_PAIR_ERROR) {
        /* The code means nothing any more; the token goes from memory. */
        sv_pair_wipe(p);
    }
}

#define PAIR_SHOW_TEXT                                                                                               \
    "Open DOORS Companion on the phone, choose to pair a unit, and point the camera at the code. The phone must be " \
    "on the same network."

void sv_pair_begin(struct sv_pair *p, const cJSON *res, const char *err)
{
    const char *uri = str(res, "uri");
    char word[48];

    memset(p, 0, sizeof(*p));
    p->scopes = SV_SCOPES_DEFAULT;
    if (!uri || strncmp(uri, "doors-pair:", 11) != 0 || strlen(uri) >= sizeof(p->uri)) {
        sv_companion_error_word(err, word, sizeof(word));
        pair_say(p, SV_PAIR_ERROR, SV_TONE_ERROR, "No pairing code",
                 sv_companion_error_text(word, err && err[0] ? "The gateway refused to open a pairing window."
                                                             : "The gateway did not answer. Try again."));
        return;
    }
    copy(p->uri, sizeof(p->uri), uri);
    p->left_ms = num(res, "expires_in_ms", 120000);
    countdown(p);
    pair_say(p, SV_PAIR_SHOWING, SV_TONE_NORMAL, "Scan with DOORS Companion", PAIR_SHOW_TEXT);
}

void sv_pair_poll(struct sv_pair *p, const cJSON *status)
{
    const cJSON *pr = cJSON_GetObjectItemCaseSensitive(status, "pair");
    const char *state = str(pr, "state");
    const char *closed = str(status, "pair_closed");

    if (!sv_pair_open(p) || !status) {
        return; /* decided, closed or never opened; or a missed poll: as it was */
    }
    if (!cJSON_IsObject(pr)) {
        if (is(closed, "expired")) {
            pair_say(p, SV_PAIR_CLOSED, SV_TONE_WARN, "The code expired",
                     "No phone paired in time. Show a new code to try again.");
        } else if (is(closed, "failed")) {
            pair_say(p, SV_PAIR_CLOSED, SV_TONE_ERROR, "Closed after wrong codes",
                     "A phone presented a wrong code, so the window closed. If that was not your phone, someone "
                     "else on this network tried. Show a new code to try again.");
        } else if (is(closed, "candidate_left")) {
            pair_say(p, SV_PAIR_CLOSED, SV_TONE_WARN, "The phone left",
                     "The phone disconnected before you decided, so it was not paired. Show a new code to try "
                     "again.");
        } else if (is(closed, "paired")) {
            pair_say(p, SV_PAIR_DONE, SV_TONE_OK, "Paired", "The phone can now connect to this unit.");
        } else {
            pair_say(p, SV_PAIR_CLOSED, SV_TONE_WARN, "Pairing closed",
                     "The window closed without a pairing. Show a new code to try again.");
        }
        return;
    }
    {
        const cJSON *l = cJSON_GetObjectItemCaseSensitive(pr, "left_ms");

        if (cJSON_IsNumber(l)) {
            p->left_ms = (long long)l->valuedouble;
            countdown(p);
        }
    }
    if (is(state, "waiting")) {
        const cJSON *cand = cJSON_GetObjectItemCaseSensitive(pr, "candidate");
        const char *key = str(cand, "fingerprint");

        if (!is_key(key)) {
            return;
        }
        /* Another phone than the one confirmed: the confirmation was not for it. */
        if (strcmp(key, p->cand_key) != 0) {
            p->keys_match = 0;
            copy(p->cand_key, sizeof(p->cand_key), key);
            key_groups(key, p->cand_key_show, sizeof(p->cand_key_show));
        }
        phone_name(p->cand_name, sizeof(p->cand_name), str(cand, "name"));
        pair_say(p, SV_PAIR_DECIDE, SV_TONE_NORMAL, "A phone wants to pair",
                 "Check that this key is the one DOORS Companion shows as This phone's key, and allow only a "
                 "phone you know.");
    } else if (is(state, "candidate")) {
        pair_say(p, SV_PAIR_CONNECTING, SV_TONE_NORMAL, "A phone is connecting...",
                 "Waiting for it to present the code.");
    } else {
        pair_say(p, SV_PAIR_SHOWING, SV_TONE_NORMAL, "Scan with DOORS Companion", PAIR_SHOW_TEXT);
    }
}

int sv_pair_open(const struct sv_pair *p)
{
    return p->phase == SV_PAIR_SHOWING || p->phase == SV_PAIR_CONNECTING || p->phase == SV_PAIR_DECIDE;
}

int sv_pair_can_allow(const struct sv_pair *p)
{
    return p->phase == SV_PAIR_DECIDE && p->keys_match && is_key(p->cand_key);
}

cJSON *sv_pair_decide_params(const struct sv_pair *p, int allow)
{
    cJSON *o;

    if (allow && !sv_pair_can_allow(p)) {
        return NULL;
    }
    o = cJSON_CreateObject();
    if (o) {
        cJSON_AddBoolToObject(o, "allow", allow != 0);
        if (allow) {
            cJSON_AddItemToObject(o, "scopes", sv_scopes_json(p->scopes));
        }
    }
    return o;
}

void sv_pair_decided(struct sv_pair *p, int allow, const cJSON *res, const char *err)
{
    char line[2 * SV_TEXT];

    if (!allow) {
        pair_say(p, SV_PAIR_CLOSED, SV_TONE_MUTED, "Refused", "The phone was not paired and cannot connect.");
        return;
    }
    if (res && flag(res, "paired")) {
        snprintf(line, sizeof(line), "%s can now connect. Its permissions and Remove are on its page under "
                                     "Companion.",
                 p->cand_name[0] ? p->cand_name : "The phone");
        pair_say(p, SV_PAIR_DONE, SV_TONE_OK, "Paired", line);
        return;
    }
    if (res) {
        pair_say(p, SV_PAIR_ERROR, SV_TONE_ERROR, "Not paired",
                 "The phone left before the decision reached it, so it was not paired. Show a new code to try "
                 "again.");
        return;
    }
    snprintf(line, sizeof(line), "The gateway could not pair the phone%s%.80s. Nothing was stored; show a new "
                                 "code to try again.",
             err && err[0] ? ": " : "", err && err[0] ? err : "");
    pair_say(p, SV_PAIR_ERROR, SV_TONE_ERROR, "Not paired", line);
}

/* ---- the device password ---------------------------------------------------------- */

int sv_device_password_check(const char *first, const char *second, char *why, size_t n)
{
    size_t len = first ? strlen(first) : 0;
    size_t i;

    if (len == 0) {
        copy(why, n, "Type a password.");
        return -1;
    }
    if (len < 8) {
        copy(why, n, "The password needs at least 8 characters.");
        return -1;
    }
    if (len > 64) {
        copy(why, n, "The password can have at most 64 characters.");
        return -1;
    }
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)first[i];

        if (ch < 0x20 || ch > 0x7e) {
            copy(why, n, "Use letters, digits, spaces and the symbols on the keyboard only.");
            return -1;
        }
    }
    if (!second || strcmp(first, second) != 0) {
        copy(why, n, "The two passwords are not the same.");
        return -1;
    }
    return 0;
}

cJSON *sv_device_password_params(const char *password)
{
    cJSON *o;

    if (!password || !password[0]) {
        return NULL;
    }
    o = cJSON_CreateObject();
    if (o) {
        cJSON_AddStringToObject(o, "password", password);
    }
    return o;
}
