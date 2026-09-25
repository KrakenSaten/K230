/*
 * Wi-Fi text rules for netd: SSIDs as bytes, wpa_supplicant control-interface
 * replies and events, security classification, passphrase validation.
 *
 * Nothing here does I/O, so every rule that decides what a user is shown or
 * what is sent to wpa_supplicant is unit-tested (tests/wifi_parse_test.c).
 *
 * SSIDs are up to 32 arbitrary bytes (IEEE 802.11). They are carried as bytes
 * everywhere inside netd, sent to wpa_supplicant in its hex form so no SSID
 * can be misparsed as a command, and shown to people as UTF-8 text in which
 * any byte that is not valid UTF-8, or a control character, is replaced with
 * '?'. The exact bytes stay available as hex (docs/api/network.md).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_WIFI_PARSE_H
#define POCKETOS_WIFI_PARSE_H

#include <stddef.h>
#include <stdint.h>

#define WIFI_SSID_MAX 32
/* Two hex digits per byte, plus the terminator. */
#define WIFI_SSID_HEX_MAX (WIFI_SSID_MAX * 2 + 1)
/* Worst case UTF-8 display text: every byte kept as a character, each at
 * most 4 bytes, but a valid 32-byte SSID can never expand, so 32 plus the
 * terminator would do; the replacement '?' is one byte too. */
#define WIFI_SSID_TEXT_MAX (WIFI_SSID_MAX + 1)
/* WPA passphrase: 8..63 printable ASCII characters (IEEE 802.11i H.4). */
#define WIFI_PASSPHRASE_MIN 8
#define WIFI_PASSPHRASE_MAX 63
#define WIFI_BSSID_LEN 17 /* aa:bb:cc:dd:ee:ff */
/* How many BSS entries one scan keeps, and how many networks it reports. */
#define WIFI_SCAN_MAX 64

struct wifi_ssid {
    uint8_t bytes[WIFI_SSID_MAX];
    uint8_t len;
};

enum wifi_security {
    WIFI_SEC_OPEN = 0,     /* no protection at all */
    WIFI_SEC_WEP,          /* broken; never joined */
    WIFI_SEC_WPA_PSK,      /* WPA (TKIP era) with a passphrase */
    WIFI_SEC_WPA2_PSK,     /* WPA2-Personal */
    WIFI_SEC_WPA2_WPA3,    /* transition mode: PSK and SAE both offered */
    WIFI_SEC_WPA3_SAE,     /* WPA3-Personal only */
    WIFI_SEC_OWE,          /* Enhanced Open */
    WIFI_SEC_ENTERPRISE,   /* 802.1X / EAP */
};

/* What netd can do with a network of a given security on a given radio. */
enum wifi_join {
    WIFI_JOIN_PSK,         /* join with key_mgmt WPA-PSK and a passphrase */
    WIFI_JOIN_SAE,         /* join with SAE (only when the driver can) */
    WIFI_JOIN_OPEN,        /* join with key_mgmt NONE, only when asked explicitly */
    WIFI_JOIN_UNSUPPORTED, /* WEP, enterprise, OWE, SAE without driver support */
};

struct wifi_bss {
    char bssid[WIFI_BSSID_LEN + 1];
    int freq_mhz;
    int signal_dbm;
    enum wifi_security security;
    struct wifi_ssid ssid;
};

/* One network as a person sees it: every BSS with the same SSID bytes
 * collapsed into one entry, keeping the strongest. */
struct wifi_network {
    struct wifi_ssid ssid;
    int signal_dbm;
    int freq_mhz;
    enum wifi_security security;
    int bss_count;
};

/* ---- SSIDs ---------------------------------------------------------------- */

/* wpa_supplicant prints SSIDs with printf_encode(): printable ASCII as is,
 * \" \\ \e \n \r \t, and \xNN for everything else. Returns 0, or -1 for a
 * malformed escape or more than 32 bytes. */
int wifi_ssid_decode_printf(const char *text, struct wifi_ssid *out);
/* Exactly 2..64 hex digits (1..32 bytes), either case. Returns 0 or -1. */
int wifi_ssid_from_hex(const char *hex, struct wifi_ssid *out);
/* UTF-8 text of 1..32 bytes, taken as its bytes. Returns 0 or -1. */
int wifi_ssid_from_text(const char *text, struct wifi_ssid *out);
void wifi_ssid_to_hex(const struct wifi_ssid *s, char *out, size_t n);
/* Display text (see the header comment). Always NUL-terminated, always valid
 * UTF-8, never contains a control character. */
