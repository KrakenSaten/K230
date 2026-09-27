#!/usr/bin/env bash
# Push Doors binaries from the Buildroot target tree to a running board
# over SSH, without reflashing. Restarts sysd, netd and radiod (and the shell
# if enabled). A board flashed before netd existed has no S55netd to stop; the
# stop loop skips a script that is not there yet, and the tar brings it. What it carries has to match what the flashed image carries, or a
# bench deployment quietly leaves the board running an older service than the
# one being tested; tests/package_sync_test.sh checks the init scripts against
# the rootfs overlay.
#
# The work on the unit is done by deploy_unit.sh, which this script copies to
# the unit with the archive and starts there detached from the ssh session, so
# a deploy over the unit's Wi-Fi address survives netd dropping that address
# (see "the unit side runs without this connection" below).
#
# Usage: deploy.sh <ip> [/path/to/T-Display-K230 checkout]
#   DEPLOY_TIMEOUT        seconds to wait for the unit's result (default 300)
#   DEPLOY_POLL_INTERVAL  seconds between polls (default 2)
# Exit status: 0 deployed; 1 refused or failed (the message says whether the
# unit was touched); 2 no result within DEPLOY_TIMEOUT, outcome unknown.
set -euo pipefail

TARGET_HOST="${1:?usage: deploy.sh <ip> [vendor checkout]}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
VENDOR_DIR="${2:-${POCKETOS_VENDOR_DIR:-${REPO_DIR}/vendor/T-Display-K230}}"
CONF="k230_pocketos_defconfig"
T="${VENDOR_DIR}/k230_linux_sdk/output/${CONF}/target"
# ServerAlive: a connection whose network went away (Wi-Fi, when netd stops)
# ends in about 15 s instead of hanging on a TCP session nobody will answer.
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no
     -o ServerAliveInterval=5 -o ServerAliveCountMax=3)
case "${TARGET_HOST}" in *@*) ;; *) TARGET_HOST="root@${TARGET_HOST}" ;; esac

# Everything the archive below carries, init scripts included, and checked
# before the board is touched: the remote half stops the services first, so a
# file missing here would otherwise be discovered after they were down. An
# init script new to the overlay reaches the target tree only when Buildroot
# finalises the rootfs (a full build_image.sh), not with pocketos-rebuild.
for f in usr/bin/doors usr/bin/pos usr/bin/pos-hwcheck usr/bin/pos-spixfer usr/bin/pos-wave usr/bin/pos-camera usr/bin/pos-zabbix usr/bin/pos-browser usr/bin/pos-record usr/bin/pos-drmtest usr/bin/pos-display-boot usr/bin/pos-supervise usr/sbin/radiod usr/sbin/sysd usr/sbin/netd usr/sbin/meshcored usr/bin/doors-shell etc/doors-release etc/pocketos-release \
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
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
tar -C "${T}" --owner=0 --group=0 --numeric-owner -cf "${WORK}/payload.tar" \
    usr/bin/doors usr/bin/pos usr/bin/pos-hwcheck usr/bin/pos-spixfer usr/bin/pos-wave usr/bin/pos-camera usr/bin/pos-zabbix usr/bin/pos-browser usr/bin/pos-record usr/bin/pos-drmtest usr/bin/pos-display-boot usr/bin/pos-supervise usr/sbin/radiod \
    usr/sbin/sysd usr/sbin/netd usr/sbin/meshcored usr/bin/doors-shell etc/doors-release etc/pocketos-release \
    usr/share/doors/THIRD_PARTY_NOTICES.txt usr/share/pocketos/THIRD_PARTY_NOTICES.txt usr/share/doors/ui \
    etc/init.d/S50sysd \
    etc/init.d/S55netd etc/init.d/S60radiod etc/init.d/S65meshcored etc/init.d/S90doors-shell

# ---- the unit side runs without this connection ---------------------------
#
# The unit-side half (deploy_unit.sh) stops netd, and netd manages wlan0. It
# used to run inside this script's ssh session with the archive piped through
# that same session, so a deploy to the unit's Wi-Fi address cut its own
# connection and left the unpack waiting forever for an archive that could no
# longer arrive, with every service stopped (unit A, 192.168.10.171, build
# 09be665, 2026-09-25; recovered by deploying again over eth0). Now:
#
#   1. stage   - the archive, deploy_unit.sh and run.sh are copied to STAGE on
#                the unit and checked against their SHA-256 sums there. Nothing
#                has been stopped yet, so a transfer that fails here costs
#                nothing.
#   2. launch  - run.sh is started detached from the ssh session (setsid +
#                nohup, every descriptor redirected), so the session can end,
#                or be cut, without the deploy noticing.
#   3. poll    - the host reads STAGE/log and STAGE/status until the unit
#                records a result. A poll that cannot connect is expected while
#                netd restarts and is retried; only DEPLOY_TIMEOUT ends it.
#
# The unit's own ordering guarantees (meshcored before radiod, no unpack over a
# service that did not stop, no start of an incomplete installation) are all in
# deploy_unit.sh, unchanged by where it runs.
#
# STAGE is fixed so a later deploy, or a person on the serial console, can find
# the last deploy's log. It is under /tmp, a tmpfs of 483.7 MB on unit A
# (VERIFIED, docs/hardware/POST_BRINGUP_REVIEW_2026-09-07.md), so the archive
# fits and a reboot clears it; the directory is root-only. The archive is
# removed from it once the deploy has finished; the log and the status stay
# until the next deploy.
STAGE=/tmp/doors-deploy
POLL_INTERVAL="${DEPLOY_POLL_INTERVAL:-2}"
TIMEOUT="${DEPLOY_TIMEOUT:-300}"
remote() { # <script>: runs <script> on the unit with STAGE set
    "${SSH[@]}" "${TARGET_HOST}" "STAGE=${STAGE}
$1"
}

