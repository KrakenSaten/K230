/*
 * sysd_storage tests: the USB drive's probe and state machine
 * (services/sysd/sysd_storage.h).
 *
 * The drive is a temporary tree: a /sys/block whose sda links into a path
 * with "usb" in it (as the kernel's does), a /dev whose sda1 is a regular
 * file holding a FAT image, and a /proc/mounts the fake mount and umount
 * below edit. Those two are what the module's ops would do to the machine;
 * here they record what they were asked, in files, because the eject's
 * umount runs in a child process. A flag file makes umount answer EBUSY,
 * another makes it slow, and the mount can be told to fail.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "sysd_storage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failed;
static int checks;
static char root[] = "/tmp/pos_storage.XXXXXX";
static char p_sys[256], p_dev[256], p_mounts[256], p_mp[256], p_log[256], p_busy[256], p_slow[256];
static int mount_errno;
static int mounts_done;

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

static void sh(const char *fmt, const char *a)
{
    char cmd[1024];

    snprintf(cmd, sizeof(cmd), fmt, a, a, a);
    if (system(cmd) != 0) {
        fprintf(stderr, "setup failed: %s\n", cmd);
        exit(1);
    }
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        exit(1);
    }
    fputs(text, f);
    fclose(f);
}

static int slurp(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    size_t got;

    out[0] = '\0';
    if (!f) {
        return -1;
    }
    got = fread(out, 1, n - 1, f);
    out[got] = '\0';
    fclose(f);
    return 0;
}

static int exists(const char *path)
{
    struct stat sb;

    return stat(path, &sb) == 0;
}

/* ---- the fake machine ------------------------------------------------------ */

static void log_line(const char *line)
{
    FILE *f = fopen(p_log, "a");

    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

static int fake_mount(const char *device, const char *dir, void *user)
{
    char line[600];
    FILE *f;

    (void)user;
    snprintf(line, sizeof(line), "mount %s %s", device, dir);
    log_line(line);
    if (mount_errno) {
        return -mount_errno;
    }
    f = fopen(p_mounts, "a");
    fprintf(f, "%s %s vfat rw,nosuid,nodev,noexec,noatime 0 0\n", device, dir);
    fclose(f);
    mounts_done++;
    return 0;
}

/* Drops every line mounted on dir from the mounts file. */
static void unmount_line(const char *dir)
{
    char text[4096];
    char out[4096] = "";
    char *line;
    char *save = NULL;

    slurp(p_mounts, text, sizeof(text));
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char src[512], d[512];

        if (sscanf(line, "%511s %511s", src, d) == 2 && strcmp(d, dir) == 0) {
            continue;
        }
        strncat(out, line, sizeof(out) - strlen(out) - 2);
        strcat(out, "\n");
    }
    put(p_mounts, out);
}

static int fake_umount(const char *dir, bool detach, void *user)
{
    char line[600];

    (void)user;
    snprintf(line, sizeof(line), "umount %s %d", dir, detach ? 1 : 0);
    log_line(line);
    if (exists(p_slow)) {
        usleep(400 * 1000);
    }
    if (!detach && exists(p_busy)) {
        return -EBUSY;
    }
    unmount_line(dir);
    return 0;
}

static const struct sysd_storage_ops fake_ops = { fake_mount, fake_umount, NULL };

/* ---- images ---------------------------------------------------------------- */

static void w16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void w32(uint8_t *p, uint32_t v)
{
    w16(p, v);
    w16(p + 2, v >> 16);
}

/* A FAT boot sector. fat32 picks the layout; the numbers are chosen so the
 * cluster count lands in the type asked for. */
