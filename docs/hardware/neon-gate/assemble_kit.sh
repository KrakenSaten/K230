#!/bin/bash
# Assemble the device kit from the build artefacts and the kit scripts.
set -euo pipefail
W=$HOME/work/neon-p98/56aefe5; S=/mnt/c/K230-wt/neon-hwkit/kit
D=$W/kit/neon-p98-56aefe5; rm -rf "$W/kit"; mkdir -p "$D/lib"
cp "$W/artifacts/doors-shell" "$D/"
cp -P "$W/artifacts/lib/"* "$D/lib/"
for f in kit.env neon-start.sh neon-restore.sh neon-status.sh neon-measure.sh; do sed 's/\r$//' "$S/$f" > "$D/$f"; done
chmod 755 "$D"/*.sh "$D/doors-shell"; chmod 644 "$D/kit.env" "$D"/lib/*.so.9.5.0
for f in "$D"/*.sh; do sh -n "$f"; done
(cd "$D" && sha256sum doors-shell lib/*.so.9.5.0 kit.env neon-*.sh > SHA256SUMS)
tar --owner=0 --group=0 --sort=name --mtime=2026-10-20 -C "$W/kit" -cf "$W/kit/neon-p98-56aefe5.tar" neon-p98-56aefe5
cp "$W/kit/neon-p98-56aefe5.tar" /mnt/c/K230-wt/neon-hwkit/
cat "$D/SHA256SUMS"; ls -l "$D/lib"; sha256sum "$W/kit/neon-p98-56aefe5.tar"; tar -tvf "$W/kit/neon-p98-56aefe5.tar"
