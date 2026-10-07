/*
 * sysd_expand tests: the microSD card's layout, and the expansion job
 * (services/sysd/sysd_expand.h).
 *
 * The card is a temporary tree: /sys/class/block with the kernel's sizes,
 * /dev/mmcblk1 a file holding the MBR, /dev/mmcblk1p2 a file holding the ext4
 * superblock, and a kernel command line. The tools are played here and do to
 * those files what they would do to the card: parted grows the MBR entry to
 * the card's end, partprobe makes the kernel see it (only when told to, so
 * the restart path can be tested), resize2fs grows the superblock. They run
 * in the job's child process, so they record what they were asked in a file.
 * The numbers are unit B's card (VERIFIED 2026-10-03).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "sysd_expand.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DISK_SECTORS 30535680ull
#define P1_START 61440ull
#define P1_SECTORS 163840ull
#define P2_START 262144ull
#define P2_SECTORS 1228800ull
#define FS_BLOCKS 153600ull

static int failed;
static int checks;
static char root[] = "/tmp/pos_expand.XXXXXX";
static char p_block[256], p_dev[256], p_cmdline[256], p_pending[256], p_log[256], p_calls[256];

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

static void path_of(char *out, size_t n, const char *rel)
{
    snprintf(out, n, "%s/%s", root, rel);
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

static void flag(const char *name, int on)
{
    char p[512];

    path_of(p, sizeof(p), name);
    if (on) {
        put(p, "");
    } else {
        unlink(p);
    }
}

static int flagged(const char *name)
{
    char p[512];

    path_of(p, sizeof(p), name);
    return access(p, F_OK) == 0;
}

static void put_u64(const char *rel, unsigned long long v)
{
    char p[512];
    char text[32];

    snprintf(p, sizeof(p), "%s/%s", p_block, rel);
    snprintf(text, sizeof(text), "%llu\n", v);
    put(p, text);
}

static unsigned long long get_u64(const char *rel)
{
    char p[512];
    unsigned long long v = 0;
    FILE *f;

    snprintf(p, sizeof(p), "%s/%s", p_block, rel);
    f = fopen(p, "r");
    if (f) {
        if (fscanf(f, "%llu", &v) != 1) {
            v = 0;
        }
        fclose(f);
    }
    return v;
}

static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t r32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_at(const char *path, off_t off, const uint8_t *buf, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT, 0644);

    if (fd < 0 || pwrite(fd, buf, n, off) != (ssize_t)n) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    close(fd);
}

static void read_at(const char *path, off_t off, uint8_t *buf, size_t n)
{
    int fd = open(path, O_RDONLY);

    memset(buf, 0, n);
    if (fd >= 0) {
        if (pread(fd, buf, n, off) < 0) {
            memset(buf, 0, n);
        }
        close(fd);
    }
}

/* An MBR with the image's two partitions; type2 lets a test make it GPT. */
static void make_mbr(uint8_t *m, uint8_t type2, uint64_t p2_sectors)
{
    memset(m, 0, 512);
    m[446 + 4] = 0x83;
    w32(m + 446 + 8, (uint32_t)P1_START);
    w32(m + 446 + 12, (uint32_t)P1_SECTORS);
    m[446 + 16 + 4] = type2;
    w32(m + 446 + 16 + 8, (uint32_t)P2_START);
    w32(m + 446 + 16 + 12, (uint32_t)p2_sectors);
    m[510] = 0x55;
    m[511] = 0xAA;
}

static void write_mbr(uint8_t type2, uint64_t p2_sectors)
{
    uint8_t m[512];
    char p[512];

    make_mbr(m, type2, p2_sectors);
    snprintf(p, sizeof(p), "%s/mmcblk1", p_dev);
    write_at(p, 0, m, sizeof(m));
}

static uint64_t mbr_p2_sectors(void)
{
    uint8_t m[512];
    char p[512];

    snprintf(p, sizeof(p), "%s/mmcblk1", p_dev);
    read_at(p, 0, m, sizeof(m));
    return r32(m + 446 + 16 + 12);
}

static void make_sb(uint8_t *sb, uint64_t blocks)
{
    memset(sb, 0, 1024);
    w32(sb + 0x04, (uint32_t)blocks);
    w32(sb + 0x18, 2); /* 4096-byte blocks */
    sb[0x38] = 0x53;
    sb[0x39] = 0xEF;
}

