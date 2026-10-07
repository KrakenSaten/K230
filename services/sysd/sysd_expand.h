/*
 * storage.expand and the "internal" half of storage.status: growing the root
 * filesystem over the rest of the microSD card (docs/api/system.md,
 * "Storage: the microSD card").
 *
 * The image's root filesystem is about 600 MiB at the end of an MBR card of
 * 16 or 64 GB, so most of the card is never used (VERIFIED on unit B:
 * mmcblk1p2 at sector 262144, 1228800 sectors, on a 30535680-sector card).
 * Nothing grows it on its own: there is no first-boot resize. The owner asks
 * (System > Storage > Expand storage), and sysd does it.
 *
 * THE PROCEDURE is the one the vendor's launcher uses
 * (vendor/T-Display-K230/k230_launcher/k230_phone_ui/src/k230_storage_expand.sh,
 * DOCUMENTED; that launcher is disabled on Doors, so nothing ran it):
 *
 *   1. parted resizepart <disk> <n> 100%   - the partition table entry
 *   2. partprobe <disk>                     - ask the kernel to re-read it
 *   3. resize2fs <partition>                - grow ext4, online
 *
 * When the kernel keeps the old size after step 2 (the partition is the
 * mounted root), a marker is written to the state directory and the owner is
 * asked to restart; the next sysd finds the marker and does step 3 itself.
 * That is the only thing sysd ever starts without being asked, and only to
 * finish what the owner asked for before the restart.
 *
 * Only the layout the image has is touched: root on mmcblk<N>p<M>, an MBR
 * (no GPT, no extended partitions), the root partition the last one on the
 * card, ext4. Anything else is "unsupported" with the reason.
 *
 * The work runs in a child process, as storage.eject does, so sysd keeps
 * answering; the tools' output goes to storage-expand.log in the log
 * directory.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SYSD_EXPAND_H
#define SYSD_EXPAND_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Less unused space than this is not worth a partition change (the vendor
 * script's threshold). */
#define SYSD_EXPAND_MIN_GAP_BYTES (256ull * 1024 * 1024)
/* A filesystem this much smaller than its partition still needs resize2fs. */
#define SYSD_EXPAND_FS_SLACK_BYTES (64ull * 1024 * 1024)

enum sysd_expand_state {
    SYSD_EXPAND_UNSUPPORTED = 0, /* not the layout this knows how to grow */
    SYSD_EXPAND_NOT_NEEDED,      /* the root filesystem already uses the card */
    SYSD_EXPAND_AVAILABLE,       /* space after the root partition: Expand */
    SYSD_EXPAND_FINISH,          /* partition grown, filesystem not yet */
    SYSD_EXPAND_REBOOT,          /* table grown, kernel still has the old size */
    SYSD_EXPAND_RUNNING
};

/* What the card and the root filesystem look like now. */
struct sysd_expand_layout {
    char part[32];           /* "mmcblk1p2" */
    char disk[32];           /* "mmcblk1" */
    int partnum;
    uint64_t disk_bytes;
    uint64_t part_start;     /* sectors, from sysfs */
    uint64_t part_sectors;   /* the kernel's idea of the partition */
    uint64_t table_sectors;  /* what the MBR on the card says */
    uint64_t fs_bytes;       /* the ext4 superblock's size */
    char reason[128];        /* why it is unsupported */
};

struct sysd_expand_paths {
    const char *sys_class_block; /* /sys/class/block */
    const char *dev;             /* /dev */
    const char *cmdline;         /* /proc/cmdline */
    const char *pending;         /* <state dir>/storage-expand.pending */
    const char *log;             /* <log dir>/storage-expand.log */
};

/* Run a tool: argv[0] is the program, input (may be NULL) is written to its
 * stdin, its output is appended to the log fd. Returns its exit status, or
 * -1 when it did not run. Production: fork and exec; a test records. Called
 * in the job's child process. */
struct sysd_expand_ops {
    int (*run)(const char *const argv[], const char *input, int log_fd, void *user);
    void *user;
};

extern const struct sysd_expand_ops sysd_expand_real_ops;

struct sysd_expand {
    struct sysd_expand_paths paths;
    struct sysd_expand_ops ops;
    pid_t pid;               /* the running job, 0 when none */
    char error[160];         /* why the last job failed, until the next one */
    bool done;               /* the last job finished the expansion */
};

/* Exit codes of the job child, also used by tests. */
enum {
    SYSD_EXPAND_EXIT_DONE = 0,
    SYSD_EXPAND_EXIT_REBOOT = 10,
    SYSD_EXPAND_EXIT_PARTED = 20,
    SYSD_EXPAND_EXIT_NOT_GROWN = 21,
    SYSD_EXPAND_EXIT_RESIZE2FS = 30,
    SYSD_EXPAND_EXIT_LAYOUT = 40
};

void sysd_expand_init(struct sysd_expand *x, const struct sysd_expand_paths *paths,
                      const struct sysd_expand_ops *ops);

/* Read the layout. Returns the state it allows (never RUNNING). */
enum sysd_expand_state sysd_expand_read(const struct sysd_expand *x, struct sysd_expand_layout *l);

/* The ext4 size in a superblock read from offset 1024 (n >= 1024 bytes of it).
 * Returns 0 and the size, or -1 when it is not ext2/3/4. */
int sysd_expand_ext4_bytes(const uint8_t *sb, size_t n, uint64_t *bytes);

/* An MBR's entry for partnum (1..4): start and length in sectors. Returns 0,
 * or -1 with a reason when the sector is not a plain MBR that has it (GPT,
 * an extended partition, no signature). */
int sysd_expand_mbr_entry(const uint8_t *mbr, size_t n, int partnum, uint64_t *start, uint64_t *sectors,
                          char *reason, size_t reason_len);

/* At sysd start: a marker from before a restart, and a partition that has
 * grown since, finish the owner's expansion. Nothing else starts by itself. */
void sysd_expand_startup(struct sysd_expand *x);

/* storage.expand. 0 = started; -1 = refused (msg says why); -2 = already
 * running; -3 = could not start. */
int sysd_expand_start(struct sysd_expand *x, char *msg, size_t msg_len);

/* Collect a finished job. */
void sysd_expand_reap(struct sysd_expand *x);
bool sysd_expand_busy(const struct sysd_expand *x);

/* The "internal" object of storage.status (caller frees). */
cJSON *sysd_expand_status(struct sysd_expand *x);

const char *sysd_expand_state_name(enum sysd_expand_state s);

#endif
