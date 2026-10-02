# Doors brand assets

Copyright (c) 2026 PocketOS authors. All rights reserved, except as stated
below. Decided by the product owner on 2026-10-02.

The files listed here are **not licensed under the Apache License 2.0**.
The rest of Doors' own artwork is (docs/licensing/B1_ARTWORK.md).

**Permission.** Anyone may copy and redistribute these files **unmodified**
as part of Doors or of a work based on Doors, in source form or in images
and binaries built from it, and may use them unmodified to refer to Doors.

**Not granted.** Any other use: modified versions of the files, use of
them for another product or service, or use that suggests the Doors
project endorses something. The Apache License 2.0 grants no rights to the
names "Doors" or "PocketOS" either (its section 6).

## The files

| Path | What it is | In the image |
| --- | --- | --- |
| `docs/design/brand/doors-threshold/brand/` | the Doors mark (compact, dark, light, 16 px) and the primary lockups with the wordmark | no |
| `docs/design/brand/doors-threshold/boot/` | the boot splash art | no |
| `platforms/k230/rootfs_overlay/logo.xrgb` | the boot splash, generated from the boot art | yes, `/logo.xrgb` and the boot partition |
| `ui/pocketui/pos_brand_mark.c` | the compact mark as an alpha mask, generated from the mark | compiled into `/usr/bin/doors-shell` |

The supplier mockups and style sheet that also show the mark
(`doors-threshold/mockups/`, `reference/`) are kept out of the public
source (docs/licensing/public-source-exclude.txt). Screenshots of Doors
in the documentation show these marks as they appear on the screen; the
marks in them keep these terms.

`docs/licensing/asset-inventory.txt` classes these files BRAND and
DERIVED-BRAND, and `tests/license_audit_test.sh` checks that every file in
those classes is listed here and that none carries an Apache-2.0 SPDX tag.
