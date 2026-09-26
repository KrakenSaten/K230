/*
 * Where the Zabbix viewer is told which server to watch, and with what.
 *
 * TWO FILES, because only one of them is secret:
 *
 *   $POCKETOS_CONFIG_DIR/zabbix.conf       (default /etc/pocketos/zabbix.conf)
 *       key=value lines, world-readable like settings.conf, and never a
 *       secret in it:
 *         url=https://zabbix.example.com/zabbix/   the frontend, or its
 *                                                  api_jsonrpc.php
 *         label=Production                          shown on OVERVIEW
 *         auth=token | password                     default token
 *         user=viewer                               password logins only
 *         verify_tls=1                              default 1
 *         ca_file=/etc/pocketos/zabbix-ca.pem       default: the system store
 *         allow_insecure_http=0                     http:// refused unless 1
 *         refresh_s=30                              problems, 15..600
 *         hosts_s=60                                hosts, 30..3600
 *         timeout_s=10                              per request, 3..30
 *         mode=live | fake                          default live
 *         scenario=demo                             for mode=fake
 *
 *   $POCKETOS_STATE_DIR/zabbix/secret      (default /var/lib/pocketos/zabbix/secret)
 *       token=<API token>   or   password=<password>
 *       Directory 0700, file 0600, the arrangement ADR-003 gave the Wi-Fi
 *       passphrases. A secret file that anyone but its owner may read is
 *       refused rather than used. `pos-zabbix set-secret` writes it from
 *       stdin, so a token never passes through argv, the environment or a
 *       shell history.
 *
 * Protection is file permissions only: there is no key store on this
 * board, the card is not encrypted, and every Doors process runs as root
 * (docs/apps/ZABBIX.md, "Security").
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_ZBX_CONFIG_H
#define POCKETOS_ZBX_CONFIG_H

#include "zabbix/zbx_fake.h"
#include "zabbix/zbx_model.h"

#include <stdbool.h>
#include <stddef.h>

#define ZBX_CONF_FILE "zabbix.conf"
#define ZBX_SECRET_DIR "zabbix"
#define ZBX_SECRET_FILE "secret"
#define ZBX_SECRET_MAX 256
#define ZBX_USER_MAX 64
#define ZBX_PATH_MAX 256
#define ZBX_CONF_BYTES_MAX 8192

#define ZBX_REFRESH_DEFAULT_S 30
#define ZBX_REFRESH_MIN_S 15
#define ZBX_REFRESH_MAX_S 600
#define ZBX_HOSTS_DEFAULT_S 60
#define ZBX_HOSTS_MIN_S 30
#define ZBX_HOSTS_MAX_S 3600
#define ZBX_TIMEOUT_DEFAULT_S 10
#define ZBX_TIMEOUT_MIN_S 3
#define ZBX_TIMEOUT_MAX_S 30

enum zbx_auth {
    ZBX_AUTH_TOKEN = 0,
    ZBX_AUTH_PASSWORD,
};

struct zbx_config {
    bool configured;            /* a usable url, or the fake */
    bool fake;
    char scenario[ZBX_FAKE_NAME_MAX];
    char url[ZBX_URL_MAX];      /* the endpoint: .../api_jsonrpc.php */
    char url_shown[ZBX_URL_MAX]; /* the frontend: scheme, host, path */
    bool https;
    char label[ZBX_TEXT_MAX];
    enum zbx_auth auth;
    char user[ZBX_USER_MAX];
    char secret[ZBX_SECRET_MAX]; /* the token or the password; never logged */
    bool have_secret;
    bool verify;
    char ca_file[ZBX_PATH_MAX];
    bool allow_http;
    int refresh_s;
    int hosts_s;
    int timeout_s;
};

void zbx_config_defaults(struct zbx_config *c);

/* The two default paths, from the pocketpaths directories. */
void zbx_config_paths(char *conf, size_t conf_len, char *secret, size_t secret_len);

/* Read the configuration (conf_path) and the secret (secret_path; NULL to
 * skip it). A missing conf file is not an error: the result is simply not
 * configured. 0 when what was read is usable (or absent), -1 with the reason
 * in err otherwise - the result is then not configured either. */
int zbx_config_load(struct zbx_config *c, const char *conf_path, const char *secret_path,
                    char *err, size_t errlen);

/* Parse the text of a conf file. Exposed for the test. */
int zbx_config_parse(struct zbx_config *c, const char *text, char *err, size_t errlen);

/* Check a secret file (permissions, owner, type) and read it into c. */
int zbx_config_read_secret(struct zbx_config *c, const char *path, char *err, size_t errlen);

/* Write a secret file atomically: kind "token" or "password", value one line
 * of printable ASCII. Creates the 0700 directory. value "" removes the file. */
int zbx_config_write_secret(const char *path, const char *kind, const char *value, char *err,
                            size_t errlen);

/* Turn what the user typed into the endpoint and the shown form. Refuses
 * anything but http:// and https://, a user:password@ part, a query or a
 * fragment, and spaces. */
int zbx_config_url(const char *in, char *endpoint, size_t endpoint_len, char *shown,
                   size_t shown_len, bool *https, char *err, size_t errlen);

/* Wipe the secret from memory. */
void zbx_config_forget_secret(struct zbx_config *c);

#endif
