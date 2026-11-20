<!-- Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0 -->
# Proposed changes to accepted records (for approval)

This package edits no ADR and no Design System section. Each item below is a
change it would need, to be written as its own ADR or amendment once the
owner approves the direction.

| # | Record | Proposed change | Needed by |
| --- | --- | --- | --- |
| 1 | ADR-008 and `docs/apps/FLEET_MULTIPLAYER.md` | **Done 2026-11-20**: ADR-008 Accepted by the owner and both files reconciled (ADR-008-RECONCILIATION.md). Was: record the owner's decision on ADR-008 and the P7 result. ADR-008 still reads "Proposed" and the protocol document still says "nothing here has been on a radio", though the gate passed on 2026-09-26. This package relies on ADR-008's decisions | before M2 |
| 2 | **ADR-017 (Accepted 2026-11-20)**, "IP peer datagrams (peerd)" | A new service with the `peer.*` surface (ARCHITECTURE §5.5): identity, LAN discovery, sealed datagrams, inbox. Links libsodium (packaged in the pinned Buildroot; owner approved 2026-11-20) into a first-party service; adds an init script, disabled until used. Fixes the port numbers, frame layout and cryptographic construction, with a review of the latter | M2 |
| 3 | ADR-008 decision 5 and its session rule ("does not even read the inbox") | The shell may **read** registered ports' inboxes while no game is open and raise a notice for a fresh invitation. It never transmits. "Opening Fleet transmits nothing" is unchanged | M4 |
| 4 | ADR-008 decision 7 (governor constants) | The constants become the *mesh profile* of a per-link profile; their values on LoRa do not change, and the simulator must reproduce today's figures. A LAN profile is added | M2 |
| 5 | ADR-008 "Identities" | A player is the key of the transport the match runs on. On LAN that is peerd's key; a match never changes transport | M2 |
| 6 | `apps/fleet` lint (`tests/fleet_lint.sh`: "one file talks to a service") | A second link file is allowed; in M3 the two move to the shared play library | M2, M3 |
| 7 | DS §60.4 (the lobby is a preview only) | The lobby tile becomes live: one target, one key stop, a live summary in place of COMING SOON | M3 |
| 8 | New DS amendment, "The Multiplayer Lobby" | The screens of UX.md: the ticket, the fixed palette with three indigo grounds and an ink, the roles, the art, the key rules | M3 |
| 9 | ADR-002 (no background app) | None. Invitation listening is a shell function, not a background app; noted so the reading is explicit | M4 |
| 10 | New ADR, "Game ports" | The port registry of ARCHITECTURE §5.2, shared by `mesh.app_*` and `peer.app_*` | M3 |
| 11 | `docs/api/` | `peer.md` (new). `mesh.md` is unchanged | M2 |

Not proposed: any change to meshcored, radiod, `match.v1`, Fleet's packet
format, the App API version, or the nine games' order on the wall.

Items 4, 5 and 6 are now **ADR-018 (Accepted 2026-11-20)**.
