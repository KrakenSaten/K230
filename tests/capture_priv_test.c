/*
 * The capture helper's least privilege (tools/capture/capture_priv.c,
 * ADR-016 rule 17 amended), tested in child processes, because a drop
 * cannot be undone.
 *
 * As root (the unit's case; run with sudo or `wsl -u root` on the host):
 *   - the identity becomes pos-capture (uid and gid POS_CAPTURE_ID), no
 *     supplementary groups, every capability set empty, no_new_privs set;
 *   - root-owned files and a root-owned 0660 device node, as many device
 *     nodes are, can no longer be opened; a raw socket cannot be made;
 *     uid 0 cannot be taken back; root's folders cannot be written;
 *   - SAVE's folder, prepared before the drop, takes a new 0600 file through
 *     its descriptor; the folder is the identity's and 0700, the files
 *     already in it stay root's;
 *   - a folder that is a link, not a folder, or someone else's is refused;
 *   - a forced failure of the last check fails closed.
 * As root and without (amendment 3), the system call filter:
 *   - installs and reads back (Seccomp 2), refusing a socket;
 *   - keeps the capture socket (a real AF_PACKET socket as root) receive-
 *     only: no write, writev or sendto; no promiscuous membership, filter,
 *     bind or connect; no copy by dup, dup3 or fcntl; no ioctl;
 *   - refuses new sockets, sendmsg and executable memory;
 *   - leaves the helper what it does: reading, writing its files and lines
 *     on the shell's socket, SAVE through a folder descriptor, counters,
 *     time, memory;
 *   - ends the process on anything else (prctl, execve, ptrace, kill, the
 *     x32 ABI): killed by SIGSYS;
 *   - refuses a capture socket on a standard descriptor, fails closed on a
 *     forced failure, and the socket's number stays taken after STOP.
 * Without root: the helper leaves nothing to switch, still drops and
 * checks, and the folder rules that need no root hold.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "../tools/capture/capture_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static int skipped;

static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static void skip(const char *what)
{
    printf("skip %s\n", what);
    skipped++;
}

/* Run fn in a child; its exit code is a bit mask of the checks that failed. */
static int in_child(int (*fn)(const char *), const char *arg)
{
    int status = 0;
    pid_t pid = fork();

    if (pid == 0) {
        _exit(fn(arg));
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return 0xff;
    }
    return WEXITSTATUS(status);
}

static char work[512];

static void path_of(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", work, name);
}

/* ---- as root ------------------------------------------------------------------------------- */

#define R_DROP 0x01
#define R_IDENTITY 0x02
#define R_FILES 0x04
#define R_NODE 0x08
#define R_SOCKET 0x10
#define R_BACK 0x20
#define R_SAVE 0x40
#define R_PARENT 0x80

static int root_child(const char *unused)
{
    struct capture_priv p;
    char path[640];
    struct stat sb;
    int bad = 0;
    int dfd;
    int fd;

    (void)unused;
    path_of(path, sizeof(path), "Captures");
    dfd = capture_captures_dir(path);
    if (dfd < 0 || capture_drop(&p) != 0) {
        return R_DROP;
    }
    if (getuid() != (uid_t)POS_CAPTURE_ID || geteuid() != (uid_t)POS_CAPTURE_ID ||
        getgid() != (gid_t)POS_CAPTURE_ID || getgroups(0, NULL) != 0 || p.cap_eff || p.cap_prm || p.cap_bnd ||
        p.no_new_privs != 1) {
        bad |= R_IDENTITY;
    }
    path_of(path, sizeof(path), "root-file");
    if (open(path, O_RDONLY | O_CLOEXEC) >= 0 || open(path, O_WRONLY | O_CLOEXEC) >= 0) {
        bad |= R_FILES;
    }
    path_of(path, sizeof(path), "root-node");
    if (open(path, O_RDWR | O_CLOEXEC) >= 0 || errno != EACCES) {
        bad |= R_NODE;
    }
    if (socket(AF_PACKET, SOCK_RAW, 0) >= 0 || socket(AF_INET, SOCK_RAW, 1) >= 0) {
        bad |= R_SOCKET;
    }
    /* an ordinary socket can be made, but in a network namespace of its own
     * it reaches no network */
    {
        struct sockaddr_in to;
        int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(9);
        inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);
        if (p.net_after == p.net_before || s < 0 || connect(s, (struct sockaddr *)&to, sizeof(to)) == 0 ||
            errno != ENETUNREACH) {
            bad |= R_SOCKET;
        }
    }
    if (setuid(0) == 0 || setresuid(0, 0, 0) == 0 || setgid(0) == 0) {
        bad |= R_BACK;
    }
    fd = openat(dfd, ".capture-1.pcap.part", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || write(fd, "x", 1) != 1 || close(fd) != 0 ||
        renameat(dfd, ".capture-1.pcap.part", dfd, "capture-1.pcap") != 0 ||
        fstatat(dfd, "capture-1.pcap", &sb, AT_SYMLINK_NOFOLLOW) != 0 || sb.st_uid != (uid_t)POS_CAPTURE_ID ||
        (sb.st_mode & 07777) != 0600) {
        bad |= R_SAVE;
    }
    path_of(path, sizeof(path), "parent-file");
    if (open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600) >= 0) {
        bad |= R_PARENT;
    }
    return bad;
}

