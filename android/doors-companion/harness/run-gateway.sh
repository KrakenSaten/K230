#!/bin/bash
# Builds the reference gatewayd's test-hooks binary into harness/out and runs
# it behind gateway_harness.py. Run inside WSL (needs gcc, libssl-dev,
# libcjson-dev, python3). The reference checkout is read and never written:
# the one compiler command is taken from its Makefile by a dry run, with the
# output path replaced by one here.
#
#   harness/run-gateway.sh [reference checkout] [tls port] [control port]
#
# With DOORS_SHELL_BIN (the simulator's pocketos-shell, built with PacketShark)
# and DOORS_CAPTURE_HELPER (pos-capture) in the environment, the real shell
# runs beside gatewayd and the capture group is served as on a unit; the
# reference checkout must then be one whose gatewayd has it (G1 or later).
#
# Copyright (c) 2026 Doors authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# By default the checkout this file is in. Until gatewayd M4.2 is on that
# branch, name a checkout that has it (services/gatewayd with gw_send.c).
REF="${1:-$(cd "$HERE/../../.." && pwd)}"
PORT="${2:-47231}"
CTL="${3:-47232}"
OUT="$HERE/out"
mkdir -p "$OUT"
# Built on the Linux side: an executable on a Windows mount may not run.
BIN="${TMPDIR:-/tmp}/doors-companion-gatewayd-testhooks"
cd "$REF"
CMD="$(make -n gatewayd-service-test 2>/dev/null | grep -- '-DGATEWAYD_TEST_HOOKS=1' | grep -v -- '-fsanitize' | head -1)"
[ -n "$CMD" ] || { echo "no gatewayd test-hooks build in $REF/Makefile: pass a checkout that has gatewayd M4.1" >&2; exit 1; }
CMD="${CMD% -o *} -o $BIN"
eval "$CMD"
git -C "$REF" rev-parse --short HEAD > "$OUT/gateway-commit.txt" 2>/dev/null || true
exec python3 "$HERE/gateway_harness.py" "$BIN" "$PORT" "$CTL"
