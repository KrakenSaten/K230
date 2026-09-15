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
for f in usr/bin/doors usr/bin/pos usr/bin/pos-hwcheck usr/bin/pos-spixfer usr/bin/pos-wave usr/bin/pos-supervise usr/sbin/radiod usr/sbin/sysd usr/sbin/netd usr/bin/pocketos-shell etc/doors-release etc/pocketos-release \
         usr/share/doors/THIRD_PARTY_NOTICES.txt usr/share/pocketos/THIRD_PARTY_NOTICES.txt \
         etc/init.d/S50sysd etc/init.d/S55netd etc/init.d/S60radiod etc/init.d/S90pocketos-shell; do
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
    usr/sbin/sysd usr/sbin/netd usr/bin/pocketos-shell etc/doors-release etc/pocketos-release \
    usr/share/doors/THIRD_PARTY_NOTICES.txt usr/share/pocketos/THIRD_PARTY_NOTICES.txt etc/init.d/S50sysd \
    etc/init.d/S55netd etc/init.d/S60radiod etc/init.d/S90pocketos-shell \
    | "${SSH[@]}" "${TARGET_HOST}" 'set -e
# The tar below replaces the binaries these services are executing, so a stop
# that did not finish has to end the deployment rather than be unpacked over.
# `|| true` hid exactly that, and the init scripts exited 0 whatever happened,
# so a board could be left running a mixture of the old services and the new
# files with nothing in the output to say so.
for s in S90pocketos-shell S60radiod S55netd S50sysd; do
	[ -x /etc/init.d/$s ] || continue
	if ! /etc/init.d/$s stop; then
		echo "deploy: $s could not be stopped; nothing has been installed" >&2
		exit 1
	fi
done
tar -C / -xf -
sync
/etc/init.d/S50sysd start
/etc/init.d/S55netd start
/etc/init.d/S60radiod start
/etc/init.d/S90pocketos-shell start
doors version; pos version; doors radio info | head -5; doors call sysd system.info | head -4'
echo "Done."