static int forced_child(const char *unused)
{
    struct capture_priv p;

    (void)unused;
    capture_priv_test_fail(1);
    return capture_drop(&p) == -1 && strstr(p.why, "forced") ? 0 : 1;
}

static int refused_dir_child(const char *name)
{
    char path[640];

    path_of(path, sizeof(path), name);
    return capture_captures_dir(path) < 0 ? 0 : 1;
}

static void as_root(void)
{
    char path[640];
    char link[640];
    struct stat sb;
    int bits;
    int fd;

    /* root's things the identity must no longer reach */
    path_of(path, sizeof(path), "root-file");
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0660);
    if (fd >= 0) {
        close(fd);
    }
    path_of(path, sizeof(path), "root-node");
    unlink(path);
    check("setup: a root-owned 0660 character device node",
          mknod(path, S_IFCHR | 0660, makedev(1, 3)) == 0 && chmod(path, 0660) == 0);
    /* SAVE's folder with a root-owned capture already in it, root's 0700 */
    path_of(path, sizeof(path), "Captures");
    mkdir(path, 0700);
    path_of(path, sizeof(path), "Captures/capture-old.pcap");
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        close(fd);
    }
    chmod(work, 0700);

    bits = in_child(root_child, NULL);
    check("as root: the drop succeeds and reads back", !(bits & R_DROP));
    check("the identity is pos-capture: uid and gid " "65532" ", no groups, every capability set empty, no_new_privs",
          !(bits & (R_DROP | R_IDENTITY)));
    check("root-owned files cannot be opened any more", !(bits & (R_DROP | R_FILES)));
    check("a root-owned 0660 device node cannot be opened any more", !(bits & (R_DROP | R_NODE)));
    check("no raw socket can be made; a network namespace of its own: an ordinary socket reaches no network",
          !(bits & (R_DROP | R_SOCKET)));
    check("uid 0 and gid 0 cannot be taken back", !(bits & (R_DROP | R_BACK)));
    check("SAVE's folder takes a new 0600 file of the identity through its descriptor", !(bits & (R_DROP | R_SAVE)));
    check("root's folder around it cannot be written", !(bits & (R_DROP | R_PARENT)));
    path_of(path, sizeof(path), "Captures");
    check("the captures folder is the identity's and 0700",
          stat(path, &sb) == 0 && sb.st_uid == (uid_t)POS_CAPTURE_ID && sb.st_gid == (gid_t)POS_CAPTURE_ID &&
              (sb.st_mode & 07777) == 0700);
    path_of(path, sizeof(path), "Captures/capture-old.pcap");
    check("a capture already there stays root's and 0600",
          stat(path, &sb) == 0 && sb.st_uid == 0 && (sb.st_mode & 07777) == 0600);

    check("a forced failure of the last check fails closed", in_child(forced_child, NULL) == 0);

    path_of(path, sizeof(path), "root-other");
    mkdir(path, 0700);
    check("a root-owned folder that is not a Captures folder is refused, and keeps its owner",
          in_child(refused_dir_child, "root-other") == 0 && stat(path, &sb) == 0 && sb.st_uid == 0);
    path_of(path, sizeof(path), "someone-elses");
    mkdir(path, 0700);
    check("setup: a folder of another user", chown(path, 1234, 1234) == 0);
    check("a captures folder of another user is refused", in_child(refused_dir_child, "someone-elses") == 0);
    path_of(path, sizeof(path), "Captures");
    path_of(link, sizeof(link), "Captures-link");
    unlink(link);
    check("setup: a link to the folder", symlink(path, link) == 0);
    check("a captures folder that is a link is refused", in_child(refused_dir_child, "Captures-link") == 0);
}

