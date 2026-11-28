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
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
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
    if (test_fail == 1) {
        return fail(p, "test: forced failure");
    }
#endif
    return 0;
}

/* ---- the system call filter (ADR-016 rule 17, amendment 3) ---------------------------- */

#if defined(__x86_64__)
#define CONFINE_ARCH AUDIT_ARCH_X86_64
#elif defined(__riscv) && __riscv_xlen == 64
#define CONFINE_ARCH AUDIT_ARCH_RISCV64
#elif defined(__aarch64__)
#define CONFINE_ARCH AUDIT_ARCH_AARCH64
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ARG_LO(i) ((uint32_t)(offsetof(struct seccomp_data, args) + 8 * (i)))
#define ARG_HI(i) (ARG_LO(i) + 4)
#else
#define ARG_LO(i) ((uint32_t)(offsetof(struct seccomp_data, args) + 8 * (i) + 4))
#define ARG_HI(i) ((uint32_t)(offsetof(struct seccomp_data, args) + 8 * (i)))
#endif

#define F_LD(off) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (off))
#define F_RET(v) BPF_STMT(BPF_RET | BPF_K, (v))
#define DENY_EPERM (SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))
#define DENY_ENOTTY (SECCOMP_RET_ERRNO | (ENOTTY & SECCOMP_RET_DATA))

/* What the helper does after the drop, and nothing else: read frames and
 * commands, write lines, its log and its saves, keep time, manage memory
 * and end. Each name is the kernel's for both architectures it runs on;
 * where one has no such call (riscv64: poll, renameat, nanosleep), the C
 * library uses the other (ppoll, renameat2, clock_nanosleep). */
static const int allow_plain[] = {
    /* files: the log, the lock, sysfs readings, SAVE through the folder's
     * descriptor; reading commands and frames */
    SYS_read, SYS_readv, SYS_close, SYS_openat, SYS_lseek, SYS_fstat, SYS_newfstatat,
#ifdef SYS_statx
    SYS_statx,
#endif
    SYS_fstatfs, SYS_unlinkat, SYS_renameat2,
#ifdef SYS_renameat
    SYS_renameat,
#endif
    SYS_fsync, SYS_fdatasync, SYS_flock,
    /* the capture socket's counters, the shell's socket */
    SYS_getsockopt, SYS_recvfrom,
    /* waiting and time */
    SYS_ppoll,
#ifdef SYS_poll
    SYS_poll,
#endif
    SYS_clock_gettime, SYS_clock_nanosleep, SYS_gettimeofday,
#ifdef SYS_time
    SYS_time, /* x86-64's time() without a vDSO; riscv64 has no such call */
#endif
#ifdef SYS_nanosleep
    SYS_nanosleep,
#endif
    /* who it is (read only) */
    SYS_getpid, SYS_getppid, SYS_gettid, SYS_getuid, SYS_geteuid, SYS_getgid, SYS_getegid,
    SYS_getresuid, SYS_getresgid,
    /* signals and the end */
    SYS_rt_sigaction, SYS_rt_sigprocmask, SYS_rt_sigreturn, SYS_restart_syscall, SYS_exit, SYS_exit_group,
    /* memory (never executable: see below) */
    SYS_brk, SYS_munmap, SYS_mremap, SYS_madvise, SYS_futex, SYS_sched_yield,
#ifdef SYS_getrandom
    SYS_getrandom,
#endif
};

/* Refused with EPERM, not fatal: the network side of a socket the helper
 * holds (sending, binding, options such as promiscuous membership or a new
 * filter, the transmit ring) and new descriptors that could copy one.
 * libpcap's own teardown asks for one of these (it unmaps the ring with
 * setsockopt before closing); refusing it is harmless, the close frees it. */
