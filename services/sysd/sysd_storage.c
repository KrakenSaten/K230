/*
 * sysd_storage implementation. See sysd_storage.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "sysd_storage.h"

#include "pocketlog/pocketlog.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

/* How much of a FAT root directory is read for the label entry. Windows puts
 * the label entry first; a whole 32 KiB cluster is far more than enough. */
#define ROOT_READ_MAX (64u * 1024u)

/* ---- the filesystem probe ----------------------------------------------------- */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* An 11-byte FAT label as text: trailing spaces gone, anything that is not
 * printable ASCII shown as '?' (the bytes are in an OEM code page nobody
 * recorded), and the placeholder "NO NAME" read as no label at all. */
static void label_text(char out[12], const uint8_t *raw)
{
    int i;
    int end = 0;

    for (i = 0; i < 11; i++) {
        uint8_t c = raw[i];

        if (i == 0 && c == 0x05) {
            c = 0xE5; /* the escape for a name that really starts with 0xE5 */
        }
        out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        if (out[i] != ' ') {
            end = i + 1;
        }
    }
    out[end] = '\0';
    if (strcmp(out, "NO NAME") == 0) {
        out[0] = '\0';
    }
}

int sysd_storage_probe(const uint8_t *b, size_t n, struct sysd_fs_probe *p)
{
    uint32_t bps;
    uint32_t spc;
    uint32_t reserved;
    uint32_t nfats;
    uint32_t root_entries;
    uint32_t total;
    uint32_t fatsz;
    uint32_t root_secs;
    uint64_t meta;
    uint64_t clusters;

    memset(p, 0, sizeof(*p));
    if (n < 512) {
        return -EINVAL;
    }
    if (memcmp(b + 3, "EXFAT   ", 8) == 0) {
        snprintf(p->fs, sizeof(p->fs), "exFAT");
        return 0;
    }
    if (memcmp(b + 3, "NTFS    ", 8) == 0) {
        snprintf(p->fs, sizeof(p->fs), "NTFS");
        return 0;
    }
    /* The FAT boot sector checks of the Microsoft FAT specification: the
     * signature, a jump, and a BIOS parameter block that adds up. */
    if (b[510] != 0x55 || b[511] != 0xAA || (b[0] != 0xEB && b[0] != 0xE9)) {
        return 0;
    }
    bps = le16(b + 11);
    spc = b[13];
    reserved = le16(b + 14);
    nfats = b[16];
    root_entries = le16(b + 17);
    total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    fatsz = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    if ((bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) || spc == 0 || (spc & (spc - 1)) ||
        reserved == 0 || nfats < 1 || nfats > 2 || total == 0 || fatsz == 0) {
        return 0;
    }
    root_secs = (root_entries * 32 + bps - 1) / bps;
    meta = (uint64_t)reserved + (uint64_t)nfats * fatsz + root_secs;
    if (meta >= total) {
        return 0;
    }
    /* The type is decided by the cluster count and by nothing else. */
    clusters = (total - meta) / spc;
    p->fat = true;
    p->bytes_per_sector = bps;
    p->sectors_per_cluster = spc;
    if (clusters < 65525) {
        snprintf(p->fs, sizeof(p->fs), clusters < 4085 ? "FAT12" : "FAT16");
        if (b[38] == 0x29) {
            label_text(p->label, b + 43);
        }
        p->root_offset = ((uint64_t)reserved + (uint64_t)nfats * fatsz) * bps;
        p->root_bytes = root_entries * 32;
    } else {
        uint32_t root_cluster = le32(b + 44);

        snprintf(p->fs, sizeof(p->fs), "FAT32");
        if (b[66] == 0x29) {
            label_text(p->label, b + 71);
        }
        if (root_cluster >= 2) {
            p->root_offset = (meta + (uint64_t)(root_cluster - 2) * spc) * bps;
            p->root_bytes = spc * bps;
        }
    }
    if (p->root_bytes > ROOT_READ_MAX) {
        p->root_bytes = ROOT_READ_MAX;
    }
    return 0;
}

