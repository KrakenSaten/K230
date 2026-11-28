/*
 * The capture helper's least privilege (ADR-016 rule 17, amended): after its
 * capture socket and its few files are open, and before it reads a frame,
 * pos-capture leaves uid 0 for a dedicated unprivileged identity, empties
 * every capability set, sets no_new_privs, and proves all of it from
 * /proc/self/status. Any step that fails, or any check that does not read
 * back as expected, fails the capture closed: no frame is processed.
 *
 * The identity is fixed (uid and gid POS_CAPTURE_ID, named pos-capture in
 * the image), so the helper reads no account database. A helper that does
 * not start as root (host tests) has no identity to leave; it still drops
 * what it holds and checks it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CAPTURE_PRIV_H
#define CAPTURE_PRIV_H

#include <sys/types.h>

#ifndef POS_CAPTURE_TEST_HOOKS
#define POS_CAPTURE_TEST_HOOKS 0
#endif

#ifndef POS_CAPTURE_ID
#define POS_CAPTURE_ID 65532 /* uid and gid of pos-capture; nobody is 65534 */
#endif

struct capture_priv {
    int was_root;         /* the helper started with an effective uid of 0 */
    uid_t ruid, euid, suid;
    gid_t rgid, egid, sgid;
    int ngroups;          /* supplementary groups after the drop */
    unsigned long long cap_inh, cap_prm, cap_eff, cap_bnd, cap_amb;
    int no_new_privs;
    unsigned long net_before, net_after; /* network namespace inodes; differ when it got its own */
    int seccomp_mode;     /* /proc's Seccomp after capture_confine(): 2 = filtered */
    int seccomp_fd;       /* the descriptor the filter keeps receive-only */
    char why[96];         /* the first step or check that failed; "" when none */
};

/* The captures folder, prepared before the drop and kept open, so that SAVE
 * works without a path to it: created 0700 if missing; refused if it is a
 * link or not a folder; mode 0700. When the helper is root, the folder is
 * handed to the capture identity only if it was just made, is the
 * identity's already, or is a root-owned folder named "Captures" whose
 * parent is root's and writable by root alone: never an arbitrary folder a
 * misconfigured path names (a home, a mount point). The files already in it
 * keep their owner. Without root, only the helper's own folder is used.
 * Returns the folder's descriptor, or -1 (SAVE then reports "nodir"; the
 * capture still runs). */
int capture_captures_dir(const char *dir);

/* Leave root for the capture identity and drop everything (see above). As
 * root it first gives the helper a network namespace of its own: the
 * capture socket it already holds keeps working, but a socket made later
 * reaches no network, and no abstract local socket of the system.
 * 0 when every check reads back as expected; -1 otherwise, with out->why
 * saying which. A caller must refuse the capture on -1. */
int capture_drop(struct capture_priv *out);

/* The last step, after capture_drop() and once the helper holds every
 * descriptor it will use (ADR-016 rule 17, amendment 3): a system call
 * filter, for this process for good. It allows what the helper does from
 * here on (reading frames and commands, writing lines, its log and its
 * saves, time, memory, the end) and nothing else:
 *   - capture_fd is receive-only: no write, writev or sendto on it;
 *   - no socket is made, bound, connected or set (no promiscuous
 *     membership, filter or transmit ring), no message sent with sendmsg;
 *   - no descriptor copied (dup, dup3, fcntl F_DUPFD), no ioctl;
 *   - no executable memory mapped;
 *   - any other system call, or another ABI's, ends the helper (killed by
 *     the kernel, which logs it).
 * capture_fd is the open capture socket (-1 without one); it must not be a
 * standard descriptor. Proved from /proc (Seccomp 2) and by a refused
 * socket(). 0, or -1 with p->why: the caller must refuse the capture. */
int capture_confine(int capture_fd, struct capture_priv *p);

/* After the capture socket is closed (STOP), keep its number taken by a
 * read-only /dev/null, so no file opened later (a save) gets the number the
 * filter refuses writes to. 0 when fd is held (or fd < 0); -1 otherwise
 * (a save may then fail; it never reaches a socket). */
int capture_reserve_fd(int fd);

#if POS_CAPTURE_TEST_HOOKS
/* Test builds only: 1 makes capture_drop() fail its last check, 2 makes
 * capture_confine() fail its last check, as a kernel that did not apply a
 * step would (tests/capture_priv_test.c, tests/pos_capture_priv_test.sh).
 * The shipped helper has no such switch. */
void capture_priv_test_fail(int on);
#endif

#endif
