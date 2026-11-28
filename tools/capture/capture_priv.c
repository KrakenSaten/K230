/*
 * The capture helper's least privilege: see capture_priv.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "capture_priv.h"

#include "pocketpaths.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_CLEAR_ALL 4
#endif

#if POS_CAPTURE_TEST_HOOKS
static int test_fail;

void capture_priv_test_fail(int on)
{
    test_fail = on;
}
#endif

/* A folder root may hand to the capture identity although it existed
 * before: named "Captures", in a parent that is root's and writable by
 * root alone, so nobody else chose or can swap it. */
static int captures_by_name(int fd, const char *dir)
{
    const char *base = strrchr(dir, '/');
    struct stat psb;

    base = base ? base + 1 : dir;
    return strcmp(base, "Captures") == 0 && fstatat(fd, "..", &psb, 0) == 0 && S_ISDIR(psb.st_mode) &&
           psb.st_uid == 0 && (psb.st_mode & 022) == 0;
}

int capture_captures_dir(const char *dir)
{
    struct stat sb;
    int created;
    int fd;

    if (!dir) {
        return -1;
    }
    created = lstat(dir, &sb) != 0 && errno == ENOENT;
    if (pocketos_mkdir_p(dir, 0700) != 0) {
        return -1;
    }
    fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        close(fd);
        return -1;
    }
    if (geteuid() == 0) {
        /* The capture identity owns the folder it saves into, and nothing
         * else of root's; the files already in it stay as they are. */
        int ours = sb.st_uid == (uid_t)POS_CAPTURE_ID;
        int may = ours || (sb.st_uid == 0 && (created || captures_by_name(fd, dir)));

        if (!may) {
            close(fd);
            return -1;
        }
        if ((!ours || sb.st_gid != (gid_t)POS_CAPTURE_ID) &&
            fchown(fd, (uid_t)POS_CAPTURE_ID, (gid_t)POS_CAPTURE_ID) != 0) {
            close(fd);
            return -1;
        }
    } else if (sb.st_uid != geteuid()) {
        close(fd);
        return -1;
    }
    /* 0700 whoever owns it now (root left it to the identity above). */
    if ((sb.st_mode & 07777) != 0700 && fchmod(fd, 0700) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Read the credentials back the way anyone else would see them. */
static int read_status(struct capture_priv *p)
{
    char line[160];
    FILE *f = fopen("/proc/self/status", "re");
    int seen = 0;

    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "CapInh: %llx", &p->cap_inh) == 1 || sscanf(line, "CapPrm: %llx", &p->cap_prm) == 1 ||
            sscanf(line, "CapEff: %llx", &p->cap_eff) == 1 || sscanf(line, "CapBnd: %llx", &p->cap_bnd) == 1 ||
            sscanf(line, "CapAmb: %llx", &p->cap_amb) == 1 || sscanf(line, "NoNewPrivs: %d", &p->no_new_privs) == 1) {
            seen++;
        }
    }
    fclose(f);
    return seen >= 5 ? 0 : -1; /* CapAmb is missing on kernels before 4.3 */
}

static unsigned long net_ns(void)
{
    char link[64] = "";
    unsigned long ino = 0;

    if (readlink("/proc/self/ns/net", link, sizeof(link) - 1) > 0) {
        sscanf(link, "net:[%lu]", &ino);
    }
    return ino;
}

static int fail(struct capture_priv *p, const char *step)
{
    if (!p->why[0]) {
        snprintf(p->why, sizeof(p->why), "%s: %s", step, errno ? strerror(errno) : "not as expected");
    }
    return -1;
}

int capture_drop(struct capture_priv *p)
{
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[2];
    uid_t id = (uid_t)POS_CAPTURE_ID;
    gid_t gid = (gid_t)POS_CAPTURE_ID;
    int cap;

    memset(p, 0, sizeof(*p));
    p->was_root = geteuid() == 0;
    p->net_before = net_ns();
    errno = 0;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return fail(p, "no_new_privs");
    }
    if (p->was_root) {
        /* The bounding set needs CAP_SETPCAP, the identity CAP_SETUID and
         * CAP_SETGID: both before the switch takes them away. */
        for (cap = 0; cap < 64; cap++) {
            if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) != 0) {
                if (errno == EINVAL) {
                    break; /* past the kernel's last capability */
                }
                return fail(p, "bounding set");
            }
        }
        /* A network namespace of its own, while CAP_SYS_ADMIN is held: the
         * open capture socket stays in the system's, new sockets do not. */
        errno = 0;
        if (unshare(CLONE_NEWNET) != 0) {
            return fail(p, "network namespace");
        }
        if (setgroups(0, NULL) != 0) {
            return fail(p, "supplementary groups");
        }
        if (setresgid(gid, gid, gid) != 0) {
            return fail(p, "group");
        }
        if (setresuid(id, id, id) != 0) {
            return fail(p, "user");
        }
    }
    /* Whatever is left (a helper started without root holds nothing, but
     * may have been handed ambient or inheritable capabilities). */
    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);
    memset(&hdr, 0, sizeof(hdr));
    memset(data, 0, sizeof(data));
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    errno = 0;
    if (syscall(SYS_capset, &hdr, data) != 0) {
        return fail(p, "capabilities");
    }
    /* A process whose credentials changed is not dumpable; say so in any
     * case, so nobody of the same identity can trace or read it. */
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    /* Prove it. */
    errno = 0;
    if (getresuid(&p->ruid, &p->euid, &p->suid) != 0 || getresgid(&p->rgid, &p->egid, &p->sgid) != 0) {
        return fail(p, "read identity");
    }
    p->ngroups = getgroups(0, NULL);
    p->net_after = net_ns();
    if (read_status(p) != 0) {
        return fail(p, "read status");
    }
    errno = 0;
    if (p->was_root && (p->ruid != id || p->euid != id || p->suid != id || p->rgid != gid || p->egid != gid ||
                        p->sgid != gid || p->ngroups != 0)) {
        return fail(p, "identity check");
    }
    if (p->ruid == 0 || p->euid == 0 || p->suid == 0) {
        return fail(p, "still uid 0");
    }
    if (p->cap_inh || p->cap_prm || p->cap_eff || p->cap_amb || (p->was_root && p->cap_bnd)) {
        return fail(p, "capability check");
    }
    if (p->no_new_privs != 1) {
        return fail(p, "no_new_privs check");
    }
    if (p->was_root && (p->net_after == 0 || p->net_after == p->net_before)) {
        return fail(p, "network namespace check");
    }
    /* The way back must be shut: becoming root again has to be refused. */
    if (setresuid(0, 0, 0) == 0 || errno != EPERM) {
        errno = 0;
        return fail(p, "uid 0 could be taken back");
    }
    errno = 0;
#if POS_CAPTURE_TEST_HOOKS
    if (test_fail) {
        return fail(p, "test: forced failure");
    }
#endif
    return 0;
}
