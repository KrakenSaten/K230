#!/usr/bin/env bash
# Push Doors binaries from the Buildroot target tree to a running board
# over SSH, without reflashing. Restarts sysd, netd and radiod (and the shell
# if enabled). A board flashed before netd existed has no S55netd to stop; the
# stop loop skips a script that is not there yet, and the tar brings it. What it carries has to match what the flashed image carries, or a
# bench deployment quietly leaves the board running an older service than the
# one being tested; tests/package_sync_test.sh checks the init scripts against
# the rootfs overlay.
#
# Usage: deploy.sh <ip> [/path/to/T-Display-K230 checkout]
set -euo pipefail

TARGET_HOST="${1:?usage: deploy.sh <ip> [vendor checkout]}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
VENDOR_DIR="${2:-${POCKETOS_VENDOR_DIR:-${REPO_DIR}/vendor/T-Display-K230}}"
CONF="k230_pocketos_defconfig"
T="${VENDOR_DIR}/k230_linux_sdk/output/${CONF}/target"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no)
case "${TARGET_HOST}" in *@*) ;; *) TARGET_HOST="root@${TARGET_HOST}" ;; esac

# Everything the archive below carries, init scripts included, and checked
# before the board is touched: the remote half stops the services first, so a
# file missing here would otherwise be discovered after they were down. An
# init script new to the overlay reaches the target tree only when Buildroot
# finalises the rootfs (a full build_image.sh), not with pocketos-rebuild.
for f in usr/bin/doors usr/bin/pos usr/bin/pos-hwcheck usr/bin/pos-spixfer usr/bin/pos-wave usr/bin/pos-supervise usr/sbin/radiod usr/sbin/sysd usr/sbin/netd usr/bin/doors-shell etc/doors-release etc/pocketos-release \
         usr/share/doors/THIRD_PARTY_NOTICES.txt usr/share/pocketos/THIRD_PARTY_NOTICES.txt \
         etc/init.d/S50sysd etc/init.d/S55netd etc/init.d/S60radiod etc/init.d/S90doors-shell; do
    [ -e "${T}/${f}" ] || { echo "missing ${T}/${f}; build the image first (a full build_image.sh for a new init script)" >&2; exit 1; }
done

echo "Deploying Doors $(cat "${REPO_DIR}/VERSION") to ${TARGET_HOST}"
# Ownership comes from the archive, not from the build host's account. Without
# this every deployed file lands owned by the builder's uid (1000 on this
# host, VERIFIED on unit A during the v0.0.7 block 2a smoke), so a uid-1000
# process could rewrite an init script that BusyBox rcS runs as root. A
# flashed image has no such problem: Buildroot assigns root ownership when it
# assembles the rootfs, and the bench path has to match it. Numeric rather
# than --owner=root: with --numeric-owner the stored name is unused anyway,
# and 0 needs no passwd lookup on the build host.
#
# The compatibility names of ADR-005 Phase 2 travel as the symlinks the target
# tree holds (usr/bin/pos -> doors, etc/pocketos-release -> doors-release,
# usr/share/pocketos/THIRD_PARTY_NOTICES.txt -> ../doors/...), not as copies,
# so the board ends up with one file under two names, like a flashed card.
# That relies on BusyBox tar (1.37.0 in the image; archival/libarchive/
# data_extract_all.c and unsafe_symlink_target.c): it unlinks an existing
# non-directory before extracting an entry, so a PocketOS-era regular file is
# replaced by the link, and it creates a link whose target contains ".." after
# every other entry. It cannot unlink a directory, which is why no directory
# is ever turned into a link.
tar -C "${T}" --owner=0 --group=0 --numeric-owner -cf - \
    usr/bin/doors usr/bin/pos usr/bin/pos-hwcheck usr/bin/pos-spixfer usr/bin/pos-wave usr/bin/pos-supervise usr/sbin/radiod \
    usr/sbin/sysd usr/sbin/netd usr/bin/doors-shell etc/doors-release etc/pocketos-release \
    usr/share/doors/THIRD_PARTY_NOTICES.txt usr/share/pocketos/THIRD_PARTY_NOTICES.txt etc/init.d/S50sysd \
    etc/init.d/S55netd etc/init.d/S60radiod etc/init.d/S90doors-shell \
    | "${SSH[@]}" "${TARGET_HOST}" 'set -e
