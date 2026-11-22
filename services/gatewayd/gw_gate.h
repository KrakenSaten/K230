/*
 * gatewayd: the device-password gate (ADR-019 rule 9b). The gateway does
 * not listen while the unit's root account has no password: a unit whose
 * sshd would admit anyone on the network is not also made a normal member
 * of that network by this service.
 *
 * "Has a password" is read from root's entries in the passwd and shadow
 * files: a password field that holds a hash. An empty field, a locked field ("!", "*", or a
 * hash with "!" in front) and a file that cannot be read all count as no
 * password - the gate fails closed.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_GATE_H
#define GATEWAYD_GW_GATE_H

#include <stdbool.h>

#define GW_GATE_PASSWD "/etc/passwd"
#define GW_GATE_SHADOW "/etc/shadow"

/* True only when root has a password a person set: root's entry in
 * `passwd` holds a hash itself, or it says "x" (shadowed) and root's entry
 * in `shadow` holds a hash. A passwd entry with an EMPTY field means login
 * needs no password whatever the shadow file says, and is "not set". */
bool gw_gate_password_set(const char *passwd, const char *shadow);

#endif
