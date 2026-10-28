# v0.3.9 integration install kit (unit A, 2026-10-28)

The scripts that put `integration/v0.3.9` at `3ccef4e2` on unit A and can
take it off again. The record of that install is in
docs/releases/v0.3.9.md, "Unit A integration install". This is a binary
update of two files on an existing card, not a fresh-card test.

| File | Where it runs | sha256 (as run on the unit) |
| --- | --- | --- |
| `xbuild.sh` | build host (WSL): cross-builds `doors-shell` and `pos-vision` from a `git archive` tar with its BUILD_ID, against the existing `k230_pocketos_defconfig` output, which it does not modify | (host only; SPDX header added afterwards) |
| `install.sh` | unit: checks, baseline, backup, swaps both binaries, restarts only S90doors-shell | `f3a5ffb2c847f0f459ce072f424d460162a900c8f175344c5f6507df3865b843` |
| `RESTORE.sh` | unit: puts the backed-up binaries back, restarts only the shell, checks against the baseline, prints `RESTORE: OK` | `411357a87ba5dc710ca55cc6317b6caa81e689b1c4443240834403cc6d911b25` |
| `smoke.sh` | unit: the automatic smoke check (no radio) | `e598d9bada0736882edb3fc0a7bd18ae0ac986bbd6ab6b230c77c416092100d7` |

Use:

1. `git archive --prefix=integ/ <commit> -o integ.tar`, then in WSL
   `bash xbuild.sh integ.tar <short commit>`.
2. Put the two binaries and the three unit scripts in one directory with a
   `SHA256SUMS` over `doors-shell pos-vision install.sh RESTORE.sh`, copy it
   to `/root/rollback-integ-<commit>/` (over the console: gzip, base64,
   `stty -echo; cat > file`; BusyBox `tar` has no `-z`, use
   `gunzip -c | tar xf -`).
3. On the unit: `sh install.sh`, then `sh smoke.sh`.
4. Roll back with `sh RESTORE.sh`. Keep the directory: it is the rollback.

`smoke.sh` reads `/var/lib/pocketos/log/doors-shell.log`, but the shell
writes `shell.log`, so on 2026-10-28 its log lines were checked by hand
instead (the release notes say what was found).