void wifi_ssid_to_text(const struct wifi_ssid *s, char *out, size_t n);
int wifi_ssid_equal(const struct wifi_ssid *a, const struct wifi_ssid *b);
/* A hidden network announces a zero-length SSID or one made of NUL bytes. */
int wifi_ssid_is_hidden(const struct wifi_ssid *s);

/* ---- security --------------------------------------------------------------- */

/* From a SCAN_RESULTS flags field such as "[WPA2-PSK-CCMP][ESS]". */
enum wifi_security wifi_security_from_flags(const char *flags);
/* Stable API name: "open", "wep", "wpa", "wpa2", "wpa2/wpa3", "wpa3", "owe",
 * "enterprise". */
const char *wifi_security_name(enum wifi_security s);
/* The inverse, for the store and for a hidden network's declared security.
 * Returns 0 or -1. */
int wifi_security_parse(const char *name, enum wifi_security *out);
/* sae_capable: whether the driver reported SAE (GET_CAPABILITY key_mgmt). */
enum wifi_join wifi_join_method(enum wifi_security s, int sae_capable);
int wifi_security_needs_passphrase(enum wifi_security s);

/* ---- passphrases ------------------------------------------------------------ */

/* 8..63 characters, each 0x20..0x7e. Returns 0 or -1. Never logs, never
 * copies. */
int wifi_passphrase_valid(const char *pass);

/* ---- wpa_supplicant replies ----------------------------------------------- */

/* SCAN_RESULTS: a header line, then bssid \t freq \t signal \t flags \t ssid.
 * Malformed lines are skipped. Entries with a hidden SSID are not returned
 * but counted in *hidden. Returns the number of entries stored in out. */
int wifi_parse_scan_results(const char *reply, struct wifi_bss *out, int max, int *hidden);
/* Collapse by SSID bytes (strongest BSS wins, its frequency and security are
 * kept), then sort strongest first, SSID bytes breaking ties. Returns the
 * number of networks. */
int wifi_networks_from_bss(const struct wifi_bss *bss, int n, struct wifi_network *out, int max);

/* The value of key in a "key=value" per line reply (STATUS, SIGNAL_POLL).
 * Returns 0 and copies it (truncated to n-1), or -1 when absent. */
int wifi_kv_get(const char *reply, const char *key, char *out, size_t n);
/* As wifi_kv_get, as a base-10 integer. Returns 0 or -1. */
int wifi_kv_get_int(const char *reply, const char *key, int *out);

/* LIST_NETWORKS: a "network id / ssid / bssid / flags" header line, then
 * id \t ssid \t bssid \t flags. Stores up to max ids in out. Returns the
 * number stored, or -1 when the reply is not such a list. A line whose id is
 * not a whole number followed by a tab (a truncated last line) is skipped. */
int wifi_parse_network_ids(const char *reply, int *out, int max);

/* Signal strength in bars, 0..4, from dBm. */
int wifi_signal_bars(int dbm);

/* ---- events ------------------------------------------------------------------ */

enum wifi_event_type {
    WIFI_EV_OTHER = 0,
    WIFI_EV_CONNECTED,
    WIFI_EV_DISCONNECTED,
    WIFI_EV_SCAN_RESULTS,
    WIFI_EV_SCAN_FAILED,
    WIFI_EV_NETWORK_NOT_FOUND,
    WIFI_EV_WRONG_KEY,      /* SSID-TEMP-DISABLED reason=WRONG_KEY, or the 4-way handshake message */
    WIFI_EV_CONN_FAILED,    /* SSID-TEMP-DISABLED with any other reason */
    WIFI_EV_ASSOC_REJECT,
    WIFI_EV_AUTH_REJECT,
    WIFI_EV_TERMINATING,
};

struct wifi_event {
    enum wifi_event_type type;
    int network_id;       /* id= when the event carries one, else -1 */
    int reason;           /* reason= / status_code= when numeric, else -1 */
    int locally_generated;
};

/* One unsolicited control-interface message, with or without the "<N>"
 * priority prefix. */
void wifi_parse_event(const char *msg, struct wifi_event *ev);

#endif