static void write_sb(uint64_t blocks)
{
    uint8_t sb[1024];
    char p[512];

    make_sb(sb, blocks);
    snprintf(p, sizeof(p), "%s/mmcblk1p2", p_dev);
    write_at(p, 1024, sb, sizeof(sb));
}

/* Unit B's card as flashed. */
static void card(void)
{
    char cmd[2048];

    snprintf(cmd, sizeof(cmd), "rm -rf %s/* && mkdir -p %s/mmcblk1 %s/mmcblk1p1 %s/mmcblk1p2", p_block, p_block,
             p_block, p_block);
    if (system(cmd) != 0) {
        exit(1);
    }
    put_u64("mmcblk1/size", DISK_SECTORS);
    put_u64("mmcblk1p1/start", P1_START);
    put_u64("mmcblk1p1/size", P1_SECTORS);
    put_u64("mmcblk1p2/start", P2_START);
    put_u64("mmcblk1p2/size", P2_SECTORS);
    write_mbr(0x83, P2_SECTORS);
    write_sb(FS_BLOCKS);
    put(p_cmdline, "root=/dev/mmcblk1p2 loglevel=8 rw rootdelay=4 rootfstype=ext4 console=ttyS0,115200\n");
    unlink(p_pending);
    put(p_calls, "");
    flag("kernel-rereads", 1);
    flag("parted-fails", 0);
    flag("resize-fails", 0);
    flag("slow", 0);
}

/* ---- the tools ------------------------------------------------------------ */

static void record(const char *const argv[], const char *input)
{
    FILE *f = fopen(p_calls, "a");
    int i;

    if (!f) {
        return;
    }
    for (i = 0; argv[i]; i++) {
        fprintf(f, "%s%s", i ? " " : "", argv[i]);
    }
    if (input) {
        fprintf(f, " <%s>", strcmp(input, "Fix\nYes\n") == 0 ? "Fix,Yes" : "other");
    }
    fprintf(f, "\n");
    fclose(f);
}

static int fake_run(const char *const argv[], const char *input, int log_fd, void *user)
{
    (void)log_fd;
    (void)user;
    record(argv, input);
    if (flagged("slow")) {
        usleep(500 * 1000);
    }
    if (strcmp(argv[0], "parted") == 0) {
        if (flagged("parted-fails")) {
            return 1;
        }
        write_mbr(0x83, DISK_SECTORS - P2_START);
        return 0;
    }
    if (strcmp(argv[0], "partprobe") == 0) {
        if (flagged("kernel-rereads")) {
            put_u64("mmcblk1p2/size", mbr_p2_sectors());
        }
        return 0;
    }
    if (strcmp(argv[0], "resize2fs") == 0) {
        if (flagged("resize-fails")) {
            return 1;
        }
        write_sb(get_u64("mmcblk1p2/size") / 8);
        return 0;
    }
    return 127;
}

static const struct sysd_expand_ops fake_ops = { fake_run, NULL };

static int calls_have(const char *text)
{
    char buf[4096] = "";
    FILE *f = fopen(p_calls, "r");

    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);

        buf[n] = '\0';
        fclose(f);
    }
    return strstr(buf, text) != NULL;
}

static enum sysd_expand_state state_of(struct sysd_expand *x)
{
    struct sysd_expand_layout l;

    return sysd_expand_read(x, &l);
}

static void wait_job(struct sysd_expand *x)
{
    int i;

    for (i = 0; i < 400 && sysd_expand_busy(x); i++) {
        usleep(10 * 1000);
        sysd_expand_reap(x);
    }
}

/* ---- tests ------------------------------------------------------------------ */

