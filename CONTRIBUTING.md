# Contributing to Doors

Doors is licensed under the Apache License, Version 2.0 (LICENSE,
docs/decisions/ADR-013-licence-apache-2.md). This file is the short version
of what that means for a contribution.

## Licence of contributions

- A contribution you submit (a patch, pull request, file or comment intended
  for inclusion) is licensed under the Apache License, Version 2.0, as its
  section 5 says, with no additional terms. There is no contributor licence
  agreement.
- Only submit work you wrote yourself, or work you have the right to submit
  under that licence.
- Sign off each commit to certify the Developer Certificate of Origin 1.1
  (https://developercertificate.org/): add `Signed-off-by: Your Name
  <you@example.com>` with `git commit -s`. The project does not enforce this
  yet; it is recommended so that every change states where it came from.

## Source headers

A new first-party source file starts its header comment with:

```text
Copyright (c) 2026 PocketOS authors.
SPDX-License-Identifier: Apache-2.0
```

(in the file's comment syntax). The copyright line keeps the PocketOS name on
purpose (ADR-005 decision 7).

Do **not** put that header on:

- third-party code, or code adapted from it: keep its own copyright and
  licence notice, unchanged, and add an entry to THIRD_PARTY_LICENSES.md (and
  to third_party/notices/SOURCES if it reaches a binary);
- kernel or other upstream patches (`platforms/k230/patches/`): they take the
  licence of the files they modify;
- generated files: they follow their generator's inputs;
- anything under `vendor/`, `third_party/` or `docs/legal/`.

`tests/license_audit_test.sh` refuses an Apache-2.0 SPDX line in those
places.

## Third-party material and models

Adding a dependency, vendored code, a font, artwork or a machine learning
model needs its licence established first, from the upstream's own licence
file or an equivalent primary source, and recorded in THIRD_PARTY_LICENSES.md
or MODEL_LICENSES.md. Material whose licence cannot be established is not
committed and not packaged. docs/licensing/APACHE_2_READINESS.md explains the
categories.

## Everything else

AGENTS.md, docs/decisions/ (accepted ADRs are binding) and
docs/BUILD_ENVIRONMENT.md describe how the project is built and changed. A
change is not done until it has been built and its tests pass.
