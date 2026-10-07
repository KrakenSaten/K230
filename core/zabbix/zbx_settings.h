/*
 * The Zabbix connection, changed on the unit itself: what the Zabbix app's
 * CONNECTION screen (DS §50) is shown, and what happens when it asks to
 * TEST or SAVE. The helper runs this; the shell never does (it links no
 * file of core/zabbix but the model and the protocol).
 *
 * WHAT CAN BE CHANGED: url=, auth= and user= in zabbix.conf, and the secret
 * (zbx_config.h) - the fields the configuration already had, nothing new.
 * Everything else in zabbix.conf - label, verify_tls, ca_file,
 * allow_insecure_http, the intervals, comments, keys this version does not
 * know - is kept exactly as written. The one exception is a `mode=fake`
 * line, which is dropped: settings typed for a real server are meant to be
 * used.
 *
 * THE SECRET NEVER TRAVELS BACK: the screen learns only whether one is
 * stored. A test or save given no new secret uses the stored one, which
 * must then be of the kind the new auth= asks for.
 *
 * TEST builds the configuration the change would make - zabbix.conf as it
 * would be written, parsed by the same zbx_config_parse() the helper starts
 * with, plus the secret - and makes one round on a client of its own:
 * apiinfo.version, user.login for a password, then problem.get. CONNECTED
 * therefore means an authenticated read worked, not merely that something
 * answered. A password session it opened is logged out. The running
 * session is not touched. One test sends at most one user.login: a wrong
 * password costs one try towards Zabbix's lockout per tap, never more.
 *
 * SAVE is TEST and then, only on CONNECTED, the commit:
 *   1. the new zabbix.conf is written to zabbix.conf.new (0644) and synced;
 *      a newly typed secret to secret.new (0600, zbx_config_stage_secret);
 *   2. the stored secret is hard-linked to secret.prev;
 *   3. secret.new is renamed over secret, then zabbix.conf.new over
 *      zabbix.conf;
 *   4. if the second rename fails, secret.prev is renamed back (or the new
 *      secret removed when there was none), so the pair is what it was;
 *   5. secret.prev is removed and both directories are synced.
 * A failure before step 3 leaves both files untouched. A save that is not
 * CONNECTED writes nothing: the previous, working settings stay in use.
 * A power cut between the two renames of step 3 can leave the new secret
 * beside the old zabbix.conf; secret.prev is then still there.
 *
 * Pure C, no LVGL; the transport and the clock are passed in, so the tests
 * run it against the fake server in temporary directories.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_ZBX_SETTINGS_H
#define POCKETOS_ZBX_SETTINGS_H

#include "zabbix/zbx_config.h"
#include "zabbix/zbx_http.h"
#include "zabbix/zbx_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the CONNECTION screen shows and sends. */
struct zbx_settings {
    char url[ZBX_URL_MAX];      /* the frontend: as stored (normalised), or as typed */
    enum zbx_auth auth;
    char user[ZBX_USER_MAX];
    bool secret_stored;         /* read only: a secret of this auth's kind is stored */
};

/* The connection as the files have it now. A missing or broken zabbix.conf
 * still gives what can be read of it; note says what is wrong ("" when the
 * files are usable or simply absent). url is "" when it is not a usable
 * address (so a user:password@ in a hand-written file is never shown). */
void zbx_settings_read(const char *conf_path, const char *secret_path, struct zbx_settings *s,
                       char *note, size_t notelen);

/* zabbix.conf's text with url=, auth= and user= set to s's (a user= line is
 * dropped when s->user is empty, as is mode=fake), everything else kept.
 * 0, or -1 when it would not fit in out or in ZBX_CONF_BYTES_MAX. */
int zbx_settings_compose(const char *old_text, const struct zbx_settings *s, char *out,
                         size_t outlen, char *err, size_t errlen);

/* TEST (save false) or SAVE these settings; new_secret NULL or "" keeps the
 * stored secret. tr reaches the server (the curl transport, or the fake in
 * the simulator and the tests). *saved says whether the files were
 * replaced; text says what happened, for the screen (never a secret). */
enum zbx_cresult zbx_settings_run(bool save, const char *conf_path, const char *secret_path,
                                  const struct zbx_settings *want, const char *new_secret,
                                  struct zbx_transport *tr, int64_t (*now_ms)(void *user),
                                  void *clock_user, bool *saved, char *text, size_t textlen);

/* rename(2) as the commit calls it: a test makes one rename fail to prove
 * the rollback. */
extern int (*zbx_settings_rename)(const char *from, const char *to);

#endif
