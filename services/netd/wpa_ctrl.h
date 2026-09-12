/*
 * A minimal client for wpa_supplicant's control interface: a Unix datagram
 * socket per interface in the ctrl_interface directory, one text command per
 * datagram, one reply per command, and unsolicited "<N>MESSAGE" events on a
 * connection that has sent ATTACH.
 *
 * Written here instead of linking libwpa_client so netd needs nothing new in
 * the image, and so the exact bytes exchanged are the ones the host tests
 * exercise against tests/fake_wpa_supplicant.c.
 *
 * Two connections, as wpa_cli uses: one for requests, whose replies are never
 * interleaved with events, and one attached for events only.
 *
 * Commands can carry a passphrase (SET_NETWORK <id> psk "..."). This module
 * never logs anything, and wipes its own copies of what it sent.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_WPA_CTRL_H
#define POCKETOS_WPA_CTRL_H

#include <stddef.h>

#define WPA_CTRL_REPLY_MAX 16384

struct wpa_ctrl {
    int fd;
    char local[108];
    char remote[108];
};

/* Connect to <ctrl_dir>/<iface>, binding a local socket in local_dir.
 * Returns 0 or -1 (errno). */
int wpa_ctrl_open(struct wpa_ctrl *c, const char *ctrl_dir, const char *iface,
                  const char *local_dir);
void wpa_ctrl_close(struct wpa_ctrl *c);
/* Send cmd and wait up to timeout_ms for its reply, which is NUL-terminated
 * into reply (at most n-1 bytes). Unsolicited messages that arrive first are
 * skipped. Returns the reply length, or -1 (errno ETIMEDOUT on no answer). */
int wpa_ctrl_request(struct wpa_ctrl *c, const char *cmd, char *reply, size_t n, int timeout_ms);
/* For the event connection: ATTACH, expecting "OK". Returns 0 or -1. */
int wpa_ctrl_attach(struct wpa_ctrl *c, int timeout_ms);
/* Read one pending message without blocking. Returns its length, 0 when
 * nothing is pending, -1 on error (the supplicant has gone). */
int wpa_ctrl_recv(struct wpa_ctrl *c, char *buf, size_t n);

#endif