static const int deny_quiet[] = {
    SYS_socket, SYS_socketpair, SYS_connect, SYS_bind, SYS_listen, SYS_accept, SYS_accept4,
    SYS_sendmsg, SYS_sendmmsg, SYS_setsockopt, SYS_shutdown, SYS_recvmsg, SYS_dup, SYS_dup3,
#ifdef SYS_dup2
    SYS_dup2,
#endif
};

#define ALLOW_N (sizeof(allow_plain) / sizeof(allow_plain[0]))
#define DENY_N (sizeof(deny_quiet) / sizeof(deny_quiet[0]))
#define PROG_MAX (2 * (ALLOW_N + DENY_N) + 64) /* two per listed call, the rest below */

/* One instruction, never past the array: a program that would not fit is
 * no program (0), and the caller refuses the capture. */
#define EMIT(ins)                                                                                                 \
    do {                                                                                                          \
        if (n >= max) {                                                                                           \
            return 0;                                                                                             \
        }                                                                                                         \
        f[n++] = (struct sock_filter)ins;                                                                         \
    } while (0)

/* Build the filter into f (max instructions). Returns its length, or 0 if
 * it does not fit. */
static unsigned build_filter(struct sock_filter *f, unsigned max, int capture_fd, pid_t self)
{
    uint32_t cap = (uint32_t)capture_fd; /* -1 (no socket) matches no real descriptor */
    unsigned n = 0;
    unsigned i;

    /* Only this architecture's system calls; any other ABI ends it. */
    EMIT(F_LD(offsetof(struct seccomp_data, arch)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, CONFINE_ARCH, 1, 0));
    EMIT(F_RET(SECCOMP_RET_KILL_PROCESS));
    EMIT(F_LD(offsetof(struct seccomp_data, nr)));
#if defined(__x86_64__)
    /* the x32 ABI shares the architecture value: refuse its numbers */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1));
    EMIT(F_RET(SECCOMP_RET_KILL_PROCESS));
#endif
    for (i = 0; i < ALLOW_N; i++) {
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)allow_plain[i], 0, 1));
        EMIT(F_RET(SECCOMP_RET_ALLOW));
    }
    for (i = 0; i < DENY_N; i++) {
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)deny_quiet[i], 0, 1));
        EMIT(F_RET(DENY_EPERM));
    }
    /* write and writev: anywhere but the capture socket (a write there
     * sends a frame). */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_write, 1, 0));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_writev, 0, 4));
    EMIT(F_LD(ARG_LO(0)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, cap, 0, 1));
    EMIT(F_RET(DENY_EPERM));
    EMIT(F_RET(SECCOMP_RET_ALLOW));
    /* sendto: the shell's socket only (not the capture socket), and never
     * to an address. */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_sendto, 0, 8));
    EMIT(F_LD(ARG_LO(0)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, cap, 5, 0));
    EMIT(F_LD(ARG_LO(4)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 3));
    EMIT(F_LD(ARG_HI(4)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    EMIT(F_RET(SECCOMP_RET_ALLOW));
    EMIT(F_RET(DENY_EPERM));
    /* fcntl: flags and close-on-exec only; never a copy of a descriptor. */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fcntl, 0, 7));
    EMIT(F_LD(ARG_LO(1)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_GETFD, 3, 0));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETFD, 2, 0));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_GETFL, 1, 0));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETFL, 0, 1));
    EMIT(F_RET(SECCOMP_RET_ALLOW));
    EMIT(F_RET(DENY_EPERM));
    /* tgkill: to itself only (abort, raise); never another process of the
     * same identity, such as the next capture's helper. */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_tgkill, 0, 4));
    EMIT(F_LD(ARG_LO(0)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)self, 0, 1));
    EMIT(F_RET(SECCOMP_RET_ALLOW));
    EMIT(F_RET(DENY_EPERM));
    /* ioctl: none (interface flags, addresses, routes); "not a terminal". */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_ioctl, 0, 1));
    EMIT(F_RET(DENY_ENOTTY));
    /* mmap and mprotect: never executable memory. */
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mmap, 1, 0));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mprotect, 0, 4));
    EMIT(F_LD(ARG_LO(2)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1));
    EMIT(F_RET(DENY_EPERM));
    EMIT(F_RET(SECCOMP_RET_ALLOW));
    /* Anything else ends the helper at once. */
    EMIT(F_RET(SECCOMP_RET_KILL_PROCESS));
    return n;
}

