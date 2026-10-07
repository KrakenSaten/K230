/*
 * sysd_expand implementation. See sysd_expand.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "sysd_expand.h"

#include "pocketlog/pocketlog.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* A partition that grew by less than this after partprobe did not grow. */
#define GROWN_SECTORS (16ull * 2048)

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- what is on the card ------------------------------------------------- */

int sysd_expand_ext4_bytes(const uint8_t *sb, size_t n, uint64_t *bytes)
{
    uint32_t log_block;
    uint64_t blocks;

    if (n < 1024 || le16(sb + 0x38) != 0xEF53) {
        return -1;
    }
    log_block = le32(sb + 0x18);
    if (log_block > 6) {
        return -1;
    }
    blocks = le32(sb + 0x04);
    if (le32(sb + 0x60) & 0x80) { /* INCOMPAT_64BIT: the high half counts too */
        blocks |= (uint64_t)le32(sb + 0x150) << 32;
    }
    *bytes = blocks * (1024ull << log_block);
    return 0;
}

int sysd_expand_mbr_entry(const uint8_t *mbr, size_t n, int partnum, uint64_t *start, uint64_t *sectors,
                          char *reason, size_t reason_len)
{
    int i;

    if (n < 512 || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        snprintf(reason, reason_len, "the card has no partition table");
        return -1;
    }
    for (i = 0; i < 4; i++) {
        uint8_t type = mbr[446 + 16 * i + 4];

        if (type == 0xEE) {
            snprintf(reason, reason_len, "the card is GPT, which this does not change");
            return -1;
        }
        if (type == 0x05 || type == 0x0F || type == 0x85) {
            snprintf(reason, reason_len, "the card has an extended partition");
            return -1;
        }
    }
    if (partnum < 1 || partnum > 4 || mbr[446 + 16 * (partnum - 1) + 4] == 0) {
        snprintf(reason, reason_len, "the root partition is not in the card's table");
        return -1;
    }
    *start = le32(mbr + 446 + 16 * (partnum - 1) + 8);
    *sectors = le32(mbr + 446 + 16 * (partnum - 1) + 12);
    return 0;
}

static int read_u64(const char *path, uint64_t *v)
{
    FILE *f = fopen(path, "re");
    unsigned long long x;
    int ok;

    if (!f) {
        return -1;
    }
    ok = fscanf(f, "%llu", &x) == 1;
    fclose(f);
    if (!ok) {
        return -1;
    }
    *v = x;
    return 0;
}

static int read_at(const char *path, off_t off, uint8_t *buf, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t got;

    if (fd < 0) {
        return -1;
    }
    got = pread(fd, buf, n, off);
    close(fd);
    return got == (ssize_t)n ? 0 : -1;
}