int sysd_storage_identify(int fd, struct sysd_fs_probe *p)
{
    uint8_t sector[512];
    uint8_t *root;
    ssize_t got;
    uint32_t i;

    got = pread(fd, sector, sizeof(sector), 0);
    if (got < 0) {
        return -errno;
    }
    if (sysd_storage_probe(sector, (size_t)got, p) < 0) {
        return -EIO;
    }
    if (!p->fat || p->root_bytes == 0) {
        return 0;
    }
    root = malloc(p->root_bytes);
    if (!root) {
        return 0; /* the boot sector's label is still there */
    }
    got = pread(fd, root, p->root_bytes, (off_t)p->root_offset);
    for (i = 0; got > 0 && i + 32 <= (uint32_t)got; i += 32) {
        const uint8_t *e = root + i;

        if (e[0] == 0x00) {
            break; /* the end of the directory */
        }
        if (e[0] == 0xE5 || e[11] == 0x0F) {
            continue; /* a deleted entry, or a piece of a long name */
        }
        if ((e[11] & 0x08) && !(e[11] & 0x10)) {
            label_text(p->label, e);
            break;
        }
    }
    free(root);
    return 0;
}

/* ---- the real system ---------------------------------------------------------- */

static int real_mount(const char *device, const char *dir, void *user)
{
    (void)user;
    if (mount(device, dir, "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME, SYSD_STORAGE_FAT_DATA) < 0) {
        return -errno;
    }
    return 0;
}

static int real_umount(const char *dir, bool detach, void *user)
{
    (void)user;
    if (umount2(dir, detach ? MNT_DETACH : 0) < 0) {
        return -errno;
    }
    return 0;
}

const struct sysd_storage_ops sysd_storage_real_ops = { real_mount, real_umount, NULL };
const struct sysd_storage_paths sysd_storage_real_paths = { "/sys/block", "/dev", "/proc/mounts",
                                                            SYSD_STORAGE_MOUNT_POINT };

/* ---- what is there -------------------------------------------------------------- */

struct drive {
    char part[32];
    char devnum[16];
    char path[PATH_MAX];
};

static int read_line(const char *path, char *out, size_t out_len)
{
    FILE *f = fopen(path, "re");
    size_t n;

    if (!f) {
        return -1;
    }
    if (!fgets(out, (int)out_len, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    n = strlen(out);
    while (n > 0 && isspace((unsigned char)out[n - 1])) {
        out[--n] = '\0';
    }
    return 0;
}

/* "sd" and letters only: a SCSI disk, never a partition of one. */
static bool scsi_disk_name(const char *name)
{
    const char *c;

    if (strncmp(name, "sd", 2) != 0 || !name[2]) {
        return false;
    }
    for (c = name + 2; *c; c++) {
        if (*c < 'a' || *c > 'z') {
            return false;
        }
    }
    return true;
}

/* sda before sdb, and sdz before sdaa. */
static bool disk_before(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);

    return la != lb ? la < lb : strcmp(a, b) < 0;
}

/* The first USB disk with a medium in it, and the partition to mount: its
 * first partition, or the disk itself when it has no partition table. */
static bool find_drive(const struct sysd_storage *st, struct drive *d)
{
    DIR *dir = opendir(st->paths.sys_block);
    struct dirent *de;
    char best[32] = "";
    char path[PATH_MAX];
    char real[PATH_MAX];
    char line[32];
    struct stat sb;

    if (!dir) {
        return false;
    }
    while ((de = readdir(dir)) != NULL) {
        if (!scsi_disk_name(de->d_name) || strlen(de->d_name) >= sizeof(best)) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", st->paths.sys_block, de->d_name);
        if (!realpath(path, real) || !strstr(real, "/usb")) {
            continue; /* not on USB: not a drive this module owns */
        }
        snprintf(path, sizeof(path), "%s/%s/size", st->paths.sys_block, de->d_name);
        if (read_line(path, line, sizeof(line)) < 0 || strtoull(line, NULL, 10) == 0) {
            continue; /* a reader with no medium */
        }
        if (!best[0] || disk_before(de->d_name, best)) {
            snprintf(best, sizeof(best), "%s", de->d_name);
        }
    }
    closedir(dir);
    if (!best[0]) {
        return false;
    }
    snprintf(path, sizeof(path), "%s/%s/%s1", st->paths.sys_block, best, best);
    if (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
        snprintf(d->part, sizeof(d->part), "%s1", best);
        snprintf(path, sizeof(path), "%s/%s/%s1/dev", st->paths.sys_block, best, best);
    } else {
        snprintf(d->part, sizeof(d->part), "%s", best);
        snprintf(path, sizeof(path), "%s/%s/dev", st->paths.sys_block, best);
    }
    if (read_line(path, d->devnum, sizeof(d->devnum)) < 0) {
        return false;
    }
    snprintf(d->path, sizeof(d->path), "%s/%s", st->paths.dev, d->part);
    return true;
}

/* The /proc/mounts entry for a mount point (by_dir) or a device (!by_dir).
 * Returns true and fills the other field. The paths this module uses have no
 * spaces, so the octal escapes of the file never matter here. */
static bool find_mount(const struct sysd_storage *st, const char *key, bool by_dir, char *other,
                       size_t other_len)
{
    FILE *f = fopen(st->paths.mounts, "re");
    char line[1024];
    char src[512];
    char dir[512];
    bool found = false;

    if (!f) {
        return false;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%511s %511s", src, dir) != 2) {
            continue;
        }
        if (strcmp(by_dir ? dir : src, key) == 0) {
            /* The last match wins: a later mount on the same point hides the
             * earlier one. */
            snprintf(other, other_len, "%s", by_dir ? src : dir);
            found = true;
        }
    }
    fclose(f);
    return found;
}

static void rmdir_mount_point(const struct sysd_storage *st)
{
    /* Only an empty directory goes. The point is removed while nothing is
     * mounted so that nothing saved "to the drive" can land on the root
     * filesystem instead. */
    if (rmdir(st->paths.mount_point) < 0 && errno != ENOENT) {
        LOG_WARN("usb: %s left in place: %s", st->paths.mount_point, strerror(errno));
    }
}

static void set_error(struct sysd_storage *st, const char *fmt, const char *a, const char *b)
{
    snprintf(st->error, sizeof(st->error), fmt, a, b ? b : "");
}

/* A drive was pulled out while still mounted. A plain unmount first; if a
 * file is still open on it that refuses with EBUSY, and only then is the
 * mount detached. That is the one lazy unmount in this module, and it is
 * safe here for a reason that does not hold for a drive still plugged in:
 * the device is gone, so there is nothing left to flush and nothing to lose
 * by not waiting, while leaving the mount in place would keep handing out a
 * filesystem with no device behind it. The detached mount goes when its last
 * file is closed. */
static void clean_stale(struct sysd_storage *st, const char *source)
{
    int r = st->ops.umount(st->paths.mount_point, false, st->ops.user);

    LOG_WARN("usb: %s was removed without Eject", source);
    if (r == -EBUSY) {
        LOG_WARN("usb: %s is busy and its device is gone; detaching it", st->paths.mount_point);
        r = st->ops.umount(st->paths.mount_point, true, st->ops.user);
    }
    if (r < 0) {
        LOG_ERROR("usb: %s could not be unmounted: %s", st->paths.mount_point, strerror(-r));
        return;
    }
    rmdir_mount_point(st);
}

/* A finished eject. */
static void reap(struct sysd_storage *st)
{
    int status = 0;
    pid_t r;

    if (st->eject_pid <= 0) {
        return;
    }
    r = waitpid(st->eject_pid, &status, WNOHANG);
    if (r == 0 || (r < 0 && errno == EINTR)) {
        return;
    }
    st->eject_pid = 0;
    if (r > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        rmdir_mount_point(st);
        st->state = SYSD_USB_EJECTED;
        snprintf(st->hold, sizeof(st->hold), "%s@%s", st->part, st->devnum);
        st->error[0] = '\0';
        LOG_INFO("usb: /dev/%s ejected; safe to remove", st->part);
        return;
    }
    st->state = SYSD_USB_MOUNTED;
    if (r > 0 && WIFEXITED(status) && WEXITSTATUS(status) == EBUSY) {
        snprintf(st->error, sizeof(st->error),
                 "The drive is in use. Close what is open on it, then eject again.");
    } else {
        snprintf(st->error, sizeof(st->error), "The drive could not be unmounted: %s",
                 r > 0 && WIFEXITED(status) ? strerror(WEXITSTATUS(status)) : "the eject did not finish");
    }
    LOG_WARN("usb: eject of /dev/%s failed: %s", st->part, st->error);
}

void sysd_storage_scan(struct sysd_storage *st)
{
    struct drive d;
    char source[512];
    char where[512];
    char id[sizeof(st->hold)];
    bool have;
    int fd;
    int r;

    reap(st);
    if (st->state == SYSD_USB_EJECTING) {
        return;
    }
    have = find_drive(st, &d);
    if (find_mount(st, st->paths.mount_point, true, source, sizeof(source))) {
        if (have && strcmp(source, d.path) == 0) {
            if (st->state != SYSD_USB_MOUNTED || strcmp(st->part, d.part) != 0) {
                /* Mounted before sysd looked: by an earlier sysd, typically. */
                snprintf(st->part, sizeof(st->part), "%s", d.part);
                snprintf(st->devnum, sizeof(st->devnum), "%s", d.devnum);
                fd = open(d.path, O_RDONLY | O_CLOEXEC);
                if (fd >= 0) {
                    sysd_storage_identify(fd, &st->probe);
                    close(fd);
                }
                st->state = SYSD_USB_MOUNTED;
                st->error[0] = '\0';
                st->hold[0] = '\0';
                LOG_INFO("usb: %s already mounted at %s", d.path, st->paths.mount_point);
            }
            return;
        }
        if (strncmp(source, st->paths.dev, strlen(st->paths.dev)) == 0 &&
            strncmp(source + strlen(st->paths.dev), "/sd", 3) == 0) {
            clean_stale(st, source);
            if (find_mount(st, st->paths.mount_point, true, source, sizeof(source))) {
                st->state = SYSD_USB_ERROR;
                set_error(st, "%s is still mounted after the drive was removed%s", st->paths.mount_point, NULL);
                return;
            }
        } else {
            st->state = SYSD_USB_ERROR;
            set_error(st, "%.60s is in use by %.60s", st->paths.mount_point, source);
            return;
        }
    }
    if (!have) {
        if (st->state != SYSD_USB_ABSENT) {
            LOG_INFO("usb: no drive");
        }
        st->state = SYSD_USB_ABSENT;
        st->part[0] = st->devnum[0] = st->hold[0] = st->error[0] = '\0';
        memset(&st->probe, 0, sizeof(st->probe));
        return;
    }
    snprintf(id, sizeof(id), "%s@%s", d.part, d.devnum);
    if (st->state == SYSD_USB_MOUNTED && strcmp(st->part, d.part) == 0 && strcmp(st->devnum, d.devnum) == 0) {
        /* Unmounted by someone else (a terminal): take that as an eject
         * rather than mounting the drive straight back. */
        st->state = SYSD_USB_EJECTED;
        snprintf(st->hold, sizeof(st->hold), "%s", id);
        rmdir_mount_point(st);
        LOG_INFO("usb: %s was unmounted outside sysd", d.path);
        return;
    }
    if (strcmp(id, st->hold) == 0) {
        return; /* ejected, unsupported or failed: left alone until it is removed */
    }

    /* A drive sysd has not seen yet. */
    snprintf(st->part, sizeof(st->part), "%s", d.part);
    snprintf(st->devnum, sizeof(st->devnum), "%s", d.devnum);
    snprintf(st->hold, sizeof(st->hold), "%s", id);
    st->error[0] = '\0';
    memset(&st->probe, 0, sizeof(st->probe));
    fd = open(d.path, O_RDONLY | O_CLOEXEC);
    r = fd < 0 ? -errno : sysd_storage_identify(fd, &st->probe);
    if (fd >= 0) {
        close(fd);
    }
    if (r < 0) {
        st->state = SYSD_USB_ERROR;
        set_error(st, "The drive cannot be read: %s%s", strerror(-r), NULL);
        LOG_WARN("usb: %s: %s", d.path, st->error);
        return;
    }
    if (!st->probe.fat) {
        st->state = SYSD_USB_UNSUPPORTED;
        LOG_INFO("usb: %s is %s; only FAT is mounted", d.path, st->probe.fs[0] ? st->probe.fs : "not recognised");
        return;
    }
    if (find_mount(st, d.path, false, where, sizeof(where))) {
        st->state = SYSD_USB_ERROR;
        set_error(st, "The drive is already mounted at %.100s%s", where, NULL);
        LOG_WARN("usb: %s: %s", d.path, st->error);
        return;
    }
    if (mkdir(st->paths.mount_point, 0755) < 0 && errno != EEXIST) {
        r = -errno;
        st->state = SYSD_USB_ERROR;
        set_error(st, "%s cannot be made: %s", st->paths.mount_point, strerror(-r));
        LOG_ERROR("usb: %s", st->error);
        return;
    }
    r = st->ops.mount(d.path, st->paths.mount_point, st->ops.user);
    if (r < 0) {
        st->state = SYSD_USB_ERROR;
        set_error(st, "The drive could not be mounted: %s%s", strerror(-r), NULL);
        LOG_ERROR("usb: %s: %s", d.path, st->error);
        rmdir_mount_point(st);
        return;
    }
    st->hold[0] = '\0';
    st->state = SYSD_USB_MOUNTED;
    LOG_INFO("usb: %s (%s%s%s) mounted at %s", d.path, st->probe.fs, st->probe.label[0] ? ", " : "",
             st->probe.label, st->paths.mount_point);
}

/* ---- events --------------------------------------------------------------------- */

void sysd_storage_init(struct sysd_storage *st, const struct sysd_storage_paths *paths,
                       const struct sysd_storage_ops *ops)
{
    memset(st, 0, sizeof(*st));
    st->paths = *paths;
    st->ops = *ops;
    st->uevent_fd = -1;
}

int sysd_storage_listen(struct sysd_storage *st)
{
    struct sockaddr_nl sa;
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);

    if (fd < 0) {
        LOG_WARN("usb: no uevent socket: %s; the drive is looked at on request only", strerror(errno));
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1; /* the kernel's own events */
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG_WARN("usb: uevent bind: %s; the drive is looked at on request only", strerror(errno));
        close(fd);
        return -1;
    }
    st->uevent_fd = fd;
    return fd;
}

