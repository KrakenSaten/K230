# RIFT management - hardware gate (NOT RUN)

Branch `feat/rift-management`. Prepared 2026-10-01; **nothing here has been run
on a unit**. Host evidence is in the branch's commits and in
`docs/apps/RIFT.md` ("Managing the node", "Emoji and other text").

State the unit's build at the top of the sheet when it is run (the shell's
`doors shell info` build id and `meshcored`'s `mesh.info` build), as every gate
sheet does.

## What changed on the unit

| Binary | Why |
| --- | --- |
| `/usr/bin/doors-shell` | RIFT: sender before message, NODES find bar and ZERO-HOP, NET, ACTIVITY's CHANNELS and THIS DEVICE controls, emoji drawn as smileys |
| `/usr/sbin/meshcored` (`S65meshcored`'s `DAEMON`) | `advert_hops` in `mesh.nodes`; `mesh.set_name`, `mesh.path_hash`, `mesh.set_path_hash`; `name_source` in `mesh.identity`; `settings.v1` |

radiod is not changed. A RIFT built from this branch talks to an older
meshcored: ZERO-HOP then has only direct routes to go on, NET has no advert
placement, RENAME is refused as an unknown method, and the path hash choices
say the service has no such setting.

## Before anything

1. Record `sha256sum` of `/usr/bin/doors-shell` and the running meshcored, and
   copy both to `/root/rollback-rift-management/` with a `RESTORE.sh` that puts
   them back and restarts `S65meshcored`.
2. Copy `/var/lib/pocketos/meshcored/{state.v1,channels.v1}` there too
   (`settings.v1` does not exist yet). **Never touch `identity.id`.**
3. Note `MESHCORED_NAME` in `/etc/default/meshcored`. On unit A it is pinned
   (`Mstr_k230`) by the owner's decision: RENAME must be refused there, which
   is itself a check (step 6). Rename on a unit without the pin, or leave it.

## Checks

RF: only steps 7 and 8 transmit, one advert each, at the unit's configured
power. Nothing else here puts a packet on the air.

1. **COMMS sender** - a channel thread with traffic: the claimed name comes
   first (`NAME? text`), a long line wraps under the name, own lines carry no
   name. Both orientations. Capture with `ffmpeg -f kmsgrab`.
2. **NODES search** - type part of a known name in portrait (touch keyboard)
   and landscape (keyboard base); the list narrows as typed; CLEAR and Esc
   restore it; a nonsense query says no node matches. With 200+ nodes, typing
   stays responsive (note shell CPU).
3. **ZERO-HOP** - turn it on; the repeaters listed should be ones known to be
   in direct range (compare `mesh.nodes` `advert_hops: 0`). An empty result
   says so. No `mesh.advert` / `tx_submitted` change while doing it.
4. **NET** - rings populated from the real mesh; tap a multi-hop node: the
   chain is written out and on-route nodes outlined. Landscape columns fit.
5. **CHANNELS** - ADD a HASHTAG channel (e.g. `#doorstest`) and check its hash
   against another MeshCore client joining the same name (T-Deck: hashtag
   channel); LEAVE it through the confirmation; `channels.v1` changes and
   nothing else does. PRIVATE: the key is shown once and gone after DONE.
   KEY: paste a known key; a mistyped key is refused before asking.
6. **RENAME** - on a pinned unit: disabled, with the MESHCORED_NAME caption.
   On an unpinned one: rename, restart `meshcored`, the name survives.
7. **Rename reaches a peer** (unpinned unit only) - after the rename, one
   ADVERT NEAR; the T-Deck shows the new name. *One packet.*
8. **Path hash 2 bytes** - choose 2 B (confirmation), then one ADVERT MESH;
   capture it on a second receiver (`tools/meshcore-frame parse`): the path
   length byte has size bits `01`. Whether repeaters on the bench mesh carry
   it onward is the open question this step answers; record which firmware
   they run. *One packet.* Then back to 1 B.
9. **Emoji** - a channel message from the T-Deck containing 🙂 ❤️ 👍 🚀
   shows `:) <3 (y)` and a box for the rocket.
10. **Back/Home** from every section, and RIFT closed and reopened three
    times: no crash, no keyboard left up, no key left on ACTIVITY.

## Rollback

`/root/rollback-rift-management/RESTORE.sh`, then `rm
/var/lib/pocketos/meshcored/settings.v1` if step 8 left anything but 1 byte
(or set it back from RIFT first).
