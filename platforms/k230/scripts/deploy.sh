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
for f in usr/bin/doors usr/bin/pos usr/bin/pos-hwcheck usr/bin/pos-spixfer usr/bin/pos-wave usr/bin/pos-camera usr/bin/pos-supervise usr/sbin/radiod usr/sbin/sysd usr/sbin/netd usr/sbin/meshcored usr/bin/doors-shell etc/doors-release etc/pocketos-release \
         usr/share/doors/THIRD_PARTY_NOTICES.txt usr/share/pocketos/THIRD_PARTY_NOTICES.txt usr/share/doors/ui \
         etc/init.d/S50sysd etc/init.d/S55netd etc/init.d/S60radiod etc/init.d/S65meshcored etc/init.d/S90doors-shell; do
    [ -e "${T}/${f}" ] || { echo "missing ${T}/${f}; build the image first (a full build_image.sh for a new init script)" >&2; exit 1; }
done
# meshcored is part of every image since its third-party notices landed
# (docs/LICENSING.md item 9), so it is required like the others: a tree without
# it is an old or incomplete build, and deploying from one would leave the unit
# with an init script and no service - or, on a unit that had it, a service
# from another build.
#
# The whole tree is then checked the way verify_image.sh checks an image: every
# service a binary and its init script together, and every binary the build its
# release file names (tools/release/check_rootfs.sh). Buildroot never deletes
# from its target tree, so a binary an earlier build left there would otherwise
# travel with this one, under a release file that describes something else.
bash "${REPO_DIR}/tools/release/check_rootfs.sh" "${T}" || {
    echo "deploy: ${T} is not one complete build (above); nothing has been touched" >&2
    exit 1
}
# The init scripts carry no build stamp, and the target tree's are only as new
# as the last full image build: Buildroot copies the rootfs overlay into the
# tree when it finalises the rootfs, and a package-only rebuild
# (pocketos-rebuild) does not. So each is compared with the overlay
# apply_to_sdk.sh applied - what the next image would carry - and a tree still
# holding an older one is refused rather than sent.
OVL="${VENDOR_DIR}/k230_linux_sdk/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay"
for s in S50sysd S55netd S60radiod S65meshcored S90doors-shell; do
    cmp -s "${T}/etc/init.d/${s}" "${OVL}/etc/init.d/${s}" || {
        echo "deploy: ${T}/etc/init.d/${s} is not the applied overlay's ${OVL}/etc/init.d/${s};" >&2
        echo "        the target tree has not been finalised since apply_to_sdk.sh. Run a full" >&2
        echo "        build_image.sh. Nothing has been touched." >&2
        exit 1
    }
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
    usr/bin/doors usr/bin/pos usr/bin/pos-hwcheck usr/bin/pos-spixfer usr/bin/pos-wave usr/bin/pos-camera usr/bin/pos-supervise usr/sbin/radiod \
    usr/sbin/sysd usr/sbin/netd usr/sbin/meshcored usr/bin/doors-shell etc/doors-release etc/pocketos-release \
    usr/share/doors/THIRD_PARTY_NOTICES.txt usr/share/pocketos/THIRD_PARTY_NOTICES.txt usr/share/doors/ui \
    etc/init.d/S50sysd \
    etc/init.d/S55netd etc/init.d/S60radiod etc/init.d/S65meshcored etc/init.d/S90doors-shell \
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
# meshcored stops before radiod, which is the order they depend in: it holds
# the radio lease for as long as it runs, and stopping it first hands that
# lease back rather than leaving radiod to notice a closed socket.
# (No apostrophes in here: this whole stanza is inside a single-quoted heredoc
# sent over ssh, and one would end it.)
# A meshcored no init script started - by hand, as every bench gate before
# S65meshcored was installed did - survives the stop of S65meshcored, or has no
# S65meshcored to be stopped by on a unit that never had one. Left running it
# keeps the old binary, the node identity and the radio lease, and blocks the
# supervised one. It is asked to leave (it writes its node table on SIGTERM)
# before radiod stops - the order the two depend in - and the deploy stops if
# it will not.
stop_unsupervised_meshcored() {
	mcd=""
	for d in /proc/[0-9]*; do
		exe=$(readlink "$d/exe" 2>/dev/null) || continue
		case "$exe" in */meshcored|*"/meshcored (deleted)") mcd="$mcd ${d#/proc/}" ;; esac
	done
	[ -n "$mcd" ] || return 0
	echo "deploy: stopping a meshcored no init script started:$mcd"
	for p in $mcd; do kill "$p" 2>/dev/null || true; done
	for p in $mcd; do
		n=0
		while kill -0 "$p" 2>/dev/null && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
		if kill -0 "$p" 2>/dev/null; then
			echo "deploy: meshcored $p did not stop; nothing has been installed" >&2
			exit 1
		fi
	done
}
for s in S90doors-shell S90pocketos-shell S65meshcored S60radiod S55netd S50sysd; do
	if [ "$s" = S60radiod ]; then stop_unsupervised_meshcored; fi
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
# What arrived, asked of the unit rather than assumed from the archive: every
# service a binary and an init script, both executable, and the supervisor they
# run under. A unit left with one half of a service is exactly the state this
# deploy exists to prevent, so it ends the deploy loudly instead of starting
# whatever happens to be whole.
incomplete=0
for f in /usr/sbin/sysd /etc/init.d/S50sysd /usr/sbin/netd /etc/init.d/S55netd \
         /usr/sbin/radiod /etc/init.d/S60radiod /usr/sbin/meshcored /etc/init.d/S65meshcored \
         /usr/bin/doors-shell /etc/init.d/S90doors-shell /usr/bin/pos-supervise; do
	[ -x "$f" ] || { echo "deploy: $f is missing or not executable after unpacking" >&2; incomplete=1; }
done
if [ "$incomplete" -ne 0 ]; then
	echo "deploy: the installation is incomplete (above); services were NOT started" >&2
	exit 1
fi
/etc/init.d/S50sysd start
/etc/init.d/S55netd start
/etc/init.d/S60radiod start
# Disabled unless the operator enabled it per unit (MESHCORED_ENABLE=1 in
# /etc/default/meshcored, which this deploy never writes); disabled, it prints
# "disabled" and returns. Enabled, starting it acquires the radio.
# A failure here is said and reported, but does not stop the shell from
# starting: a unit left without a screen is worse than one left without mesh.
mcd_failed=0
/etc/init.d/S65meshcored start || mcd_failed=1
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
if [ "$mcd_failed" -ne 0 ]; then
	echo "deploy: S65meshcored did not start (above); every other service is up" >&2
	exit 1
fi
doors version; pos version; doors radio info | head -5; doors call sysd system.info | head -4'
echo "Done."