void sysd_storage_on_uevent(struct sysd_storage *st)
{
    char buf[8192];
    bool block = false;
    ssize_t n;

    for (;;) {
        n = recv(st->uevent_fd, buf, sizeof(buf) - 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ENOBUFS) {
                block = true; /* events were lost: look anyway */
                continue;
            }
            break;
        }
        buf[n] = '\0';
        for (char *p = buf; p < buf + n; p += strlen(p) + 1) {
            if (strcmp(p, "SUBSYSTEM=block") == 0) {
                block = true;
            }
        }
    }
    if (block) {
        sysd_storage_scan(st);
    }
}

bool sysd_storage_busy(const struct sysd_storage *st)
{
    return st->eject_pid > 0;
}

/* ---- the API -------------------------------------------------------------------- */

const char *sysd_storage_state_name(enum sysd_usb_state s)
{
    switch (s) {
    case SYSD_USB_ABSENT: return "absent";
    case SYSD_USB_MOUNTED: return "mounted";
    case SYSD_USB_EJECTING: return "ejecting";
    case SYSD_USB_EJECTED: return "ejected";
    case SYSD_USB_UNSUPPORTED: return "unsupported";
    case SYSD_USB_ERROR: return "error";
    }
    return "error";
}

static void add_string_or_null(cJSON *o, const char *key, const char *value)
{
    if (value && value[0]) {
        cJSON_AddStringToObject(o, key, value);
    } else {
        cJSON_AddNullToObject(o, key);
    }
}