static void test_parsers(void)
{
    uint8_t m[512];
    uint8_t sb[1024];
    uint64_t start = 0, len = 0, bytes = 0;
    char why[128];

    make_mbr(m, 0x83, P2_SECTORS);
    check("mbr: the root entry's start and length",
          sysd_expand_mbr_entry(m, 512, 2, &start, &len, why, sizeof(why)) == 0 && start == P2_START &&
              len == P2_SECTORS);
    check("mbr: an empty slot is not the root partition", sysd_expand_mbr_entry(m, 512, 3, &start, &len, why,
                                                                                sizeof(why)) < 0);
    make_mbr(m, 0xEE, P2_SECTORS);
    check("mbr: a GPT card is refused, and says so",
          sysd_expand_mbr_entry(m, 512, 2, &start, &len, why, sizeof(why)) < 0 && strstr(why, "GPT"));
    make_mbr(m, 0x05, P2_SECTORS);
    check("mbr: an extended partition is refused",
          sysd_expand_mbr_entry(m, 512, 2, &start, &len, why, sizeof(why)) < 0 && strstr(why, "extended"));
    make_mbr(m, 0x83, P2_SECTORS);
    m[511] = 0;
    check("mbr: no signature, no table", sysd_expand_mbr_entry(m, 512, 2, &start, &len, why, sizeof(why)) < 0);

    make_sb(sb, FS_BLOCKS);
    check("ext4: blocks times the block size", sysd_expand_ext4_bytes(sb, 1024, &bytes) == 0 &&
                                                   bytes == FS_BLOCKS * 4096);
    w32(sb + 0x60, 0x80);
    w32(sb + 0x150, 1);
    check("ext4: a 64-bit filesystem's high half counts",
          sysd_expand_ext4_bytes(sb, 1024, &bytes) == 0 && bytes == ((1ull << 32) + FS_BLOCKS) * 4096);
    sb[0x38] = 0;
    check("ext4: no magic, not ext4", sysd_expand_ext4_bytes(sb, 1024, &bytes) < 0);
}

static void test_layout(const struct sysd_expand_paths *paths)
{
    struct sysd_expand x;
    struct sysd_expand_layout l;
    cJSON *o;
    char p[512];

    sysd_expand_init(&x, paths, &fake_ops);
    card();
    check("unit B's card as flashed: available", sysd_expand_read(&x, &l) == SYSD_EXPAND_AVAILABLE);
    check("its root is mmcblk1p2 on mmcblk1", strcmp(l.part, "mmcblk1p2") == 0 && strcmp(l.disk, "mmcblk1") == 0 &&
                                                  l.partnum == 2);
    check("its sizes", l.disk_bytes == DISK_SECTORS * 512 && l.part_sectors == P2_SECTORS &&
                           l.table_sectors == P2_SECTORS && l.fs_bytes == FS_BLOCKS * 4096);
    o = sysd_expand_status(&x);
    check("status: available, can expand, the unused space",
          strcmp(cJSON_GetObjectItem(o, "state")->valuestring, "available") == 0 &&
              cJSON_IsTrue(cJSON_GetObjectItem(o, "can_expand")) &&
              cJSON_GetObjectItem(o, "unused_bytes")->valuedouble ==
                  (double)((DISK_SECTORS - P2_START - P2_SECTORS) * 512) &&
              strcmp(cJSON_GetObjectItem(o, "device")->valuestring, "/dev/mmcblk1p2") == 0 &&
              cJSON_IsNull(cJSON_GetObjectItem(o, "error")) && cJSON_IsFalse(cJSON_GetObjectItem(o, "done")));
    cJSON_Delete(o);

    put_u64("mmcblk1/size", P2_START + P2_SECTORS + 100 * 2048);
    check("less than 256 MiB after it: not needed", sysd_expand_read(&x, &l) == SYSD_EXPAND_NOT_NEEDED);
    card();

    put(p_cmdline, "root=/dev/sda2 rw\n");
    check("root not on the card: unsupported", sysd_expand_read(&x, &l) == SYSD_EXPAND_UNSUPPORTED &&
                                                   strstr(l.reason, "not on the microSD"));
    put(p_cmdline, "root=PARTUUID=1234-02 rw\n");
    check("root not named as a device: unsupported", sysd_expand_read(&x, &l) == SYSD_EXPAND_UNSUPPORTED);
    put(p_cmdline, "nfsroot=/dev/mmcblk1p2 rw\n");
    check("nfsroot= is not root=", sysd_expand_read(&x, &l) == SYSD_EXPAND_UNSUPPORTED);
    card();

    snprintf(p, sizeof(p), "%s/mmcblk1p3", p_block);
    mkdir(p, 0755);
    put_u64("mmcblk1p3/start", P2_START + P2_SECTORS);
    put_u64("mmcblk1p3/size", 2048);
    check("a partition after the root: unsupported", sysd_expand_read(&x, &l) == SYSD_EXPAND_UNSUPPORTED &&
                                                         strstr(l.reason, "follows"));
    card();
    write_mbr(0xEE, P2_SECTORS);
    check("a GPT card: unsupported", sysd_expand_read(&x, &l) == SYSD_EXPAND_UNSUPPORTED &&
                                         strstr(l.reason, "GPT"));
    card();
    put_u64("mmcblk1p2/start", P2_START + 8);
    check("table and kernel disagree: unsupported", sysd_expand_read(&x, &l) == SYSD_EXPAND_UNSUPPORTED);
    card();
    {
        uint8_t zero[1024];

        memset(zero, 0, sizeof(zero));
        snprintf(p, sizeof(p), "%s/mmcblk1p2", p_dev);
        write_at(p, 1024, zero, sizeof(zero));
    }
    check("not ext4: unsupported", sysd_expand_read(&x, &l) == SYSD_EXPAND_UNSUPPORTED &&
                                       strstr(l.reason, "ext4"));
    card();
    o = sysd_expand_status(&x);
    check("status of a supported card has no reason", cJSON_IsNull(cJSON_GetObjectItem(o, "reason")));
    cJSON_Delete(o);
}

