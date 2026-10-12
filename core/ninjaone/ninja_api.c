/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "ninja_api.h"
#include <cjson/cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static cJSON *field(cJSON *v, const char *key) { return cJSON_GetObjectItemCaseSensitive(v, key); }
static int ident(cJSON *v)
{
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble > 0 &&
        v->valuedouble <= INT32_MAX && floor(v->valuedouble) == v->valuedouble ? (int)v->valuedouble : 0;
}
static void copy_text(char *out, size_t cap, cJSON *v, const char *fallback)
{
    const char *p = cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : fallback;
    size_t used = 0;
    while (*p && used + 1 < cap) {
        unsigned char first = (unsigned char)*p;
        unsigned width = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
                         first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        bool valid = width != 0;
        for (unsigned i = 1; valid && i < width; i++)
            if (!p[i] || ((unsigned char)p[i] & 0xc0) != 0x80) valid = false;
        if (valid && width > 2) {
            unsigned char second = (unsigned char)p[1];
            if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
                (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) valid = false;
        }
        if (!valid) { out[used++] = '?'; p++; continue; }
        if (used + width >= cap) break;
        if (width == 1 && (first < 32 || first == 127)) out[used++] = ' ';
        else { memcpy(out + used, p, width); used += width; }
        p += width;
    }
    out[used] = 0;
}
int ninja_decode(struct ninja_snapshot *s, char kind, const char *body, size_t len,
                 int after, int *next, unsigned *rows)
{
    const char *end = NULL;
    cJSON *root, *v;
    int rc = NINJA_MALFORMED, cursor = after;
    *rows = 0; *next = after;
    if (!len || len > NINJA_RESPONSE_MAX || memchr(body, 0, len)) return rc;
    root = cJSON_ParseWithLengthOpts(body, len, &end, false);
    if (!root) return rc;
    while (end < body + len && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) end++;
    if (end != body + len || !cJSON_IsArray(root)) goto done;
    cJSON_ArrayForEach(v, root) {
        int id;
        if (!cJSON_IsObject(v)) goto done;
        (*rows)++;
        if (kind == 'o' || kind == 'd') {
            id = ident(field(v, "id"));
            if (id <= cursor) goto done;
            cursor = id;
            if (kind == 'o') {
                if (s->norgs == NINJA_ORGS) { s->partial = 1; continue; }
                struct ninja_org *o = &s->orgs[s->norgs++];
                o->id = id; copy_text(o->name, sizeof(o->name), field(v, "name"), "Unnamed organization");
            } else {
                cJSON *offline = field(v, "offline"), *contact = field(v, "lastContact");
                if (s->ndevices == NINJA_DEVICES) { s->partial = 1; continue; }
                struct ninja_device *d = &s->devices[s->ndevices++];
                d->id = id; d->org = ident(field(v, "organizationId"));
                d->offline = cJSON_IsBool(offline) ? cJSON_IsTrue(offline) : -1;
                d->last_contact = cJSON_IsNumber(contact) && isfinite(contact->valuedouble) &&
                    contact->valuedouble > 0 ? contact->valuedouble : 0;
                cJSON *name = field(v, "displayName");
                if (!cJSON_IsString(name) || !name->valuestring[0]) name = field(v, "systemName");
                copy_text(d->name, sizeof(d->name), name, "Unnamed device");
            }
        } else if (kind == 'a') {
            id = ident(field(v, "deviceId"));
            if (!id) goto done;
            if (s->nalerts == NINJA_ALERTS) { s->partial = 1; continue; }
            struct ninja_alert *a = &s->alerts[s->nalerts++];
            a->device = id;
            copy_text(a->uid, sizeof(a->uid), field(v, "uid"), "Unknown alert");
            copy_text(a->message, sizeof(a->message), field(v, "message"), "No alert message");
            copy_text(a->severity, sizeof(a->severity), field(v, "severity"), "Unknown");
        } else goto done;
    }
    *next = cursor; rc = NINJA_OK;
done:
    cJSON_Delete(root); return rc;
}
int ninja_collect(struct ninja_snapshot *s, ninja_get_fn get, void *ctx, unsigned page_size)
{
    char path[100];
    if (!page_size || page_size > 100) return NINJA_MALFORMED;
    memset(s, 0, sizeof(*s)); s->magic = NINJA_MAGIC;
    for (int list = 0; list < 3; list++) {
        char kind = "oda"[list];
        int after = 0;
        for (unsigned page = 0; page < 10; page++) {
            char *body = NULL;
            size_t len = 0;
            unsigned rows = 0;
            int next = after, rc;
            if (kind == 'a') snprintf(path, sizeof(path), "/v2/alerts");
            else snprintf(path, sizeof(path), "/v2/%s?pageSize=%u&after=%d",
                          kind == 'o' ? "organizations" : "devices", page_size, after);
            rc = get(ctx, path, &body, &len, &s->retry_s);
            if (!rc) rc = ninja_decode(s, kind, body, len, after, &next, &rows);
            free(body);
            if (rc) { s->error = rc; return rc; }
            if (kind == 'a' || rows < page_size) break;
            if (rows > page_size || next <= after) { s->error = NINJA_MALFORMED; return s->error; }
            after = next;
            if ((kind == 'o' && s->norgs == NINJA_ORGS) ||
                (kind == 'd' && s->ndevices == NINJA_DEVICES) || page == 9) {
                s->partial = 1; break;
            }
        }
    }
    s->updated = (int64_t)time(NULL); return NINJA_OK;
}
int ninja_base_valid(const char *base)
{
    /* Tenant region selection only: never send a token to an arbitrary URL. */
    static const char *const hosts[] = { "https://app.ninjarmm.com", "https://eu.ninjarmm.com",
        "https://oc.ninjarmm.com", "https://ca.ninjarmm.com", "https://us2.ninjarmm.com",
        "https://app.ninjaone.com" };
    for (unsigned i = 0; i < sizeof(hosts)/sizeof(hosts[0]); i++) if (!strcmp(base, hosts[i])) return 1;
    return 0;
}