static void fat_sector(uint8_t *b, int bits, const char *boot_label)
{
    memset(b, 0, 512);
    b[0] = 0xEB;
    b[1] = 0x58;
    b[2] = 0x90;
    memcpy(b + 3, "MSWIN4.1", 8);
    w16(b + 11, 512);
    b[16] = 2;
    if (bits == 32) {
        b[13] = 1;               /* 1 sector per cluster */
        w16(b + 14, 32);         /* reserved */
        w32(b + 32, 200000);     /* total sectors: ~196 768 clusters */
        w32(b + 36, 1600);       /* sectors per FAT */
        w32(b + 44, 2);          /* the root directory's cluster */
        b[66] = 0x29;
        memcpy(b + 71, boot_label, 11);
        memcpy(b + 82, "FAT32   ", 8);
    } else if (bits == 16) {
        b[13] = 4;
        w16(b + 14, 4);
        w16(b + 17, 512);        /* root entries */
        w16(b + 19, 40000);
        w16(b + 22, 40);
        b[38] = 0x29;
        memcpy(b + 43, boot_label, 11);
    } else {
        b[13] = 1;
        w16(b + 14, 1);
        w16(b + 17, 224);
        w16(b + 19, 2880);
        w16(b + 22, 9);
        b[38] = 0x29;
        memcpy(b + 43, boot_label, 11);
    }
    b[510] = 0x55;
    b[511] = 0xAA;
}

/* A FAT32 image file: the boot sector, and a root directory that holds a
 * long-name piece, a deleted entry, then the label entry. */
static void write_fat32(const char *path, const char *dir_label)
{
    uint8_t b[512];
    uint8_t e[96];
    struct sysd_fs_probe p;
    int fd;

    fat_sector(b, 32, "BOOTLABEL  ");
    sysd_storage_probe(b, sizeof(b), &p);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || pwrite(fd, b, 512, 0) != 512) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    memset(e, 0, sizeof(e));
    e[0] = 0x41;
    e[11] = 0x0F;               /* a long-name piece */
    memcpy(e + 32, "OLDLABEL   ", 11);
    e[32] = 0xE5;               /* deleted */
    e[32 + 11] = 0x08;
    memcpy(e + 64, dir_label, 11);
    e[64 + 11] = 0x08;          /* the volume label */
    if (pwrite(fd, e, sizeof(e), (off_t)p.root_offset) != (ssize_t)sizeof(e)) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    close(fd);
}

static void write_sector(const char *path, const char *oem)
{
    uint8_t b[512];
    int fd;

    memset(b, 0, sizeof(b));
    b[0] = 0xEB;
    memcpy(b + 3, oem, 8);
    b[510] = 0x55;
    b[511] = 0xAA;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, b, sizeof(b)) != (ssize_t)sizeof(b)) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    close(fd);
}

/* ---- the drive ------------------------------------------------------------- */

/* Plug a USB disk "sda" in: partitioned (sda1) or not, devnum minor m. */
static void plug(bool partitioned, int minor)
{
    char cmd[1024];

    snprintf(cmd, sizeof(cmd),
             "rm -rf %s/devices && mkdir -p %s/devices/platform/usb1/1-1/host0/block/sda%s && "
             "echo 120164352 > %s/devices/platform/usb1/1-1/host0/block/sda/size && "
             "echo 8:%d > %s/devices/platform/usb1/1-1/host0/block/sda/dev && "
             "ln -sfn ../../devices/platform/usb1/1-1/host0/block/sda %s/sda",
             root, root, partitioned ? "/sda1" : "", root, minor * 16, root, p_sys);
    if (system(cmd) != 0) {
        exit(1);
    }
    if (partitioned) {
        snprintf(cmd, sizeof(cmd), "echo 8:%d > %s/devices/platform/usb1/1-1/host0/block/sda/sda1/dev",
                 minor * 16 + 1, root);
        if (system(cmd) != 0) {
            exit(1);
        }
    }
}

static void unplug(void)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/sda", p_sys);
    unlink(path);
}

static const char *state(struct sysd_storage *st)
{
    return sysd_storage_state_name(st->state);
}

static cJSON *status(struct sysd_storage *st)
{
    return sysd_storage_status(st);
}

static const char *sfield(cJSON *r, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "usb"), key);

    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static const cJSON *field(cJSON *r, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "usb"), key);
}

/* Scan until the eject child has been collected (2 s at most). */
static void wait_eject(struct sysd_storage *st)
{
    int i;

    for (i = 0; i < 200 && sysd_storage_busy(st); i++) {
        usleep(10 * 1000);
        sysd_storage_scan(st);
    }
}

static int log_has(const char *text)
{
    char buf[8192];

    slurp(p_log, buf, sizeof(buf));
    return strstr(buf, text) != NULL;
}

static int log_count(const char *text)
{
    char buf[8192];
    const char *p = buf;
    int n = 0;

    slurp(p_log, buf, sizeof(buf));
    while ((p = strstr(p, text)) != NULL) {
        n++;
        p += strlen(text);
    }
    return n;
}

