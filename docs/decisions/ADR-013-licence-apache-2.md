# ADR-013: Doors is licensed under the Apache License 2.0

Status: Accepted (product owner, 2026-10-01)
Date: 2026-10-01
Deciders: product owner (final), AI engineering partner (author)

## Context

Doors' own licence has been open since the licensing register was started
(docs/LICENSING.md, open item 1, 2026-09-04). On 2026-09-13 the owner chose
to keep it open: no licence was granted, source files said "License: see
LICENSE (TBD)", the Buildroot package said "Not yet decided" and
`POCKETOS_REDISTRIBUTE = NO`, and external redistribution was not
authorised. That decision was recorded in docs/LICENSING.md, not in an ADR.

On 2026-10-01 the owner decided the licence. Doors is to be open source,
free of charge, usable, modifiable and redistributable by others, permissively
licensed, and usable commercially, under the Apache License 2.0. The choice of
licence is the owner's and is not reopened here.

Constraints already binding:

- AGENTS.md: vendor/ is read-only reference material; hardware and licence
  claims carry evidence classes; nothing is invented.
- ADR-005 decision 6: history is not rewritten. Decision 7: the "PocketOS
  authors" copyright lines are not changed by the rebrand; they belong to the
  licence decision.
- docs/LICENSING.md: third-party material, models and vendor packages each
  keep their own terms, several of them unresolved (items 2, 3, 5, 8, 10, 11).

## Options

The owner chose the licence; this record does not compare licences. The
options that remained were about how to apply it:

1. **Relicense everything in the repository.** Wrong: the repository holds
   third-party licence texts, verbatim upstream files, owner-supplied artwork
   with no stated terms, and kernel patches that are GPL-2.0 by derivation.
   None of these can be made Apache-2.0 by a header.
2. **Apache-2.0 for original Doors material, everything else keeps its own
   terms, with an audit of what is which.** Chosen.

## Decision

1. **Original Doors material is licensed under the Apache License,
   Version 2.0**: the code, build and packaging scripts, tests and the
   documentation written for the project. `LICENSE` at the repository root is
   the licence text exactly as the ASF publishes it; `NOTICE` carries the
   project's copyright line and points to the third-party notices.
2. **Third-party material keeps its own licence.** It is listed in
   `THIRD_PARTY_LICENSES.md` (inventory and compatibility) and
   `THIRD_PARTY_NOTICES.txt` (texts, shipped in the image), and machine
   learning models in `MODEL_LICENSES.md`. Nothing third-party is relabelled
   Apache-2.0, and no upstream notice is replaced.
3. **Not covered by this decision**, each for its own reason and recorded in
   docs/licensing/APACHE_2_READINESS.md: the kernel patches in
   `platforms/k230/patches/linux/` (modifications of GPL-2.0 files, so
   GPL-2.0); generated files whose inputs are third-party (the IBM Plex
   bitmap fonts, OFL-1.1). The artwork under `docs/design/brand/` was made by
   the owner with ChatGPT and is Apache-2.0 like the code (owner, 2026-10-02,
   docs/licensing/B1_ARTWORK.md), except the brand assets in decision 5.
4. **Source headers.** The copyright line stays "Copyright (c) 2026 PocketOS
   authors." (ADR-005 decision 7; who the line names is unchanged by this
   decision). On files that carry that project header, the pointer "License:
   see LICENSE (TBD)." becomes an SPDX line, `SPDX-License-Identifier:
   Apache-2.0`. New first-party files carry both lines. Third-party files,
   imported patches, generated files and files whose provenance is unclear
   get no Apache-2.0 header; the policy is in
   docs/licensing/APACHE_2_READINESS.md §2.
5. **Names and logos.** The licence grants no trademark rights (Apache-2.0
   section 6). The Doors mark, lockups and boot splash are reserved, not
   Apache-2.0; anyone may redistribute them unmodified as part of Doors or a
   work based on it (owner, 2026-10-02; docs/licensing/BRAND.md). Nothing here
   claims a trademark or a registration.
6. **Contributions** are accepted under the project's licence (Apache-2.0
   section 5), with a Developer Certificate of Origin sign-off recommended and
   no contributor licence agreement (CONTRIBUTING.md).
7. **Choosing the licence is not publishing.** The repository stays private,
   no release or image is published and repository visibility is not changed
   by this decision. `POCKETOS_REDISTRIBUTE` stays `NO` until the readiness
   audit clears the source repository. Publication is a later owner action,
   gated by docs/licensing/APACHE_2_READINESS.md.
8. **Superseded:** the owner's 2026-09-13 "no licence" decision in
   docs/LICENSING.md (open item 1), for Doors' own code only. The rest of that
   register stands.

## Consequences

Needed now (branch `chore/apache-2-license`): `LICENSE`, `NOTICE`,
`THIRD_PARTY_LICENSES.md`, `MODEL_LICENSES.md`, `CONTRIBUTING.md`, a licence
section in README.md, the SPDX header change on first-party files, the
Buildroot package naming Apache-2.0 for Doors, the image installing
`LICENSE` and `NOTICE` in `/usr/share/doors/` beside the notices, legal-info
collecting both with their sha256 in `pocketos.hash`, and
`tests/license_audit_test.sh` plus the updated `tests/notices_test.sh`
holding all of it.

Before the repository is public: the blockers in
docs/licensing/APACHE_2_READINESS.md §14, chiefly the rights in the
owner-supplied artwork and the design-tool exports committed with it.

Before an image is distributed: the same, plus the model (docs/LICENSING.md
item 10), the vendor packages without licence metadata (item 5), the
toolchain runtime licences (item 8) and the legal-info archive (item 3).

Risks: a contributor file that is not original slipping in under the
project header. Mitigated by the audit test (no Apache-2.0 SPDX under the
third-party, legal, vendor and patch paths) and by review.

Migration cost: one line per first-party file; no runtime change.

## Evidence

- Owner decision, 2026-10-01: Apache-2.0, permissive, commercial use
  allowed (DOCUMENTED: the owner's brief for this branch).
- `LICENSE` is byte-identical to https://www.apache.org/licenses/LICENSE-2.0.txt
  (11,358 bytes, sha256 `cfc7749b…bc523d30`), fetched 2026-10-01, and to the
  copy OpenCV ships in docs/legal/licenses/opencv4-4.10.0/LICENSE (VERIFIED).
- Authorship: every commit on master is by the owner's account or by Claude
  sessions the owner ran; no other contributor appears in the history
  (VERIFIED from `git log`, docs/licensing/APACHE_2_READINESS.md §10; this is
  evidence, not a legal finding).
