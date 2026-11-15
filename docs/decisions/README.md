# Architecture Decision Records

One file per decision, numbered, never rewritten once Accepted. A change of
mind gets a new ADR that supersedes the old one.

Status values: Proposed, Accepted, Superseded by ADR-nnn, Rejected.

Template:

```markdown
# ADR-nnn: Title
Status / Date / Deciders
## Context        (problem and constraints)
## Options        (meaningful alternatives with trade-offs)
## Decision       (what and why)
## Consequences   (needed now / useful soon / future; risks; migration cost)
## Evidence       (what this rests on, with VERIFIED / DOCUMENTED / ASSUMED)
```

Accepted ADRs are binding. Proposed ADRs are drafts awaiting the product
owner's decision.

## Index

The status is each ADR's own status line, shortened; the ADR file is
authoritative.

| ADR | Decision | Status (from the file) |
|---|---|---|
| [001](ADR-001-pocketos-base.md) | Base platform | Accepted, 2026-09-04 |
| [002](ADR-002-app-model.md) | Application and service model | Accepted, 2026-09-04 |
| [003](ADR-003-wifi-credentials.md) | Storing Wi-Fi credentials | Accepted for this milestone, 2026-09-13 |
| [004](ADR-004-audio-ownership.md) | Who owns the audio hardware | Accepted for this milestone, 2026-09-13 |
| [005](ADR-005-product-name-doors.md) | Product name Doors | Accepted for Phases 1, 2 and 3 |
| [006](ADR-006-camera-ownership.md) | Who owns the camera | Accepted, 2026-09-25 |
| [007](ADR-007-zabbix-viewer.md) | A Zabbix monitoring viewer | Accepted, 2026-09-26 (decisions 1-3) |
| [008](ADR-008-fleet-multiplayer.md) | Two-player Fleet over the mesh | Proposed, 2026-09-25 |
| [009](ADR-009-browser-engine.md) | A browser without a browser engine | Accepted, 2026-09-27 |
| [010](ADR-010-recorder-audio.md) | Audio hardware for the Recorder | Accepted, 2026-09-27 |
| [011](ADR-011-kernel-patches.md) | Doors-owned kernel patches | Accepted, 2026-09-27 |
| [012](ADR-012-video-playback.md) | Video decoder and sound card for Video | Proposed |
| [013](ADR-013-licence-apache-2.md) | Licence: Apache-2.0 | Accepted, 2026-10-01 |
| [014](ADR-014-foreground-audio-effects.md) | Foreground mixed audio effects | Accepted scope by owner instruction, 2026-10-22 |
| [015](ADR-015-offline-library.md) | An offline library from Kiwix archives | Proposed |
