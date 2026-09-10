#!/usr/bin/env bash
# Push PocketOS binaries from the Buildroot target tree to a running board
# over SSH, without reflashing. Restarts sysd and radiod (and the shell if
# enabled). What it carries has to match what the flashed image carries, or a
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

for f in usr/bin/pos usr/bin/pos-hwcheck usr/bin/pos-spixfer usr/bin/pos-supervise usr/sbin/radiod usr/sbin/sysd usr/bin/pocketos-shell etc/pocketos-release; do
    [ -e "${T}/${f}" ] || { echo "missing ${T}/${f}; build the image first" >&2; exit 1; }
done

echo "Deploying PocketOS $(cat "${REPO_DIR}/VERSION") to ${TARGET_HOST}"
# Ownership comes from the archive, not from the build host's account. Without
# this every deployed file lands owned by the builder's uid (1000 on this
# host, VERIFIED on unit A during the v0.0.7 block 2a smoke), so a uid-1000
# process could rewrite an init script that BusyBox rcS runs as root. A
# flashed image has no such problem: Buildroot assigns root ownership when it
# assembles the rootfs, and the bench path has to match it. Numeric rather
# than --owner=root: with --numeric-owner the stored name is unused anyway,
# and 0 needs no passwd lookup on the build host.
tar -C "${T}" --owner=0 --group=0 --numeric-owner -cf - \
    usr/bin/pos usr/bin/pos-hwcheck usr/bin/pos-spixfer usr/bin/pos-supervise usr/sbin/radiod \
    usr/sbin/sysd usr/bin/pocketos-shell etc/pocketos-release etc/init.d/S50sysd etc/init.d/S60radiod \
    etc/init.d/S90pocketos-shell \
    | "${SSH[@]}" "${TARGET_HOST}" 'set -e
/etc/init.d/S90pocketos-shell stop >/dev/null 2>&1 || true
/etc/init.d/S60radiod stop >/dev/null 2>&1 || true
/etc/init.d/S50sysd stop >/dev/null 2>&1 || true
tar -C / -xf -
sync
/etc/init.d/S50sysd start
/etc/init.d/S60radiod start
/etc/init.d/S90pocketos-shell start
pos version; pos radio info | head -5; pos call sysd system.info | head -4'
echo "Done."