/* ---- the system call filter (as root and without) ------------------------------------------ */

#define S_INSTALL 0x01
#define S_SEND 0x02
#define S_SOCKOPT 0x04
#define S_NEWSOCK 0x08
#define S_COPY 0x10
#define S_EXEC 0x20
#define S_ALLOWED 0x40
#define S_RESERVE 0x80
#define S_WORDS 0x100

/* The capture descriptor: a real capture socket as root, a datagram socket
 * standing in for one without. stdout becomes a stream socket, as the
 * shell's is on the unit. */
static int filter_setup(int *capfd, int *shell)
{
    int sv[2];

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0 || dup2(sv[1], STDOUT_FILENO) < 0) {
        return -1;
    }
    close(sv[1]);
    *shell = sv[0];
    *capfd = geteuid() == 0 ? socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL))
                            : socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    return *capfd > 2 ? 0 : -1;
}

static int refused(long rc)
{
    return rc == -1 && errno == EPERM;
}

static int filter_child(const char *unused)
{
    struct capture_priv p;
    struct packet_mreq mr;
    struct sock_filter all = BPF_STMT(BPF_RET | BPF_K, 0xffff);
    struct sock_fprog fp = { 1, &all };
    struct sockaddr_ll ll;
    struct ifreq ifr;
    struct iovec iov = { (void *)"x", 1 };
    struct msghdr mh;
    struct statvfs vfs;
    struct timespec ts;
    struct stat sb;
    struct pollfd pf;
    char path[640];
    char buf[8];
    int sv[2];
    int bad = 0;
    int capfd;
    int shell;
    int dfd;
    int fd;
    int v = 0;
    socklen_t vl = sizeof(v);
    void *m;

    (void)unused;
    path_of(path, sizeof(path), "f-captures");
    dfd = capture_captures_dir(path);
    if (dfd < 0 || filter_setup(&capfd, &shell) != 0 || capture_drop(&p) != 0 || capture_confine(capfd, &p) != 0 ||
        p.seccomp_mode != 2 || p.seccomp_fd != capfd) {
        return S_INSTALL;
    }
    /* receive-only */
    memset(&ll, 0, sizeof(ll));
    ll.sll_family = AF_PACKET;
    ll.sll_ifindex = 1;
    ll.sll_halen = 6;
    if (!refused(write(capfd, "x", 1)) || !refused(writev(capfd, &iov, 1)) ||
        !refused(sendto(capfd, "x", 1, 0, NULL, 0)) ||
        !refused(sendto(capfd, "x", 1, 0, (struct sockaddr *)&ll, sizeof(ll)))) {
        bad |= S_SEND;
    }
    memset(&mr, 0, sizeof(mr));
    mr.mr_ifindex = 1;
    mr.mr_type = PACKET_MR_PROMISC;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "lo");
    if (!refused(setsockopt(capfd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr))) ||
        !refused(setsockopt(capfd, SOL_SOCKET, SO_ATTACH_FILTER, &fp, sizeof(fp))) ||
        !refused(setsockopt(capfd, SOL_SOCKET, SO_DETACH_FILTER, &v, sizeof(v))) ||
        !refused(bind(capfd, (struct sockaddr *)&ll, sizeof(ll))) ||
        !refused(connect(capfd, (struct sockaddr *)&ll, sizeof(ll))) ||
        ioctl(capfd, SIOCGIFFLAGS, &ifr) != -1 || ioctl(capfd, SIOCSIFFLAGS, &ifr) != -1) {
        bad |= S_SOCKOPT;
    }
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (!refused(socket(AF_INET, SOCK_DGRAM, 0)) || !refused(socket(AF_PACKET, SOCK_RAW, 0)) ||
        !refused(socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) || !refused(sendmsg(STDOUT_FILENO, &mh, MSG_DONTWAIT))) {
        bad |= S_NEWSOCK;
    }
    if (!refused(dup(capfd)) || !refused(dup3(capfd, 60, 0)) || !refused(fcntl(capfd, F_DUPFD, 0)) ||
        !refused(fcntl(capfd, F_DUPFD_CLOEXEC, 0)) || !refused(fcntl(capfd, F_SETOWN, getpid())) ||
        !refused(fcntl(capfd, F_SETSIG, SIGUSR1))) {
        bad |= S_COPY;
    }
    /* the filter reads the words the kernel uses: a descriptor with high bits
     * set is still the capture socket; an address above 4 GiB is still an
     * address; signals go to itself only */
    {
        struct flock fl;

        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_RDLCK;
        if (!refused(syscall(SYS_write, (long)((1ULL << 32) | (unsigned)capfd), "x", 1L)) ||
            !refused(syscall(SYS_sendto, 1L, "x", 1L, 0L, (long)(1ULL << 32), 16L)) ||
            !refused(fcntl(dfd, F_SETLK, &fl)) || !refused(syscall(SYS_tgkill, (long)getppid(), (long)getppid(), 0L)) ||
            syscall(SYS_tgkill, (long)getpid(), (long)syscall(SYS_gettid), 0L) != 0) {
            bad |= S_WORDS;
        }
    }
    m = mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m != MAP_FAILED || errno != EPERM) {
        bad |= S_EXEC;
    }
    m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || !refused(mprotect(m, 4096, PROT_READ | PROT_EXEC))) {
        bad |= S_EXEC;
    }
    /* what the helper does */
    pf.fd = capfd;
    pf.events = POLLIN;
    fd = openat(dfd, ".capture-1.pcap.part", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (m == MAP_FAILED || mprotect(m, 4096, PROT_READ) != 0 || munmap(m, 4096) != 0 ||
        send(STDOUT_FILENO, "line\n", 5, MSG_DONTWAIT | MSG_NOSIGNAL) != 5 || write(STDOUT_FILENO, "w\n", 2) != 2 ||
        write(shell, "cmd\n", 4) != 4 || recv(STDOUT_FILENO, buf, sizeof(buf), MSG_DONTWAIT) != 4 ||
        getsockopt(capfd, SOL_SOCKET, SO_TYPE, &v, &vl) != 0 || poll(&pf, 1, 0) < 0 ||
        fcntl(capfd, F_GETFL) < 0 || fcntl(capfd, F_SETFL, O_NONBLOCK) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || fd < 0 || write(fd, "x", 1) != 1 || fsync(fd) != 0 ||
        close(fd) != 0 || renameat(dfd, ".capture-1.pcap.part", dfd, "capture-1.pcap") != 0 ||
        fstatat(dfd, "capture-1.pcap", &sb, AT_SYMLINK_NOFOLLOW) != 0 || fstatvfs(dfd, &vfs) != 0 ||
        unlinkat(dfd, "capture-1.pcap", 0) != 0 || !malloc(8 << 20) || getppid() <= 0) {
        bad |= S_ALLOWED;
    }
    /* STOP: the socket's number stays taken, also with a free number below
     * it (the shell's end, made before it); a save gets another */
    v = capfd;
    if (shell >= capfd || close(shell) != 0 || close(capfd) != 0 || capture_reserve_fd(v) != 0 || fstat(v, &sb) != 0 || !S_ISCHR(sb.st_mode) ||
        (fd = openat(dfd, "after", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0 || fd == v ||
        write(fd, "x", 1) != 1) {
        bad |= S_RESERVE;
    }
    return bad;
}

/* After the filter, a call it does not allow ends the process (SIGSYS). */
static int killed_child(const char *which)
{
    struct capture_priv p;
    int capfd;
    int shell;

    if (filter_setup(&capfd, &shell) != 0 || capture_drop(&p) != 0 || capture_confine(capfd, &p) != 0) {
        return 1;
    }
    if (strcmp(which, "prctl") == 0) {
        prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
    } else if (strcmp(which, "execve") == 0) {
        char *argv[] = { (char *)"/bin/true", NULL };

        execve("/bin/true", argv, argv + 1);
    } else if (strcmp(which, "ptrace") == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
    } else if (strcmp(which, "x32") == 0) {
#if defined(__x86_64__)
        syscall(0x40000000L | SYS_getpid);
#else
        return 2; /* no second ABI to try */
#endif
    } else if (strcmp(which, "kill") == 0) {
        kill(getppid(), 0);
    } else if (strcmp(which, "sendfile") == 0) {
        syscall(SYS_sendfile, (long)capfd, 0L, 0L, 1L);
    } else if (strcmp(which, "splice") == 0) {
        syscall(SYS_splice, 0L, 0L, (long)capfd, 0L, 1L, 0L);
    } else if (strcmp(which, "vmsplice") == 0) {
        syscall(SYS_vmsplice, (long)capfd, 0L, 0L, 0L);
    } else if (strcmp(which, "pwrite") == 0) {
        syscall(SYS_pwrite64, (long)capfd, "x", 1L, 0L);
    } else if (strcmp(which, "io_uring") == 0) {
        syscall(425L, 1L, 0L); /* io_uring_setup */
    } else if (strcmp(which, "pidfd_getfd") == 0) {
        syscall(438L, 0L, (long)capfd, 0L); /* pidfd_getfd */
    } else if (strcmp(which, "clone") == 0) {
        syscall(SYS_clone, (long)SIGCHLD, 0L, 0L, 0L, 0L);
    } else if (strcmp(which, "linkat") == 0) {
        syscall(SYS_linkat, (long)AT_FDCWD, "/tmp/a", (long)AT_FDCWD, "/tmp/b", 0L);
    }
    return 3; /* still alive */
}

/* 0 when the child was killed by SIGSYS; its exit code + 1 otherwise. */
static int child_signal(const char *which)
{
    int status = 0;
    pid_t pid = fork();

    if (pid == 0) {
        _exit(killed_child(which));
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        return 0xff;
    }
    if (WIFSIGNALED(status)) {
        return WTERMSIG(status) == SIGSYS ? 0 : 0x80 | WTERMSIG(status);
    }
    return WEXITSTATUS(status) + 1;
}

static int standard_fd_child(const char *unused)
{
    struct capture_priv p;

    (void)unused;
    return capture_drop(&p) == 0 && capture_confine(STDOUT_FILENO, &p) == -1 && strstr(p.why, "standard") ? 0 : 1;
}

static int forced_filter_child(const char *unused)
{
    struct capture_priv p;
    int capfd;
    int shell;

    (void)unused;
    capture_priv_test_fail(2);
    return filter_setup(&capfd, &shell) == 0 && capture_drop(&p) == 0 && capture_confine(capfd, &p) == -1 &&
                   strstr(p.why, "forced")
               ? 0
               : 1;
}

static void the_filter(void)
{
    const char *who = geteuid() == 0 ? "as root, a real capture socket" : "without root, a stand-in socket";
    char what[220];
    int bits = in_child(filter_child, NULL);
    int r[4];

    snprintf(what, sizeof(what), "filter (%s): installs after the drop, reads back as Seccomp 2 (bits %#x)", who,
             (unsigned)bits);
    check(what, !(bits & S_INSTALL));
    check("filter: the capture socket only receives (write, writev, sendto with or without an address refused)",
          !(bits & (S_INSTALL | S_SEND)));
    check("filter: no promiscuous membership, filter change, bind, connect or ioctl on it",
          !(bits & (S_INSTALL | S_SOCKOPT)));
    check("filter: no new socket, no socket pair, no sendmsg", !(bits & (S_INSTALL | S_NEWSOCK)));
    check("filter: no copy of a descriptor (dup, dup3, fcntl F_DUPFD, F_DUPFD_CLOEXEC, F_SETOWN)",
          !(bits & (S_INSTALL | S_COPY)));
    check("filter: no executable memory (mmap, mprotect)", !(bits & (S_INSTALL | S_EXEC)));
    check("filter: the helper's own calls still work (lines on the shell's socket, SAVE through the folder, "
          "counters, poll, time, memory)",
          !(bits & (S_INSTALL | S_ALLOWED)));
    check("filter: after STOP the socket's number stays taken, also over a free number below it; a save gets another",
          !(bits & (S_INSTALL | S_RESERVE)));
    check("filter: it reads the kernel's words (a descriptor with high bits, an address above 4 GiB); fcntl F_SETSIG "
          "and F_SETLK refused; tgkill to itself only",
          !(bits & (S_INSTALL | S_WORDS)));
    r[0] = child_signal("prctl");
    r[1] = child_signal("execve");
    r[2] = child_signal("ptrace");
    r[3] = child_signal("kill");
    snprintf(what, sizeof(what),
             "filter: a call it does not allow ends the process with SIGSYS: prctl (%d), execve (%d), ptrace (%d), "
             "kill (%d)",
             r[0], r[1], r[2], r[3]);
    check(what, r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0);
#if defined(__x86_64__)
    check("filter: the x32 ABI ends the process with SIGSYS", child_signal("x32") == 0);
#endif
    {
        static const char *const more[] = { "sendfile", "splice", "vmsplice", "pwrite", "io_uring", "pidfd_getfd",
                                            "clone", "linkat" };
        char got[160] = "";
        int all = 1;
        size_t i;

        for (i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
            int rc = child_signal(more[i]);

            all &= rc == 0;
            snprintf(got + strlen(got), sizeof(got) - strlen(got), " %s=%d", more[i], rc);
        }
        snprintf(what, sizeof(what), "filter: sendfile, splice, vmsplice and pwrite to the socket, io_uring, pidfd_getfd, "
                 "clone and linkat end the process with SIGSYS (%s )", got);
        check(what, all);
    }
    check("filter: a capture socket on a standard descriptor is refused", in_child(standard_fd_child, NULL) == 0);
    check("filter: a forced failure of its last check fails closed", in_child(forced_filter_child, NULL) == 0);
}

/* ---- without root -------------------------------------------------------------------------- */

static int user_child(const char *unused)
{
    struct capture_priv p;

    (void)unused;
    return capture_drop(&p) == 0 && !p.was_root && p.euid == geteuid() && p.cap_eff == 0 && p.cap_prm == 0 &&
                   p.no_new_privs == 1
               ? 0
               : 1;
}

static void as_user(void)
{
    char path[640];
    char link[640];
    struct stat sb;
    int fd;

    if (geteuid() != 0) {
        check("without root: nothing to switch, everything held is dropped and checked", in_child(user_child, NULL) == 0);
    }
    path_of(path, sizeof(path), "u-captures/deeper");
    fd = capture_captures_dir(path);
    check("a missing captures folder is made 0700 and opened",
          fd >= 0 && fstat(fd, &sb) == 0 && S_ISDIR(sb.st_mode) && (sb.st_mode & 07777) == 0700);
    if (fd >= 0) {
        close(fd);
    }
    path_of(link, sizeof(link), "u-link");
    unlink(link);
    if (symlink(path, link) == 0) {
        check("a captures folder that is a link is refused", capture_captures_dir(link) < 0);
    }
    path_of(path, sizeof(path), "u-file");
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        close(fd);
    }
    check("a captures 'folder' that is a file is refused", capture_captures_dir(path) < 0);
    check("no folder at all is refused", capture_captures_dir(NULL) < 0);
}

int main(void)
{
    char cwd[256];

    snprintf(work, sizeof(work), "%s/out/capture-priv-%ld", getcwd(cwd, sizeof(cwd)) ? cwd : ".", (long)getpid());
    mkdir("out", 0755);
    if (mkdir(work, 0700) != 0) {
        perror(work);
        return 2;
    }
    as_user();
    the_filter();
    if (geteuid() == 0) {
        as_root();
    } else {
        skip("the root checks: run as root (sudo, or wsl -u root) to test the identity switch");
    }
    {
        char cmd[700];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", work);
        if (system(cmd) != 0) {
            fprintf(stderr, "could not remove %s\n", work);
        }
    }
    printf("capture_priv_test: %d failure(s), %d skipped\n", failures, skipped);
    return failures ? 1 : 0;
}