/* root= from the kernel command line, as a device name: "mmcblk1p2". */
static int root_name(const struct sysd_expand *x, char *out, size_t out_len)
{
    char line[1024];
    FILE *f = fopen(x->paths.cmdline, "re");
    char *p;
    size_t len;

    if (!f) {
        return -1;
    }
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    for (p = strstr(line, "root="); p && p != line && !isspace((unsigned char)p[-1]);
         p = strstr(p + 1, "root=")) {
    }
    if (!p || strncmp(p + 5, "/dev/", 5) != 0) {
        return -1;
    }
    p += 10;
    len = strcspn(p, " \t\n");
    if (len == 0 || len >= out_len) {
        return -1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* "mmcblk1p2" -> "mmcblk1", 2. */
static int split_mmc(const char *part, char *disk, size_t disk_len, int *partnum)
{
    const char *p = strrchr(part, 'p');
    const char *c;

    if (strncmp(part, "mmcblk", 6) != 0 || !p || p <= part + 6 || !p[1] || (size_t)(p - part) >= disk_len) {
        return -1;
    }
    for (c = part + 6; c < p; c++) {
        if (!isdigit((unsigned char)*c)) {
            return -1;
        }
    }
    for (c = p + 1; *c; c++) {
        if (!isdigit((unsigned char)*c)) {
            return -1;
        }
    }
    memcpy(disk, part, (size_t)(p - part));
    disk[p - part] = '\0';
    *partnum = atoi(p + 1);
    return 0;
}

static enum sysd_expand_state unsupported(struct sysd_expand_layout *l, const char *why)
{
    snprintf(l->reason, sizeof(l->reason), "%s", why);
    return SYSD_EXPAND_UNSUPPORTED;
}

enum sysd_expand_state sysd_expand_read(const struct sysd_expand *x, struct sysd_expand_layout *l)
{
    char path[PATH_MAX];
    uint8_t buf[1024];
    uint64_t sectors;
    uint64_t start;
    size_t dl;
    DIR *dir;
    struct dirent *de;
    char why[128];

    memset(l, 0, sizeof(*l));
    if (root_name(x, l->part, sizeof(l->part)) < 0) {
        return unsupported(l, "the root device is not named on the kernel command line");
    }
    if (split_mmc(l->part, l->disk, sizeof(l->disk), &l->partnum) < 0) {
        return unsupported(l, "the root filesystem is not on the microSD card");
    }
    snprintf(path, sizeof(path), "%s/%s/size", x->paths.sys_class_block, l->disk);
    if (read_u64(path, &sectors) < 0) {
        return unsupported(l, "the card's size cannot be read");
    }
    l->disk_bytes = sectors * 512;
    snprintf(path, sizeof(path), "%s/%s/start", x->paths.sys_class_block, l->part);
    if (read_u64(path, &l->part_start) < 0) {
        return unsupported(l, "the root partition cannot be read");
    }
    snprintf(path, sizeof(path), "%s/%s/size", x->paths.sys_class_block, l->part);
    if (read_u64(path, &l->part_sectors) < 0) {
        return unsupported(l, "the root partition cannot be read");
    }

    /* Nothing may follow the root partition: growing it to the card's end
     * would run over it. */
    dir = opendir(x->paths.sys_class_block);
    if (!dir) {
        return unsupported(l, "the card's partitions cannot be listed");
    }
    dl = strlen(l->disk);
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, l->disk, dl) != 0 || de->d_name[dl] != 'p' || strcmp(de->d_name, l->part) == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s/start", x->paths.sys_class_block, de->d_name);
        if (read_u64(path, &start) == 0 && start > l->part_start) {
            closedir(dir);
            return unsupported(l, "another partition follows the root partition");
        }
    }
    closedir(dir);

    snprintf(path, sizeof(path), "%s/%s", x->paths.dev, l->disk);
    if (read_at(path, 0, buf, 512) < 0) {
        return unsupported(l, "the card's partition table cannot be read");
    }
    if (sysd_expand_mbr_entry(buf, 512, l->partnum, &start, &l->table_sectors, why, sizeof(why)) < 0) {
        return unsupported(l, why);
    }
    if (start != l->part_start || (start + l->table_sectors) * 512 > l->disk_bytes) {
        return unsupported(l, "the card's table and the kernel disagree about the root partition");
    }
    snprintf(path, sizeof(path), "%s/%s", x->paths.dev, l->part);
    if (read_at(path, 1024, buf, 1024) < 0 || sysd_expand_ext4_bytes(buf, 1024, &l->fs_bytes) < 0) {
        return unsupported(l, "the root filesystem is not ext4");
    }

    if (l->table_sectors > l->part_sectors) {
        return SYSD_EXPAND_REBOOT;
    }
    if (l->part_sectors * 512 > l->fs_bytes + SYSD_EXPAND_FS_SLACK_BYTES) {
        return SYSD_EXPAND_FINISH;
    }
    if (l->disk_bytes - (l->part_start + l->table_sectors) * 512 >= SYSD_EXPAND_MIN_GAP_BYTES) {
        return SYSD_EXPAND_AVAILABLE;
    }
    return SYSD_EXPAND_NOT_NEEDED;
}

/* ---- the job ---------------------------------------------------------------- */