static void fresh(struct sysd_storage *st, const struct sysd_storage_paths *paths)
{
    put(p_log, "");
    mount_errno = 0;
    unlink(p_busy);
    unlink(p_slow);
    sysd_storage_init(st, paths, &fake_ops);
}

/* ---- tests ----------------------------------------------------------------- */

static void test_probe(void)
{
    uint8_t b[512];
    struct sysd_fs_probe p;

    fat_sector(b, 32, "MY STICK   ");
    check("probe: FAT32 by its cluster count", sysd_storage_probe(b, 512, &p) == 0 && p.fat &&
                                                   strcmp(p.fs, "FAT32") == 0);
    check("probe: the FAT32 boot sector label, trailing spaces trimmed", strcmp(p.label, "MY STICK") == 0);
    check("probe: the root directory of FAT32 is its root cluster",
          p.root_offset == (32 + 2 * 1600) * 512ull && p.root_bytes == 512);
    fat_sector(b, 16, "NO NAME    ");
    check("probe: FAT16", sysd_storage_probe(b, 512, &p) == 0 && p.fat && strcmp(p.fs, "FAT16") == 0);
    check("probe: NO NAME is no label", p.label[0] == '\0');
    check("probe: the FAT16 root directory follows the FATs",
          p.root_offset == (4 + 2 * 40) * 512ull && p.root_bytes == 512 * 32);
    fat_sector(b, 12, "FLOPPY\x01    ");
    check("probe: FAT12", sysd_storage_probe(b, 512, &p) == 0 && p.fat && strcmp(p.fs, "FAT12") == 0);
    check("probe: an unprintable label byte shows as ?", strcmp(p.label, "FLOPPY?") == 0);

    memset(b, 0, sizeof(b));
    memcpy(b + 3, "EXFAT   ", 8);
    check("probe: exFAT is named, and is not FAT", sysd_storage_probe(b, 512, &p) == 0 && !p.fat &&
                                                       strcmp(p.fs, "exFAT") == 0);
    memcpy(b + 3, "NTFS    ", 8);
    check("probe: NTFS is named, and is not FAT", sysd_storage_probe(b, 512, &p) == 0 && !p.fat &&
                                                      strcmp(p.fs, "NTFS") == 0);
    memset(b, 0, sizeof(b));
    check("probe: zeros are nothing", sysd_storage_probe(b, 512, &p) == 0 && !p.fat && p.fs[0] == '\0');
    fat_sector(b, 32, "X          ");
    b[510] = 0;
    check("probe: no 55 AA signature, no FAT", sysd_storage_probe(b, 512, &p) == 0 && !p.fat);
    fat_sector(b, 32, "X          ");
    b[13] = 3;
    check("probe: a cluster size that is not a power of two, no FAT", sysd_storage_probe(b, 512, &p) == 0 && !p.fat);
    fat_sector(b, 32, "X          ");
    w16(b + 11, 500);
    check("probe: an impossible sector size, no FAT", sysd_storage_probe(b, 512, &p) == 0 && !p.fat);
    check("probe: less than a sector is refused", sysd_storage_probe(b, 100, &p) == -EINVAL);
}

static void test_identify(void)
{
    char path[512];
    struct sysd_fs_probe p;
    int fd;

    snprintf(path, sizeof(path), "%s/image", root);
    write_fat32(path, "SANDISK    ");
    fd = open(path, O_RDONLY);
    check("identify: reads a FAT32 image", fd >= 0 && sysd_storage_identify(fd, &p) == 0 && p.fat);
    check("identify: the root directory label wins over the boot sector's, "
          "past a long-name piece and a deleted label",
          strcmp(p.label, "SANDISK") == 0);
    if (fd >= 0) {
        close(fd);
    }
    unlink(path);
}

