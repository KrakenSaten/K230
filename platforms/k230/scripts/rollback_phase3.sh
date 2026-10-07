#!/usr/bin/env bash
# Take the Doors Phase 3 shell identity off a unit, so that a pre-Phase-3
# deploy can put the PocketOS-era one back (ADR-005 Phase 3).
#
# This is the whole rollback: it removes an identity, it does not install one.
# After it runs the unit has no shell service at all - deliberately, because
# that is the only state from which exactly one can be installed again. The
# next step is an ordinary deploy.sh from a pre-Phase-3 checkout, which brings
# /usr/bin/pocketos-shell and /etc/init.d/S90pocketos-shell and starts them.
#
# What it keeps, and why:
#   /etc/default/pocketos-shell   what the restored service reads. If only the
#                                 Phase 3 file exists it is copied here whole,
#                                 never merged, so hand-edited settings survive
#                                 the round trip.
#   /etc/default/doors-shell      left in place: harmless to the old service,
#                                 and it is what a second migration would read.
#   /var/lib/pocketos/log         history, including supervise-doors-shell.log.
#   /run/pocketos, /var/lib/pocketos, /etc/pocketos  untouched (Phase 4, if ever).
#   the Phase 2 names (doors, pos, /etc/doors-release ...) untouched: they are
#   not this phase's, and the pre-Phase-3 deploy carries them too.
#
# Usage: rollback_phase3.sh <ip> [--dry-run]
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

TARGET_HOST="${1:?usage: rollback_phase3.sh <ip> [--dry-run]}"
DRY="${2:-}"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no)
case "${TARGET_HOST}" in *@*) ;; *) TARGET_HOST="root@${TARGET_HOST}" ;; esac
case "${DRY}" in ""|--dry-run) ;; *) echo "usage: rollback_phase3.sh <ip> [--dry-run]" >&2; exit 2 ;; esac

echo "Rolling the Doors Phase 3 shell identity off ${TARGET_HOST}${DRY:+ (dry run)}"
"${SSH[@]}" "${TARGET_HOST}" "DRY='${DRY}' sh -s" <<'REMOTE'
set -e
run() { if [ -n "$DRY" ]; then echo "  would: $*"; else "$@"; fi; }

echo "before:"
ls -1 /etc/init.d/S90doors-shell /etc/init.d/S90pocketos-shell 2>/dev/null | sed 's/^/  init: /' || true
ls -1 /usr/bin/doors-shell /usr/bin/pocketos-shell 2>/dev/null | sed 's/^/  bin:  /' || true
ls -1 /etc/default/doors-shell /etc/default/pocketos-shell 2>/dev/null | sed 's/^/  conf: /' || true

# 1. Stop the Doors shell, and anything PocketOS-era that is somehow also up.
for s in S90doors-shell S90pocketos-shell; do
	[ -x /etc/init.d/$s ] || continue
	if ! /etc/init.d/$s stop; then
		echo "rollback: $s could not be stopped; nothing has been removed" >&2
		exit 1
	fi
done

# 2. Nothing of either identity may still be running: removing the binary from
#    under a live shell leaves a process holding DRM with no way to restart it.
for d in /proc/[0-9]*; do
	exe=$(readlink "$d/exe" 2>/dev/null) || continue
	case "$exe" in
	*/doors-shell|*/pocketos-shell)
		echo "rollback: a shell is still running (${d#/proc/}: $exe); nothing has been removed" >&2
		exit 1 ;;
	esac
done

# 3. Settings first, while both names are still around: the restored service
#    reads /etc/default/pocketos-shell, so a unit configured only through the
#    Phase 3 file would otherwise come back with defaults and, on this board,
#    with the panel handed to nobody (ENABLE=0).
if [ -r /etc/default/doors-shell ] && [ ! -e /etc/default/pocketos-shell ]; then
	echo "  copying /etc/default/doors-shell to /etc/default/pocketos-shell (whole file)"
	run cp /etc/default/doors-shell /etc/default/pocketos-shell
elif [ -e /etc/default/pocketos-shell ]; then
	echo "  /etc/default/pocketos-shell kept as it is; /etc/default/doors-shell not merged into it"
fi

# 4. The identity itself: init script before binary, so an interrupted
#    rollback cannot leave a service whose daemon is gone.
run rm -f /etc/init.d/S90doors-shell
run rm -f /usr/bin/doors-shell
# 5. Runtime state, so system.status stops listing a service that is not there.
run rm -f /run/pocketos/doors-shell.pid /run/pocketos/doors-shell.state \
          /run/pocketos/doors-shell.crashloop /var/run/doors-shell-supervise.pid

echo "after:"
left=$(ls -1 /etc/init.d/S90doors-shell /etc/init.d/S90pocketos-shell 2>/dev/null | wc -l)
bins=$(ls -1 /usr/bin/doors-shell /usr/bin/pocketos-shell 2>/dev/null | wc -l)
states=$(ls -1 /run/pocketos/doors-shell.state /run/pocketos/pocketos-shell.state 2>/dev/null | wc -l)
shells=0
for d in /proc/[0-9]*; do
	exe=$(readlink "$d/exe" 2>/dev/null) || continue
	case "$exe" in */doors-shell|*/pocketos-shell) shells=$((shells + 1)) ;; esac
done
echo "  init scripts=$left binaries=$bins supervisor states=$states shell processes=$shells"
ls -1 /etc/default/doors-shell /etc/default/pocketos-shell 2>/dev/null | sed 's/^/  conf: /' || true
if [ -z "$DRY" ] && { [ "$left" -ne 0 ] || [ "$bins" -ne 0 ] || [ "$shells" -ne 0 ]; }; then
	echo "rollback: the unit still carries a shell identity; it is not ready for a pre-Phase-3 deploy" >&2
	exit 1
fi
echo "  ready for a pre-Phase-3 deploy.sh (it installs S90pocketos-shell and /usr/bin/pocketos-shell)"
REMOTE
echo "Done."
