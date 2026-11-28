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
    char why[96];         /* the first step or check that failed; "" when none */
};

/* The captures folder, prepared before the drop and kept open, so that SAVE
 * works without a path to it: created 0700 if missing; refused if it is a
 * link, not a folder, or owned by anyone but root, the capture identity or
 * the helper itself; mode 0700; when the helper is root, owned by the
 * capture identity, the files already in it left as they are. Returns the folder's descriptor, or
 * -1 (SAVE then reports "nodir"; the capture still runs). */
int capture_captures_dir(const char *dir);

/* Leave root for the capture identity and drop everything (see above).
 * 0 when every check reads back as expected; -1 otherwise, with out->why
 * saying which. A caller must refuse the capture on -1. */
int capture_drop(struct capture_priv *out);

#if POS_CAPTURE_TEST_HOOKS
/* Test builds only: capture_drop() fails its last check, as a kernel that
 * did not apply a step would make it (tests/capture_priv_test.c,
 * tests/pos_capture_priv_test.sh). The shipped helper has no such switch. */
void capture_priv_test_fail(int on);
#endif

#endif
