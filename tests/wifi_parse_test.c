/*
 * netd's Wi-Fi text rules (services/netd/wifi_parse.c): SSID encodings,
 * scan-result parsing, duplicate and hidden SSIDs, security classification,
 * STATUS/SIGNAL_POLL values, events and passphrase validation.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "wifi_parse.h"

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

static int ssid_is(const struct wifi_ssid *s, const char *bytes, size_t len)
{
    return s->len == len && memcmp(s->bytes, bytes, len) == 0;
}

static const char *text_of(const struct wifi_ssid *s)
{
    static char buf[WIFI_SSID_TEXT_MAX];

    wifi_ssid_to_text(s, buf, sizeof(buf));
    return buf;
}

int main(void)
{
    struct wifi_ssid s;
    struct wifi_ssid t;
    struct wifi_bss bss[WIFI_SCAN_MAX];
    struct wifi_network nets[WIFI_SCAN_MAX];
    struct wifi_event ev;
    char hex[WIFI_SSID_HEX_MAX];
    char buf[256];
    int hidden;
    int n;
    int v;
    int i;

    /* ---- SSID encodings ----------------------------------------------------- */
    check("plain SSID decodes", wifi_ssid_decode_printf("HomeNet", &s) == 0 && ssid_is(&s, "HomeNet", 7));
    check("escaped quote and backslash", wifi_ssid_decode_printf("a\\\"b\\\\c", &s) == 0 &&
                                             ssid_is(&s, "a\"b\\c", 5));
    check("escaped control characters", wifi_ssid_decode_printf("\\e\\n\\r\\t", &s) == 0 &&
                                            ssid_is(&s, "\x1b\n\r\t", 4));
    check("hex escapes give arbitrary bytes", wifi_ssid_decode_printf("\\x00\\xff\\xC3\\xa6", &s) == 0 &&
                                                  ssid_is(&s, "\x00\xff\xc3\xa6", 4));
    check("32 bytes decode", wifi_ssid_decode_printf("0123456789abcdef0123456789abcdef", &s) == 0 && s.len == 32);
    check("33 bytes are refused", wifi_ssid_decode_printf("0123456789abcdef0123456789abcdefX", &s) == -1);
    check("33 bytes through escapes are refused",
          wifi_ssid_decode_printf("0123456789abcdef0123456789abcde\\x41\\x42", &s) == -1);
    check("an unknown escape is refused", wifi_ssid_decode_printf("a\\qb", &s) == -1);
    check("a short hex escape is refused", wifi_ssid_decode_printf("a\\x4", &s) == -1);
    check("a non-hex escape is refused", wifi_ssid_decode_printf("a\\xzz", &s) == -1);
    check("a trailing backslash is refused", wifi_ssid_decode_printf("a\\", &s) == -1);
    check("empty decodes to a zero-length SSID", wifi_ssid_decode_printf("", &s) == 0 && s.len == 0);

    check("hex to SSID", wifi_ssid_from_hex("486f6d65", &s) == 0 && ssid_is(&s, "Home", 4));
    check("upper-case hex", wifi_ssid_from_hex("486F6D65", &s) == 0 && ssid_is(&s, "Home", 4));
    check("odd hex is refused", wifi_ssid_from_hex("486", &s) == -1);
    check("empty hex is refused", wifi_ssid_from_hex("", &s) == -1);
    check("NULL hex is refused", wifi_ssid_from_hex(NULL, &s) == -1);
    check("non-hex is refused", wifi_ssid_from_hex("48zz", &s) == -1);
    memset(buf, 'a', 66);
    buf[66] = '\0';
    check("66 hex digits (33 bytes) are refused", wifi_ssid_from_hex(buf, &s) == -1);
    buf[64] = '\0';
    check("64 hex digits (32 bytes) are accepted", wifi_ssid_from_hex(buf, &s) == 0 && s.len == 32);
    wifi_ssid_decode_printf("\\x00\\x7f\\xff A", &s);
    wifi_ssid_to_hex(&s, hex, sizeof(hex));
    check("SSID to hex is lower case and exact", strcmp(hex, "007fff2041") == 0);
    check("hex round trip", wifi_ssid_from_hex(hex, &t) == 0 && wifi_ssid_equal(&s, &t));
    check("text SSID", wifi_ssid_from_text("Kafé", &s) == 0 && s.len == 5);
    check("empty text SSID is refused", wifi_ssid_from_text("", &s) == -1);
    check("33-byte text SSID is refused", wifi_ssid_from_text("0123456789abcdef0123456789abcdefX", &s) == -1);
    check("32 bytes of UTF-8 text are accepted", wifi_ssid_from_text("æææææææææææææææø", &s) == 0 && s.len == 32);

    /* display text */
    wifi_ssid_from_text("Kafé ø", &s);
    check("valid UTF-8 is shown as is", strcmp(text_of(&s), "Kafé ø") == 0);
    wifi_ssid_decode_printf("a\\xffb", &s);
    check("an invalid byte is shown as ?", strcmp(text_of(&s), "a?b") == 0);
    wifi_ssid_decode_printf("a\\nb\\tc\\x7f", &s);
    check("control characters are shown as ?", strcmp(text_of(&s), "a?b?c?") == 0);
    wifi_ssid_decode_printf("\\xc3", &s);
    check("a truncated sequence is ?", strcmp(text_of(&s), "?") == 0);
    wifi_ssid_decode_printf("\\xc0\\xaf", &s);
    check("an overlong encoding is not passed through", strcmp(text_of(&s), "??") == 0);
    wifi_ssid_decode_printf("\\xed\\xa0\\x80", &s);
    check("a surrogate is not passed through", strcmp(text_of(&s), "???") == 0);
    wifi_ssid_decode_printf("\\xc2\\x85", &s);
    check("a C1 control is not passed through", strcmp(text_of(&s), "??") == 0);
    wifi_ssid_decode_printf("\\xf0\\x9f\\x93\\xb6", &s);
    check("a 4-byte character is kept", strcmp(text_of(&s), "\xf0\x9f\x93\xb6") == 0);
    wifi_ssid_decode_printf("\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff"
                            "\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff", &s);
    check("32 invalid bytes fit the text buffer", strlen(text_of(&s)) == 32);
    {
        char small[4];

        wifi_ssid_from_text("abcdef", &s);
        wifi_ssid_to_text(&s, small, sizeof(small));
        check("text output is bounded and terminated", strcmp(small, "abc") == 0);
        wifi_ssid_from_text("aæ", &s);
        wifi_ssid_to_text(&s, small, 3);
        check("a character that does not fit is not split", strcmp(small, "a") == 0);
    }

    memset(&s, 0, sizeof(s));
    check("zero-length SSID is hidden", wifi_ssid_is_hidden(&s));
    s.len = 5;
    check("all-NUL SSID is hidden", wifi_ssid_is_hidden(&s));
    s.bytes[4] = 'x';
    check("an SSID with a non-NUL byte is not hidden", !wifi_ssid_is_hidden(&s));

    /* ---- security ------------------------------------------------------------- */
    check("WPA2-PSK", wifi_security_from_flags("[WPA2-PSK-CCMP][ESS]") == WIFI_SEC_WPA2_PSK);
    check("WPA2-PSK with WPS", wifi_security_from_flags("[WPA2-PSK-CCMP][WPS][ESS]") == WIFI_SEC_WPA2_PSK);
    check("WPA and WPA2 mixed", wifi_security_from_flags("[WPA-PSK-CCMP+TKIP][WPA2-PSK-CCMP+TKIP][ESS]") ==
                                    WIFI_SEC_WPA2_PSK);
    check("WPA only", wifi_security_from_flags("[WPA-PSK-TKIP][ESS]") == WIFI_SEC_WPA_PSK);
    check("WPA2 PSK-SHA256", wifi_security_from_flags("[WPA2-PSK-SHA256-CCMP][ESS]") == WIFI_SEC_WPA2_PSK);
    check("transition mode", wifi_security_from_flags("[WPA2-PSK+SAE-CCMP][ESS]") == WIFI_SEC_WPA2_WPA3);
    check("WPA3 only", wifi_security_from_flags("[WPA2-SAE-CCMP][ESS]") == WIFI_SEC_WPA3_SAE);
    check("WPA3 with a '-' in the cipher", wifi_security_from_flags("[WPA2-SAE-CCMP-256][ESS]") == WIFI_SEC_WPA3_SAE);
    check("WPA3 SAE-EXT-KEY", wifi_security_from_flags("[WPA2-SAE-EXT-KEY-GCMP-256][ESS]") == WIFI_SEC_WPA3_SAE);
    check("RSN naming", wifi_security_from_flags("[RSN-SAE-CCMP][MESH]") == WIFI_SEC_WPA3_SAE);
    check("FT PSK", wifi_security_from_flags("[WPA2-PSK+FT/PSK-CCMP][ESS]") == WIFI_SEC_WPA2_PSK);
    check("enterprise", wifi_security_from_flags("[WPA2-EAP-CCMP][ESS]") == WIFI_SEC_ENTERPRISE);
    check("enterprise suite B", wifi_security_from_flags("[WPA2-EAP-SUITE-B-192-GCMP-256][ESS]") ==
                                    WIFI_SEC_ENTERPRISE);
    check("enterprise wins over PSK", wifi_security_from_flags("[WPA2-EAP+PSK-CCMP][ESS]") == WIFI_SEC_ENTERPRISE);
    check("WEP", wifi_security_from_flags("[WEP][ESS]") == WIFI_SEC_WEP);
    check("OWE", wifi_security_from_flags("[WPA2-OWE-CCMP][ESS]") == WIFI_SEC_OWE);
    check("OWE transition open half is open", wifi_security_from_flags("[OWE-TRANS-OPEN][ESS]") == WIFI_SEC_OPEN);
    check("open", wifi_security_from_flags("[ESS]") == WIFI_SEC_OPEN);
    check("no flags is open", wifi_security_from_flags("") == WIFI_SEC_OPEN);
    check("NULL flags is open", wifi_security_from_flags(NULL) == WIFI_SEC_OPEN);
    check("an unknown RSN key management is not guessed to be open",
          wifi_security_from_flags("[WPA2-FOO-CCMP][ESS]") == WIFI_SEC_ENTERPRISE);
    check("an unterminated group does not crash", wifi_security_from_flags("[WPA2-PSK-CCMP") == WIFI_SEC_OPEN);

    check("security names", strcmp(wifi_security_name(WIFI_SEC_WPA2_WPA3), "wpa2/wpa3") == 0 &&
                                strcmp(wifi_security_name(WIFI_SEC_OPEN), "open") == 0);
    {
        enum wifi_security sec;

        check("security name round trip", wifi_security_parse("wpa2", &sec) == 0 && sec == WIFI_SEC_WPA2_PSK);
        check("unknown security name", wifi_security_parse("wpa4", &sec) == -1);
        check("NULL security name", wifi_security_parse(NULL, &sec) == -1);
    }
    check("PSK joins with PSK", wifi_join_method(WIFI_SEC_WPA2_PSK, 0) == WIFI_JOIN_PSK);
    check("WPA joins with PSK", wifi_join_method(WIFI_SEC_WPA_PSK, 0) == WIFI_JOIN_PSK);
    check("transition without SAE joins with PSK", wifi_join_method(WIFI_SEC_WPA2_WPA3, 0) == WIFI_JOIN_PSK);
    check("transition with SAE joins with SAE", wifi_join_method(WIFI_SEC_WPA2_WPA3, 1) == WIFI_JOIN_SAE);
    check("WPA3 without SAE is unsupported", wifi_join_method(WIFI_SEC_WPA3_SAE, 0) == WIFI_JOIN_UNSUPPORTED);
    check("WPA3 with SAE joins with SAE", wifi_join_method(WIFI_SEC_WPA3_SAE, 1) == WIFI_JOIN_SAE);
    check("WEP is unsupported", wifi_join_method(WIFI_SEC_WEP, 1) == WIFI_JOIN_UNSUPPORTED);
    check("enterprise is unsupported", wifi_join_method(WIFI_SEC_ENTERPRISE, 1) == WIFI_JOIN_UNSUPPORTED);
    check("OWE is unsupported", wifi_join_method(WIFI_SEC_OWE, 1) == WIFI_JOIN_UNSUPPORTED);
    check("open is a separate join", wifi_join_method(WIFI_SEC_OPEN, 0) == WIFI_JOIN_OPEN);
    check("open needs no passphrase", !wifi_security_needs_passphrase(WIFI_SEC_OPEN));
    check("WPA2 needs a passphrase", wifi_security_needs_passphrase(WIFI_SEC_WPA2_PSK));

    /* ---- passphrases ---------------------------------------------------------------- */
    check("8 characters", wifi_passphrase_valid("abcdefgh") == 0);
    check("63 characters", wifi_passphrase_valid("123456789012345678901234567890123456789012345678901234567890123") == 0);
    check("7 characters are refused", wifi_passphrase_valid("abcdefg") == -1);
    check("64 characters are refused", wifi_passphrase_valid("1234567890123456789012345678901234567890123456789012345678901234") == -1);
    check("empty is refused", wifi_passphrase_valid("") == -1);
    check("NULL is refused", wifi_passphrase_valid(NULL) == -1);
    check("spaces and punctuation are allowed", wifi_passphrase_valid("a \"b\" \\c' ~!") == 0);
    check("a newline is refused", wifi_passphrase_valid("abcdefgh\n") == -1);
    check("a tab is refused", wifi_passphrase_valid("abcd\tefgh") == -1);
    check("DEL is refused", wifi_passphrase_valid("abcdefgh\x7f") == -1);
    check("non-ASCII is refused", wifi_passphrase_valid("blåbærsyltetøy") == -1);
    {
        char *big = malloc(100001);

        memset(big, 'x', 100000);
        big[100000] = '\0';
        check("a huge string is refused", wifi_passphrase_valid(big) == -1);
        free(big);
    }

    /* ---- scan results ------------------------------------------------------------------ */
    n = wifi_parse_scan_results(
        "bssid / frequency / signal level / flags / ssid\n"
        "aa:bb:cc:dd:ee:01\t2412\t-40\t[WPA2-PSK-CCMP][ESS]\tHome\n"
        "aa:bb:cc:dd:ee:02\t2437\t-71\t[WPA2-PSK-CCMP][ESS]\tHome\n"
        "aa:bb:cc:dd:ee:03\t5180\t-60\t[WPA2-PSK+SAE-CCMP][ESS]\tOffice 5G\n"
        "aa:bb:cc:dd:ee:04\t2462\t-80\t[ESS]\tCafe\\x20Free\n"
        "aa:bb:cc:dd:ee:05\t2412\t-50\t[WPA2-PSK-CCMP][ESS]\t\n"
        "aa:bb:cc:dd:ee:06\t2412\t-55\t[WPA2-PSK-CCMP][ESS]\t\\x00\\x00\\x00\n"
        "aa:bb:cc:dd:ee:07\t2412\t-65\t[WEP][ESS]\tOld\n"
        "aa:bb:cc:dd:ee:08\t2412\t-66\t[WPA2-EAP-CCMP][ESS]\tCorp\n",
        bss, WIFI_SCAN_MAX, &hidden);
    check("scan: visible entries parsed", n == 6);
    check("scan: hidden entries counted, not returned", hidden == 2);
    check("scan: bssid", strcmp(bss[0].bssid, "aa:bb:cc:dd:ee:01") == 0);
    check("scan: frequency", bss[2].freq_mhz == 5180);
    check("scan: signal", bss[1].signal_dbm == -71);
    check("scan: flags", bss[2].security == WIFI_SEC_WPA2_WPA3);
    check("scan: escaped SSID", ssid_is(&bss[3].ssid, "Cafe Free", 9));
    check("scan: SSID with a space", ssid_is(&bss[2].ssid, "Office 5G", 9));

    n = wifi_parse_scan_results(
        "bssid / frequency / signal level / flags / ssid\n"
        "not-a-bssid\t2412\t-40\t[ESS]\tA\n"
        "aa:bb:cc:dd:ee:01\tabc\t-40\t[ESS]\tB\n"
        "aa:bb:cc:dd:ee:01\t2412\t-4x\t[ESS]\tC\n"
        "aa:bb:cc:dd:ee:01\t2412\t-40\t[ESS]\n"
        "aa:bb:cc:dd:ee:01\t2412\t-40\t[ESS]\tbad\\qescape\n"
        "aa:bb:cc:dd:ee:01\t2412\t-40\t[ESS]\t0123456789abcdef0123456789abcdefX\n"
        "\n"
        "aa:bb:cc:dd:ee:0g\t2412\t-40\t[ESS]\tD\n"
        "aa:bb:cc:dd:ee:09\t2412\t-40\t[ESS]\tGood\r\n",
        bss, WIFI_SCAN_MAX, &hidden);
    check("scan: malformed lines are skipped, good ones kept", n == 1 && ssid_is(&bss[0].ssid, "Good", 4));
    check("scan: a CR before the newline is not part of the SSID", bss[0].ssid.len == 4);
    check("scan: empty reply", wifi_parse_scan_results("", bss, WIFI_SCAN_MAX, &hidden) == 0 && hidden == 0);
    check("scan: header only", wifi_parse_scan_results("bssid / frequency / signal level / flags / ssid\n",
                                                       bss, WIFI_SCAN_MAX, &hidden) == 0);
    check("scan: NULL reply", wifi_parse_scan_results(NULL, bss, WIFI_SCAN_MAX, &hidden) == 0);
    check("scan: no header line", wifi_parse_scan_results("aa:bb:cc:dd:ee:09\t2412\t-40\t[ESS]\tX",
                                                          bss, WIFI_SCAN_MAX, NULL) == 1);
    {
        char *big = malloc(200 * 80);
        char *p = big;

        p += sprintf(p, "bssid / frequency / signal level / flags / ssid\n");
        for (i = 0; i < 100; i++) {
            p += sprintf(p, "aa:bb:cc:dd:%02x:%02x\t2412\t-%d\t[ESS]\tnet%d\n", i / 256, i % 256, 30 + i % 60, i);
        }
        n = wifi_parse_scan_results(big, bss, WIFI_SCAN_MAX, &hidden);
        check("scan: more entries than room are bounded", n == WIFI_SCAN_MAX);
        free(big);
    }

    /* ---- duplicates, order ---------------------------------------------------------- */
    n = wifi_parse_scan_results(
        "bssid / frequency / signal level / flags / ssid\n"
        "aa:bb:cc:dd:ee:01\t2412\t-71\t[WPA2-PSK-CCMP][ESS]\tHome\n"
        "aa:bb:cc:dd:ee:02\t5200\t-45\t[WPA2-PSK+SAE-CCMP][ESS]\tHome\n"
        "aa:bb:cc:dd:ee:03\t2437\t-60\t[ESS]\tGuest\n"
        "aa:bb:cc:dd:ee:04\t2462\t-60\t[ESS]\tAlpha\n"
        "aa:bb:cc:dd:ee:05\t2412\t-90\t[WPA2-PSK-CCMP][ESS]\thome\n",
        bss, WIFI_SCAN_MAX, &hidden);
    n = wifi_networks_from_bss(bss, n, nets, WIFI_SCAN_MAX);
    check("duplicates: one network per SSID", n == 4);
    check("duplicates: strongest first", ssid_is(&nets[0].ssid, "Home", 4) && nets[0].signal_dbm == -45);
    check("duplicates: the strongest BSS's frequency is kept", nets[0].freq_mhz == 5200);
    check("duplicates: and its security", nets[0].security == WIFI_SEC_WPA2_WPA3);
    check("duplicates: BSS counted", nets[0].bss_count == 2);
    check("equal signal: SSID bytes decide", ssid_is(&nets[1].ssid, "Alpha", 5) && ssid_is(&nets[2].ssid, "Guest", 5));
    check("SSIDs are case-sensitive bytes", ssid_is(&nets[3].ssid, "home", 4));
    check("network list bounded", wifi_networks_from_bss(bss, 5, nets, 2) == 2);

    /* ---- STATUS / SIGNAL_POLL ---------------------------------------------------------- */
    {
        const char *status = "bssid=aa:bb:cc:dd:ee:01\nfreq=2412\nssid=Home\\x20Net\nid=3\nmode=station\n"
                             "pairwise_cipher=CCMP\nkey_mgmt=WPA2-PSK\nwpa_state=COMPLETED\n"
                             "ip_address=192.168.1.20\naddress=88:3b:dc:b7:9e:c7\n";

        check("kv: wpa_state", wifi_kv_get(status, "wpa_state", buf, sizeof(buf)) == 0 &&
                                   strcmp(buf, "COMPLETED") == 0);
        check("kv: ssid raw", wifi_kv_get(status, "ssid", buf, sizeof(buf)) == 0 &&
                                  strcmp(buf, "Home\\x20Net") == 0);
        check("kv: a key is not matched by prefix", wifi_kv_get(status, "ss", buf, sizeof(buf)) == -1);
        check("kv: a key is not matched as a suffix", wifi_kv_get(status, "id", buf, sizeof(buf)) == 0 &&
                                                          strcmp(buf, "3") == 0);
        check("kv: bssid", wifi_kv_get(status, "bssid", buf, sizeof(buf)) == 0 &&
                               strcmp(buf, "aa:bb:cc:dd:ee:01") == 0);
        check("kv: int", wifi_kv_get_int(status, "freq", &v) == 0 && v == 2412);
        check("kv: absent", wifi_kv_get(status, "nothere", buf, sizeof(buf)) == -1);
        check("kv: not an int", wifi_kv_get_int(status, "mode", &v) == -1);
        check("kv: truncated to the buffer", wifi_kv_get(status, "address", buf, 6) == 0 &&
                                                 strcmp(buf, "88:3b") == 0);
        check("kv: NULL reply", wifi_kv_get(NULL, "a", buf, sizeof(buf)) == -1);
        check("signal poll RSSI", wifi_kv_get_int("RSSI=-54\nLINKSPEED=65\nNOISE=9999\nFREQUENCY=2437\n",
                                                  "RSSI", &v) == 0 && v == -54);
        check("signal poll with CRLF", wifi_kv_get_int("RSSI=-61\r\n", "RSSI", &v) == 0 && v == -61);
    }
    check("bars 4", wifi_signal_bars(-40) == 4);
    check("bars 3", wifi_signal_bars(-60) == 3);
    check("bars 2", wifi_signal_bars(-70) == 2);
    check("bars 1", wifi_signal_bars(-85) == 1);
    check("bars 0", wifi_signal_bars(-95) == 0);
    check("bars edge -55", wifi_signal_bars(-55) == 4 && wifi_signal_bars(-56) == 3);

    /* ---- events ------------------------------------------------------------------------- */
    wifi_parse_event("<3>CTRL-EVENT-CONNECTED - Connection to aa:bb:cc:dd:ee:01 completed [id=2 id_str=]", &ev);
    check("event: connected with id", ev.type == WIFI_EV_CONNECTED && ev.network_id == 2);
    wifi_parse_event("<3>CTRL-EVENT-DISCONNECTED bssid=aa:bb:cc:dd:ee:01 reason=3 locally_generated=1", &ev);
    check("event: disconnected, reason, local", ev.type == WIFI_EV_DISCONNECTED && ev.reason == 3 &&
                                                    ev.locally_generated == 1);
    wifi_parse_event("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"Home\" auth_failures=1 duration=10 reason=WRONG_KEY", &ev);
    check("event: wrong key", ev.type == WIFI_EV_WRONG_KEY && ev.network_id == 0);
    wifi_parse_event("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=4 ssid=\"x\" auth_failures=2 duration=20 reason=CONN_FAILED", &ev);
    check("event: other temp-disable", ev.type == WIFI_EV_CONN_FAILED && ev.network_id == 4);
    wifi_parse_event("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=4 ssid=\"reason=WRONG_KEY\" auth_failures=2 duration=20 reason=CONN_FAILED", &ev);
    check("event: an SSID cannot fake the reason", ev.type == WIFI_EV_CONN_FAILED);
    wifi_parse_event("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=4 ssid=\"a reason=WRONG_KEY\" auth_failures=2 duration=20 reason=CONN_FAILED", &ev);
    check("event: not even with a space in it", ev.type == WIFI_EV_CONN_FAILED);
    wifi_parse_event("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=4 ssid=\"a reason=CONN_FAILED\" auth_failures=1 duration=10 reason=WRONG_KEY", &ev);
    check("event: and the real WRONG_KEY is still seen", ev.type == WIFI_EV_WRONG_KEY);
    wifi_parse_event("<3>WPA: 4-Way Handshake failed - pre-shared key may be incorrect", &ev);
    check("event: 4-way handshake failure", ev.type == WIFI_EV_WRONG_KEY && ev.network_id == -1);
    wifi_parse_event("<3>CTRL-EVENT-SCAN-RESULTS ", &ev);
    check("event: scan results", ev.type == WIFI_EV_SCAN_RESULTS);
    wifi_parse_event("CTRL-EVENT-SCAN-FAILED ret=-16", &ev);
    check("event: scan failed without a prefix", ev.type == WIFI_EV_SCAN_FAILED);
    wifi_parse_event("<3>CTRL-EVENT-NETWORK-NOT-FOUND ", &ev);
    check("event: network not found", ev.type == WIFI_EV_NETWORK_NOT_FOUND);
    wifi_parse_event("<3>CTRL-EVENT-ASSOC-REJECT bssid=aa:bb:cc:dd:ee:01 status_code=17", &ev);
    check("event: assoc reject", ev.type == WIFI_EV_ASSOC_REJECT && ev.reason == 17);
    wifi_parse_event("<3>CTRL-EVENT-AUTH-REJECT aa:bb:cc:dd:ee:01 auth_type=0 auth_transaction=2 status_code=1", &ev);
    check("event: auth reject", ev.type == WIFI_EV_AUTH_REJECT && ev.reason == 1);
    wifi_parse_event("<2>CTRL-EVENT-TERMINATING ", &ev);
    check("event: terminating", ev.type == WIFI_EV_TERMINATING);
    wifi_parse_event("<3>CTRL-EVENT-BSS-ADDED 0 aa:bb:cc:dd:ee:01", &ev);
    check("event: others are ignored", ev.type == WIFI_EV_OTHER);
    wifi_parse_event("", &ev);
    check("event: empty", ev.type == WIFI_EV_OTHER);
    wifi_parse_event(NULL, &ev);
    check("event: NULL", ev.type == WIFI_EV_OTHER);
    wifi_parse_event("<3>CTRL-EVENT-DISCONNECTED bssid=aa reason=99999999999", &ev);
    check("event: an absurd reason is not trusted", ev.type == WIFI_EV_DISCONNECTED && ev.reason == -1);

    printf("wifi_parse_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
