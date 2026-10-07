/*
 * storage.status and storage.eject: the USB drive (docs/api/system.md,
 * "Storage").
 *
 * sysd is the one owner of the drive's mount. Nothing else mounts, unmounts
 * or ejects it: Files asks sysd, and sysd answers with what it did.
 *
 * Scope (v1): one USB drive, its first partition (or the whole disk when it
 * has no partition table), FAT12/16/32 only, mounted at /media/usb. exFAT and
 * NTFS are recognised so the owner can be told why the drive was not
 * mounted, and are never mounted.
 *
 * DETECTION. The kernel's block uevents (NETLINK_KOBJECT_UEVENT, the same
 * stream BusyBox mdev -d reads; any number of listeners may) wake sysd, and
 * sysd then looks at /sys/block and /proc/mounts afresh. Nothing is kept from
 * the event itself, so a missed or merged event is repaired by the next scan.
 * A scan also runs at start (a drive inserted before boot sent its events
 * before anyone listened) and on every storage.status.
 *
 * EJECT. syncfs and sync, then a plain umount(2), in a child process so that
 * sysd keeps answering while the drive flushes. A busy drive is reported as
 * busy and stays mounted: no lazy unmount. The one lazy unmount is for a
 * drive that has already been pulled out while something still had a file
 * open on it: there is nothing left to flush, and a mount whose device is
 * gone must stop accepting new opens (sysd_storage.c, clean_stale()).
 *
 * After an eject the same device is not mounted again until it is removed:
 * "Safe to remove" stays true until the drive is gone.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SYSD_STORAGE_H
#define SYSD_STORAGE_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SYSD_STORAGE_MOUNT_POINT "/media/usb"

enum sysd_usb_state {
    SYSD_USB_ABSENT = 0,  /* no USB drive */
    SYSD_USB_MOUNTED,     /* mounted at the mount point */
    SYSD_USB_EJECTING,    /* syncing and unmounting */
    SYSD_USB_EJECTED,     /* unmounted by storage.eject: safe to remove */
    SYSD_USB_UNSUPPORTED, /* a drive whose filesystem is not FAT */
    SYSD_USB_ERROR        /* a FAT drive that could not be mounted, or a mount point in use */
};

/* What a partition's first sector (and, for FAT, its root directory) says. */
struct sysd_fs_probe {
    char fs[8];      /* "FAT12" "FAT16" "FAT32" "exFAT" "NTFS", or "" when unknown */
    bool fat;        /* one of the three FATs: the only thing mounted */
    char label[12];  /* the volume label, printable ASCII, "" when none */
    /* FAT geometry, for finding the root directory's label entry. */
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint64_t root_offset; /* bytes from the partition start */
    uint32_t root_bytes;  /* how much of the root directory to read */
};

/* Identify a filesystem from its first sector. n must be at least 512.
 * Returns 0 and fills p (fs "" when nothing was recognised). The boot
 * sector's label is used when it has one; sysd_storage_identify() then
 * prefers the root directory's label entry, which is what Windows writes. */
int sysd_storage_probe(const uint8_t *sector, size_t n, struct sysd_fs_probe *p);

/* The probe above, read from an open partition, and the label from the root
 * directory when it is FAT. Returns 0 or -errno. */
int sysd_storage_identify(int fd, struct sysd_fs_probe *p);

/* Where the module looks. Production: /sys/block, /dev, /proc/mounts and
 * SYSD_STORAGE_MOUNT_POINT; a test points them into a temporary tree. */
struct sysd_storage_paths {
    const char *sys_block;
    const char *dev;
    const char *mounts;
    const char *mount_point;
};

/* The two things that change the system. Production: mount(2) with the
 * options below and umount2(2); a test records them instead. Both return 0
 * or -errno. umount is called in the eject child as well, so a test's fake
 * must leave its trace somewhere a parent process can see (a file). */
struct sysd_storage_ops {
    int (*mount)(const char *device, const char *dir, void *user);
    int (*umount)(const char *dir, bool detach, void *user);
    void *user;
};

/* mount(2) flags and data for a removable FAT drive. Exposed for the docs and
 * tests: nosuid, nodev, noexec, noatime; UTF-8 names, mixed-case short names
 * kept, writes flushed early (flush), files 0644 and folders 0755, and the
 * filesystem remounted read-only on an error rather than carrying on. */
#define SYSD_STORAGE_FAT_DATA "utf8,shortname=mixed,flush,fmask=0133,dmask=0022,errors=remount-ro"
extern const struct sysd_storage_ops sysd_storage_real_ops;
extern const struct sysd_storage_paths sysd_storage_real_paths;

struct sysd_storage {
    struct sysd_storage_paths paths;
    struct sysd_storage_ops ops;
    enum sysd_usb_state state;
    char part[32];       /* "sda1", or "sda" for a drive without a partition table */
    char devnum[16];     /* "8:1", from sysfs: which device this is */
    struct sysd_fs_probe probe;
    char error[160];     /* why the drive is not mounted, or why an eject failed */
    char hold[48];       /* part@devnum not to mount again until it is gone */
    pid_t eject_pid;     /* the eject child, 0 when none */
    int uevent_fd;
};

void sysd_storage_init(struct sysd_storage *st, const struct sysd_storage_paths *paths,
                       const struct sysd_storage_ops *ops);
/* Listen for block uevents. Returns the fd to poll (also kept in uevent_fd),
 * or -1 when the socket cannot be had; sysd then still scans on status. */
int sysd_storage_listen(struct sysd_storage *st);
/* The uevent fd is readable: drain it and scan when a block device changed. */
void sysd_storage_on_uevent(struct sysd_storage *st);
/* Look at the drive and act: mount a new FAT drive, clean up after one that
 * was pulled out, collect a finished eject. */
void sysd_storage_scan(struct sysd_storage *st);
/* An eject is running: the main loop polls more often until it ends. */
bool sysd_storage_busy(const struct sysd_storage *st);
/* The storage.status result (caller frees). Scans first. */
cJSON *sysd_storage_status(struct sysd_storage *st);
/* storage.eject. 0 = started (state EJECTING); -1 = nothing mounted to eject;
 * -2 = an eject is already running; -3 = it could not be started (fork).
 * msg says why on a refusal. */
int sysd_storage_eject(struct sysd_storage *st, char *msg, size_t msg_len);
void sysd_storage_close(struct sysd_storage *st);

const char *sysd_storage_state_name(enum sysd_usb_state s);

#endif
