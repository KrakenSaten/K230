/*
 * netd's Wi-Fi store: whether Wi-Fi is on, and the networks a person has
 * joined, with their passphrases.
 *
 * This is the one place PocketOS persists a secret, under the rules of
 * docs/decisions/ADR-003-wifi-credentials.md:
 *
 *   file       $POCKETOS_STATE_DIR/netd/wifi.conf (default /var/lib/pocketos/netd)
 *   directory  0700, file 0600, both root; tightened on every save, and a file
 *              found wider than 0600 is reported so the caller can say so
 *   writes     temp file (O_EXCL, O_NOFOLLOW, 0600) + fsync + rename + fsync
 *              of the directory, so a power cut leaves the old file or the new
 *   format     "key=value" lines; a network is
 *              network=<ssid hex> <security> <hidden 0|1> <passphrase hex or ->
 *              The passphrase is hex so no character of it can break the line;
 *              hex is an encoding, not protection, and the doc says so.
 *
 * A file that does not parse completely is not partly believed: load reports
 * it as damaged and hands back an empty store, and the caller moves the file
 * aside before the next save rather than overwriting it.
 *
 * Nothing here logs. Passphrases never leave this module except to the
 * caller that asked, and wifi_store_wipe() clears them from memory.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_WIFI_STORE_H
#define POCKETOS_WIFI_STORE_H

#include "wifi_parse.h"

#define WIFI_STORE_FILE "wifi.conf"
#define WIFI_STORE_DAMAGED_FILE "wifi.conf.damaged"
#define WIFI_STORE_MAX 16
#define WIFI_STORE_VERSION 1

struct wifi_saved {
    struct wifi_ssid ssid;
    enum wifi_security security;
    int hidden;
    char passphrase[WIFI_PASSPHRASE_MAX + 1]; /* empty for an open network */
};

struct wifi_store {
    int enabled;
    int count;
    struct wifi_saved net[WIFI_STORE_MAX]; /* most recently joined first */
};

enum wifi_store_load_result {
    WIFI_STORE_LOADED = 0,
    WIFI_STORE_ABSENT = 1,   /* no file yet: defaults (off, no networks) */
    WIFI_STORE_DAMAGED = -1, /* present but not parseable: defaults, file left alone */
    WIFI_STORE_UNREADABLE = -2, /* present but could not be read (errno) */
};

void wifi_store_init(struct wifi_store *st);
/* Loads dir/wifi.conf. *too_open is set to 1 when the file's mode allows
 * group or other any access (it is not changed here). */
enum wifi_store_load_result wifi_store_load(const char *dir, struct wifi_store *st, int *too_open);
/* Creates dir (0700) if needed and writes atomically. Returns 0 or -1 (errno). */
int wifi_store_save(const char *dir, const struct wifi_store *st);
/* Renames a damaged file aside (dir/wifi.conf.damaged, replacing an older
 * one). Returns 0, or -1 (errno). */
int wifi_store_set_aside(const char *dir);

/* Index of the network with these SSID bytes, or -1. */
int wifi_store_find(const struct wifi_store *st, const struct wifi_ssid *ssid);
/* Insert or replace, moving it to the front. When the store is full the
 * oldest network is dropped to make room. Returns 0, or -1 for an invalid
 * entry (bad SSID, a passphrase that fails wifi_passphrase_valid where one is
 * needed, one present where none is). */
int wifi_store_put(struct wifi_store *st, const struct wifi_saved *net);
/* Returns 0, or -1 when not present. */
int wifi_store_remove(struct wifi_store *st, const struct wifi_ssid *ssid);
/* Zero every passphrase held in memory. */
void wifi_store_wipe(struct wifi_store *st);

#endif
