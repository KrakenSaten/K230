# ADR-018: Fleet link profiles and a match save per transport

Status: Accepted (product owner, 2026-11-20), as written: existing LoRa
behaviour and `match.v1` compatibility remain unchanged; the LAN save is
separate; one active match per device. Nothing is implemented yet.
Date: 2026-11-20
Deciders: product owner (final), AI engineering partner (author)

Builds on ADR-008 and ADR-017 (both Accepted 2026-11-20).

## Context

Fleet gains a second transport, the local network. Two things in Fleet are
sized or shaped for exactly one:

- **Pacing.** The retry schedule and airtime governor are constants chosen
  for LoRa (`FLEET_RETRY_BASE_MS` 7000, `_CAP_MS` 60000, `_MAX` 6;
  `FLEET_GOV_BURST` 6, `_REFILL_MS` 5000, `_HOUR_MS` 90000;
  `apps/fleet/net/fleet_match.h`). Only the retry base is taken from the
  link (`fleet_match_set_retry_base`). VERIFIED source.
- **The save.** `match.v1` holds one match and the identity that made it.
  Decoded under another identity it returns -2, and the session then sets
  `identity_changed` and `store_fault` (`fleet_match_save.c`,
  `fleet_session.c`). A link learns its identity only after its first poll.
  VERIFIED source.

The owner's conditions (2026-11-20): existing LoRa behaviour and `match.v1`
compatibility are preserved; the LAN save is separate; one active match.

## Options

### Pacing

1. Keep the constants for every transport. A LAN match would wait 7 s for a
   lost packet and stop after six quick shots. Rejected.
2. Compile-time constants per transport (`#if`). Two builds of one state
   machine; the simulator could not compare them. Rejected.
3. **A profile the link reports and the match reads.**

### Save

1. One file, transport chosen by the saved key. Needs the link's key before
   the file can be attributed, and a wrong guess is a store fault that the
   next match overwrites. Rejected.
2. A `match.v2` with a transport field. Changes a format verified on
   hardware and breaks a downgrade. Rejected.
3. **One file per transport, the same codec.**

## Decision

### 1. Link profile

A link reports, once it is open, the figures a match may assume:

| Field | Mesh profile (today's values, unchanged) | LAN profile (starting values) |
| --- | --- | --- |
| link-wait base | 7000 ms, or meshcored's `est_timeout_ms` | 500 ms |
| link-wait cap | 60000 ms | 5000 ms |
| tries before "lost" | 6 | 6 |
| governor burst / refill | 6 / 5000 ms | unmetered |
| airtime budget per hour | 90000 ms | none |
| chat budgets | 2 tokens, 50000 ms, 20000 ms | none |
| busy retry | 1500 ms | 1500 ms |

- The match reads the profile where it read the constants. **With the mesh
  profile the state machine's behaviour is identical to today's**: this is a
  refactor on LoRa, proved by the suites of "Validation", not a tuning.
- LAN figures are starting values, to be set by the simulator as LoRa's
  were (ADR-008 decision 7), and may change without a new ADR.

### 2. Human waits are not link waits

How long an INVITE or an ACCEPT waits for a *person* is the same on every
transport: the total window today's mesh schedule gives (six tries, about
three minutes). On a fast link the repeats inside that window follow the
profile; the window does not shrink. Waiting for a RESULT, COMMIT, REVEAL,
SYNC or END follows the link profile entirely. The probe intervals after a
link is lost (120 s, 5 min) are kept on both.

### 3. A save per transport

| Transport | File | Format |
| --- | --- | --- |
| LoRa | `$POCKETOS_STATE_DIR/fleet/match.v1` | unchanged: 655 bytes, magic `PFM1`, version 1 |
| LAN | `$POCKETOS_STATE_DIR/fleet/match-lan.v1` | the same codec |

- The transport is chosen **by file, before any byte is decoded**. A file
  is only ever decoded by the session of its own transport.
