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
#include <linux/if_ether.h>
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
    path_of(path, sizeof(path), "captures");
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
    path_of(path, sizeof(path), "captures");
    mkdir(path, 0700);
    path_of(path, sizeof(path), "captures/capture-old.pcap");
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
    check("no raw socket can be made", !(bits & (R_DROP | R_SOCKET)));
    check("uid 0 and gid 0 cannot be taken back", !(bits & (R_DROP | R_BACK)));
    check("SAVE's folder takes a new 0600 file of the identity through its descriptor", !(bits & (R_DROP | R_SAVE)));
    check("root's folder around it cannot be written", !(bits & (R_DROP | R_PARENT)));
    path_of(path, sizeof(path), "captures");
    check("the captures folder is the identity's and 0700",
          stat(path, &sb) == 0 && sb.st_uid == (uid_t)POS_CAPTURE_ID && sb.st_gid == (gid_t)POS_CAPTURE_ID &&
              (sb.st_mode & 07777) == 0700);
    path_of(path, sizeof(path), "captures/capture-old.pcap");
    check("a capture already there stays root's and 0600",
          stat(path, &sb) == 0 && sb.st_uid == 0 && (sb.st_mode & 07777) == 0600);

    check("a forced failure of the last check fails closed", in_child(forced_child, NULL) == 0);

    path_of(path, sizeof(path), "someone-elses");
    mkdir(path, 0700);
    check("setup: a folder of another user", chown(path, 1234, 1234) == 0);
    check("a captures folder of another user is refused", in_child(refused_dir_child, "someone-elses") == 0);
    path_of(path, sizeof(path), "captures");
    path_of(link, sizeof(link), "captures-link");
    unlink(link);
    check("setup: a link to the folder", symlink(path, link) == 0);
    check("a captures folder that is a link is refused", in_child(refused_dir_child, "captures-link") == 0);
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
