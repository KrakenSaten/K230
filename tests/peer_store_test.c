/*
 * peerd's persistent state (services/peerd/peer_store.h; ADR-017 sections 2,
 * 5, 6, 8): first creation, every restart, and above all what is never
 * repaired by making something new.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "peer_store.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static char root[64];
static int dirs;

/* A fresh, empty state directory path (not created). */
static const char *fresh(void)
{
    static char path[128];

    snprintf(path, sizeof(path), "%s/d%d", root, dirs++);
    return path;
}

static void path_of(char *out, size_t n, const char *dir, const char *file)
{
    snprintf(out, n, "%s/%s", dir, file);
}

static long size_of(const char *dir, const char *file)
{
    char path[256];
    struct stat st;

    path_of(path, sizeof(path), dir, file);
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int mode_of(const char *dir, const char *file)
{
    char path[256];
    struct stat st;

    if (file) {
        path_of(path, sizeof(path), dir, file);
    } else {
        snprintf(path, sizeof(path), "%s", dir);
    }
    return stat(path, &st) == 0 ? (int)(st.st_mode & 0777) : -1;
}

static int read_file(const char *dir, const char *file, uint8_t *buf, size_t max)
{
    char path[256];
    FILE *f;
    size_t n;

    path_of(path, sizeof(path), dir, file);
    f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    n = fread(buf, 1, max, f);
    fclose(f);
    return (int)n;
}

static void write_file(const char *dir, const char *file, const uint8_t *buf, size_t n)
{
    char path[256];
    FILE *f;

    path_of(path, sizeof(path), dir, file);
    f = fopen(path, "wb");
    if (f) {
        fwrite(buf, 1, n, f);
        fclose(f);
    }
}

static void flip(const char *dir, const char *file, size_t at)
{
    uint8_t buf[512];
    int n = read_file(dir, file, buf, sizeof(buf));

    if (n > 0 && at < (size_t)n) {
        buf[at] ^= 0x01;
        write_file(dir, file, buf, (size_t)n);
    }
}

static void remove_file(const char *dir, const char *file)
{
    char path[256];

    path_of(path, sizeof(path), dir, file);
    unlink(path);
}

static int count_files(const char *dir)
{
    char cmd[300];
    FILE *p;
    int n = -1;

    snprintf(cmd, sizeof(cmd), "ls -A '%s' 2>/dev/null | wc -l", dir);
    p = popen(cmd, "r");
    if (p) {
        if (fscanf(p, "%d", &n) != 1) {
            n = -1;
        }
        pclose(p);
    }
    return n;
}

/* Boot a directory and keep nothing but what the caller asked for. */
static int boot(const char *dir, struct ps_boot *b)
{
    int rc = ps_boot(dir, b);

    return rc;
}

static void done(struct ps_boot *b)
{
    pc_identity_wipe(&b->id);
}

/* ---- first creation, and the runs after it ------------------------------------------- */

static void test_first_and_next(void)
{
    const char *dir = fresh();
    struct ps_boot b;
    uint8_t key1[PC_KEY];
    uint8_t id_before[128];
    uint8_t id_after[128];
    uint8_t ea[32];
    uint8_t eb[32];
    int n1;
    int i;
    int ok;

    check("first: an empty place boots", boot(dir, &b) == 0 && b.fault == PS_FAULT_NONE);
    check("first: a new identity was made, and said so", b.created == 1 && b.id.sk != NULL);
    check("first: this run's epoch is 2 - one above the initial value",
          b.epoch == PS_EPOCH_INITIAL + 1);
    check("first: no kept peers, no name, no fault", b.kept_n == 0 && !b.kept_fault && b.name[0] == '\0');
    check("first: the directory is 0700", mode_of(dir, NULL) == 0700);
    check("first: identity.v1 is 74 bytes, 0600", size_of(dir, "identity.v1") == PS_IDENTITY_SIZE &&
          PS_IDENTITY_SIZE == 74 && mode_of(dir, "identity.v1") == 0600);
    check("first: both epoch files are 16 bytes, 0600",
          size_of(dir, "epoch.a") == PS_EPOCH_SIZE && size_of(dir, "epoch.b") == PS_EPOCH_SIZE &&
          mode_of(dir, "epoch.a") == 0600 && mode_of(dir, "epoch.b") == 0600);
    check("first: nothing else was left behind - no temporary file, no kept list",
          count_files(dir) == 3);
    memcpy(key1, b.id.pk, PC_KEY);
    done(&b);
    n1 = read_file(dir, "identity.v1", id_before, sizeof(id_before));

    check("next: the second run is the same identity, not a new one",
          boot(dir, &b) == 0 && b.created == 0 && memcmp(b.id.pk, key1, PC_KEY) == 0);
    check("next: with the next epoch", b.epoch == 3);
    done(&b);
    check("next: identity.v1 was not rewritten",
          read_file(dir, "identity.v1", id_after, sizeof(id_after)) == n1 &&
          memcmp(id_before, id_after, (size_t)n1) == 0);
    ok = 1;
    for (i = 4; i <= 40; i++) {
        ok &= boot(dir, &b) == 0 && b.epoch == (uint64_t)i && memcmp(b.id.pk, key1, PC_KEY) == 0;
        done(&b);
    }
    check("next: every run after it raises the epoch by exactly one", ok);
    read_file(dir, "epoch.a", ea, sizeof(ea));
    read_file(dir, "epoch.b", eb, sizeof(eb));
    check("next: the two copies hold the last two epochs, one each",
          (ea[11] == 40 && eb[11] == 39) || (ea[11] == 39 && eb[11] == 40));
    check("next: still three files", count_files(dir) == 3);
}

/* ---- the epoch ------------------------------------------------------------------------- */

static void test_epoch(void)
{
    const char *dir = fresh();
    struct ps_boot b;
    uint8_t key[PC_KEY];
    uint8_t raw[32];

    boot(dir, &b);
    memcpy(key, b.id.pk, PC_KEY);
    done(&b);                                   /* epoch 2: a = 1, b = 2 */
    boot(dir, &b);
    done(&b);                                   /* epoch 3 */
    flip(dir, "epoch.a", 6);
    check("epoch: one copy damaged, the other is used and the run goes on",
          boot(dir, &b) == 0 && memcmp(b.id.pk, key, PC_KEY) == 0);
    /* a = 3 was the newest and is the damaged one; b = 2 survives. The lost
     * copy may have held an epoch already used on the air, so this run is
     * two above the survivor, never the same number again. */
    check("epoch: and it is two above the surviving copy - the lost one's number is not reused",
          b.epoch == 4);
    done(&b);
    {
        uint8_t ea[32];
        uint8_t eb[32];

        read_file(dir, "epoch.a", ea, sizeof(ea));
        read_file(dir, "epoch.b", eb, sizeof(eb));
        check("epoch: after the repair the two copies are one apart again",
              (ea[11] == 4 && eb[11] == 3) || (ea[11] == 3 && eb[11] == 4));
    }
    check("epoch: and the damaged copy was the one written over",
          boot(dir, &b) == 0 && size_of(dir, "epoch.a") == PS_EPOCH_SIZE &&
          size_of(dir, "epoch.b") == PS_EPOCH_SIZE);
    {
        uint64_t before = b.epoch;

        done(&b);
        remove_file(dir, "epoch.b");
        check("epoch: one copy gone, the other is used, never a restart from 1",
              boot(dir, &b) == 0 && b.epoch >= before);
        done(&b);
    }
    /* Both unusable, the identity intact: a fault, and nothing is made new. */
    flip(dir, "epoch.a", 5);
    flip(dir, "epoch.b", 5);
    check("epoch: no valid copy while the identity exists is epoch_fault",
          boot(dir, &b) == -1 && b.fault == PS_FAULT_EPOCH && b.id.sk == NULL);
    check("epoch: the fault has its name", strcmp(ps_fault_name(b.fault), "epoch_fault") == 0);
    check("epoch: and it stays one - the next start does not begin again from 1",
          boot(dir, &b) == -1 && b.fault == PS_FAULT_EPOCH);
    remove_file(dir, "epoch.a");
    remove_file(dir, "epoch.b");
    check("epoch: both copies gone while the identity exists is the same fault",
          boot(dir, &b) == -1 && b.fault == PS_FAULT_EPOCH && size_of(dir, "epoch.a") == -1);
    check("epoch: the identity file is still there, untouched", size_of(dir, "identity.v1") == PS_IDENTITY_SIZE);

    /* A truncated and a padded epoch file are not epoch files. */
    dir = fresh();
    boot(dir, &b);
    done(&b);
    read_file(dir, "epoch.a", raw, sizeof(raw));
    write_file(dir, "epoch.a", raw, 15);
    read_file(dir, "epoch.b", raw, sizeof(raw));
    raw[16] = 0;
    write_file(dir, "epoch.b", raw, 17);
    check("epoch: a short copy and a long one are both refused", boot(dir, &b) == -1 &&
          b.fault == PS_FAULT_EPOCH);

    /* The limit: a device that reached it does not wrap. */
    dir = fresh();
    boot(dir, &b);
    done(&b);
    {
        uint8_t top[PS_EPOCH_SIZE] = { 'P', 'D', 'E', '1', 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE };
        uint32_t h = 2166136261u;
        int i;

        for (i = 0; i < 12; i++) {
            h = (h ^ top[i]) * 16777619u;
        }
        top[12] = (uint8_t)(h >> 24);
        top[13] = (uint8_t)(h >> 16);
        top[14] = (uint8_t)(h >> 8);
        top[15] = (uint8_t)h;
        write_file(dir, "epoch.a", top, sizeof(top));
        check("epoch: at its limit there is no next one: a fault, not a wrap",
              boot(dir, &b) == -1 && b.fault == PS_FAULT_EPOCH);
    }
}

/* ---- the identity: never regenerated silently ---------------------------------------------- */

static void test_identity_faults(void)
{
    const char *dir;
    struct ps_boot b;

    /* A device that has run, and whose identity file is then gone. */
    dir = fresh();
    boot(dir, &b);
    done(&b);
    boot(dir, &b);
    done(&b);
    remove_file(dir, "identity.v1");
    check("identity: gone from a device that has run is identity_fault",
          boot(dir, &b) == -1 && b.fault == PS_FAULT_IDENTITY);
    check("identity: and no new one was made in its place", size_of(dir, "identity.v1") == -1 &&
          count_files(dir) == 2);
    check("identity: the fault has its name", strcmp(ps_fault_name(b.fault), "identity_fault") == 0);

    /* A first creation a power cut interrupted: epoch files, initial, and
     * nothing else. That, and only that, is finished. */
    dir = fresh();
    boot(dir, &b);
    done(&b);
    remove_file(dir, "identity.v1");
    {
        uint8_t one[PS_EPOCH_SIZE] = { 'P', 'D', 'E', '1', 0, 0, 0, 0, 0, 0, 0, 1 };
        uint32_t h = 2166136261u;
        int i;

        for (i = 0; i < 12; i++) {
            h = (h ^ one[i]) * 16777619u;
        }
        one[12] = (uint8_t)(h >> 24);
        one[13] = (uint8_t)(h >> 16);
        one[14] = (uint8_t)(h >> 8);
        one[15] = (uint8_t)h;
        write_file(dir, "epoch.a", one, sizeof(one));
        write_file(dir, "epoch.b", one, sizeof(one));
        check("identity: epoch files at their initial value and nothing else is a creation to finish",
              boot(dir, &b) == 0 && b.created == 1 && b.epoch == 2);
        done(&b);
        remove_file(dir, "identity.v1");
        remove_file(dir, "epoch.b");
        write_file(dir, "epoch.a", one, sizeof(one));
        check("identity: so is one epoch file alone, initial", boot(dir, &b) == 0 && b.created == 1);
        done(&b);
        /* ... but not with a kept list beside it: this device has played. */
        remove_file(dir, "identity.v1");
        write_file(dir, "epoch.a", one, sizeof(one));
        write_file(dir, "epoch.b", one, sizeof(one));
        ps_kept_save(dir, NULL, 0);
        check("identity: with a kept list beside them it is a fault, not a creation",
              boot(dir, &b) == -1 && b.fault == PS_FAULT_IDENTITY && size_of(dir, "identity.v1") == -1);
        remove_file(dir, "kept.v1");
        /* ... nor when an epoch file is damaged. */
        flip(dir, "epoch.a", 9);
        check("identity: with a damaged epoch file beside them it is a fault too",
              boot(dir, &b) == -1 && b.fault == PS_FAULT_IDENTITY && size_of(dir, "identity.v1") == -1);
    }

    /* First creation leaves nothing half-written behind, and a stray
     * temporary file from an interrupted one is not state. */
    dir = fresh();
    boot(dir, &b);
    done(&b);
    check("creation: no temporary file is left", size_of(dir, "epoch.a.tmp") == -1 &&
          size_of(dir, "epoch.b.tmp") == -1 && size_of(dir, "identity.v1.tmp") == -1);
    dir = fresh();
    mkdir(dir, 0700);
    write_file(dir, "epoch.a.tmp", (const uint8_t *)"half", 4);
    check("creation: only a temporary file from a cut-off first start, and the device still gets its identity",
          boot(dir, &b) == 0 && b.created == 1 && b.epoch == 2);
    done(&b);
    {
        /* One copy already written by the interrupted creation is kept as it is. */
        uint8_t one[PS_EPOCH_SIZE] = { 'P', 'D', 'E', '1', 0, 0, 0, 0, 0, 0, 0, 1 };
        uint32_t h = 2166136261u;
        struct stat before;
        struct stat after;
        char path[256];
        int i;

        for (i = 0; i < 12; i++) {
            h = (h ^ one[i]) * 16777619u;
        }
        one[12] = (uint8_t)(h >> 24);
        one[13] = (uint8_t)(h >> 16);
        one[14] = (uint8_t)(h >> 8);
        one[15] = (uint8_t)h;
        dir = fresh();
        mkdir(dir, 0700);
        write_file(dir, "epoch.a", one, sizeof(one));
        path_of(path, sizeof(path), dir, "epoch.a");
        stat(path, &before);
        check("creation: resumed with one epoch copy already there, it finishes",
              boot(dir, &b) == 0 && b.created == 1 && size_of(dir, "epoch.b") == PS_EPOCH_SIZE);
        done(&b);
        stat(path, &after);
        check("creation: and the copy that was there was not replaced by a new file",
              before.st_ino == after.st_ino);
        /* An empty epoch file and no identity: not a state this code leaves,
         * and not one it makes an identity over. */
        dir = fresh();
        mkdir(dir, 0700);
        write_file(dir, "epoch.a", one, 0);
        check("creation: an empty epoch file and nothing else is a fault, not a new device",
              boot(dir, &b) == -1 && b.fault == PS_FAULT_IDENTITY && size_of(dir, "identity.v1") == -1);
    }

    /* A damaged, short, long or inconsistent identity file. */
    dir = fresh();
    boot(dir, &b);
    done(&b);
    flip(dir, "identity.v1", 20);
    check("identity: one flipped bit in it is identity_fault", boot(dir, &b) == -1 &&
          b.fault == PS_FAULT_IDENTITY && b.id.sk == NULL);
    check("identity: the damaged file is left as it is: it is the only evidence",
          size_of(dir, "identity.v1") == PS_IDENTITY_SIZE);
    dir = fresh();
    boot(dir, &b);
    done(&b);
    {
        uint8_t raw[128];
        int n = read_file(dir, "identity.v1", raw, sizeof(raw));

        write_file(dir, "identity.v1", raw, (size_t)n - 1);
        check("identity: a file one byte short is refused", boot(dir, &b) == -1 &&
              b.fault == PS_FAULT_IDENTITY);
        raw[n] = 0;
        write_file(dir, "identity.v1", raw, (size_t)n + 1);
        check("identity: a file one byte long is refused", boot(dir, &b) == -1 &&
              b.fault == PS_FAULT_IDENTITY);
        /* The two halves of the key not belonging together, re-sealed with
         * a correct checksum: only the key check can catch it. */
        {
            uint32_t h = 2166136261u;
            int i;

            raw[6 + 40] ^= 0x10;
            for (i = 0; i < 6 + PC_SECRET; i++) {
                h = (h ^ raw[i]) * 16777619u;
            }
            raw[70] = (uint8_t)(h >> 24);
            raw[71] = (uint8_t)(h >> 16);
            raw[72] = (uint8_t)(h >> 8);
            raw[73] = (uint8_t)h;
            write_file(dir, "identity.v1", raw, (size_t)n);
            check("identity: a key whose public half is not its seed's is refused, checksum or not",
                  boot(dir, &b) == -1 && b.fault == PS_FAULT_IDENTITY);
        }
    }
    /* A symbolic link where the identity should be is not followed. */
    dir = fresh();
    boot(dir, &b);
    done(&b);
    {
        char from[256];
        char to[256];

        path_of(from, sizeof(from), dir, "identity.v1");
        path_of(to, sizeof(to), dir, "elsewhere");
        check("identity: a symbolic link in its place is not followed",
              rename(from, to) == 0 && symlink(to, from) == 0 && boot(dir, &b) == -1 &&
              b.fault == PS_FAULT_IDENTITY);
    }
    /* Somewhere that cannot be a directory. */
    check("store: a state path that cannot be made is a store fault",
          boot("/proc/definitely/not/here", &b) == -1 && b.fault == PS_FAULT_IO);
    check("store: nothing to boot is refused", ps_boot(NULL, &b) == -1 && ps_boot(root, NULL) == -1);
}

/* ---- kept peers and the name ------------------------------------------------------------------ */

static void test_kept(void)
{
    const char *dir = fresh();
    struct ps_boot b;
    struct ps_kept recs[PS_KEPT_MAX + 1];
    struct pc_identity other[PS_KEPT_MAX];
    uint8_t own[PC_KEY];
    int i;

    boot(dir, &b);
    memcpy(own, b.id.pk, PC_KEY);
    done(&b);
    memset(recs, 0, sizeof(recs));
    for (i = 0; i < PS_KEPT_MAX; i++) {
        pc_identity_generate(&other[i]);
        memcpy(recs[i].key, other[i].pk, PC_KEY);
        recs[i].epoch = 100 + (uint64_t)i;
        recs[i].mark = 0x1122334455667788ull + (uint64_t)i;
        recs[i].addr = 0xC0A80100u + (uint32_t)i;
        recs[i].port = (uint16_t)(47230 + i);
    }
    check("kept: four records are written", ps_kept_save(dir, recs, PS_KEPT_MAX) == 0 &&
          mode_of(dir, "kept.v1") == 0600 &&
          size_of(dir, "kept.v1") == PS_KEPT_HEADER + 4 * PS_KEPT_RECORD + 4);
    check("kept: no temporary file is left", size_of(dir, "kept.v1.tmp") == -1);
    check("kept: and read back whole at the next start", boot(dir, &b) == 0 && b.kept_n == 4 &&
          !b.kept_fault && memcmp(b.kept[2].key, recs[2].key, PC_KEY) == 0 &&
          b.kept[2].epoch == 102 && b.kept[2].mark == recs[2].mark && b.kept[2].addr == recs[2].addr &&
          b.kept[2].port == recs[2].port);
    done(&b);
    check("kept: five is refused", ps_kept_save(dir, recs, PS_KEPT_MAX + 1) == -1);
    check("kept: one record replaces the four", ps_kept_save(dir, recs + 1, 1) == 0 &&
          boot(dir, &b) == 0 && b.kept_n == 1 && memcmp(b.kept[0].key, recs[1].key, PC_KEY) == 0);
    done(&b);
    check("kept: an empty list is a list", ps_kept_save(dir, NULL, 0) == 0 && boot(dir, &b) == 0 &&
          b.kept_n == 0 && !b.kept_fault);
    done(&b);

    ps_kept_save(dir, recs, 3);
    flip(dir, "kept.v1", 30);
    check("kept: a damaged list is kept_fault - and peerd still boots, with none",
          boot(dir, &b) == 0 && b.kept_fault == 1 && b.kept_n == 0 && b.fault == PS_FAULT_NONE);
    done(&b);
    {
        uint8_t raw[400];
        int n;

        ps_kept_save(dir, recs, 3);
        n = read_file(dir, "kept.v1", raw, sizeof(raw));
        write_file(dir, "kept.v1", raw, (size_t)n - 1);
        check("kept: a truncated list is kept_fault", boot(dir, &b) == 0 && b.kept_fault && b.kept_n == 0);
        done(&b);
    }
    /* Our own key, the same peer twice, a key that cannot be used: lists
     * this code never writes. */
    memcpy(recs[1].key, own, PC_KEY);
    ps_kept_save(dir, recs, 3);
    check("kept: a list holding this device's own key is kept_fault",
          boot(dir, &b) == 0 && b.kept_fault && b.kept_n == 0);
    done(&b);
    memcpy(recs[1].key, recs[0].key, PC_KEY);
    ps_kept_save(dir, recs, 3);
    check("kept: a list holding one peer twice is kept_fault",
          boot(dir, &b) == 0 && b.kept_fault && b.kept_n == 0);
    done(&b);
    memset(recs[1].key, 0, PC_KEY);
    recs[1].key[0] = 1;                         /* the identity point: order 1 */
    ps_kept_save(dir, recs, 3);
    check("kept: a list holding a small-order key is kept_fault",
          boot(dir, &b) == 0 && b.kept_fault && b.kept_n == 0);
    done(&b);

    check("name: a name is stored and read back",
          ps_name_save(dir, "Bj\xc3\xb8rn") == 0 && boot(dir, &b) == 0 &&
          strcmp(b.name, "Bj\xc3\xb8rn") == 0);
    done(&b);
    check("name: one with a control character, or too long, is not stored",
          ps_name_save(dir, "a\x01") == -1 && ps_name_save(dir, "1234567890123456789012345") == -1);
    check("name: an empty one clears it", ps_name_save(dir, "") == 0 && boot(dir, &b) == 0 &&
          b.name[0] == '\0');
    done(&b);
    {
        uint8_t junk[40];

        memset(junk, 0x01, sizeof(junk));
        write_file(dir, "name.v1", junk, sizeof(junk));
        check("name: a name file that is not a name is ignored, not a fault",
              boot(dir, &b) == 0 && b.name[0] == '\0' && b.fault == PS_FAULT_NONE);
        done(&b);
    }
    for (i = 0; i < PS_KEPT_MAX; i++) {
        pc_identity_wipe(&other[i]);
    }
}

int main(void)
{
    char cmd[128];

    if (pc_init() != 0) {
        printf("FAIL libsodium cannot start\n");
        return 1;
    }
    snprintf(root, sizeof(root), "/tmp/peer_store.XXXXXX");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    test_first_and_next();
    test_epoch();
    test_identity_faults();
    test_kept();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        perror("rm");
    }
    printf("peer_store_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