- **Only the mesh session can name `match.v1`.** No LAN code path reads,
  writes, renames or removes it; `tests/fleet_lint.sh` enforces it.
- Both files are 0600 and written temp-flush-rename-dirsync.
- A build without this ADR ignores `match-lan.v1` and finds `match.v1` as
  it left it: downgrade is safe, and there is no migration.

### 4. One active match per device

ADR-008 decision 4 stands, across both files. A session does not start
(invite or accept) while the other transport's file holds a match from
DEPLOY to REVEAL; Fleet says which transport holds it and offers RESUME. An
invitation arriving on the idle transport is not read, exactly as none is
read today before the player engages multiplayer. A finished match waiting
to be put away does not block the other transport.

### 5. Identity

A player is the key of the transport the match runs on: meshcored's on
LoRa, peerd's on the LAN (ADR-017). A match never changes transport.

## Relation to ADR-008

| ADR-008 | Here |
| --- | --- |
| Decisions 1-3 (transport E, Fleet owns its protocol, commitment) | unchanged; the LAN link is a second link under the same seam |
| Decision 4 (one match per device) | kept, across both files |
| Decision 5 (opening Fleet transmits nothing) and the session rule that nothing is read before the player engages | unchanged, on both transports |
| Decision 6 (paused, never forfeited) | unchanged, on both |
| Decision 7 (governor figures) | kept exactly, as the mesh profile |
| Decision 8 (multiplayer needs meshcored) | holds for LoRa; LAN needs peerd in the same way, and Fleet starts neither |
| Decisions 9, 10 | unchanged |

## Validation

1. A **golden fixture**: a `match.v1` written by the build before this
   change (mid-battle, a pending shot, tombstones) loads, restores and
   re-encodes byte for byte.
2. `fleet_match_test`, `fleet_mp_sim_test` and `fleet-mp-e2e` give the same
   results with the mesh profile as before the change (frames per ply,
   airtime, worst hour).
3. A simulator invariant: the invite and accept windows are equal under both
   profiles.
4. `fleet_save_test` and `fleet_session_test`: each file independent of the
   other; the one-match rule in both directions; a LAN session with a
   damaged `match.v1` present leaves it untouched.
5. The LoRa regression steps of the M2 hardware gate (M2-PLAN.md §3, step 7),
   including resuming a `match.v1` saved before the install.

## Consequences

Needed now: `fleet_match` takes a profile at init; `fleet_store` takes a
transport; two lint rules (two link files; only the mesh session names
`match.v1`); the fixture.

Useful soon: the profile moves to the shared play library with the link
seam (M3).

Future: "one match per device" could become "one per transport" with no
format change; that would be a new decision.

Risks: a refactor of verified code - mitigated by item 2 above being a hard
gate; a player with a LoRa match in hand cannot start a LAN one until it is
finished or forfeited.

Migration cost if rejected: none on LoRa; LAN play would need another
answer to both problems.

## Evidence

| Claim | Class | Basis |
| --- | --- | --- |
| The retry and governor constants and their values | VERIFIED source | `apps/fleet/net/fleet_match.h` |
| Only the retry base comes from the link today | VERIFIED source | `fleet_match_set_retry_base`, `fleet_link_ops.retry_base` |
| Decoding under another identity returns -2 and sets a store fault | VERIFIED source | `fleet_match_save.c`, `fleet_session.c` (`make_ready`) |
| `match.v1` is 655 bytes, 0600, written temp-rename-dirsync | VERIFIED host and hardware | docs/apps/FLEET_MULTIPLAYER.md; FLEET_MULTIPLAYER_GATE.md, "The match.v1 file mode" |
| LoRa behaviour with today's constants | VERIFIED hardware | the three gates listed in ADR-008-RECONCILIATION.md |
| The LAN profile's figures | ASSUMED | starting values; the simulator sets them |
| The same codec can hold a LAN match | ASSUMED | it stores two 32-byte keys and no transport-specific field; to be confirmed by `fleet_save_test` |
