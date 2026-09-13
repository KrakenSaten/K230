/*
 * Settings view model (apps/settings/settings_view.c): Wi-Fi states and
 * failures as a person reads them, the network list, what a tap on a network
 * does, the parameters a join sends, passphrase feedback, and brightness
 * stepping.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
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

    printf("settings_view_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
