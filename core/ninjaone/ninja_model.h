/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef NINJA_MODEL_H
#define NINJA_MODEL_H
#include <stdbool.h>
#include <stdint.h>
#define NINJA_ORGS 250
#define NINJA_DEVICES 500
#define NINJA_ALERTS 500
#define NINJA_TEXT 160
#define NINJA_MAGIC 0x4e494e32u
#define NINJA_REFRESH_S 120
#define NINJA_RESPONSE_MAX (2u * 1024u * 1024u)
enum ninja_error { NINJA_OK, NINJA_SETUP, NINJA_AUTH, NINJA_DENIED, NINJA_RATE,
                   NINJA_NETWORK, NINJA_MALFORMED, NINJA_TOO_LARGE, NINJA_HELPER,
                   NINJA_RENEWING, NINJA_DISCONNECTED, NINJA_RENEW_FAILED };
struct ninja_org { int32_t id; char name[NINJA_TEXT]; };
struct ninja_device {
    int32_t id, org;
    int32_t offline; /* -1 unknown; exactly the API boolean otherwise */
    double last_contact; /* Raw API timestamp; 0 unknown. Unit not inferred. */
    char name[NINJA_TEXT];
};
struct ninja_alert { int32_t device; char uid[80], message[NINJA_TEXT], severity[64]; };
/* Private, same-build helper IPC. No JSON/parser/credentials in the shell.
 * A complete fixed-size frame replaces the cache atomically. */
struct ninja_snapshot {
    uint32_t magic;
    int32_t error, retry_s, demo, partial;
    int64_t updated, expires;
    char profile[25]; /* Nonsecret tenant namespace, never a credential. */
    uint32_t norgs, ndevices, nalerts;
    struct ninja_org orgs[NINJA_ORGS];
    struct ninja_device devices[NINJA_DEVICES];
    struct ninja_alert alerts[NINJA_ALERTS];
};
bool ninja_snapshot_valid(const struct ninja_snapshot *s);
const char *ninja_error_text(int error);
const struct ninja_device *ninja_device_find(const struct ninja_snapshot *s, int id);
const char *ninja_org_name(const struct ninja_snapshot *s, int id);
unsigned ninja_affected(const struct ninja_snapshot *s, int org);
#endif
