/*
 * What netd touches outside itself: the network interface, the processes it
 * runs (wpa_supplicant, udhcpc) and the process table it checks for a
 * wpa_supplicant that is not its own.
 *
 * Every path is fixed in the shipped binary. A build compiled with
 * -DNETD_TEST_HOOKS=1 (tests/netd-testhooks only, see the Makefile) reads
 * NETD_TEST_ROOT, NETD_TEST_WPA_SUPPLICANT and NETD_TEST_UDHCPC instead, and
 * keeps the interface's flags and address in files under the test root,
 * so tests/netd_test.sh can run the real netd against a fake supplicant with
 * no radio and no root. The shipped netd does not contain those names; the
 * test checks.
 *
 * Nothing here builds a shell command: children are started with an argv
 * array through execve, so no SSID or passphrase can ever be interpreted by
 * a shell, and neither is ever on a command line.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_NETD_SYS_H
#define POCKETOS_NETD_SYS_H

#include <net/if.h>
#include <stddef.h>
#include <sys/types.h>

#define NETD_PATH_MAX 256

struct netd_sys {
    char iface[IFNAMSIZ];
    char class_net[NETD_PATH_MAX];      /* /sys/class/net */
    char proc[NETD_PATH_MAX];           /* /proc */
    char wpa_supplicant[NETD_PATH_MAX]; /* /usr/sbin/wpa_supplicant */
    char udhcpc[NETD_PATH_MAX];         /* /sbin/udhcpc */
    char dhcp_script[NETD_PATH_MAX];    /* /usr/share/udhcpc/default.script */
    int test_root;                      /* 1 when the paths point into a test tree */
};

/* A valid interface name: 1..15 of [A-Za-z0-9_.-]. Returns 0 or -1. */
int netd_iface_name_valid(const char *name);

/* Resolve the paths for iface (validated by the caller). */
void netd_sys_init(struct netd_sys *s, const char *iface);

/* The interface exists and is a wireless one (it has a wireless/ or
 * phy80211 entry in sysfs). */
int netd_iface_present(const struct netd_sys *s);
/* Bring the interface administratively up or down. Returns 0 or -1. */
int netd_iface_set_up(const struct netd_sys *s, int up);
/* Its IPv4 address as text into buf. Returns 0, or -1 when it has none. */
int netd_iface_ipv4(const struct netd_sys *s, char *buf, size_t n);

/* The pid of a running wpa_supplicant whose command line names the
 * interface, other than own (0 for none), or 0 when there is none. */
pid_t netd_foreign_supplicant(const struct netd_sys *s, pid_t own);

/* Start argv[0] with argv, stdin from /dev/null, stdout and stderr
 * inherited, every other descriptor closed, and SIGTERM delivered to it if
 * netd dies. Returns the pid or -1. */
pid_t netd_spawn(const char *const argv[]);
/* SIGTERM, wait up to timeout_ms, then SIGKILL and reap. Returns the wait
 * status, or -1 when the pid was not a child. */
int netd_stop_child(pid_t pid, int timeout_ms);

#endif
