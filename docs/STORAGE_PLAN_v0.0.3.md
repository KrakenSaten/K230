# Storage plan (written for v0.0.3, deferred past it)

Status: architecture proposal written after the first hardware session
(2026-09-07). The partition layout here is still unimplemented: 0.0.3 keeps
the v0.0.1 layout unchanged, and the move is the theme of the release after
it, together with the app stores.

What 0.0.3 did land is the prerequisite. `core/pocketpaths` now owns the four
roots and their overrides, so the paths this plan moves are defined in one
module instead of as literals in pocketipc.h, pocketlog.h, settings.h and
three app stores. The app stores still carry their own copies and adopt the
module together with the partition move, which keeps that change in one
release rather than two.

## What we have (VERIFIED on unit A)

- Card: 58.6 GB microSD. Image: `sysimage-sdcard.img`, 763 MB, GPT with
  U-Boot at fixed offsets, `boot` (80 MB ext4, kernel and DTBs) at 30 MB and
  `rootfs` (600 MB ext4) at 128 MB. Everything past 728 MB is unused.
- The LILYGO BSP removes the Kendryte SDK's first-boot resize, so the root
  filesystem stays 600 MB: 401 MB used by the vendor userspace plus PocketOS,
  131 MB free. `/var/log` is tmpfs; PocketOS logs, crash reports and app
  state live under `/var/lib/pocketos` on the root filesystem; the vendor
  launcher keeps its media under `/root/*` on the same filesystem.
- Golden-image philosophy so far: one raw image, checksummed, written whole;
  the running system is the image plus a handful of files the operator
  writes (`/etc/default/*`, `/etc/pocketos/settings.conf`, `/var/lib/pocketos`).

## Goals for v0.0.3

1. Keep the golden image small, whole and reproducible: the image must stay
   a fixed-size raw file whose checksum means something.
2. Separate what the image owns (system) from what the device accumulates
   (state, logs, large content), so a re-flash of the system does not erase
   the device's data and a corrupted data area does not stop the system from
   booting.
3. Give large content (game assets, map tiles, AI models) room without
   growing the system partition.
4. Make recovery a card-level operation: any card with the golden image
   boots; the data partition is optional.

## Proposal

### Partition layout

Keep the vendor's first three partitions exactly as they are (U-Boot
offsets, `boot` 80 MB, `rootfs` 600 MB) so the vendor tooling, U-Boot
environment and recovery expectations stay valid. Add one partition:

| Partition | Size | Filesystem | Mount | Owner |
| --- | --- | --- | --- | --- |
| rootfs (p2) | 600 MB, unchanged | ext4 | `/` | image |
| **data (p3)** | rest of the card, created on first boot | ext4 (`-O ^64bit`, journal on) | `/data` | device |

Rationale for not growing the rootfs: a grown root filesystem makes the
image's checksum meaningless for the running system, mixes system and data
on one filesystem, and the LILYGO BSP already decided against the SDK's
resize; a separate data partition keeps the root at its shipped size and
its content equal to the image.

### First-boot creation of `/data`

A PocketOS init script (`S05pocketos-data`, before S60) that, if the card
has no third partition and `/etc/pocketos/no-data-partition` does not exist,
creates p3 from the end of p2 to the end of the card with `parted`/`sfdisk`
(both in the image; `parted` already is), formats it once
(`mkfs.ext4 -L pocketos-data`), and mounts it. If p3 exists it is mounted;
if it fails fsck it is left unmounted and PocketOS falls back to the root
filesystem paths with a WARN in the shell log and a `--` style indicator
later. No resize of the root filesystem, ever.

### Directory strategy on `/data`

```
/data/pocketos/state/<app>/      app-owned state (today /var/lib/pocketos/<app>)
/data/pocketos/log/              pocketlog files, crash reports, supervisor logs
/data/pocketos/assets/<app>/     large game assets, read-mostly
/data/pocketos/maps/             map tiles (the vendor launcher expects /root/maps; a symlink keeps it working)
/data/pocketos/models/           AI models (nncase/kmodel files)
/data/pocketos/updates/          staged images or packages for a later updater
```

Transition: `/var/lib/pocketos` becomes a symlink to `/data/pocketos/state`
(and `log` likewise) when `/data` is mounted, created by the same init
script; with no data partition the directories stay where they are today.
The `POCKETOS_STATE_DIR` and `POCKETOS_LOG_DIR` environment variables
already exist for the apps and services, so the code does not change; only
the init scripts point them at `/data` when it is available.

### Sizes and limits

- Logs: keep pocketlog's per-process cap (2 x 512 KB); add a crash-report
  cap (keep the newest N, say 20) in pocketlog or in the init script.
- Assets and models: no quota in v0.0.3; report free space in `pos system
  info` and in the System app.
- Maps: the vendor launcher's `/root/maps` cache can be redirected to
  `/data/pocketos/maps` by a symlink so it stops filling the root filesystem.

### Update and recovery implications

- Re-flashing the golden image rewrites p1 and p2 and the U-Boot area, and
  leaves p3 untouched only if the flash tool writes the image's exact size
  (763 MB) rather than zeroing the card. Document: "write the image, do not
  'clean' the card", and have the first-boot script recognise an existing
  `pocketos-data` label and keep it.
- A damaged data partition never blocks boot: everything on `/data` is
  optional, and the fallback is the v0.0.1 behaviour.
- A future A/B system update needs two root partitions; leaving p3 at the
  end of the card keeps that option open (insert p2b between p2 and p3 by
  shrinking p3 from its start, or re-flash with a new layout and keep p3).
- `BUILD_INFO.txt` and `SHA256SUMS.txt` continue to describe the image only;
  the data partition is never part of the image.

### What the golden-image philosophy keeps

The image stays a fixed 763 MB raw file with a checksum; the system
partition is never grown or modified by PocketOS itself; every device-side
change lives in the small set of operator files or on `/data`. "Flash the
golden image" remains the recovery step, and it does not touch the data
partition.

## Not in this plan

Encryption of `/data`, per-app quotas, an updater, and moving the vendor
launcher's directories. Those need decisions of their own.