cp "${SCRIPT_DIR}/deploy_unit.sh" "${WORK}/deploy_unit.sh"
# What the detached process runs: the deploy, then its result, written last and
# renamed into place so the host never reads half a status. Its pid tells a
# later deploy that this one is still going.
cat > "${WORK}/run.sh" <<'EOF'
#!/bin/sh
echo $$ > pid
sh ./deploy_unit.sh ./payload.tar > log 2>&1
rc=$?
rm -f ./payload.tar
echo "$rc" > status.new
mv status.new status
EOF
(cd "${WORK}" && sha256sum payload.tar deploy_unit.sh run.sh > SHA256SUMS)

# 1. stage
tar -C "${WORK}" --owner=0 --group=0 --numeric-owner -cf - SHA256SUMS payload.tar deploy_unit.sh run.sh \
    | remote 'set -e
if [ -s "$STAGE/pid" ] && [ ! -e "$STAGE/status" ] && kill -0 "$(cat "$STAGE/pid")" 2>/dev/null; then
	echo "deploy: another deploy is still running on this unit ($STAGE/log); nothing has been touched" >&2
	exit 1
fi
rm -rf "$STAGE"
mkdir -m 0700 "$STAGE"
cd "$STAGE"
tar -xf -
if ! sha256sum -c SHA256SUMS; then
	echo "deploy: the staged files do not match what was sent; nothing has been stopped" >&2
	exit 1
fi' || {
    echo "deploy: staging on ${TARGET_HOST} failed (above); nothing on the unit has been stopped" >&2
    exit 1
}

# 2. launch. A failure here does not say whether the unit started it (the
# connection can go after the command ran), so it is not the end: the polls
# find out, and a unit that never started it has no pid file.
remote 'cd "$STAGE" || exit 1
setsid nohup sh ./run.sh < /dev/null > /dev/null 2>&1 &' < /dev/null ||
    echo "deploy: the start command did not complete; asking the unit whether it started"

# 3. poll. The first line is the state, the rest the whole log so far; only the
# part not yet shown is printed, so the output reads like the old single stream.
POLL='cd "$STAGE" 2>/dev/null || { echo gone; exit 0; }
if [ -s status ]; then echo "status $(cat status)"
elif [ ! -s pid ]; then echo notstarted
elif kill -0 "$(cat pid)" 2>/dev/null; then echo running
elif [ -s status ]; then echo "status $(cat status)"
else echo died; fi
cat log 2>/dev/null || true'
started=${SECONDS}
shown=0
lost=0
notstarted=0
fail() { echo "deploy: $*" >&2; echo "        The unit keeps its log in ${STAGE}/log." >&2; exit 1; }
while :; do
    if remote "${POLL}" < /dev/null > "${WORK}/poll" 2> "${WORK}/poll.err"; then
        if [ "${lost}" -ne 0 ]; then
            echo "deploy: ${TARGET_HOST} answers again"
            lost=0
        fi
        state=$(head -n 1 "${WORK}/poll")
        tail -n +2 "${WORK}/poll" > "${WORK}/log"
        size=$(wc -c < "${WORK}/log")
        if [ "${size}" -gt "${shown}" ]; then
            tail -c +"$((shown + 1))" "${WORK}/log"
            shown=${size}
        fi
        case "${state}" in
        "status 0")
            echo "Done."
            exit 0 ;;
        status\ *)
            fail "the deploy on the unit failed (exit ${state#status }, above)" ;;
        running)
            notstarted=0 ;;
        notstarted)
            notstarted=$((notstarted + 1))
            # run.sh writes its pid before anything else, so a unit that has
            # none after this long never started it, and nothing was stopped.
            [ "${notstarted}" -lt 5 ] || fail "the deploy never started on the unit; nothing has been stopped" ;;
        died)
            fail "the deploy on the unit ended without recording a result (killed?); services may be stopped" ;;
        gone)
            fail "${STAGE} is gone from the unit (rebooted?); what the deploy did is unknown" ;;
        *)
            fail "unexpected answer from the unit: ${state}" ;;
        esac
    else
        if [ "${lost}" -eq 0 ]; then
            echo "deploy: ${TARGET_HOST} does not answer ($(tail -n 1 "${WORK}/poll.err"))."
            echo "        Expected while netd restarts if this is the unit's Wi-Fi address; the deploy"
            echo "        carries on there without this connection. Still polling."
        fi
        lost=$((lost + 1))
    fi
    if [ $((SECONDS - started)) -ge "${TIMEOUT}" ]; then
        echo "deploy: no result from ${TARGET_HOST} after ${TIMEOUT} s (DEPLOY_TIMEOUT)." >&2
        echo "        The deploy runs on the unit by itself and is not stopped by this. Reach the" >&2
        echo "        unit another way (eth0, or the serial console) and read ${STAGE}/log and" >&2
        echo "        ${STAGE}/status: status holds the exit code once it has finished." >&2
        exit 2
    fi
    sleep "${POLL_INTERVAL}"
done
