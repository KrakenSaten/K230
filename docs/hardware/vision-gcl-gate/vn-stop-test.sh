#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# Stop this kit's test shell (only processes running exactly the kit's
# doors-shell) and wait for its pos-vision to leave with it.
K=$(cd "$(dirname "$0")" && pwd)
for p in $(pidof doors-shell); do
	[ "$(readlink /proc/$p/exe)" = "$K/doors-shell" ] || continue
	kill "$p"; i=0
	while kill -0 "$p" 2>/dev/null && [ $i -lt 10 ]; do sleep 1; i=$((i+1)); done
	kill -0 "$p" 2>/dev/null && { echo "$p not gone after 10 s, SIGKILL"; kill -KILL "$p"; sleep 1; }
done
rm -f "$K/test-shell.pid"
# The helper leaves with the shell (PR_SET_PDEATHSIG); give it its grace.
i=0
while [ -n "$(pidof pos-vision)" ] && [ $i -lt 6 ]; do sleep 1; i=$((i+1)); done
for p in $(pidof pos-vision); do echo "pos-vision $p $(readlink /proc/$p/exe) still running" >&2; done
exit 0
