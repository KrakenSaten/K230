/*
 * PocketFleet's link to the local network: peerd's app datagrams
 * (peer.app_send, peer.app_inbox, the peer.app event; docs/api/peer.md) over
 * pocketipc, port FLEET_PEER_PORT. The sibling of fleet_link_mesh.c, and with
 * it one of the two files in Fleet that talk to a service.
 *
 * It never opens a socket to the network: peerd holds the UDP socket, the
 * identity and the cryptography (ADR-017), and everything Fleet sends is a
 * packet handed to it.
 *
 * Beyond what every link does, this one keeps three things in step with
 * peerd, all of them only while it is polled - which the session does only
 * once the player has engaged multiplayer (fleet_session.h):
 *
 *   visibility  peerd is silent until asked. While the link is polled and
 *               its owner wants the device seen (fleet_link_peer_set_visible;
 *               it does unless told otherwise) it renews peerd's 30 s
 *               visibility lease; told otherwise, or closed, it ends the
 *               lease. The app wants it only while the lobby is on screen, so
 *               a Fleet that is anywhere else announces nothing and hears no
 *               stranger. (With a match in hand peerd stays bound for the
 *               kept opponent, and for nobody else.)
 *   the opponent  the session tells the link who the match in hand is with
 *               (fleet_link_ops.match_peer). The link has peerd keep that
 *               peer - so the match survives a restart with nobody visible -
 *               and release every other kept peer.
 *   an address  for a network where announcements do not get through:
 *               fleet_link_peer_probe() asks the device at an address who it
 *               is.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETFLEET_LINK_PEER_H
#define POCKETFLEET_LINK_PEER_H

#include "fleet_link.h"

#define FLEET_PEER_SERVICE "peerd"
/* The same number as on the mesh: a game has one port on every transport. */
#define FLEET_PEER_PORT 1

/* Whether peerd's socket exists on this device (service NULL: the usual
 * one). A look at the filesystem and nothing else: nothing is connected to
 * and nothing is asked, so a Fleet that has not been taken into multiplayer
 * still says nothing to any service. It is how the app decides whether to
 * offer the local network at all. */
int fleet_link_peer_present(const char *service);

/* A link to peerd on its usual socket (service NULL), or on another one
 * (tests). Connects lazily, on the first poll. NULL when out of memory. */
struct fleet_link *fleet_link_peer_open(const char *service);

/* Whether this device is to be visible on the network: announced, and
 * heard by strangers. On (the default) the lease is asked for and renewed;
 * off it is ended and not asked for again - once the opponent of a match
 * just begun has been kept, so that the match never has a moment in which
 * peerd would hear neither a stranger nor a kept peer. Datagrams with a kept
 * opponent go on either way. A no-op on a link that is not this one. */
void fleet_link_peer_set_visible(struct fleet_link *link, int visible);

/* What is wrong, for the words on the screen. NO_SERVICE and FAULT are why
 * the link is not up; NO_NETWORK is said of a link that is. */
enum fleet_link_peer_trouble {
    FLEET_PEER_OK = 0,
    FLEET_PEER_NO_SERVICE,      /* peerd does not answer */
    FLEET_PEER_FAULT,           /* peerd is closed by a fault it names (fault, below) */
    /* peerd is ready and has no interface to announce on: nobody will be
     * found by listening. The link is still up - a device at a known address
     * (a kept opponent, one asked by address) is reached all the same. */
    FLEET_PEER_NO_NETWORK,
};

struct fleet_link_peer_info {
    enum fleet_link_peer_trouble trouble;
    char fault[24];             /* "identity_fault", "epoch_fault", ... or "" */
    char address[16];           /* this device's address, dotted, or "" */
    int visible;                /* peerd says it is announcing */
    int kept;                   /* how many peers peerd keeps, as last listed; -1 before that */
};

/* What peerd last said. Returns 0, or -1 when link is not a peer link. */
int fleet_link_peer_info(const struct fleet_link *link, struct fleet_link_peer_info *out);

/* Whether peerd has shown this listed player to be live - an answer bound
 * to a question of ours, or a datagram that opened (ADR-017, section 5).
 * Until then a listed player is a claim: an announcement can be a recording.
 * 1, 0, or -1 when the key is not listed. A lobby must not present an
 * unproved player as present. */
int fleet_link_peer_proved(const struct fleet_link *link, const uint8_t key[FLEET_KEY_BYTES]);

enum fleet_link_peer_probe_state {
    FLEET_PEER_PROBE_NONE = 0,
    FLEET_PEER_PROBE_PENDING,
    FLEET_PEER_PROBE_ANSWERED,  /* the device there answered: it is now a player in the list */
    FLEET_PEER_PROBE_NO_ANSWER,
    FLEET_PEER_PROBE_REFUSED,   /* not an address, or peerd would not ask */
};

/* Ask the device at a dotted IPv4 address who it is. One probe at a time: a
 * new one replaces the last. Returns 0 when the question was handed to
 * peerd, -1 when it could not be (no connection, or address is not one). */
int fleet_link_peer_probe(struct fleet_link *link, const char *address);
enum fleet_link_peer_probe_state fleet_link_peer_probe_state(const struct fleet_link *link);
/* When ANSWERED: who answered. Returns 0 and the key, or -1. */
int fleet_link_peer_probe_key(const struct fleet_link *link, uint8_t key[FLEET_KEY_BYTES]);

#endif
