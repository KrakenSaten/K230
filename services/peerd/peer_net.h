/*
 * peerd's sockets: the one file that touches the network and the kernel's
 * interface list. One UDP socket, bound only while peerd has a reason to be
 * heard (peer_core.h, pcore_wants_socket).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PEERD_NET_H
#define PEERD_NET_H

#include "peer_core.h"

#include <stddef.h>
#include <stdint.h>

/* A non-blocking UDP socket bound to port on every address, able to send
 * broadcasts. The fd, or -1. */
int pnet_open(uint16_t port);
void pnet_close(int fd);

/* The interfaces to announce on: up, IPv4, able to broadcast, not loopback.
 * Returns how many were written. */
int pnet_ifaces(struct pcore_iface *out, int max);

/* 0 when the datagram was handed to the kernel. A broadcast goes with TTL 1. */
int pnet_send(int fd, uint32_t dst, uint16_t port, const uint8_t *buf, size_t n, int broadcast);
/* The local address a datagram to dst would leave from, or 0. */
uint32_t pnet_source_for(uint32_t dst, uint16_t port);
/* One datagram. Returns its length, 0 when none is waiting (or an empty one
 * was taken), -1 on an error. The length is the datagram's real one and may
 * exceed max: only max bytes were written, and the caller's length checks
 * refuse anything that long. */
long pnet_recv(int fd, uint32_t *src, uint16_t *src_port, uint8_t *buf, size_t max);

#endif
