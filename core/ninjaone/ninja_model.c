/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "ninja_model.h"
#include <math.h>
#include <string.h>
static bool text_ok(const char *s, size_t n)
{
    const char *end = memchr(s, 0, n);
    if (!end) return false;
    for (; s < end; s++) if ((unsigned char)*s < 32 || *s == 127) return false;
    return true;
}
bool ninja_snapshot_valid(const struct ninja_snapshot *s)
{
    if (s->magic != NINJA_MAGIC || s->error < NINJA_OK || s->error > NINJA_HELPER ||
        s->retry_s < 0 || s->retry_s > 86400 || s->demo < 0 || s->demo > 1 ||
        s->partial < 0 || s->partial > 1 || s->norgs > NINJA_ORGS ||
        s->ndevices > NINJA_DEVICES || s->nalerts > NINJA_ALERTS || s->updated < 0) return false;
    for (unsigned i = 0; i < s->norgs; i++)
        if (s->orgs[i].id <= 0 || !text_ok(s->orgs[i].name, NINJA_TEXT)) return false;
    for (unsigned i = 0; i < s->ndevices; i++) {
        const struct ninja_device *d = &s->devices[i];
        if (d->id <= 0 || d->org < 0 || d->offline < -1 || d->offline > 1 ||
            !isfinite(d->last_contact) || d->last_contact < 0 ||
            !text_ok(d->name, NINJA_TEXT)) return false;
    }
    for (unsigned i = 0; i < s->nalerts; i++)
        if (s->alerts[i].device <= 0 || !text_ok(s->alerts[i].uid, 80) ||
            !text_ok(s->alerts[i].message, NINJA_TEXT) || !text_ok(s->alerts[i].severity, 64)) return false;
    return true;
}
const char *ninja_error_text(int error)
{
    static const char *const text[] = { "Updated", "Setup needed: provision a monitoring token locally",
        "Authentication expired: sign in locally, then refresh", "Access denied: check monitoring scope and permissions",
        "Rate limited: waiting before another request", "Network or TLS failure: check connection and system clock",
        "Malformed API response", "Response exceeds the monitoring limit", "Monitoring helper failed or timed out: check local installation" };
    return error >= NINJA_OK && error <= NINJA_HELPER ? text[error] : "Helper failed";
}
const struct ninja_device *ninja_device_find(const struct ninja_snapshot *s, int id)
{
    for (unsigned i = 0; i < s->ndevices; i++) if (s->devices[i].id == id) return &s->devices[i];
    return NULL;
}
const char *ninja_org_name(const struct ninja_snapshot *s, int id)
{
    for (unsigned i = 0; i < s->norgs; i++) if (s->orgs[i].id == id) return s->orgs[i].name;
    return "Unknown organization";
}
unsigned ninja_affected(const struct ninja_snapshot *s, int org)
{
    unsigned count = 0;
    for (unsigned i = 0; i < s->nalerts; i++) {
        const struct ninja_device *d = ninja_device_find(s, s->alerts[i].device);
        bool seen = false;
        if (org && (!d || d->org != org)) continue;
        for (unsigned j = 0; j < i; j++) if (s->alerts[j].device == s->alerts[i].device) seen = true;
        if (!seen) count++;
    }
    return count;
}
