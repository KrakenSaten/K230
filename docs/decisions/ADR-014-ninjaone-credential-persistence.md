# ADR-014: Optional NinjaOne refresh-token persistence

Status: Proposed — awaiting the product owner's decision; **not implemented**.
Date: 2026-10-12
Deciders: product owner (final), AI engineering partner (author)

## Context

Native PKCE and public-client refresh grants are documented by NinjaOne. The
runtime implementation now renews authorization without workstation provisioning,
but `/run` credentials disappear on reboot. ARCHITECTURE.md prohibits other
components from persisting secrets. ADR-003 permits Wi-Fi credentials and ADR-007
permits Zabbix credentials specifically; neither authorizes a NinjaOne store.

## Options

- Keep runtime-only credentials: current implementation, no new exception;
  a workstation login is required after reboot.
- Introduce a hardware-backed encrypted store: no verified key-storage facility
  or unlock design exists for this unit; requires a separate design and evidence.
- Extend the existing permission-only model to an opt-in NinjaOne store: enables
  unattended boot renewal, with the same root/physical-card exposure as the
  accepted exceptions. Proposed below; it is not encrypted storage.

## Proposed decision / exact amendment

Add a narrowly scoped exception to ARCHITECTURE.md's credential rule:

> In addition to ADR-003 and ADR-007, pos-ninjaone may persist one public Native
> refresh token under ADR-014, only after explicit “Remember on this device”
> consent. Default storage remains runtime-only. No shared client secret or
> password may be stored. Other components still may not persist secrets.

Do not amend the binding policy until this proposal is accepted.

## Implementation design after approval

- One helper-owned `/var/lib/pocketos/ninjaone/credentials` file in a 0700
  directory, file 0600, owner root, no links/extra hard links, bounded versioned
  format. Persist only refresh token and nonsecret region, client ID, tenant
  namespace and consent. Access token/expiry remain in `/run`; boot obtains a
  new access token. Tokens never enter favorites/settings, UI IPC, argv,
  environment or logs. No shell command handles credential bytes.
- The sign-in pipeline carries a nonsecret remember choice, default false.
  Show the permission-only/root/card risk before consent; the app displays
  only storage choice and connection state. Runtime-only re-provisioning must
  explicitly erase any older persistent login, never silently reactivate it.
- One helper lock serializes import, renewal and Forget. Stage using
  O_EXCL/O_NOFOLLOW, file fsync, rename and directory fsync. Commit the rotated
  refresh token durably before publishing the paired runtime access token.
  Persist a pending-renewal marker before sending the grant. After a power
  interruption or ambiguous transport outcome, require sign-in again: no
  assumed single-use-token replay or rotation grace period.
- “Disconnect / Forget credentials” cancels the app's helper, then under the
  lock erases runtime and persistent credentials, staged files and markers,
  fsyncs directories and stops polling. Report incomplete deletion as failure;
  never report success while a stored login can be restored. Local forget does
  not revoke the grant at NinjaOne; admin dashboard revocation is separate.
- Synthetic checks extend the current interruption/rotation/forget tests to
  the persistent path and reboot restoration. Hardware validation checks the
  actual state mount, ownership, fsync/power-loss behavior and clock recovery.

## Consequences

Permissions do not encrypt data or protect against root, a compromised root
process or physical card access. All current Doors processes run as root.
Unlinking is not guaranteed forensic erasure, particularly on flash. This
proposal requires explicit acceptance of those risks; a key stored on the same
card does not solve them. No dependency/toolchain change or migration of old
unnamespaced favorites is needed.

## Evidence

- DOCUMENTED: [NinjaOne refresh grants](https://app.ninjarmm.com/apidocs-beta/authorization/refresh-tokens)
  make the secret optional for public clients and return refresh tokens.
- VERIFIED (synthetic host only): runtime atomic rotation, exclusive lock,
  interrupted-write/renewal refusal and Forget; see NINJAONE_VALIDATION.md.
- DOCUMENTED: binding storage restrictions and permission-only exceptions in
  ARCHITECTURE.md, ADR-003 and ADR-007.
- ASSUMED / untested: live tenant rotation lifetime, filesystem durability on
  K230 and future reboot restoration. Approval alone is not validation.
