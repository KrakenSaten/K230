#!/bin/bash
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# WSL side: put the cross-built shell and helper of build dir $1 (from the
# pinned SDK build, docs/apps/RELAY_NINE_DEVICE.md) beside these scripts in
# kit directory $2, with SHA256SUMS. Copy $2 to /root/relay-nine-<commit>.
set -eu
B=${1:?build dir with build/pocketos-shell and src/tools/audio-effects/pos-audio-effects}
K=${2:?kit dir}
G=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$K"
cp "$B/build/pocketos-shell" "$K/doors-shell"
cp "$B/src/tools/audio-effects/pos-audio-effects" "$K/pos-audio-effects"
for f in kit.env rn-start.sh rn-restore.sh rn-status.sh rn-measure.sh rn-scenario.sh rn-drive.py rn-audio.py; do
	sed 's/\r$//' "$G/$f" > "$K/$f"
done
chmod 755 "$K"/*.sh "$K"/*.py "$K/doors-shell" "$K/pos-audio-effects"
cd "$K" && sha256sum doors-shell pos-audio-effects kit.env rn-*.sh rn-*.py > SHA256SUMS && cat SHA256SUMS