cJSON *sysd_storage_status(struct sysd_storage *st)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *usb = cJSON_AddObjectToObject(result, "usb");
    char device[PATH_MAX];
    struct statvfs vfs;
    bool present;

    sysd_storage_scan(st);
    present = st->state != SYSD_USB_ABSENT;
    cJSON_AddStringToObject(usb, "state", sysd_storage_state_name(st->state));
    cJSON_AddBoolToObject(usb, "present", present);
    snprintf(device, sizeof(device), "%s/%s", st->paths.dev, st->part);
    add_string_or_null(usb, "device", present && st->part[0] ? device : NULL);
    add_string_or_null(usb, "filesystem", present ? (st->probe.fs[0] ? st->probe.fs : "unknown") : NULL);
    add_string_or_null(usb, "label", present ? st->probe.label : NULL);
    if (st->state == SYSD_USB_MOUNTED && statvfs(st->paths.mount_point, &vfs) == 0) {
        cJSON_AddStringToObject(usb, "mount_path", st->paths.mount_point);
        cJSON_AddNumberToObject(usb, "total_bytes", (double)vfs.f_blocks * (double)vfs.f_frsize);
        cJSON_AddNumberToObject(usb, "free_bytes", (double)vfs.f_bavail * (double)vfs.f_frsize);
    } else {
        add_string_or_null(usb, "mount_path", st->state == SYSD_USB_MOUNTED ? st->paths.mount_point : NULL);
        cJSON_AddNullToObject(usb, "total_bytes");
        cJSON_AddNullToObject(usb, "free_bytes");
    }
    cJSON_AddBoolToObject(usb, "safe_to_remove", st->state == SYSD_USB_EJECTED);
    add_string_or_null(usb, "error", st->error);
    return result;
}

