#!/bin/bash
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# Focused offline UI matrix and independent rendered-QR decoding.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=${SYSTEM_APP_TEST:-$(dirname "${SHELL_BIN:?set SHELL_BIN or SYSTEM_APP_TEST}")/system_app_test}
CAPTURES=${FEEDBACK_CAPTURE_DIR:-$(mktemp -d)}
mkdir -p "$CAPTURES"
LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT
SDL_VIDEODRIVER=dummy FEEDBACK_ONLY=1 FEEDBACK_CAPTURE_DIR="$CAPTURES" "$BIN" >"$LOG" 2>&1 || {
    cat "$LOG"; exit 1;
}
cat "$LOG"
if rg -q '^\[(Warn|Error)\]' "$LOG"; then exit 1; fi
DECODER=${ZBARIMG:-zbarimg}
if ! command -v "$DECODER" >/dev/null; then
    echo 'SKIP independent QR decoding: zbarimg unavailable (UI checks completed)'
    exit 0
fi
python3 - "$DECODER" "$CAPTURES" <<'PY'
import pathlib, subprocess, sys
expected = b'https://forms.cloud.microsoft/Pages/ResponsePage.aspx?id=vs8BABdVnkWHpewtoKeA6lgv2Sex3hFLvOE0xvUNR19UOTdaVDhCMU5MTVg4T1dBSzFXRzBTN0pYNi4u&lang=en-US'
paths = sorted(pathlib.Path(sys.argv[2]).glob('*.png'))
assert len(paths) == 18, f'expected 18 matrix captures, got {len(paths)}'
for path in paths:
    result = subprocess.run([sys.argv[1], '-q', '--raw', str(path)], capture_output=True, check=True)
    assert result.stdout == expected + b'\n', (path, result.stdout)
    print(f'ok   independent software decode: {path.name}')
print('Decoded 18 rendered QR images to the exact owner URL. Physical K230 display scanning remains a hardware check.')
PY
