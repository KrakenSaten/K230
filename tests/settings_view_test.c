/*
 * Settings view model (apps/settings/settings_view.c): Wi-Fi states and
 * failures as a person reads them, the network list, what a tap on a network
 * does, the parameters a join sends, passphrase feedback, and brightness
 * stepping.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "settings_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void check_str(const char *name, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want) == 0) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s: got \"%s\", want \"%s\"\n", name, got, want);
        failed++;
    }
}

static cJSON *json(const char *text)
{
    cJSON *j = cJSON_Parse(text);

    if (!j) {
        fprintf(stderr, "bad fixture: %s\n", text);
        exit(2);
    }
    return j;
}

static void status(struct sv_wifi *w, const char *text)
{
    cJSON *j = json(text);

    sv_wifi_apply_status(w, j);
    cJSON_Delete(j);
}

static void networks(struct sv_wifi *w, const char *text)
{
    cJSON *j = json(text);

    sv_wifi_apply_networks(w, j);
    cJSON_Delete(j);
}

#define ST(state, reason, extra) \
    "{\"api_version\":0,\"available\":true,\"enabled\":true,\"state\":\"" state "\",\"reason\":" reason \
    ",\"scanning\":false" extra "}"

int main(void)
{
    struct sv_wifi w;
    struct sv_brightness b;
    struct sv_network n;
    char body[256];
    char why[96];
    cJSON *p;
    char *text;

    /* ---- before anything answers ---------------------------------------------- */
    sv_wifi_init(&w);
    check_str("init: checking", w.headline, "Checking Wi-Fi...");
    check("init: nothing usable", !w.toggle_enabled && !w.can_scan && !w.list_visible);

    /* ---- states ------------------------------------------------------------------- */
    sv_wifi_apply_status(&w, NULL);
    check_str("netd silent", w.headline, "Wi-Fi service is not running");
    check("netd silent: error tone", w.tone == SV_TONE_ERROR);
    check("netd silent: switch unusable", !w.toggle_enabled && !w.reachable);

    status(&w, "{\"available\":false,\"enabled\":false,\"state\":\"unavailable\",\"reason\":\"no_interface\"}");
    check_str("no interface", w.headline, "No Wi-Fi hardware found");
    check("no interface: switch unusable", !w.toggle_enabled);
    check("no interface: no list", !w.list_visible && !w.can_scan);

    status(&w, "{\"available\":false,\"enabled\":true,\"state\":\"unavailable\",\"reason\":\"interface_busy\"}");
    check_str("interface busy", w.headline, "Wi-Fi is in use by another program");
    check("interface busy: warn tone", w.tone == SV_TONE_WARN);
    check("interface busy: can still be turned off", w.toggle_enabled);

    status(&w, "{\"available\":true,\"enabled\":false,\"state\":\"off\",\"reason\":null,\"scanning\":false}");
    check_str("off", w.headline, "Wi-Fi is off");
    check("off: can be turned on", w.toggle_enabled && !w.enabled);
    check("off: no scan, no list", !w.can_scan && !w.list_visible);

    status(&w, ST("starting", "null", ""));
    check_str("starting", w.headline, "Starting Wi-Fi...");
    check("starting: no scan yet", !w.can_scan);

    status(&w, ST("disconnected", "null", ""));
    check_str("disconnected", w.headline, "Not connected");
    check("disconnected: scan and list", w.can_scan && w.list_visible);
    check("disconnected: nothing to disconnect", !w.can_disconnect);

    status(&w, ST("connecting", "null", ",\"ssid\":\"Home\""));
    check_str("connecting names the network", w.headline, "Connecting to Home...");
    check("connecting: can be cancelled", w.can_disconnect);

    status(&w, ST("obtaining_ip", "null", ",\"ssid\":\"Home\""));
    check_str("obtaining an address", w.headline, "Getting an address from Home...");

    status(&w, ST("connected", "null", ",\"ssid\":\"Home\",\"ipv4\":\"192.168.1.20\",\"signal_bars\":3"));
    check_str("connected", w.headline, "Connected to Home");
    check("connected: ok tone", w.tone == SV_TONE_OK);
    check_str("connected: address and signal", w.detail, "192.168.1.20 \xc2\xb7 Signal 3/4");
    check("connected: can disconnect", w.can_disconnect);

    status(&w, ST("connected", "null", ",\"ssid\":\"Home\",\"ipv4\":\"10.0.0.2\",\"signal_bars\":null"));
    check_str("connected without a signal reading", w.detail, "10.0.0.2");

    status(&w, ST("failed", "\"auth_failed\"", ",\"ssid\":\"Home\""));
    check_str("wrong passphrase", w.headline, "Wrong passphrase for Home");
    check("wrong passphrase: error tone", w.tone == SV_TONE_ERROR);
    check("wrong passphrase: the list stays usable", w.list_visible && w.can_scan);
    status(&w, ST("failed", "\"auth_failed\"", ",\"ssid\":null"));
    check_str("wrong passphrase without an ssid", w.headline, "Wrong passphrase for the network");
    check("failed without an ssid: nothing to disconnect", !w.can_disconnect);
    status(&w, ST("failed", "\"not_found\"", ",\"ssid\":\"Cabin\""));
    check_str("not found", w.headline, "Cabin did not answer");
    status(&w, ST("failed", "\"assoc_failed\"", ",\"ssid\":\"Cabin\""));
    check_str("refused", w.headline, "Cabin refused the connection");
    status(&w, ST("failed", "\"timeout\"", ",\"ssid\":\"Cabin\""));
    check_str("timeout", w.headline, "Could not connect to Cabin");
    status(&w, ST("failed", "\"dhcp_failed\"", ",\"ssid\":\"Cabin\""));
    check_str("dhcp failed", w.headline, "Connected to Cabin, but got no address");
    check("dhcp failed: warn tone, not error", w.tone == SV_TONE_WARN);
    check("dhcp failed: can disconnect", w.can_disconnect);
    status(&w, ST("failed", "\"supplicant_failed\"", ""));
    check_str("supplicant failed", w.headline, "Wi-Fi could not be started");
    status(&w, ST("failed", "\"something_new\"", ""));
    check_str("an unknown reason still says failed", w.headline, "Wi-Fi failed");
    status(&w, ST("hyperspace", "null", ""));
    check_str("an unknown state is not a guess", w.headline, "Wi-Fi state unknown");

    status(&w, "{\"available\":true,\"enabled\":true,\"state\":\"disconnected\",\"reason\":null,\"scanning\":true}");
    check("scanning: scan button off", !w.can_scan && w.scanning);

    status(&w, ST("disconnected", "null", ",\"store\":\"damaged\""));
    check("damaged store is mentioned", strstr(w.store_note, "could not be read") != NULL);
    status(&w, ST("disconnected", "null", ",\"store\":\"unwritable\""));
    check("unwritable store is mentioned", strstr(w.store_note, "could not be saved") != NULL);
    status(&w, ST("disconnected", "null", ""));
    check("healthy store says nothing", w.store_note[0] == '\0');

    p = json("[1,2]");
    sv_wifi_apply_status(&w, p);
    cJSON_Delete(p);
    check("a non-object status counts as silence", !w.reachable);

    /* ---- the network list ---------------------------------------------------------------- */
    status(&w, ST("connected", "null", ",\"ssid\":\"Home\",\"ipv4\":\"192.168.1.20\",\"signal_bars\":4"));
    networks(&w, "{\"scanning\":false,\"age_s\":3,\"hidden_count\":0,\"networks\":["
                 "{\"ssid\":\"Home\",\"ssid_hex\":\"486f6d65\",\"signal_bars\":4,\"security\":\"wpa2\","
                 "\"supported\":true,\"needs_passphrase\":true,\"saved\":true,\"connected\":true},"
                 "{\"ssid\":\"Caf\xc3\xa9\",\"ssid_hex\":\"436166c3a9\",\"signal_bars\":3,\"security\":\"wpa2/wpa3\","
                 "\"supported\":true,\"needs_passphrase\":true,\"saved\":true,\"connected\":false},"
                 "{\"ssid\":\"Guest\",\"ssid_hex\":\"4775657374\",\"signal_bars\":2,\"security\":\"open\","
                 "\"supported\":true,\"needs_passphrase\":false,\"saved\":false,\"connected\":false},"
                 "{\"ssid\":\"W3\",\"ssid_hex\":\"5733\",\"signal_bars\":2,\"security\":\"wpa3\","
                 "\"supported\":false,\"needs_passphrase\":true,\"saved\":false,\"connected\":false},"
                 "{\"ssid\":\"New\",\"ssid_hex\":\"4e6577\",\"signal_bars\":1,\"security\":\"wpa2\","
                 "\"supported\":true,\"needs_passphrase\":true,\"saved\":false,\"connected\":false}]}");
    check("list: five rows", w.net_count == 5);
    check_str("list: ssid text", w.nets[1].ssid, "Caf\xc3\xa9");
    check_str("list: detail", w.nets[0].detail, "WPA2 \xc2\xb7 4/4");
    check_str("list: transition label", w.nets[1].detail, "WPA2/3 \xc2\xb7 3/4");
    check_str("list: open label", w.nets[2].detail, "Open \xc2\xb7 2/4");
    check_str("list: unsupported says so", w.nets[3].detail, "WPA3 \xc2\xb7 not supported");
    check_str("list: connected badge", w.nets[0].badge, "CONNECTED");
    check_str("list: saved badge", w.nets[1].badge, "SAVED");
    check_str("list: no badge", w.nets[4].badge, "");
    check("list: nothing to explain", w.list_note[0] == '\0');

    check("tap connected: disconnect/forget", sv_join_kind(&w.nets[0]) == SV_JOIN_CONNECTED);
    check("tap saved: join without typing", sv_join_kind(&w.nets[1]) == SV_JOIN_SAVED);
    check("tap open: warn first", sv_join_kind(&w.nets[2]) == SV_JOIN_OPEN);
    check("tap unsupported: explain", sv_join_kind(&w.nets[3]) == SV_JOIN_UNSUPPORTED);
    check("tap new: ask for a passphrase", sv_join_kind(&w.nets[4]) == SV_JOIN_PASSPHRASE);
    n = w.nets[2];
    n.security[0] = '\0';
    n.needs_passphrase = 0;
    check("a supported network that needs nothing but is not open is not joined blindly",
          sv_join_kind(&n) == SV_JOIN_UNSUPPORTED);

    sv_join_text(&w.nets[2], body, sizeof(body));
    check("open sheet warns about encryption", strstr(body, "not encrypted") != NULL);
    sv_join_text(&w.nets[3], body, sizeof(body));
    check("WPA3 sheet explains the hardware", strstr(body, "WPA3-only") != NULL);
    sv_join_text(&w.nets[4], body, sizeof(body));
    check("new network sheet asks for the passphrase", strstr(body, "Enter the passphrase") != NULL);
    sv_join_text(&w.nets[1], body, sizeof(body));
    check("saved sheet says saved", strstr(body, "Saved network") != NULL);

    /* a missed poll keeps the list */
    sv_wifi_apply_networks(&w, NULL);
    check("a missed networks poll keeps the rows", w.net_count == 5);

    networks(&w, "{\"scanning\":true,\"age_s\":null,\"hidden_count\":0,\"networks\":[]}");
    check_str("first scan in progress", w.list_note, "Scanning...");
    networks(&w, "{\"scanning\":false,\"age_s\":null,\"hidden_count\":0,\"networks\":[]}");
    check_str("never scanned", w.list_note, "Tap Scan to look for networks.");
    networks(&w, "{\"scanning\":false,\"age_s\":1,\"hidden_count\":0,\"networks\":[]}");
    check_str("nothing found", w.list_note, "No networks found.");
    networks(&w, "{\"scanning\":false,\"age_s\":1,\"hidden_count\":2,\"networks\":["
                 "{\"ssid\":\"A\",\"ssid_hex\":\"41\",\"signal_bars\":1,\"security\":\"wpa2\",\"supported\":true,"
                 "\"needs_passphrase\":true}]}");
    check_str("hidden networks are mentioned", w.list_note, "2 hidden networks not shown.");
    {
        char big[8192];
        int o = snprintf(big, sizeof(big), "{\"scanning\":false,\"age_s\":1,\"hidden_count\":0,\"networks\":[");
        int i;

        for (i = 0; i < 20; i++) {
            o += snprintf(big + o, sizeof(big) - (size_t)o,
                          "%s{\"ssid\":\"net%d\",\"ssid_hex\":\"6e6574%02x\",\"signal_bars\":1,"
                          "\"security\":\"wpa2\",\"supported\":true,\"needs_passphrase\":true}",
                          i ? "," : "", i, i);
        }
        snprintf(big + o, sizeof(big) - (size_t)o, "]}");
        networks(&w, big);
        check("the list is bounded", w.net_count == SV_MAX_NETWORKS);
        check_str("and says how many are left out", w.list_note, "8 weaker networks not shown.");
    }
    networks(&w, "{\"scanning\":false,\"age_s\":1,\"hidden_count\":0,\"networks\":["
                 "{\"ssid\":null,\"ssid_hex\":\"41\"},{\"ssid\":\"B\"},"
                 "{\"ssid\":\"C\",\"ssid_hex\":\"43\",\"signal_bars\":2,\"security\":\"wpa2\",\"supported\":true,"
                 "\"needs_passphrase\":true}]}");
    check("malformed entries are skipped", w.net_count == 1 && strcmp(w.nets[0].ssid, "C") == 0);
    {
        /* 32 bytes of UTF-8 fit the row */
        networks(&w, "{\"scanning\":false,\"age_s\":1,\"hidden_count\":0,\"networks\":["
                     "{\"ssid\":\"\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\xc3\xa6\","
                     "\"ssid_hex\":\"c3a6\",\"signal_bars\":2,\"security\":\"wpa2\",\"supported\":true,"
                     "\"needs_passphrase\":true}]}");
        check("a 32-byte UTF-8 SSID is kept whole", strlen(w.nets[0].ssid) == 32);
    }

    /* ---- what a join sends ----------------------------------------------------------------- */
    memset(&n, 0, sizeof(n));
    strcpy(n.ssid, "New");
    strcpy(n.ssid_hex, "4e6577");
    strcpy(n.security, "wpa2");
    n.supported = 1;
    n.needs_passphrase = 1;
    p = sv_connect_params(&n, SV_JOIN_PASSPHRASE, "correct horse");
    text = cJSON_PrintUnformatted(p);
    check_str("new network: ssid_hex and passphrase", text, "{\"ssid_hex\":\"4e6577\",\"passphrase\":\"correct horse\"}");
    free(text);
    cJSON_Delete(p);
    check("new network without a passphrase sends nothing", sv_connect_params(&n, SV_JOIN_PASSPHRASE, NULL) == NULL);
    n.saved = 1;
    p = sv_connect_params(&n, SV_JOIN_SAVED, NULL);
    text = cJSON_PrintUnformatted(p);
    check_str("saved network: no passphrase travels", text, "{\"ssid_hex\":\"4e6577\"}");
    free(text);
    cJSON_Delete(p);
    p = sv_connect_params(&n, SV_JOIN_SAVED, "ignored-anyway");
    text = cJSON_PrintUnformatted(p);
    check("saved network: a stray passphrase is not sent", strstr(text, "ignored") == NULL);
    free(text);
    cJSON_Delete(p);
    p = sv_connect_params(&n, SV_JOIN_OPEN, NULL);
    text = cJSON_PrintUnformatted(p);
    check_str("open network: allow_open, explicitly", text, "{\"ssid_hex\":\"4e6577\",\"allow_open\":true}");
    free(text);
    cJSON_Delete(p);
    check("unsupported joins nothing", sv_connect_params(&n, SV_JOIN_UNSUPPORTED, "x") == NULL);
    check("connected joins nothing", sv_connect_params(&n, SV_JOIN_CONNECTED, NULL) == NULL);
    p = sv_ssid_params(&n);
    text = cJSON_PrintUnformatted(p);
    check_str("forget identifies by exact bytes", text, "{\"ssid_hex\":\"4e6577\"}");
    free(text);
    cJSON_Delete(p);

    /* ---- passphrase feedback ------------------------------------------------------------------ */
    check("8 characters ok", sv_passphrase_check("12345678", why, sizeof(why)) == 0 && why[0] == '\0');
    check("7 too short", sv_passphrase_check("1234567", why, sizeof(why)) == -1 && strstr(why, "At least 8"));
    check("empty too short", sv_passphrase_check("", why, sizeof(why)) == -1);
    check("NULL too short", sv_passphrase_check(NULL, why, sizeof(why)) == -1);
    check("64 too long", sv_passphrase_check("1234567890123456789012345678901234567890123456789012345678901234",
                                             why, sizeof(why)) == -1 && strstr(why, "At most 63"));
    check("non-ASCII explained", sv_passphrase_check("bl\xc3\xa5\xc3\xa6rsyltet\xc3\xb8y", why, sizeof(why)) == -1 &&
                                     strstr(why, "ASCII"));
    check("the message never repeats the passphrase",
          sv_passphrase_check("secretpw\x01", why, sizeof(why)) == -1 && strstr(why, "secretpw") == NULL);

    /* ---- brightness ----------------------------------------------------------------------------- */
    sv_brightness_apply(&b, 60, 10, 100);
    check_str("brightness value", b.value, "60 %");
    check("brightness both ways", b.supported && b.can_down && b.can_up);
    sv_brightness_apply(&b, 10, 10, 100);
    check("at the floor: no down", !b.can_down && b.can_up);
    sv_brightness_apply(&b, 100, 10, 100);
    check("at the ceiling: no up", b.can_down && !b.can_up);
    sv_brightness_apply(&b, -1, 10, 100);
    check("unsupported: nothing to press", !b.supported && !b.can_down && !b.can_up);
    check_str("unsupported: value", b.value, "--");
    check("unsupported: says why", strstr(b.note, "no brightness control") != NULL);
    sv_brightness_apply(&b, 3, 10, 100);
    check("below the floor (set elsewhere): can only go up", !b.can_down && b.can_up);

    check("step up", sv_brightness_step(60, 1, 10, 100, 10) == 70);
    check("step down", sv_brightness_step(60, -1, 10, 100, 10) == 50);
    check("step up at the ceiling stays", sv_brightness_step(100, 1, 10, 100, 10) == 100);
    check("step down at the floor stays", sv_brightness_step(10, -1, 10, 100, 10) == 10);
    check("off-grid up goes to the next step", sv_brightness_step(55, 1, 10, 100, 10) == 60);
    check("off-grid down goes to the step below", sv_brightness_step(55, -1, 10, 100, 10) == 50);
    check("below the floor goes to the floor either way", sv_brightness_step(3, -1, 10, 100, 10) == 10 &&
                                                              sv_brightness_step(0, 1, 10, 100, 10) == 10);
    check("near the ceiling clamps", sv_brightness_step(95, 1, 10, 100, 10) == 100);
    check("a zero step is treated as 10", sv_brightness_step(40, 1, 10, 100, 0) == 50);

    /* ---- rotation ---------------------------------------------------------------------------------- */
    {
        struct sv_rotation r;

        sv_rotation_apply(&r, 0, 1, 0, 0, 0, 0);
        check("automatic, no keyboard, portrait: selected and explained",
              r.selected == 0 && strcmp(r.note, "Portrait: no keyboard detected. Automatic turns to landscape "
                                                "with a keyboard.") == 0);
        sv_rotation_apply(&r, 0, 1, 1, 1, 0, 1);
        check_str("automatic with a keyboard", r.note, "Landscape, because a keyboard is attached.");
        sv_rotation_apply(&r, 2, 1, 0, 1, 1, 0);
        check("landscape chosen while portrait shows: says it is being applied, and what that costs",
              r.selected == 2 &&
                  strcmp(r.note, "Turning to landscape now. The screen goes dark for a moment and Doors opens "
                                 "on the launcher.") == 0);
        sv_rotation_apply(&r, 1, 1, 1, 0, 1, 1);
        check_str("portrait chosen while landscape shows", r.note,
                  "Turning to portrait now. The screen goes dark for a moment and Doors opens on the launcher.");
        /* Automatic, a keyboard just attached: the same note, because the
         * keyboard changes the orientation exactly as a tap does. */
        sv_rotation_apply(&r, 0, 1, 0, 1, 1, 1);
        check("automatic applying after a keyboard was attached", r.selected == 0 &&
              strncmp(r.note, "Turning to landscape now.", 25) == 0);
        sv_rotation_apply(&r, 1, 1, 0, 0, 0, 1);
        check_str("forced portrait, nothing pending", r.note, "Portrait, whatever the keyboard.");
        sv_rotation_apply(&r, 2, 1, 1, 1, 0, 0);
        check_str("forced landscape, nothing pending", r.note, "Landscape, whatever the keyboard.");
        sv_rotation_apply(&r, 0, 0, 0, 0, 0, 0);
        check("an unrecognised stored value is said first",
              strncmp(r.note, "The stored rotation was not recognised, so Automatic is used. Portrait", 70) == 0);
        sv_rotation_apply(&r, 7, 1, 0, 0, 0, 0);
        check("an out-of-range mode selects Automatic", r.selected == 0);
        /* An external display: the screen never turns, the mode sets how the
         * unit is held, and the note says that instead of a turn. */
        sv_rotation_apply(&r, 2, 1, 1, 1, 0, 0);
        sv_rotation_external(&r, 1, 0);
        check_str("external, Landscape: the hold, not the screen", r.note,
                  "HDMI: the screen does not turn. Camera picture turned for the unit held in landscape.");
        sv_rotation_apply(&r, 0, 1, 1, 1, 0, 0);
        sv_rotation_external(&r, 1, 0);
        check_str("external, Automatic without a keyboard", r.note,
                  "HDMI: the screen does not turn. Camera picture turned for the unit held in portrait (no keyboard).");
        sv_rotation_apply(&r, 0, 1, 1, 1, 0, 1);
        sv_rotation_external(&r, 1, 1);
        check("external, Automatic with a keyboard: whole, and the selection kept", r.selected == 0 &&
              strcmp(r.note, "HDMI: the screen does not turn. Camera picture turned for the unit held in landscape "
                             "(keyboard attached).") == 0);
        /* A stored value that was not a mode is still said on HDMI, whole. */
        sv_rotation_apply(&r, 0, 0, 1, 1, 0, 0);
        sv_rotation_external(&r, 0, 0);
        check_str("external, unrecognised stored value: said first, and whole", r.note,
                  "Stored rotation not recognised: Automatic. HDMI: camera picture turned for the unit held in "
                  "portrait.");
        sv_rotation_apply(&r, 0, 0, 1, 1, 0, 1);
        sv_rotation_external(&r, 0, 1);
        check_str("external, unrecognised stored value with a keyboard", r.note,
                  "Stored rotation not recognised: Automatic. HDMI: camera picture turned for the unit held in "
                  "landscape.");
    }
    {
        int pct;
        int ok = 1;

        for (pct = 0; pct <= 100; pct++) {
            int up = sv_brightness_step(pct, 1, 10, 100, 10);
            int down = sv_brightness_step(pct, -1, 10, 100, 10);

            if (up < 10 || up > 100 || down < 10 || down > 100 || (pct >= 10 && pct < 100 && up <= pct) ||
                (pct > 10 && down >= pct)) {
                ok = 0;
            }
        }
        check("every level steps inside the range and always moves", ok);
    }

    check_str("label wpa2", sv_security_label("wpa2"), "WPA2");
    check_str("label unknown", sv_security_label("zzz"), "Unknown");
    check_str("label NULL", sv_security_label(NULL), "Unknown");

    /* ---- Sound (DS §52.3) ---- */
    {
        struct sv_volume v;

        sv_volume_apply(&v, 60, 0, 1, 10, 100);
        check_str("volume value", v.value, "60 %");
        check_str("volume summary", v.summary, "Volume 60 %");
        check("volume: both steps, no note", v.can_down && v.can_up && v.note[0] == '\0' && !v.muted);
        sv_volume_apply(&v, 60, 1, 1, 10, 100);
        check_str("muted summary keeps the level", v.summary, "Muted (volume 60 %)");
        check("muted: says why nothing is heard", strstr(v.note, "Muted") != NULL && v.muted);
        sv_volume_apply(&v, 10, 0, 1, 10, 100);
        check("at the floor only + steps", !v.can_down && v.can_up);
        sv_volume_apply(&v, 100, 0, 1, 10, 100);
        check("at the top only - steps", v.can_down && !v.can_up);
        sv_volume_apply(&v, 40, 0, 0, 10, 100);
        check_str("no sound card: summary", v.summary, "No sound output");
        check("no sound card: the level can still be set, and the note says why nothing plays",
              v.can_down && v.can_up && strstr(v.note, "No sound card") != NULL);
    }

    /* ---- the keyboard base ---- */
    {
        char state[24];
        char summary[112];

        sv_keyboard_text(2, 30, state, sizeof(state), summary, sizeof(summary));
        check_str("base attached: chip", state, "ATTACHED");
        check_str("base attached with its light", summary, "Keyboard base attached \xc2\xb7 light 30 %");
        sv_keyboard_text(2, 0, state, sizeof(state), summary, sizeof(summary));
        check_str("light at 0 is off", summary, "Keyboard base attached \xc2\xb7 light off");
        sv_keyboard_text(2, -1, state, sizeof(state), summary, sizeof(summary));
        check_str("no light to drive: not mentioned", summary, "Keyboard base attached");
        sv_keyboard_text(1, 30, state, sizeof(state), summary, sizeof(summary));
        check("base absent", strcmp(state, "NOT ATTACHED") == 0 && strcmp(summary, "Keyboard base not attached") == 0);
        sv_keyboard_text(0, -1, state, sizeof(state), summary, sizeof(summary));
        check("presence unknown is said as unknown", strcmp(state, "UNKNOWN") == 0);
    }

    /* ---- Power & Sleep ---- */
    {
        struct sv_timer t;
        char line[112];

        sv_timer_apply(&t, 0, 0);
        check("screen never: value Never, - steps, + does not", strcmp(t.value, "Never") == 0 && t.can_down && !t.can_up);
        sv_timer_apply(&t, 0, 30);
        check("screen 30 s: the shortest, only + steps", strcmp(t.value, "30 s") == 0 && !t.can_down && t.can_up);
        sv_timer_apply(&t, 1, 300);
        check("lock 5 min: both", strcmp(t.value, "5 min") == 0 && t.can_down && t.can_up);
        sv_timer_apply(&t, 1, 60);
        check("lock 1 min is the lock's shortest", !t.can_down && t.can_up);
        sv_power_summary(0, 0, line, sizeof(line));
        check_str("summary: nothing set", line, "Screen stays on, no automatic lock");
        sv_power_summary(60, 0, line, sizeof(line));
        check_str("summary: screen only", line, "Screen off after 1 min");
        sv_power_summary(0, 1800, line, sizeof(line));
        check_str("summary: lock only", line, "Lock after 30 min");
        sv_power_summary(30, 300, line, sizeof(line));
        check_str("summary: both", line, "Screen off after 30 s \xc2\xb7 lock after 5 min");
    }

    /* ---- Power & Sleep: the battery, read-only ---------------------------- */
    {
        struct sv_battery b;
        cJSON *st;

        sv_battery_apply(&b, NULL);
        check_str("battery: sysd not answering", b.value[0], "System service not answering");
        check_str("and nothing else shown", b.value[1], "-");
        check_str("battery rows are labelled", b.label[3], "Level");

        st = cJSON_Parse("{\"power\":{\"source\":\"external\",\"battery\":null}}");
        sv_battery_apply(&b, st);
        check_str("battery: none, external power (unit A without the base)", b.value[0],
                  "No battery, external power");
        check_str("no level without a battery", b.value[3], "-");
        cJSON_Delete(st);

        st = cJSON_Parse("{\"power\":{\"source\":\"battery\",\"battery\":{\"present\":true,"
                         "\"capacity_percent\":null,\"status\":\"discharging\",\"voltage_v\":3.787,"
                         "\"current_a\":-0.57,\"reading\":\"ok\",\"age_s\":12,"
                         "\"gauge\":{\"validated\":false,\"soc_percent\":29}}}}");
        sv_battery_apply(&b, st);
        check_str("base gauge: status", b.value[0], "On battery");
        check_str("base gauge: voltage", b.value[1], "3.79 V");
        check_str("base gauge: signed current, said in words", b.value[2],
                  "-0.57 A (out of the battery)");
        check_str("base gauge: level unknown, the unvalidated SOC not used", b.value[3], "Unknown");
        check_str("base gauge: how old", b.value[4], "12 s ago");
        cJSON_Delete(st);

        st = cJSON_Parse("{\"power\":{\"source\":\"external\",\"battery\":{\"present\":true,"
                         "\"capacity_percent\":null,\"status\":\"charging\",\"voltage_v\":4.1,"
                         "\"current_a\":0.8,\"reading\":\"ok\",\"age_s\":3}}}");
        sv_battery_apply(&b, st);
        check_str("charging", b.value[0], "Charging");
        check_str("charging current", b.value[2], "+0.80 A (into the battery)");
        cJSON_Delete(st);

        st = cJSON_Parse("{\"power\":{\"battery\":{\"present\":null,\"capacity_percent\":null,"
                         "\"status\":null,\"voltage_v\":null,\"current_a\":null,"
                         "\"reading\":\"stale\",\"age_s\":400}}}");
        sv_battery_apply(&b, st);
        check_str("stale: said", b.value[0], "No recent reading");
        check_str("stale: no voltage", b.value[1], "-");
        check_str("stale: no current", b.value[2], "-");
        check_str("stale: no level", b.value[3], "-");
        check_str("stale: its age, and why it is not shown", b.value[4], "400 s ago (too old to show)");
        cJSON_Delete(st);

        st = cJSON_Parse("{\"power\":{\"battery\":{\"reading\":\"no-answer\"}}}");
        sv_battery_apply(&b, st);
        check_str("no answer from the gauge", b.value[0], "Gauge not answering");
        cJSON_Delete(st);
        st = cJSON_Parse("{\"power\":{\"battery\":{\"reading\":\"stopped\"}}}");
        sv_battery_apply(&b, st);
        check_str("reading stopped", b.value[0], "Not reading (keyboard protection)");
        cJSON_Delete(st);

        st = cJSON_Parse("{\"power\":{\"battery\":{\"present\":true,\"capacity_percent\":76,"
                         "\"status\":\"charging\",\"reading\":\"ok\"}}}");
        sv_battery_apply(&b, st);
        check_str("a kernel gauge's own percentage is the level", b.value[3], "76 %");
        cJSON_Delete(st);
    }

    /* ---- the battery in two lines, for the wide shape ---------------------- */
    {
        struct sv_battery b;
        char first[160];
        char second[160];
        cJSON *st;

        st = cJSON_Parse("{\"power\":{\"source\":\"battery\",\"battery\":{\"present\":true,"
                         "\"capacity_percent\":null,\"status\":\"discharging\",\"voltage_v\":3.787,"
                         "\"current_a\":-0.57,\"reading\":\"ok\",\"age_s\":12}}}");
        sv_battery_apply(&b, st);
        sv_battery_compact(&b, first, sizeof(first), second, sizeof(second));
        check_str("compact: status, volts, signed amps", first, "On battery \xc2\xb7 3.79 V \xc2\xb7 -0.57 A");
        check_str("compact: level and age", second, "Level unknown \xc2\xb7 updated 12 s ago");
        cJSON_Delete(st);

        st = cJSON_Parse("{\"power\":{\"battery\":{\"present\":true,\"capacity_percent\":76,"
                         "\"status\":\"charging\",\"reading\":\"ok\"}}}");
        sv_battery_apply(&b, st);
        sv_battery_compact(&b, first, sizeof(first), second, sizeof(second));
        check_str("compact: nothing measured left out", first, "Charging");
        check_str("compact: a level, no age", second, "Level 76 %");
        cJSON_Delete(st);

        st = cJSON_Parse("{\"power\":{\"battery\":{\"reading\":\"stale\",\"age_s\":400}}}");
        sv_battery_apply(&b, st);
        sv_battery_compact(&b, first, sizeof(first), second, sizeof(second));
        check_str("compact: stale said", first, "No recent reading");
        check_str("compact: stale age, capitalised", second, "Updated 400 s ago (too old to show)");
        cJSON_Delete(st);

        sv_battery_apply(&b, NULL);
        sv_battery_compact(&b, first, sizeof(first), second, sizeof(second));
        check_str("compact: sysd not answering", first, "System service not answering");
        check_str("compact: second line empty", second, "");
    }

    /* ---- Companion: the paired phones and their Vision switch ----------------- */
    {
        static const char *const K1 = "0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9";
        static const char *const K2 = "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";
        struct sv_companion c;
        cJSON *st;
        cJSON *cl;
        cJSON *p;
        char text[2048];
        char sig[1024];
        char sig2[1024];
        char *out;

        sv_companion_apply(&c, NULL, NULL);
        check("companion: gatewayd not answering is not running, with no phone listed", !c.reachable && c.count == 0);
        check_str("companion: and says so", c.headline, "Not running");
        check_str("companion: its line in the list", c.summary, "Not running");
        check("companion: with a note in place of the rows", strstr(c.note, "not running") != NULL);

        st = json("{\"listening\":false,\"closed\":\"no_clients\",\"clients\":0}");
        cl = json("{\"clients\":[],\"max\":8}");
        sv_companion_apply(&c, st, cl);
        check("companion: running with nobody paired", c.reachable && c.count == 0);
        check_str("companion: closed because nobody is paired", c.headline, "Off: no phone is paired");
        check_str("companion: no phone", c.note, "No phone is paired with this unit.");
        check_str("companion: the list's line", c.summary, "No phone paired");
        /* A status without the list is not a list of nobody. */
        sv_companion_apply(&c, st, NULL);
        check("companion: a missing list is no answer, not an empty one", !c.reachable);
        cJSON_Delete(st);
        cJSON_Delete(cl);

        st = json("{\"listening\":true,\"address\":\"192.168.10.171\",\"port\":47231}");
        snprintf(text, sizeof(text),
                 "{\"clients\":["
                 "{\"fingerprint\":\"%s\",\"name\":\"Andr\xc3\xa9's Flip8\",\"scopes\":[\"status\",\"mesh_read\"],\"connected\":true},"
                 "{\"fingerprint\":\"%s\",\"name\":\"\",\"scopes\":[\"status\",\"vision\"],\"connected\":false},"
                 "{\"fingerprint\":\"nothex\",\"name\":\"bad key\",\"scopes\":[\"vision\"],\"connected\":true},"
                 "{\"fingerprint\":\"%.63sG\",\"name\":\"capital in the key\",\"scopes\":[\"vision\"]},"
                 "{\"name\":\"no key\",\"scopes\":[\"vision\"]}],\"max\":8}",
                 K1, K2, K1);
        cl = json(text);
        sv_companion_apply(&c, st, cl);
        check_str("companion: listening, and where", c.headline, "On \xc2\xb7 192.168.10.171:47231");
        check("companion: in the tone of something that works", c.tone == SV_TONE_OK);
        check("companion: two phones; one without a usable key is never listed", c.count == 2);
        check_str("companion: a name with a Latin-1 letter is kept", c.phones[0].name, "Andr\xc3\xa9's Flip8");
        check_str("companion: the key's first eight bytes, as the phone shows its own", c.phones[0].detail,
                  "Key 0A1B 2C3D 4E5F 6071 \xc2\xb7 connected");
        check("companion: connected, and not shown Vision", c.phones[0].connected && !c.phones[0].vision);
        check_str("companion: a phone that gave no name", c.phones[1].name, "Unnamed phone");
        check_str("companion: not connected says nothing more", c.phones[1].detail, "Key FFEE DDCC BBAA 9988");
        check("companion: and that one is shown Vision", !c.phones[1].connected && c.phones[1].vision);
        check_str("companion: the list's line counts both", c.summary, "2 phones \xc2\xb7 Vision shown to 1");
        check("companion: no note over rows", c.note[0] == '\0');

        p = sv_companion_vision_params(&c.phones[0], 1);
        out = p ? cJSON_PrintUnformatted(p) : NULL;
        snprintf(text, sizeof(text), "{\"client\":\"%s\",\"scope\":\"vision\",\"on\":true}", K1);
        check_str("companion: switching on names the whole key, the scope and true, and nothing else", out ? out : "", text);
        cJSON_free(out);
        cJSON_Delete(p);
        p = sv_companion_vision_params(&c.phones[1], 0);
        out = p ? cJSON_PrintUnformatted(p) : NULL;
        snprintf(text, sizeof(text), "{\"client\":\"%s\",\"scope\":\"vision\",\"on\":false}", K2);
        check_str("companion: switching off the same with false", out ? out : "", text);
        cJSON_free(out);
        cJSON_Delete(p);
        check("companion: nothing is sent for a phone that is not there", sv_companion_vision_params(NULL, 1) == NULL);

        sv_companion_signature(&c, sig, sizeof(sig));
        c.phones[0].vision = 1;
        c.phones[1].connected = 1;
        sv_companion_signature(&c, sig2, sizeof(sig2));
        check("companion: a switch or a connection changing is not another list", strcmp(sig, sig2) == 0);
        c.count = 1;
        sv_companion_signature(&c, sig2, sizeof(sig2));
        check("companion: a phone gone is", strcmp(sig, sig2) != 0);
        cJSON_Delete(st);
        cJSON_Delete(cl);

        /* A name is what a phone said: only what the fonts carry is drawn. */
        st = json("{\"listening\":false,\"closed\":\"password_required\"}");
        snprintf(text, sizeof(text),
                 "{\"clients\":[{\"fingerprint\":\"%s\",\"name\":\"a\\u0007b\xe2\x82\xac" "c\xf0\x9f\x93\xb1\xc3\",\"scopes\":[]}]}", K1);
        cl = json(text);
        sv_companion_apply(&c, st, cl);
        check_str("companion: a control character, a euro sign, an emoji and half a letter are each one '?'",
                  c.count == 1 ? c.phones[0].name : "", "a?b?c??");
        check_str("companion: closed for want of a password", c.headline, "Off: this unit has no password set");
        check_str("companion: one phone, none shown Vision", c.summary, "1 phone \xc2\xb7 Vision not shown to any");
        cJSON_Delete(cl);
        /* The longest name gatewayd keeps, 32 bytes, is drawn whole; one ending in a two-byte letter too. */
        snprintf(text, sizeof(text),
                 "{\"clients\":[{\"fingerprint\":\"%s\",\"name\":\"A phone with a rather long name!\",\"scopes\":[]},"
                 "{\"fingerprint\":\"%s\",\"name\":\"012345678901234567890123456 \xc3\xa9\xc3\xa9\",\"scopes\":[]}]}", K1, K2);
        cl = json(text);
        sv_companion_apply(&c, st, cl);
        check_str("companion: a name of 32 bytes is whole", c.count == 2 ? c.phones[0].name : "", "A phone with a rather long name!");
        cJSON_Delete(cl);
        snprintf(text, sizeof(text), "{\"clients\":[{\"fingerprint\":\"%s\",\"name\":\"   \",\"scopes\":[]},"
                                     "{\"fingerprint\":\"%s\",\"name\":\"012345678901234567890123456 \xc3\xa9\xc3\xa9\",\"scopes\":[]}]}", K1, K2);
        cl = json(text);
        sv_companion_apply(&c, st, cl);
        check_str("companion: a name of spaces alone is no name", c.count == 2 ? c.phones[0].name : "", "Unnamed phone");
        check_str("companion: and one that ends in two-byte letters", c.count == 2 ? c.phones[1].name : "",
                  "012345678901234567890123456 \xc3\xa9\xc3\xa9");
        cJSON_Delete(st);
        cJSON_Delete(cl);

        st = json("{\"listening\":false,\"closed\":\"state_fault\"}");
        cl = json("{\"clients\":[]}");
        sv_companion_apply(&c, st, cl);
        check("companion: a state fault is an error, said", c.tone == SV_TONE_ERROR && strstr(c.headline, "Fault") != NULL);
        cJSON_Delete(st);
        cJSON_Delete(cl);

        /* More than gatewayd pairs: the page's rows end where its arrays do. */
        {
            size_t o = (size_t)snprintf(text, sizeof(text), "{\"clients\":[");
            int i;

            for (i = 0; i < SV_MAX_PHONES + 3; i++) {
                o += (size_t)snprintf(text + o, sizeof(text) - o, "%s{\"fingerprint\":\"%02x%.62s\",\"name\":\"p\",\"scopes\":[]}",
                                      i ? "," : "", i, K1);
            }
            snprintf(text + o, sizeof(text) - o, "]}");
        }
        st = json("{\"listening\":true}");
        cl = json(text);
        sv_companion_apply(&c, st, cl);
        check("companion: never more rows than the page has", cl != NULL && c.count == SV_MAX_PHONES);
        check_str("companion: listening without an address is still on", c.headline, "On");
        cJSON_Delete(st);
        cJSON_Delete(cl);
    }


    /* ---- Companion 2.0: sysd folded in, permissions, pairing, the password (ADR-019 amendment 2) ---- */
    {
#define VK1 "0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9"
#define VK2 "c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
        struct sv_companion c;
        struct sv_pair p;
        cJSON *st;
        cJSON *cl;
        cJSON *sys;
        cJSON *o;
        char *out;
        char text[256];
        unsigned s;

        /* A unit as it comes: off, no password, SSH open. */
        sv_companion_apply(&c, NULL, NULL);
        sys = json("{\"installed\":true,\"enabled\":false,\"running\":false,\"password_set\":false,\"ssh\":\"open\","
                   "\"state\":\"idle\"}");
        sv_companion_system(&c, sys);
        check_str("c2: off", c.headline, "Off");
        check("c2: the switch reads OFF and cannot be tapped without a password",
              !c.switch_on && !c.switch_can && strcmp(c.switch_label, "OFF") == 0);
        check_str("c2: and says what to do", c.hint, "Set a device password below to turn the gateway on.");
        check("c2: as a warning", c.hint_tone == SV_TONE_WARN);
        check_str("c2: no password", c.pw_headline, "Not set");
        check_str("c2: set, not change", c.pw_button, "SET PASSWORD");
        check("c2: the password can be set", c.pw_can);
        check("c2: SSH open is a warning that says what setting a password does",
              c.pw_ssh_tone == SV_TONE_WARN && strstr(c.pw_ssh, "changes that to a key only") != NULL);
        check("c2: no pairing while off", !c.pair_can && strcmp(c.pair_hint, "Turn the gateway on to pair a phone.") == 0);
        check_str("c2: no list while off, and why", c.note, "The paired phones are listed while the gateway is on.");
        check_str("c2: the list's line", c.summary, "Off \xc2\xb7 no device password");
        cJSON_Delete(sys);

        /* Password set, still off: the switch can be tapped. */
        sys = json("{\"installed\":true,\"enabled\":false,\"password_set\":true,\"ssh\":\"hardened\",\"state\":\"idle\"}");
        sv_companion_system(&c, sys);
        check("c2: with a password the switch can be tapped", c.switch_can && !c.switch_on);
        check_str("c2: change, not set", c.pw_button, "CHANGE PASSWORD");
        check_str("c2: SSH hardened", c.pw_ssh, "SSH: root signs in with a key only.");
        check_str("c2: off with a password", c.summary, "Off");
        cJSON_Delete(sys);

        /* Starting. */
        sys = json("{\"installed\":true,\"enabled\":false,\"password_set\":true,\"ssh\":\"hardened\",\"state\":\"running\","
                   "\"action\":\"enable\"}");
        sv_companion_system(&c, sys);
        check("c2: STARTING, ON, not tappable while sysd works",
              strcmp(c.switch_label, "STARTING") == 0 && c.switch_on && !c.switch_can && strcmp(c.headline, "Turning on...") == 0);
        check("c2: nothing else can be started meanwhile", !c.pw_can && !c.pair_can);
        cJSON_Delete(sys);
        sys = json("{\"installed\":true,\"enabled\":true,\"password_set\":true,\"ssh\":\"hardened\",\"state\":\"running\","
                   "\"action\":\"disable\"}");
        sv_companion_system(&c, sys);
        check("c2: STOPPING reads OFF", strcmp(c.switch_label, "STOPPING") == 0 && !c.switch_on);
        cJSON_Delete(sys);

        /* On, listening, two phones. */
        st = json("{\"listening\":true,\"address\":\"192.168.1.20\",\"port\":47231,\"device_id\":\"" VK2 "\"}");
        cl = json("{\"clients\":[{\"fingerprint\":\"" VK1 "\",\"name\":\"Flip8\",\"scopes\":[\"status\",\"mesh_read\","
                  "\"mesh_send\",\"vision\"],\"connected\":true,\"paired_wall_s\":1795521600},"
                  "{\"fingerprint\":\"" VK2 "\",\"name\":\"B\",\"scopes\":[],\"connected\":false}],\"max\":8}");
        sv_companion_apply(&c, st, cl);
        sys = json("{\"installed\":true,\"enabled\":true,\"password_set\":true,\"ssh\":\"hardened\",\"state\":\"done\","
                   "\"action\":\"enable\"}");
        sv_companion_system(&c, sys);
        check_str("c2: on, listening, where", c.headline, "On \xc2\xb7 192.168.1.20:47231");
        check_str("c2: this unit's key as phones show it", c.device_key, "C0C1 C2C3 C4C5 C6C7");
        check("c2: pairing can start", c.pair_can && !c.pair_hint[0]);
        check_str("c2: what a phone may do, in words", c.phones[0].perms, "Status, RIFT, sending, Vision");
        check_str("c2: and one that may do nothing", c.phones[1].perms, "Nothing");
        check("c2: the scopes as bits", c.phones[0].scopes == (SV_SCOPE_STATUS | SV_SCOPE_MESH_READ | SV_SCOPE_MESH_SEND |
                                                               SV_SCOPE_VISION));
        check_str("c2: its key in fours", c.phones[0].key_show, "0A1B 2C3D 4E5F 6071");
        sv_phone_paired_text(&c.phones[0], text, sizeof(text));
        check("c2: when it was paired, by the date", strncmp(text, "Paired ", 7) == 0 && strstr(text, "Nov 2026") != NULL);
        sv_phone_paired_text(&c.phones[1], text, sizeof(text));
        check_str("c2: and nothing when the clock was not set then", text, "");
        check_str("c2: the list's line", c.summary, "On \xc2\xb7 2 phones paired \xc2\xb7 connected");
        check("c2: a phone found by its whole key, and nothing by a prefix",
              sv_companion_find(&c, VK1) == &c.phones[0] && sv_companion_find(&c, "0a1b2c3d") == NULL);
        cJSON_Delete(st);
        cJSON_Delete(sys);

        /* Why it does not listen. */
        {
            static const struct {
                const char *closed;
                const char *headline;
                const char *hint_part;
            } cases[] = {
                { "no_clients", "On: waiting for a phone to be paired", "Pair one below." },
                { "password_required", "On, but closed: no device password", "Set a device password below" },
                { "no_address", "On, but closed: no Wi-Fi address", "Settings > Network" },
                { "public_address", "On, but closed: not a private network", "only listens on private networks" },
                { "identity_fault", "Fault: its stored state cannot be used", "nothing was replaced" },
                { "state_fault", "Fault: its stored state cannot be used", "list of phones is damaged" },
                { "listen_failed", "On, but closed", "TCP 47231" },
            };
            size_t k;

            sys = json("{\"installed\":true,\"enabled\":true,\"password_set\":true,\"ssh\":\"hardened\",\"state\":\"idle\"}");
            for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
                char name[160];

                snprintf(text, sizeof(text), "{\"listening\":false,\"closed\":\"%s\"}", cases[k].closed);
                st = json(text);
                sv_companion_apply(&c, st, cl);
                sv_companion_system(&c, sys);
                snprintf(name, sizeof(name), "c2: closed by %s: said, with what to do", cases[k].closed);
                check(name, strcmp(c.headline, cases[k].headline) == 0 && strstr(c.hint, cases[k].hint_part) != NULL);
                cJSON_Delete(st);
            }
            st = json("{\"listening\":false,\"closed\":\"identity_fault\"}");
            sv_companion_apply(&c, st, cl);
            sv_companion_system(&c, sys);
            check("c2: an identity fault: no pairing, said", !c.pair_can && strstr(c.pair_hint, "stored state") != NULL);
            cJSON_Delete(st);
            cJSON_Delete(sys);
        }
        /* Switched on, gatewayd silent. */
        sv_companion_apply(&c, NULL, NULL);
        sys = json("{\"installed\":true,\"enabled\":true,\"password_set\":true,\"ssh\":\"hardened\",\"state\":\"idle\"}");
        sv_companion_system(&c, sys);
        check("c2: on but silent is an error with what to do",
              c.tone == SV_TONE_ERROR && strcmp(c.headline, "On, but not answering") == 0 &&
                  strstr(c.hint, "Turn it off and on again") != NULL && !c.pair_can);
        check_str("c2: and the list says why it is empty", c.note, "The gateway does not answer, so its phones cannot be listed.");
        cJSON_Delete(sys);
        /* A failed change says so over the state. */
        sys = json("{\"installed\":true,\"enabled\":false,\"password_set\":true,\"ssh\":\"hardened\",\"state\":\"failed\","
                   "\"action\":\"enable\",\"error\":\"no_answer\",\"message\":\"...\"}");
        sv_companion_system(&c, sys);
        check("c2: a failed start: what happened and what to do, as an error",
              c.hint_tone == SV_TONE_ERROR && strstr(c.hint, "did not answer in time, so it is off again") != NULL);
        cJSON_Delete(sys);
        sys = json("{\"installed\":true,\"enabled\":false,\"password_set\":true,\"ssh\":\"open\",\"state\":\"failed\","
                   "\"action\":\"password\",\"error\":\"ssh_failed\"}");
        sv_companion_system(&c, sys);
        check("c2: a failed password: on the password panel, not the gateway's",
              c.pw_note_tone == SV_TONE_ERROR && strstr(c.pw_note, "SSH could not be set") != NULL &&
                  c.hint_tone != SV_TONE_ERROR);
        cJSON_Delete(sys);
        sys = json("{\"installed\":true,\"enabled\":false,\"password_set\":true,\"ssh\":\"unknown\",\"state\":\"idle\"}");
        sv_companion_system(&c, sys);
        check("c2: SSH that cannot be judged stops the password here, said as an error",
              !c.pw_can && c.pw_ssh_tone == SV_TONE_ERROR && strstr(c.pw_ssh, "includes other files") != NULL);
        cJSON_Delete(sys);
        sys = json("{\"installed\":false,\"enabled\":false,\"password_set\":false,\"ssh\":\"open\",\"state\":\"idle\"}");
        sv_companion_system(&c, sys);
        check("c2: not installed: said, and no switch",
              !c.switch_can && strcmp(c.headline, "Not installed on this unit") == 0 && strcmp(c.summary, "Not installed") == 0);
        cJSON_Delete(sys);
        sv_companion_system(&c, NULL);
        check("c2: no sysd: no switch, and the page says why",
              !c.switch_can && !c.pw_can && strstr(c.hint, "system service does not answer") != NULL);
        /* Eight phones: no ninth. */
        {
            char big[4096];
            size_t o = (size_t)snprintf(big, sizeof(big), "{\"clients\":[");
            int i;

            for (i = 0; i < 8; i++) {
                o += (size_t)snprintf(big + o, sizeof(big) - o, "%s{\"fingerprint\":\"%02x%.62s\",\"name\":\"p\",\"scopes\":[]}",
                                      i ? "," : "", i, VK1);
            }
            snprintf(big + o, sizeof(big) - o, "],\"max\":8}");
            st = json("{\"listening\":true}");
            cJSON_Delete(cl);
            cl = json(big);
            sv_companion_apply(&c, st, cl);
            sys = json("{\"installed\":true,\"enabled\":true,\"password_set\":true,\"ssh\":\"hardened\",\"state\":\"idle\"}");
            sv_companion_system(&c, sys);
            check("c2: eight phones: no ninth, and why", !c.pair_can && strstr(c.pair_hint, "Remove one first") != NULL);
            cJSON_Delete(st);
            cJSON_Delete(sys);
        }
        cJSON_Delete(cl);

        /* The words for refusals. */
        sv_companion_error_word("password_required: this unit has no device password; set one first", text, sizeof(text));
        check_str("c2: sysd's word is the part before the colon", text, "password_required");
        sv_companion_error_word("client list full", text, sizeof(text));
        check_str("c2: gatewayd's whole message is its word", text, "client list full");
        check("c2: every word sysd and gatewayd refuse with has a sentence",
              strstr(sv_companion_error_text("not_installed", ""), "Update DOORS") &&
                  strstr(sv_companion_error_text("ssh_failed", ""), "Restart the unit") &&
                  strstr(sv_companion_error_text("start_failed", ""), "companion.log") &&
                  strstr(sv_companion_error_text("stop_failed", ""), "restart it now") &&
                  strstr(sv_companion_error_text("write_failed", ""), "Nothing was changed") &&
                  strstr(sv_companion_error_text("password_layout", ""), "Nothing was changed") &&
                  strstr(sv_companion_error_text("password_failed", ""), "unchanged") &&
                  strstr(sv_companion_error_text("busy", ""), "Wait a moment") &&
                  strstr(sv_companion_error_text("client list full", ""), "Remove one first") &&
                  strstr(sv_companion_error_text("a pairing window is open", ""), "already showing"));
        check_str("c2: an unknown word gives the fallback", sv_companion_error_text("no_such_word", "fallback"), "fallback");

        /* An interrupted activation being undone, and its outcome (fail-closed
         * activation, ADR-019 amendment 2). */
        sv_companion_apply(&c, NULL, NULL);
        sys = json("{\"installed\":true,\"enabled\":false,\"password_set\":true,\"ssh\":\"hardened\",\"state\":\"running\","
                   "\"action\":\"recover\"}");
        sv_companion_system(&c, sys);
        check("c2: undoing an interrupted activation reads STOPPING, OFF, not tappable",
              strcmp(c.switch_label, "STOPPING") == 0 && !c.switch_on && !c.switch_can &&
                  strcmp(c.headline, "Turning off...") == 0);
        cJSON_Delete(sys);
        sys = json("{\"installed\":true,\"enabled\":false,\"password_set\":true,\"ssh\":\"hardened\",\"state\":\"failed\","
                   "\"action\":\"recover\",\"error\":\"activation_interrupted\",\"message\":\"...\"}");
        sv_companion_system(&c, sys);
        check("c2: and then: off, and why, as an error, with what to do",
              c.switch_can && strcmp(c.switch_label, "OFF") == 0 && c.hint_tone == SV_TONE_ERROR &&
                  strstr(c.hint, "did not finish") && strstr(c.hint, "switched off again") && strstr(c.hint, "Turn it on again"));
        cJSON_Delete(sys);

        /* ---- permissions ---- */
        s = sv_scope_toggle(SV_SCOPES_DEFAULT, SV_SCOPE_MESH_READ);
        check("c2: reading off takes sending with it", s == SV_SCOPE_STATUS);
        s = sv_scope_toggle(SV_SCOPE_STATUS, SV_SCOPE_MESH_SEND);
        check("c2: sending on brings reading", s == (SV_SCOPE_STATUS | SV_SCOPE_MESH_READ | SV_SCOPE_MESH_SEND));
        s = sv_scope_toggle(0, SV_SCOPE_CAPTURE_CONTROL);
        check("c2: PacketShark control on brings its figures", s == (SV_SCOPE_CAPTURE_VIEW | SV_SCOPE_CAPTURE_CONTROL));
        s = sv_scope_toggle(s, SV_SCOPE_CAPTURE_VIEW);
        check("c2: figures off takes control with them", s == 0);
        check("c2: control off leaves the figures",
              sv_scope_toggle(SV_SCOPE_CAPTURE_VIEW | SV_SCOPE_CAPTURE_CONTROL, SV_SCOPE_CAPTURE_CONTROL) == SV_SCOPE_CAPTURE_VIEW);
        check("c2: Vision needs nothing and takes nothing", sv_scope_toggle(0, SV_SCOPE_VISION) == SV_SCOPE_VISION &&
                                                                sv_scope_toggle(SV_SCOPES_DEFAULT | SV_SCOPE_VISION, SV_SCOPE_VISION) ==
                                                                    SV_SCOPES_DEFAULT);
        o = sv_scopes_json(SV_SCOPE_VISION | SV_SCOPE_STATUS | SV_SCOPE_CAPTURE_VIEW);
        out = cJSON_PrintUnformatted(o);
        check_str("c2: scopes as gatewayd names them, in its order", out ? out : "", "[\"status\",\"capture_view\",\"vision\"]");
        check("c2: and read back", sv_scopes_from_json(o) == (SV_SCOPE_VISION | SV_SCOPE_STATUS | SV_SCOPE_CAPTURE_VIEW));
        free(out);
        cJSON_Delete(o);
        o = json("[\"status\",\"root\",7,\"vision\"]");
        check("c2: an unknown or a non-string scope is left out", sv_scopes_from_json(o) == (SV_SCOPE_STATUS | SV_SCOPE_VISION));
        cJSON_Delete(o);
        {
            struct sv_phone ph;

            memset(&ph, 0, sizeof(ph));
            snprintf(ph.key, sizeof(ph.key), "%s", VK1);
            o = sv_companion_set_scopes_params(&ph, SV_SCOPE_STATUS | SV_SCOPE_MESH_READ);
            out = cJSON_PrintUnformatted(o);
            check_str("c2: set_scopes names the whole key and the whole set", out ? out : "",
                      "{\"client\":\"" VK1 "\",\"scopes\":[\"status\",\"mesh_read\"]}");
            free(out);
            cJSON_Delete(o);
            o = sv_companion_revoke_params(&ph);
            out = cJSON_PrintUnformatted(o);
            check_str("c2: revoke names the whole key and nothing else", out ? out : "", "{\"client\":\"" VK1 "\"}");
            free(out);
            cJSON_Delete(o);
            snprintf(ph.key, sizeof(ph.key), "0a1b");
            check("c2: nothing is sent for a phone without a whole key",
                  sv_companion_set_scopes_params(&ph, 1) == NULL && sv_companion_revoke_params(&ph) == NULL);
        }

        /* ---- pairing ---- */
        o = json("{\"uri\":\"doors-pair:1?h=192.168.1.20&p=47231&fp=AAAA&t=SECRETTOKEN&n=Doors\",\"expires_in_ms\":120000}");
        sv_pair_begin(&p, o, "");
        cJSON_Delete(o);
        check("c2: a window: the code, two minutes, the default permissions",
              p.phase == SV_PAIR_SHOWING && strstr(p.uri, "SECRETTOKEN") && strcmp(p.countdown, "Closes in 2:00") == 0 &&
                  p.scopes == SV_SCOPES_DEFAULT && sv_pair_open(&p));
        check("c2: no decision can be sent with no phone waiting", sv_pair_decide_params(&p, 1) == NULL);
        st = json("{\"pairing\":true,\"pair\":{\"state\":\"candidate\",\"left_ms\":61000}}");
        sv_pair_poll(&p, st);
        cJSON_Delete(st);
        check("c2: a phone at the door, and the time from gatewayd",
              p.phase == SV_PAIR_CONNECTING && strcmp(p.countdown, "Closes in 1:01") == 0);
        sv_pair_poll(&p, NULL);
        check("c2: a missed poll changes nothing", p.phase == SV_PAIR_CONNECTING);
        st = json("{\"pairing\":true,\"pair\":{\"state\":\"waiting\",\"left_ms\":50000,\"candidate\":{\"name\":\"Pixel\\u0007 8\","
                  "\"fingerprint\":\"" VK2 "\"}}}");
        sv_pair_poll(&p, st);
        check("c2: the waiting phone by its name (made safe) and its key",
              p.phase == SV_PAIR_DECIDE && strcmp(p.cand_name, "Pixel? 8") == 0 && strcmp(p.cand_key_show, "C0C1 C2C3 C4C5 C6C7") == 0);
        check("c2: ALLOW waits for the keys to be compared", !sv_pair_can_allow(&p) && sv_pair_decide_params(&p, 1) == NULL);
        o = sv_pair_decide_params(&p, 0);
        out = cJSON_PrintUnformatted(o);
        check_str("c2: REFUSE needs no comparison, and carries no scopes, only the key it is about", out ? out : "",
                  "{\"allow\":false,\"fingerprint\":\"" VK2 "\"}");
        free(out);
        cJSON_Delete(o);
        p.keys_match = 1;
        p.scopes = sv_scope_toggle(p.scopes, SV_SCOPE_MESH_READ);
        o = sv_pair_decide_params(&p, 1);
        out = cJSON_PrintUnformatted(o);
        check_str("c2: ALLOW carries the confirmed key and exactly the chosen scopes", out ? out : "",
                  "{\"allow\":true,\"fingerprint\":\"" VK2 "\",\"scopes\":[\"status\"]}");
        free(out);
        cJSON_Delete(o);
        sv_pair_poll(&p, st);
        check("c2: the same phone polled again keeps the comparison", p.keys_match);
        cJSON_Delete(st);
        st = json("{\"pairing\":true,\"pair\":{\"state\":\"waiting\",\"left_ms\":40000,\"candidate\":{\"name\":\"Other\","
                  "\"fingerprint\":\"" VK1 "\"}}}");
        sv_pair_poll(&p, st);
        cJSON_Delete(st);
        check("c2: another phone's key undoes the comparison", !p.keys_match && !sv_pair_can_allow(&p) &&
                                                                 strcmp(p.cand_key_show, "0A1B 2C3D 4E5F 6071") == 0);
        p.keys_match = 1;
        o = json("{\"paired\":true}");
        sv_pair_decided(&p, 1, o, "");
        cJSON_Delete(o);
        check("c2: paired: said, and the token wiped",
              p.phase == SV_PAIR_DONE && strstr(p.text, "Other can now connect") && !p.uri[0] && !sv_pair_open(&p));
        /* Every way a window ends without a pairing. */
        {
            static const struct {
                const char *closed;
                const char *headline;
            } ends[] = {
                { "expired", "The code expired" },
                { "failed", "Closed after wrong codes" },
                { "candidate_left", "The phone left" },
                { "cancelled", "Pairing closed" },
            };
            size_t k;

            for (k = 0; k < sizeof(ends) / sizeof(ends[0]); k++) {
                char name[120];

                o = json("{\"uri\":\"doors-pair:1?t=SECRET\",\"expires_in_ms\":120000}");
                sv_pair_begin(&p, o, "");
                cJSON_Delete(o);
                snprintf(text, sizeof(text), "{\"pairing\":false,\"pair_closed\":\"%s\"}", ends[k].closed);
                st = json(text);
                sv_pair_poll(&p, st);
                cJSON_Delete(st);
                snprintf(name, sizeof(name), "c2: closed by %s: said, and the token wiped", ends[k].closed);
                check(name, p.phase == SV_PAIR_CLOSED && strcmp(p.headline, ends[k].headline) == 0 && !p.uri[0]);
            }
        }
        sv_pair_begin(&p, NULL, "client list full");
        check("c2: a window refused: why, in words", p.phase == SV_PAIR_ERROR && strstr(p.text, "Remove one first"));
        sv_pair_begin(&p, NULL, "");
        check("c2: no answer: said", p.phase == SV_PAIR_ERROR && strstr(p.text, "did not answer"));
        o = json("{\"uri\":\"https://example.com/\",\"expires_in_ms\":1}");
        sv_pair_begin(&p, o, "");
        cJSON_Delete(o);
        check("c2: anything but a doors-pair URI is never drawn", p.phase == SV_PAIR_ERROR && !p.uri[0]);
        o = json("{\"uri\":\"doors-pair:1?t=SECRET\"}");
        sv_pair_begin(&p, o, "");
        cJSON_Delete(o);
        p.phase = SV_PAIR_DECIDE;
        sv_pair_decided(&p, 1, NULL, "could not pair");
        check("c2: a pairing gatewayd could not store: said, nothing stored", p.phase == SV_PAIR_ERROR &&
                                                                               strstr(p.text, "Nothing was stored") && !p.uri[0]);

        /* ---- the device password ---- */
        check("c2: no password", sv_device_password_check("", "", text, sizeof(text)) < 0);
        check_str("c2: too short", sv_device_password_check("1234567", "1234567", text, sizeof(text)) < 0 ? text : "",
                  "The password needs at least 8 characters.");
        check("c2: 64 is the most", sv_device_password_check("0123456789012345678901234567890123456789012345678901234567890123",
                                                             "0123456789012345678901234567890123456789012345678901234567890123",
                                                             text, sizeof(text)) == 0);
        check("c2: a letter outside ASCII is refused", sv_device_password_check("abcdefg\xc3\xa9", "abcdefg\xc3\xa9", text,
                                                                                sizeof(text)) < 0);
        check_str("c2: two different ones", sv_device_password_check("abcdefgh", "abcdefgH", text, sizeof(text)) < 0 ? text : "",
                  "The two passwords are not the same.");
        check("c2: the same twice is taken", sv_device_password_check("abc defgh", "abc defgh", text, sizeof(text)) == 0);
        o = sv_device_password_params("abc defgh");
        out = cJSON_PrintUnformatted(o);
        check_str("c2: one member, the password", out ? out : "", "{\"password\":\"abc defgh\"}");
        free(out);
        cJSON_Delete(o);
        check("c2: no params for no password", sv_device_password_params("") == NULL);
    }

    printf("settings_view_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