static int real_run(const char *const argv[], const char *input, int log_fd, void *user)
{
    int in[2] = { -1, -1 };
    int status = 0;
    pid_t pid;

    (void)user;
    if (input && pipe2(in, O_CLOEXEC) < 0) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        if (input) {
            close(in[0]);
            close(in[1]);
        }
        return -1;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_RDONLY);

        dup2(input ? in[0] : null, STDIN_FILENO);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
        }
        setenv("PATH", "/sbin:/usr/sbin:/bin:/usr/bin", 1);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (input) {
        ssize_t w;

        close(in[0]);
        w = write(in[1], input, strlen(input));
        (void)w;
        close(in[1]);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

const struct sysd_expand_ops sysd_expand_real_ops = { real_run, NULL };

static void note(int fd, const char *text)
{
    if (fd >= 0) {
        ssize_t w = write(fd, text, strlen(text));

        (void)w;
    }
}

/* The child: everything that changes the card, in order, and an exit code
 * the parent turns into a state. */
static void job(const struct sysd_expand *x, bool truncate_log)
{
    struct sysd_expand_layout l;
    struct sysd_expand_layout after;
    enum sysd_expand_state s = sysd_expand_read(x, &l);
    char disk[PATH_MAX];
    char part[PATH_MAX];
    char num[12];
    char line[256];
    int log_fd = open(x->paths.log, O_WRONLY | O_CREAT | O_CLOEXEC | (truncate_log ? O_TRUNC : O_APPEND), 0644);
    int rc;

    snprintf(disk, sizeof(disk), "%s/%s", x->paths.dev, l.disk);
    snprintf(part, sizeof(part), "%s/%s", x->paths.dev, l.part);
    snprintf(num, sizeof(num), "%d", l.partnum);
    snprintf(line, sizeof(line), "storage expand: %.100s, %s, table %llu sectors, kernel %llu, filesystem %llu bytes\n",
             part, sysd_expand_state_name(s), (unsigned long long)l.table_sectors,
             (unsigned long long)l.part_sectors, (unsigned long long)l.fs_bytes);
    note(log_fd, line);

    if (s == SYSD_EXPAND_AVAILABLE) {
        /* The root partition is in use, so parted asks whether to go on; it
         * is answered as the vendor script answers it, and run in script
         * mode if that does not do. */
        const char *const ask[] = { "parted", "---pretend-input-tty", disk, "resizepart", num, "100%", NULL };
        const char *const script[] = { "parted", "-s", disk, "resizepart", num, "100%", NULL };
        const char *const probe[] = { "partprobe", disk, NULL };

        if (x->ops.run(ask, "Fix\nYes\n", log_fd, x->ops.user) != 0 &&
            x->ops.run(script, NULL, log_fd, x->ops.user) != 0) {
            _exit(SYSD_EXPAND_EXIT_PARTED);
        }
        sync();
        x->ops.run(probe, NULL, log_fd, x->ops.user);
        sleep(1);
        sysd_expand_read(x, &after);
        if (after.part_sectors < l.part_sectors + GROWN_SECTORS) {
            if (after.table_sectors >= l.table_sectors + GROWN_SECTORS) {
                /* The table on the card is grown; the kernel will read it at
                 * the next boot, and the next sysd finishes. */
                int fd = open(x->paths.pending, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

                if (fd >= 0) {
                    note(fd, l.part);
                    note(fd, "\n");
                    fsync(fd);
                    close(fd);
                }
                sync();
                note(log_fd, "storage expand: restart needed for the kernel to see the new size\n");
                _exit(SYSD_EXPAND_EXIT_REBOOT);
            }
            _exit(SYSD_EXPAND_EXIT_NOT_GROWN);
        }
    } else if (s != SYSD_EXPAND_FINISH) {
        _exit(SYSD_EXPAND_EXIT_LAYOUT);
    }
    {
        const char *const grow[] = { "resize2fs", part, NULL };

        rc = x->ops.run(grow, NULL, log_fd, x->ops.user);
    }
    if (rc != 0) {
        _exit(SYSD_EXPAND_EXIT_RESIZE2FS);
    }
    sync();
    unlink(x->paths.pending);
    note(log_fd, "storage expand: done\n");
    _exit(SYSD_EXPAND_EXIT_DONE);
}

void sysd_expand_init(struct sysd_expand *x, const struct sysd_expand_paths *paths,
                      const struct sysd_expand_ops *ops)
{
    memset(x, 0, sizeof(*x));
    x->paths = *paths;
    x->ops = *ops;
}

static int spawn(struct sysd_expand *x, bool truncate_log)
{
    pid_t pid = fork();

    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        job(x, truncate_log);
    }
    x->pid = pid;
    x->error[0] = '\0';
    x->done = false;
    return 0;
}

void sysd_expand_startup(struct sysd_expand *x)
{
    struct sysd_expand_layout l;
    enum sysd_expand_state s;

    if (access(x->paths.pending, F_OK) != 0) {
        return;
    }
    s = sysd_expand_read(x, &l);
    if (s == SYSD_EXPAND_FINISH) {
        LOG_INFO("storage: finishing the expansion asked for before the restart");
        if (spawn(x, false) < 0) {
            LOG_ERROR("storage: the expansion could not be finished: %s", strerror(errno));
        }
        return;
    }
    if (s == SYSD_EXPAND_REBOOT) {
        LOG_WARN("storage: the kernel still has the old partition size after a restart");
        return;
    }
    LOG_WARN("storage: an expansion marker was left (%s); removed", sysd_expand_state_name(s));
    unlink(x->paths.pending);
}

int sysd_expand_start(struct sysd_expand *x, char *msg, size_t msg_len)
{
    struct sysd_expand_layout l;
    enum sysd_expand_state s;

    sysd_expand_reap(x);
    if (x->pid > 0) {
        snprintf(msg, msg_len, "the storage expansion is already running");
        return -2;
    }
    s = sysd_expand_read(x, &l);
    switch (s) {
    case SYSD_EXPAND_AVAILABLE:
    case SYSD_EXPAND_FINISH:
        break;
    case SYSD_EXPAND_NOT_NEEDED:
        snprintf(msg, msg_len, "the storage already uses the whole card");
        return -1;
    case SYSD_EXPAND_REBOOT:
        snprintf(msg, msg_len, "restart to finish the storage expansion");
        return -1;
    default:
        snprintf(msg, msg_len, "%s", l.reason[0] ? l.reason : "this card cannot be expanded");
        return -1;
    }
    if (spawn(x, s == SYSD_EXPAND_AVAILABLE) < 0) {
        snprintf(msg, msg_len, "the expansion could not start: %s", strerror(errno));
        return -3;
    }
    LOG_INFO("storage: expanding /dev/%s (%s)", l.part, sysd_expand_state_name(s));
    return 0;
}

void sysd_expand_reap(struct sysd_expand *x)
{
    int status = 0;
    pid_t r;
    int code;

    if (x->pid <= 0) {
        return;
    }
    r = waitpid(x->pid, &status, WNOHANG);
    if (r == 0 || (r < 0 && errno == EINTR)) {
        return;
    }
    x->pid = 0;
    code = r > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    switch (code) {
    case SYSD_EXPAND_EXIT_DONE:
        x->done = true;
        LOG_INFO("storage: expansion done");
        return;
    case SYSD_EXPAND_EXIT_REBOOT:
        LOG_INFO("storage: partition grown; a restart finishes the expansion");
        return;
    case SYSD_EXPAND_EXIT_PARTED:
        snprintf(x->error, sizeof(x->error), "The partition could not be grown (parted).");
        break;
    case SYSD_EXPAND_EXIT_NOT_GROWN:
        snprintf(x->error, sizeof(x->error), "The partition did not grow.");
        break;
    case SYSD_EXPAND_EXIT_RESIZE2FS:
        snprintf(x->error, sizeof(x->error), "The filesystem could not be grown (resize2fs).");
        break;
    case SYSD_EXPAND_EXIT_LAYOUT:
        snprintf(x->error, sizeof(x->error), "The card changed before the expansion started.");
        break;
    default:
        snprintf(x->error, sizeof(x->error), "The expansion did not finish.");
        break;
    }
    LOG_ERROR("storage: %s See %s.", x->error, x->paths.log);
}

bool sysd_expand_busy(const struct sysd_expand *x)
{
    return x->pid > 0;
}

const char *sysd_expand_state_name(enum sysd_expand_state s)
{
    switch (s) {
    case SYSD_EXPAND_UNSUPPORTED: return "unsupported";
    case SYSD_EXPAND_NOT_NEEDED: return "not_needed";
    case SYSD_EXPAND_AVAILABLE: return "available";
    case SYSD_EXPAND_FINISH: return "finish";
    case SYSD_EXPAND_REBOOT: return "reboot_required";
    case SYSD_EXPAND_RUNNING: return "running";
    }
    return "unsupported";
}

cJSON *sysd_expand_status(struct sysd_expand *x)
{
    struct sysd_expand_layout l;
    enum sysd_expand_state s;
    cJSON *o = cJSON_CreateObject();
    char device[64];
    uint64_t end;

    sysd_expand_reap(x);
    s = sysd_expand_read(x, &l);
    if (x->pid > 0) {
        s = SYSD_EXPAND_RUNNING;
    }
    cJSON_AddStringToObject(o, "state", sysd_expand_state_name(s));
    if (l.part[0]) {
        snprintf(device, sizeof(device), "/dev/%s", l.part);
        cJSON_AddStringToObject(o, "device", device);
    } else {
        cJSON_AddNullToObject(o, "device");
    }
    if (l.disk_bytes && l.table_sectors) {
        end = (l.part_start + l.table_sectors) * 512;
        cJSON_AddNumberToObject(o, "disk_bytes", (double)l.disk_bytes);
        cJSON_AddNumberToObject(o, "partition_bytes", (double)l.part_sectors * 512.0);
        cJSON_AddNumberToObject(o, "filesystem_bytes", (double)l.fs_bytes);
        cJSON_AddNumberToObject(o, "unused_bytes", end < l.disk_bytes ? (double)(l.disk_bytes - end) : 0.0);
    } else {
        cJSON_AddNullToObject(o, "disk_bytes");
        cJSON_AddNullToObject(o, "partition_bytes");
        cJSON_AddNullToObject(o, "filesystem_bytes");
        cJSON_AddNullToObject(o, "unused_bytes");
    }
    cJSON_AddBoolToObject(o, "can_expand", s == SYSD_EXPAND_AVAILABLE || s == SYSD_EXPAND_FINISH);
    if (s == SYSD_EXPAND_UNSUPPORTED && l.reason[0]) {
        cJSON_AddStringToObject(o, "reason", l.reason);
    } else {
        cJSON_AddNullToObject(o, "reason");
    }
    if (x->error[0]) {
        cJSON_AddStringToObject(o, "error", x->error);
    } else {
        cJSON_AddNullToObject(o, "error");
    }
    cJSON_AddBoolToObject(o, "done", x->done);
    return o;
}