static void test_expand(const struct sysd_expand_paths *paths)
{
    struct sysd_expand x;
    char msg[160];
    char text[64] = "";
    cJSON *o;
    FILE *f;

    /* The kernel sees the new size at once: one go, no restart. */
    sysd_expand_init(&x, paths, &fake_ops);
    card();
    check("expand: started", sysd_expand_start(&x, msg, sizeof(msg)) == 0 && sysd_expand_busy(&x));
    o = sysd_expand_status(&x);
    check("status: running while it runs, and cannot be asked again",
          strcmp(cJSON_GetObjectItem(o, "state")->valuestring, "running") == 0 &&
              cJSON_IsFalse(cJSON_GetObjectItem(o, "can_expand")));
    cJSON_Delete(o);
    wait_job(&x);
    check("expand: done", !sysd_expand_busy(&x) && x.done && x.error[0] == '\0');
    check("parted grew partition 2 to the end, answering its in-use question",
          calls_have("parted ---pretend-input-tty ") && calls_have("/dev/mmcblk1 resizepart 2 100% <Fix,Yes>"));
    check("then partprobe asked the kernel", calls_have("partprobe "));
    check("then resize2fs grew the root filesystem", calls_have("resize2fs ") && calls_have("/dev/mmcblk1p2\n"));
    check("and the root filesystem now uses the card", state_of(&x) == SYSD_EXPAND_NOT_NEEDED);
    check("no marker is left", access(p_pending, F_OK) != 0);
    check("the tools' output went to the log", access(p_log, F_OK) == 0);
    check("a second expand is refused: nothing to do", sysd_expand_start(&x, msg, sizeof(msg)) == -1 &&
                                                           strstr(msg, "whole card"));

    /* The kernel keeps the old size: the restart path. */
    sysd_expand_init(&x, paths, &fake_ops);
    card();
    flag("kernel-rereads", 0);
    sysd_expand_start(&x, msg, sizeof(msg));
    wait_job(&x);
    check("restart path: not done, and not an error", !x.done && x.error[0] == '\0');
    check("the table is grown, the kernel is not: reboot required", state_of(&x) == SYSD_EXPAND_REBOOT);
    check("resize2fs did not run", !calls_have("resize2fs"));
    f = fopen(p_pending, "r");
    if (f) {
        if (!fgets(text, sizeof(text), f)) {
            text[0] = '\0';
        }
        fclose(f);
    }
    check("a marker names the root partition", strcmp(text, "mmcblk1p2\n") == 0);
    check("expand again says to restart", sysd_expand_start(&x, msg, sizeof(msg)) == -1 && strstr(msg, "restart"));
    o = sysd_expand_status(&x);
    check("status: reboot_required, no unused space left in the table",
          strcmp(cJSON_GetObjectItem(o, "state")->valuestring, "reboot_required") == 0 &&
              cJSON_GetObjectItem(o, "unused_bytes")->valuedouble == 0);
    cJSON_Delete(o);

    /* The restart: the kernel reads the grown table, and the next sysd finishes. */
    put_u64("mmcblk1p2/size", DISK_SECTORS - P2_START);
    put(p_calls, "");
    sysd_expand_init(&x, paths, &fake_ops);
    check("after the restart: finish", state_of(&x) == SYSD_EXPAND_FINISH);
    sysd_expand_startup(&x);
    check("startup finishes the owner's expansion by itself", sysd_expand_busy(&x));
    wait_job(&x);
    check("with resize2fs only", x.done && calls_have("resize2fs") && !calls_have("parted"));
    check("and the marker goes", access(p_pending, F_OK) != 0 && state_of(&x) == SYSD_EXPAND_NOT_NEEDED);

    /* No first-boot resize: without the marker, startup starts nothing. */
    card();
    put_u64("mmcblk1p2/size", DISK_SECTORS - P2_START);
    write_mbr(0x83, DISK_SECTORS - P2_START);
    sysd_expand_init(&x, paths, &fake_ops);
    sysd_expand_startup(&x);
    check("no marker: startup starts nothing, even with a grown partition", !sysd_expand_busy(&x) &&
                                                                          !calls_have("resize2fs"));
    card();
    put(p_pending, "mmcblk1p2\n");
    sysd_expand_init(&x, paths, &fake_ops);
    sysd_expand_startup(&x);
    check("a marker with nothing to finish is removed, nothing runs",
          !sysd_expand_busy(&x) && access(p_pending, F_OK) != 0 && !calls_have("parted"));

    /* parted refuses both ways. */
    card();
    flag("parted-fails", 1);
    sysd_expand_init(&x, paths, &fake_ops);
    sysd_expand_start(&x, msg, sizeof(msg));
    wait_job(&x);
    check("parted failing: tried interactive, then script mode", calls_have("<Fix,Yes>") &&
                                                                  calls_have("parted -s "));
    check("an error naming parted, and nothing changed", strstr(x.error, "parted") &&
                                                          state_of(&x) == SYSD_EXPAND_AVAILABLE &&
                                                          !calls_have("resize2fs"));
    o = sysd_expand_status(&x);
    check("status carries the error", cJSON_IsString(cJSON_GetObjectItem(o, "error")));
    cJSON_Delete(o);

    /* resize2fs fails: the next Expand only grows the filesystem. */
    card();
    flag("resize-fails", 1);
    sysd_expand_init(&x, paths, &fake_ops);
    sysd_expand_start(&x, msg, sizeof(msg));
    wait_job(&x);
    check("resize2fs failing: an error naming it", strstr(x.error, "resize2fs") && !x.done);
    check("the partition is grown, the filesystem is not: finish", state_of(&x) == SYSD_EXPAND_FINISH);
    flag("resize-fails", 0);
    put(p_calls, "");
    check("Expand again", sysd_expand_start(&x, msg, sizeof(msg)) == 0);
    wait_job(&x);
    check("runs resize2fs only, and is done", x.done && x.error[0] == '\0' && !calls_have("parted") &&
                                                 state_of(&x) == SYSD_EXPAND_NOT_NEEDED);

    /* One at a time. */
    card();
    flag("slow", 1);
    sysd_expand_init(&x, paths, &fake_ops);
    sysd_expand_start(&x, msg, sizeof(msg));
    check("a second expand while one runs is busy", sysd_expand_start(&x, msg, sizeof(msg)) == -2);
    wait_job(&x);
    flag("slow", 0);
}

int main(void)
{
    struct sysd_expand_paths paths;
    char cmd[600];

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    path_of(p_block, sizeof(p_block), "sys/class/block");
    path_of(p_dev, sizeof(p_dev), "dev");
    path_of(p_cmdline, sizeof(p_cmdline), "cmdline");
    path_of(p_pending, sizeof(p_pending), "storage-expand.pending");
    path_of(p_log, sizeof(p_log), "storage-expand.log");
    path_of(p_calls, sizeof(p_calls), "calls");
    snprintf(cmd, sizeof(cmd), "mkdir -p %s %s", p_block, p_dev);
    if (system(cmd) != 0) {
        return 1;
    }
    paths.sys_class_block = p_block;
    paths.dev = p_dev;
    paths.cmdline = p_cmdline;
    paths.pending = p_pending;
    paths.log = p_log;

    test_parsers();
    test_layout(&paths);
    test_expand(&paths);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", root);
    }
    printf("sysd_expand_test: %d/%d checks passed\n", checks - failed, checks);
    return failed ? 1 : 0;
}