static void test_machine(const struct sysd_storage_paths *paths)
{
    struct sysd_storage st;
    char dev1[512];
    char dev0[512];
    char mounts[4096];
    char want[1024];
    char msg[160];
    cJSON *r;

    snprintf(dev1, sizeof(dev1), "%s/sda1", p_dev);
    snprintf(dev0, sizeof(dev0), "%s/sda", p_dev);

    /* Nothing plugged in; a disk that is not on USB is not ours either. */
    fresh(&st, paths);
    r = status(&st);
    check("absent: state absent, not present", strcmp(sfield(r, "state"), "absent") == 0 &&
                                                   cJSON_IsFalse(field(r, "present")));
    check("absent: no device, filesystem or space", cJSON_IsNull(field(r, "device")) &&
                                                       cJSON_IsNull(field(r, "filesystem")) &&
                                                       cJSON_IsNull(field(r, "total_bytes")));
    cJSON_Delete(r);
    sh("mkdir -p %s/devices/platform/mmc0/block/sdq && echo 100 > %s/devices/platform/mmc0/block/sdq/size", root);
    {
        char cmd[512];

        snprintf(cmd, sizeof(cmd), "echo 8:0 > %s/devices/platform/mmc0/block/sdq/dev && "
                                   "ln -sfn ../../devices/platform/mmc0/block/sdq %s/sdq", root, p_sys);
        check("setup", system(cmd) == 0);
    }
    sysd_storage_scan(&st);
    check("absent: an sd disk that is not on USB is not mounted", strcmp(state(&st), "absent") == 0 &&
                                                                     mounts_done == 0);
    {
        char path[512];

        snprintf(path, sizeof(path), "%s/sdq", p_sys);
        unlink(path);
    }

    /* A FAT32 stick. */
    write_fat32(dev1, "SANDISK    ");
    plug(true, 0);
    sysd_storage_scan(&st);
    check("plug: a FAT32 partition is mounted", strcmp(state(&st), "mounted") == 0 && mounts_done == 1);
    snprintf(want, sizeof(want), "mount %s %s", dev1, p_mp);
    check("plug: its first partition, at the mount point", log_has(want));
    check("plug: the mount point was made", exists(p_mp));
    r = status(&st);
    check("status: mounted, present, not safe to remove",
          strcmp(sfield(r, "state"), "mounted") == 0 && cJSON_IsTrue(field(r, "present")) &&
              cJSON_IsFalse(field(r, "safe_to_remove")));
    check("status: device, filesystem and label", sfield(r, "device") && strcmp(sfield(r, "device"), dev1) == 0 &&
                                                      strcmp(sfield(r, "filesystem"), "FAT32") == 0 &&
                                                      strcmp(sfield(r, "label"), "SANDISK") == 0);
    check("status: the mount path and its space",
          sfield(r, "mount_path") && strcmp(sfield(r, "mount_path"), p_mp) == 0 &&
              cJSON_IsNumber(field(r, "total_bytes")) && field(r, "total_bytes")->valuedouble > 0 &&
              cJSON_IsNumber(field(r, "free_bytes")) &&
              field(r, "free_bytes")->valuedouble <= field(r, "total_bytes")->valuedouble);
    check("status: no error", cJSON_IsNull(field(r, "error")));
    cJSON_Delete(r);
    sysd_storage_scan(&st);
    sysd_storage_scan(&st);
    check("plug: scanning again mounts nothing more", mounts_done == 1);

    /* Eject. */
    check("eject: accepted", sysd_storage_eject(&st, msg, sizeof(msg)) == 0 && strcmp(state(&st), "ejecting") == 0);
    wait_eject(&st);
    check("eject: ends ejected", strcmp(state(&st), "ejected") == 0);
    snprintf(want, sizeof(want), "umount %s 0", p_mp);
    check("eject: a plain unmount, never a lazy one", log_has(want) && log_count(" 1\n") == 0);
    slurp(p_mounts, mounts, sizeof(mounts));
    check("eject: nothing is mounted any more", strstr(mounts, p_mp) == NULL);
    check("eject: the mount point is gone, so nothing can be saved there by mistake", !exists(p_mp));
    r = status(&st);
    check("status: ejected is present and safe to remove",
          strcmp(sfield(r, "state"), "ejected") == 0 && cJSON_IsTrue(field(r, "present")) &&
              cJSON_IsTrue(field(r, "safe_to_remove")) && cJSON_IsNull(field(r, "mount_path")) &&
              cJSON_IsNull(field(r, "total_bytes")));
    cJSON_Delete(r);
    sysd_storage_scan(&st);
    check("ejected: the same drive is not mounted again while it stays in", mounts_done == 1 &&
                                                                         strcmp(state(&st), "ejected") == 0);
    check("ejected: a second eject is refused, nothing is mounted", sysd_storage_eject(&st, msg, sizeof(msg)) == -1 &&
                                                                    strstr(msg, "no USB drive") != NULL);

    /* Out and in again. */
    unplug();
    sysd_storage_scan(&st);
    check("unplug: absent again", strcmp(state(&st), "absent") == 0);
    plug(true, 0);
    sysd_storage_scan(&st);
    check("replug: mounted again", strcmp(state(&st), "mounted") == 0 && mounts_done == 2);

    /* A busy drive stays mounted, and says so. */
    put(p_busy, "");
    check("busy: eject accepted", sysd_storage_eject(&st, msg, sizeof(msg)) == 0);
    wait_eject(&st);
    check("busy: still mounted", strcmp(state(&st), "mounted") == 0);
    check("busy: the reason is given", strstr(st.error, "in use") != NULL);
    check("busy: no lazy unmount was tried", log_count(" 1\n") == 0);
    slurp(p_mounts, mounts, sizeof(mounts));
    check("busy: the mount is still there", strstr(mounts, p_mp) != NULL);
    r = status(&st);
    check("status: the eject's failure is in error", sfield(r, "error") && strstr(sfield(r, "error"), "in use"));
    cJSON_Delete(r);
    unlink(p_busy);

    /* A second eject while one runs. */
    put(p_slow, "");
    check("slow: eject accepted", sysd_storage_eject(&st, msg, sizeof(msg)) == 0);
    check("slow: another while it runs is busy", sysd_storage_eject(&st, msg, sizeof(msg)) == -2);
    r = status(&st);
    check("status: ejecting while it runs", strcmp(sfield(r, "state"), "ejecting") == 0);
    cJSON_Delete(r);
    wait_eject(&st);
    check("slow: and then ejected, with the earlier error cleared", strcmp(state(&st), "ejected") == 0 &&
                                                                    st.error[0] == '\0');
    unlink(p_slow);

    /* Pulled out while mounted. */
    unplug();
    sysd_storage_scan(&st);
    plug(true, 0);
    sysd_storage_scan(&st);
    check("setup: mounted for the pull", strcmp(state(&st), "mounted") == 0);
    put(p_log, "");
    unplug();
    sysd_storage_scan(&st);
    snprintf(want, sizeof(want), "umount %s 0", p_mp);
    check("pulled: the stale mount is unmounted, plainly", log_has(want) && log_count(" 1\n") == 0);
    slurp(p_mounts, mounts, sizeof(mounts));
    check("pulled: and gone", strstr(mounts, p_mp) == NULL && !exists(p_mp));
    check("pulled: absent", strcmp(state(&st), "absent") == 0);

    plug(true, 0);
    sysd_storage_scan(&st);
    put(p_log, "");
    put(p_busy, "");
    unplug();
    sysd_storage_scan(&st);
    snprintf(want, sizeof(want), "umount %s 1", p_mp);
    check("pulled busy: the plain unmount first, detached only when that is refused",
          log_count("umount") == 2 && log_has(want));
    slurp(p_mounts, mounts, sizeof(mounts));
    check("pulled busy: gone", strstr(mounts, p_mp) == NULL && strcmp(state(&st), "absent") == 0);
    unlink(p_busy);

    /* Unmounted from a terminal: an eject, not a reason to mount it again. */
    plug(true, 0);
    sysd_storage_scan(&st);
    unmount_line(p_mp);
    sysd_storage_scan(&st);
    check("outside: unmounted elsewhere reads as ejected", strcmp(state(&st), "ejected") == 0);
    sysd_storage_scan(&st);
    slurp(p_mounts, mounts, sizeof(mounts));
    check("outside: and is not mounted again", strstr(mounts, p_mp) == NULL);
    unplug();
    sysd_storage_scan(&st);

    /* An earlier sysd's mount is kept, not made twice. */
    fresh(&st, paths);
    plug(true, 0);
    mkdir(p_mp, 0755);
    snprintf(want, sizeof(want), "%s %s vfat rw 0 0\n", dev1, p_mp);
    put(p_mounts, want);
    sysd_storage_scan(&st);
    check("adopt: a mount already there is taken as mounted", strcmp(state(&st), "mounted") == 0 &&
                                                                 log_count("mount ") == 0);
    check("adopt: and its label read", strcmp(st.probe.label, "SANDISK") == 0);
    unmount_line(p_mp);
    rmdir(p_mp);
    unplug();
    sysd_storage_scan(&st);

    /* The mount point taken by something else. */
    fresh(&st, paths);
    plug(true, 0);
    snprintf(want, sizeof(want), "tmpfs %s tmpfs rw 0 0\n", p_mp);
    put(p_mounts, want);
    sysd_storage_scan(&st);
    check("taken: the mount point in use by another mount is an error, untouched",
          strcmp(state(&st), "error") == 0 && strstr(st.error, "in use by tmpfs") && log_count("mount") == 0);
    put(p_mounts, "");
    unplug();
    sysd_storage_scan(&st);

    /* exFAT and NTFS are named and left alone. */
    fresh(&st, paths);
    write_sector(dev1, "EXFAT   ");
    plug(true, 0);
    r = status(&st);
    check("exFAT: unsupported, not mounted", strcmp(sfield(r, "state"), "unsupported") == 0 &&
                                                strcmp(sfield(r, "filesystem"), "exFAT") == 0 &&
                                                log_count("mount") == 0);
    cJSON_Delete(r);
    unplug();
    sysd_storage_scan(&st);
    write_sector(dev1, "NTFS    ");
    plug(true, 0);
    r = status(&st);
    check("NTFS: unsupported, not mounted", strcmp(sfield(r, "state"), "unsupported") == 0 &&
                                               strcmp(sfield(r, "filesystem"), "NTFS") == 0 &&
                                               log_count("mount") == 0);
    cJSON_Delete(r);
    unplug();
    sysd_storage_scan(&st);
    write_sector(dev1, "WHATEVER");
    plug(true, 0);
    r = status(&st);
    check("unknown: unsupported, filesystem unknown", strcmp(sfield(r, "state"), "unsupported") == 0 &&
                                                         strcmp(sfield(r, "filesystem"), "unknown") == 0);
    cJSON_Delete(r);
    unplug();
    sysd_storage_scan(&st);

    /* A FAT drive the kernel will not mount: said once, not retried. */
    fresh(&st, paths);
    write_fat32(dev1, "SANDISK    ");
    mount_errno = EINVAL;
    plug(true, 0);
    sysd_storage_scan(&st);
    check("refused: an error with the reason", strcmp(state(&st), "error") == 0 &&
                                                  strstr(st.error, "could not be mounted") != NULL);
    check("refused: the empty mount point is removed again", !exists(p_mp));
    sysd_storage_scan(&st);
    sysd_storage_scan(&st);
    check("refused: not retried while the drive stays in", log_count("mount ") == 1);
    unplug();
    sysd_storage_scan(&st);
    mount_errno = 0;

    /* No partition table: the whole disk. */
    fresh(&st, paths);
    write_fat32(dev0, "WHOLE      ");
    plug(false, 0);
    sysd_storage_scan(&st);
    snprintf(want, sizeof(want), "mount %s %s", dev0, p_mp);
    check("whole disk: a FAT disk without partitions is mounted whole", strcmp(state(&st), "mounted") == 0 &&
                                                                          log_has(want));
    check("whole disk: its label", strcmp(st.probe.label, "WHOLE") == 0);
}

int main(void)
{
    struct sysd_storage_paths paths;

    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(p_sys, sizeof(p_sys), "%s/sys/block", root);
    snprintf(p_dev, sizeof(p_dev), "%s/dev", root);
    snprintf(p_mounts, sizeof(p_mounts), "%s/mounts", root);
    snprintf(p_mp, sizeof(p_mp), "%s/media/usb", root);
    snprintf(p_log, sizeof(p_log), "%s/ops.log", root);
    snprintf(p_busy, sizeof(p_busy), "%s/umount-busy", root);
    snprintf(p_slow, sizeof(p_slow), "%s/umount-slow", root);
    sh("mkdir -p %s/sys/block %s/dev %s/media", root);
    put(p_mounts, "");
    paths.sys_block = p_sys;
    paths.dev = p_dev;
    paths.mounts = p_mounts;
    paths.mount_point = p_mp;

    test_probe();
    test_identify();
    test_machine(&paths);

    sh("rm -rf %s", root);
    printf("sysd_storage_test: %d/%d checks passed\n", checks - failed, checks);
    return failed ? 1 : 0;
}
