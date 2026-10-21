#!/bin/bash
# Assemble the device kit from the build artefacts and the kit scripts.
#   assemble_kit.sh [SHELL_COMMIT [LIB_COMMIT [FULL_SHELL_COMMIT]]]
# Defaults: 56aefe5 for both, as first staged. The LVGL set may come from an
# earlier build: a rebuild of the same pinned source and configuration
# exports the same symbols but embeds its build path, so reusing the set
# already on the unit keeps those files byte-identical.
set -euo pipefail
SC=${1:-56aefe5}; LC=${2:-$SC}; FULL=${3:-}
W=$HOME/work/neon-p98/$SC; LW=$HOME/work/neon-p98/$LC; S=/mnt/c/K230-wt/neon-hwkit/kit
D=$W/kit/neon-p98-$SC; rm -rf "$W/kit"; mkdir -p "$D/lib"
cp "$W/artifacts/doors-shell" "$D/"
cp -P "$LW/artifacts/lib/"* "$D/lib/"
for f in kit.env neon-start.sh neon-restore.sh neon-status.sh neon-measure.sh; do sed 's/\r$//' "$S/$f" > "$D/$f"; done
[ -z "$FULL" ] || sed -i "s/^TEST_COMMIT=.*/TEST_COMMIT=$FULL/" "$D/kit.env"
chmod 755 "$D"/*.sh "$D/doors-shell"; chmod 644 "$D/kit.env" "$D"/lib/*.so.9.5.0
for f in "$D"/*.sh; do sh -n "$f"; done
(cd "$D" && sha256sum doors-shell lib/*.so.9.5.0 kit.env neon-*.sh > SHA256SUMS)
tar --owner=0 --group=0 --sort=name --mtime=2026-10-20 -C "$W/kit" -cf "$W/kit/neon-p98-$SC.tar" "neon-p98-$SC"
cp "$W/kit/neon-p98-$SC.tar" /mnt/c/K230-wt/neon-hwkit/
grep TEST_COMMIT "$D/kit.env"; cat "$D/SHA256SUMS"; sha256sum "$W/kit/neon-p98-$SC.tar"