# The tar below replaces the binaries these services are executing, so a stop
# that did not finish has to end the deployment rather than be unpacked over.
# `|| true` hid exactly that, and the init scripts exited 0 whatever happened,
# so a board could be left running a mixture of the old services and the new
# files with nothing in the output to say so.
#
# Both shell services are stopped, the Doors one first: on a unit that has been
# here before only the first exists, on a PocketOS-era unit only the second,
# and on a unit whose migration was interrupted both may. Stopping is the only
# thing done to the old service before the payload is unpacked - the removals
# come after, so an interrupted deploy leaves a unit that still has one
# working shell rather than none (ADR-005 Phase 3).
for s in S90doors-shell S90pocketos-shell S60radiod S55netd S50sysd; do
	[ -x /etc/init.d/$s ] || continue
	if ! /etc/init.d/$s stop; then
		echo "deploy: $s could not be stopped; nothing has been installed" >&2
		exit 1
	fi
done
# No shell of either identity may still hold DRM or shell.sock when the files
# under it are replaced. The pid files are the supervisors own records; the
# /proc sweep catches a daemon whose supervisor was killed and whose pid file
# is therefore stale or gone.
for pf in /var/run/doors-shell-supervise.pid /run/pocketos/doors-shell.pid \
          /var/run/pocketos-shell-supervise.pid /run/pocketos/pocketos-shell.pid; do
	[ -s "$pf" ] || continue
	p=$(cat "$pf")
	if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then
		echo "deploy: a shell is still running (pid $p from $pf); nothing has been installed" >&2
		exit 1
	fi
done
for d in /proc/[0-9]*; do
	exe=$(readlink "$d/exe" 2>/dev/null) || continue
	case "$exe" in
	*/doors-shell|*/pocketos-shell)
		echo "deploy: a shell is still running (${d#/proc/}: $exe); nothing has been installed" >&2
		exit 1 ;;
	esac
done
tar -C / -xf -
sync
# The PocketOS-era identity goes now, and in this order: the init script first,
# so a reboot in the middle of the next two commands cannot start a service
# whose binary is gone, and never the other way round. The settings file is
# deliberately left alone - it is what S90doors-shell falls back to, and it is
# what a rollback needs (ADR-005 Phase 3, docs/hardware/DOORS_PHASE3_GATE.md).
rm -f /etc/init.d/S90pocketos-shell
rm -f /usr/bin/pocketos-shell
# The supervisor state of a service that no longer exists would otherwise stay
# in the tmpfs until the next reboot and show up as a second service row in
# system.status. The logs stay: they are history, and they are persistent.
rm -f /run/pocketos/pocketos-shell.pid /run/pocketos/pocketos-shell.state \
      /run/pocketos/pocketos-shell.crashloop /var/run/pocketos-shell-supervise.pid
/etc/init.d/S50sysd start
/etc/init.d/S55netd start
/etc/init.d/S60radiod start
/etc/init.d/S90doors-shell start
# One shell, and the unit says so itself rather than the deploy assuming it.
shells=0
for d in /proc/[0-9]*; do
	exe=$(readlink "$d/exe" 2>/dev/null) || continue
	case "$exe" in */doors-shell|*/pocketos-shell) shells=$((shells + 1)) ;; esac
done
scripts=$(ls /etc/init.d/S90doors-shell /etc/init.d/S90pocketos-shell 2>/dev/null | wc -l)
states=$(ls /run/pocketos/doors-shell.state /run/pocketos/pocketos-shell.state 2>/dev/null | wc -l)
echo "deploy: shell processes=$shells init scripts=$scripts supervisor states=$states"
if [ "$shells" -gt 1 ] || [ "$scripts" -gt 1 ] || [ "$states" -gt 1 ]; then
	echo "deploy: more than one shell identity is present; this unit needs attention" >&2
	exit 1
fi
doors version; pos version; doors radio info | head -5; doors call sysd system.info | head -4'
echo "Done."
