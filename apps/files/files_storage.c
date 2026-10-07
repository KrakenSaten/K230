/*
 * Files' Storage screen text. See files_storage.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "files_storage.h"

#include "files_view.h"

#include <stdio.h>
#include <string.h>

static void copy_string(char *out, size_t out_len, const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    snprintf(out, out_len, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static int64_t number(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsNumber(v) && v->valuedouble >= 0 ? (int64_t)v->valuedouble : -1;
}

void files_usb_parse(const cJSON *result, struct files_usb *u)
{
    static const struct {
        const char *name;
        enum files_usb_state state;
    } states[] = {
        { "absent", FILES_USB_ABSENT },       { "mounted", FILES_USB_MOUNTED },
        { "ejecting", FILES_USB_EJECTING },   { "ejected", FILES_USB_EJECTED },
        { "unsupported", FILES_USB_UNSUPPORTED }, { "error", FILES_USB_ERROR },
    };
    const cJSON *usb = result ? cJSON_GetObjectItemCaseSensitive(result, "usb") : NULL;
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(usb, "state");
    size_t i;

    memset(u, 0, sizeof(*u));
    u->total = u->free = -1;
    if (!cJSON_IsObject(usb) || !cJSON_IsString(st)) {
        return; /* FILES_USB_UNKNOWN */
    }
    u->state = FILES_USB_ERROR; /* a state this Files does not know */
    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        if (strcmp(st->valuestring, states[i].name) == 0) {
            u->state = states[i].state;
        }
    }
    copy_string(u->fs, sizeof(u->fs), usb, "filesystem");
    copy_string(u->label, sizeof(u->label), usb, "label");
    copy_string(u->path, sizeof(u->path), usb, "mount_path");
    copy_string(u->error, sizeof(u->error), usb, "error");
    u->total = number(usb, "total_bytes");
    u->free = number(usb, "free_bytes");
    if (u->state == FILES_USB_MOUNTED && !u->path[0]) {
        u->state = FILES_USB_ERROR; /* nowhere to open */
    }
}

void files_space_caption(char *out, size_t out_len, int64_t total, int64_t avail)
{
    char t[32];
    char a[32];

    if (total < 0 || avail < 0) {
        snprintf(out, out_len, "%s", "");
        return;
    }
    files_view_size(t, sizeof(t), total);
    files_view_size(a, sizeof(a), avail);
    snprintf(out, out_len, "%s free of %s", a, t);
}

void files_usb_caption(char *out, size_t out_len, const struct files_usb *u)
{
    char label[sizeof(u->label)];
    char space[80];

    switch (u->state) {
    case FILES_USB_UNKNOWN:
        snprintf(out, out_len, "%s", "Storage service not answering");
        return;
    case FILES_USB_ABSENT:
        snprintf(out, out_len, "%s", "Not connected");
        return;
    case FILES_USB_EJECTING:
        snprintf(out, out_len, "%s", "Ejecting\xE2\x80\xA6");
        return;
    case FILES_USB_EJECTED:
        snprintf(out, out_len, "%s", "Safe to remove");
        return;
    case FILES_USB_UNSUPPORTED:
        if (u->fs[0] && strcmp(u->fs, "unknown") != 0) {
            snprintf(out, out_len, "%s is not supported \xE2\x80\x94 use FAT32", u->fs);
        } else {
            snprintf(out, out_len, "%s", "Format not recognised \xE2\x80\x94 use FAT32");
        }
        return;
    case FILES_USB_ERROR:
        snprintf(out, out_len, "%s", u->error[0] ? u->error : "The drive cannot be used");
        return;
    case FILES_USB_MOUNTED:
        break;
    }
    files_view_name(label, sizeof(label), u->label);
    files_space_caption(space, sizeof(space), u->total, u->free);
    snprintf(out, out_len, "%s%s%s%s%s", label, label[0] ? " \xC2\xB7 " : "", u->fs[0] ? u->fs : "Mounted",
             space[0] ? " \xC2\xB7 " : "", space);
}

bool files_usb_can_eject(const struct files_usb *u)
{
    return u->state == FILES_USB_MOUNTED;
}
