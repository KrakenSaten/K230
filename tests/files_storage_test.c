/*
 * Files' Storage screen text: sysd's storage.status read into a struct, and
 * the words each state puts on screen (apps/files/files_storage.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "files_storage.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static void read_json(const char *json, struct files_usb *u)
{
    cJSON *r = json ? cJSON_Parse(json) : NULL;

    files_usb_parse(r, u);
    cJSON_Delete(r);
}

static int caption_is(const struct files_usb *u, const char *want)
{
    char line[256];

    files_usb_caption(line, sizeof(line), u);
    if (strcmp(line, want) != 0) {
        printf("     got \"%s\", want \"%s\"\n", line, want);
        return 0;
    }
    return 1;
}

int main(void)
{
    struct files_usb u;
    char line[128];

    read_json(NULL, &u);
    check("no answer from sysd: unknown", u.state == FILES_USB_UNKNOWN && !files_usb_can_eject(&u));
    check("and says so", caption_is(&u, "Storage service not answering"));

    read_json("{\"usb\":{\"state\":\"absent\",\"present\":false,\"device\":null,\"filesystem\":null,"
              "\"label\":null,\"mount_path\":null,\"total_bytes\":null,\"free_bytes\":null,"
              "\"safe_to_remove\":false,\"error\":null}}",
              &u);
    check("absent", u.state == FILES_USB_ABSENT && u.total == -1 && !files_usb_can_eject(&u));
    check("absent: Not connected", caption_is(&u, "Not connected"));

    read_json("{\"usb\":{\"state\":\"mounted\",\"present\":true,\"device\":\"/dev/sda1\","
              "\"filesystem\":\"FAT32\",\"label\":\"SANDISK\",\"mount_path\":\"/media/usb\","
              "\"total_bytes\":61505273856,\"free_bytes\":56908316672,\"safe_to_remove\":false,\"error\":null}}",
              &u);
    check("mounted: state, path and space", u.state == FILES_USB_MOUNTED && strcmp(u.path, "/media/usb") == 0 &&
                                                u.total == 61505273856LL && u.free == 56908316672LL);
    check("mounted: Eject is offered", files_usb_can_eject(&u));
    check("mounted: label, filesystem, free of total",
          caption_is(&u, "SANDISK \xC2\xB7 FAT32 \xC2\xB7 53 GB free of 57 GB"));

    read_json("{\"usb\":{\"state\":\"mounted\",\"filesystem\":\"FAT32\",\"label\":null,"
              "\"mount_path\":\"/media/usb\",\"total_bytes\":null,\"free_bytes\":null}}",
              &u);
    check("mounted without a label or space: just the filesystem", caption_is(&u, "FAT32"));

    read_json("{\"usb\":{\"state\":\"mounted\",\"filesystem\":\"FAT32\",\"mount_path\":null}}", &u);
    check("mounted with nowhere to open is an error", u.state == FILES_USB_ERROR && !files_usb_can_eject(&u));

    read_json("{\"usb\":{\"state\":\"ejecting\",\"filesystem\":\"FAT32\"}}", &u);
    check("ejecting: no second Eject", u.state == FILES_USB_EJECTING && !files_usb_can_eject(&u));
    check("ejecting: says so", caption_is(&u, "Ejecting\xE2\x80\xA6"));

    read_json("{\"usb\":{\"state\":\"ejected\",\"safe_to_remove\":true,\"filesystem\":\"FAT32\"}}", &u);
    check("ejected: Safe to remove", u.state == FILES_USB_EJECTED && caption_is(&u, "Safe to remove"));

    read_json("{\"usb\":{\"state\":\"unsupported\",\"filesystem\":\"exFAT\"}}", &u);
    check("exFAT: named, with what to use", caption_is(&u, "exFAT is not supported \xE2\x80\x94 use FAT32"));
    read_json("{\"usb\":{\"state\":\"unsupported\",\"filesystem\":\"NTFS\"}}", &u);
    check("NTFS: named", caption_is(&u, "NTFS is not supported \xE2\x80\x94 use FAT32"));
    read_json("{\"usb\":{\"state\":\"unsupported\",\"filesystem\":\"unknown\"}}", &u);
    check("unknown format", caption_is(&u, "Format not recognised \xE2\x80\x94 use FAT32"));

    read_json("{\"usb\":{\"state\":\"error\",\"error\":\"The drive could not be mounted: Invalid argument\"}}", &u);
    check("error: sysd's reason", caption_is(&u, "The drive could not be mounted: Invalid argument"));
    read_json("{\"usb\":{\"state\":\"error\"}}", &u);
    check("error without a reason", caption_is(&u, "The drive cannot be used"));
    read_json("{\"usb\":{\"state\":\"melting\"}}", &u);
    check("a state this Files does not know is an error, never mountable",
          u.state == FILES_USB_ERROR && !files_usb_can_eject(&u));

    read_json("{\"usb\":{\"state\":\"mounted\",\"filesystem\":\"FAT32\",\"label\":\"A\\u00ff\\u00fe\","
              "\"mount_path\":\"/media/usb\"}}",
              &u);
    check("a label is drawn as given when it is UTF-8", caption_is(&u, "A\xC3\xBF\xC3\xBE \xC2\xB7 FAT32"));

    files_space_caption(line, sizeof(line), 601849856LL, 86716416LL);
    check("internal: free of total", strcmp(line, "83 MB free of 574 MB") == 0);
    files_space_caption(line, sizeof(line), -1, -1);
    check("internal: nothing when unknown", line[0] == '\0');

    printf("files_storage_test: %d/%d checks passed\n", checks - failed, checks);
    return failed ? 1 : 0;
}