/* The eject itself, in the child: flush this filesystem, then everything,
 * then a plain unmount. The exit status is 0 or the errno of the unmount. */
static void eject_child(const struct sysd_storage *st)
{
    int fd = open(st->paths.mount_point, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int r;

    if (fd >= 0) {
        syncfs(fd);
        close(fd); /* before the unmount, which this fd would hold busy */
    }
    sync();
    r = st->ops.umount(st->paths.mount_point, false, st->ops.user);
    _exit(r == 0 ? 0 : (-r > 0 && -r < 256 ? -r : EIO));
}

int sysd_storage_eject(struct sysd_storage *st, char *msg, size_t msg_len)
{
    pid_t pid;

    sysd_storage_scan(st);
    if (st->state == SYSD_USB_EJECTING) {
        snprintf(msg, msg_len, "an eject is already running");
        return -2;
    }
    if (st->state != SYSD_USB_MOUNTED) {
        snprintf(msg, msg_len, "no USB drive is mounted");
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        snprintf(msg, msg_len, "eject could not start: %s", strerror(errno));
        return -3;
    }
    if (pid == 0) {
        eject_child(st);
    }
    st->eject_pid = pid;
    st->state = SYSD_USB_EJECTING;
    st->error[0] = '\0';
    LOG_INFO("usb: ejecting /dev/%s", st->part);
    return 0;
}

void sysd_storage_close(struct sysd_storage *st)
{
    if (st->uevent_fd >= 0) {
        close(st->uevent_fd);
        st->uevent_fd = -1;
    }
}