#if defined(__SANITIZE_ADDRESS__)
/* Sanitizer builds only (the shipped helper has no sanitizer): the leak
 * scan at exit traces the process (prctl, clone, ptrace), which the filter
 * refuses by design, so a confined sanitizer build skips that scan. Every
 * other sanitizer check still runs under the filter; a helper that never
 * confines keeps the scan. */
static int confined;

int __lsan_is_turned_off(void);
int __lsan_is_turned_off(void)
{
    return confined;
}
#endif

static int read_seccomp(int *mode, int *filters)
{
    char line[160];
    FILE *f = fopen("/proc/self/status", "re");

    *mode = -1;
    *filters = -1;
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "Seccomp: %d", mode) != 1) {
            sscanf(line, "Seccomp_filters: %d", filters);
        }
    }
    fclose(f);
    return *mode < 0 ? -1 : 0;
}

int capture_confine(int capture_fd, struct capture_priv *p)
{
#ifdef CONFINE_ARCH
    struct sock_filter f[PROG_MAX];
    struct sock_fprog prog;
    int mode;
    int filters;
    int probe;

    p->why[0] = '\0';
    errno = 0;
    if (capture_fd >= 0 && capture_fd <= 2) {
        /* a standard descriptor must stay writable: the capture socket
         * cannot be one of them */
        return fail(p, "filter: the capture socket is a standard descriptor");
    }
    if (read_seccomp(&mode, &filters) != 0 || mode != 0) {
        errno = 0;
        return fail(p, "filter: already filtered or unreadable");
    }
    memset(&prog, 0, sizeof(prog));
    prog.len = (unsigned short)build_filter(f, PROG_MAX, capture_fd, getpid());
    prog.filter = f;
    if (prog.len == 0) {
        errno = 0;
        return fail(p, "filter: the program does not fit");
    }
    errno = 0;
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog) != 0) {
        return fail(p, "filter: install");
    }
    p->seccomp_fd = capture_fd;
#if defined(__SANITIZE_ADDRESS__)
    confined = 1;
#endif
    /* Prove it, from /proc and by asking for what it refuses. */
    errno = 0;
    if (read_seccomp(&mode, &filters) != 0 || mode != 2 || (filters != -1 && filters != 1)) {
        errno = 0;
        return fail(p, "filter check");
    }
    p->seccomp_mode = mode;
    probe = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (probe >= 0 || errno != EPERM) {
        if (probe >= 0) {
            close(probe);
        }
        errno = 0;
        return fail(p, "filter check: a socket could be made");
    }
    errno = 0;
#if POS_CAPTURE_TEST_HOOKS
    if (test_fail == 2) {
        return fail(p, "test: forced filter failure");
    }
#endif
    return 0;
#else
    (void)capture_fd;
    errno = 0;
    return fail(p, "filter: no filter for this architecture");
#endif
}

int capture_reserve_fd(int fd)
{
    int held[64];
    int nheld = 0;
    int got = -1;
    int i;

    if (fd < 0) {
        return 0;
    }
    /* open() takes the lowest free number: fill the holes below fd, keep
     * fd, give the rest back. */
    while (nheld < (int)(sizeof(held) / sizeof(held[0]))) {
        int d = open("/dev/null", O_RDONLY | O_CLOEXEC);

        if (d < 0 || d > fd) {
            if (d >= 0) {
                close(d);
            }
            break;
        }
        if (d == fd) {
            got = d;
            break;
        }
        held[nheld++] = d;
    }
    for (i = 0; i < nheld; i++) {
        close(held[i]);
    }
    return got == fd ? 0 : -1;
}
