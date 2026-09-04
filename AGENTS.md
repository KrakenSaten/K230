# Instructions for AI agents working in this repository

PocketOS is a handheld operating environment for the LILYGO T-Display K230.
The human developer is the product owner and final authority.

Read before changing anything:

- docs/decisions/ (accepted ADRs are binding; ADR-001 defines the base platform)
- docs/hardware/T-DISPLAY-K230.md (hardware baseline with evidence classes)
- docs/BUILD_ENVIRONMENT.md (host, toolchain, pinned SDK commits, WSL gotchas)

Rules:

- English for code, comments, docs and commits.
- Classify hardware claims as VERIFIED, DOCUMENTED or ASSUMED. Never invent
  pins, registers, device paths or SDK APIs.
- vendor/ is read-only reference material. First-party code lives outside it.
- Public APIs use generic names (radio.*, display.*), never K230-specific ones.
- Do not upgrade toolchain, SDK, kernel or dependencies without a proposal.
- A change is not done until it has been built; say what could not be tested.
