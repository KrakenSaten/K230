#!/usr/bin/env bash
# Is this root filesystem a complete, single-build Doors installation?
#
# Usage: check_rootfs.sh <root>
#
# <root> is a Buildroot target tree (deploy.sh), a rootfs extracted from an
# SD-card image (verify_image.sh), or a test's fake root. Nothing is modified.
#
# Unit A was found (2026-09-22) with /usr/sbin/meshcored installed and no
# /etc/init.d/S65meshcored: bench procedures had copied the binary alone, so
# nothing started it at boot, and /etc/doors-release named a different build
# from the binaries beside it. Each half of that is a check here, run before
# anything reaches a unit:
#
#  1. SERVICES COME WHOLE. Every service is a binary and the init script that
#     starts it, and one without the other is refused in both directions - a
#     binary nothing starts, and a script that starts nothing. Both must be
#     executable (BusyBox rcS runs `$i start`), the script must name that
#     binary as its DAEMON, and pos-supervise, which every script runs its
#     daemon under, must be there too.
#  2. ONE BUILD. /etc/doors-release names the build (BUILD_ID=<id>); every Doors
#     binary carries the stamp "DOORS_BUILD_ID=<id>" (core/pocketlog/pocketlog.c,
#     tools/pos/pos.c), and each must name the same build as the release file.
#     A stale binary left in a build tree by an earlier build, or one copied in
#     from another, fails here instead of producing a unit whose release file
#     describes something it is not running.
#  3. NO PER-UNIT SETTINGS. /etc/default/meshcored decides whether a unit runs
#     a radio service and is written on the unit; shipped in a tree it would
#     decide that for every unit at once.
#
# Exit 0 when every check passes, 1 when any fails, 2 on usage.
#
# check_rootfs.sh --list prints every path it reads, one per line, relative to
# the root, so verify_image.sh extracts exactly those rather than keeping a
# list of its own that could fall behind this one.
set -uo pipefail

failed=0
ok()   { echo "  ok   $*"; }
fail() { echo "  FAIL $*"; failed=$((failed + 1)); }

# service | binary | init script. The order is the boot order.
SERVICES="sysd|usr/sbin/sysd|etc/init.d/S50sysd
netd|usr/sbin/netd|etc/init.d/S55netd
radiod|usr/sbin/radiod|etc/init.d/S60radiod
meshcored|usr/sbin/meshcored|etc/init.d/S65meshcored
doors-shell|usr/bin/doors-shell|etc/init.d/S90doors-shell"
SUPERVISE=usr/bin/pos-supervise
# The binaries that carry a build stamp. pos-wave and pos-spixfer link no
# pocketlog and print no build; pos-supervise and pos-hwcheck are scripts.
STAMPED="usr/bin/doors usr/bin/doors-shell usr/sbin/radiod usr/sbin/sysd usr/sbin/netd usr/sbin/meshcored"
PER_UNIT="etc/default/meshcored"

if [ "${1:-}" = "--list" ]; then
    {
        echo etc/doors-release
        echo "${SUPERVISE}"
        while IFS='|' read -r _ bin init; do echo "${bin}"; echo "${init}"; done <<< "${SERVICES}"
        for b in ${STAMPED} ${PER_UNIT}; do echo "${b}"; done
    } | sort -u
    exit 0
fi

ROOT="${1:-}"
[ -n "${ROOT}" ] && [ -d "${ROOT}" ] || { echo "usage: $(basename "$0") <root> | --list" >&2; exit 2; }

echo "Installation in ${ROOT}:"

# ---- 1. services come whole -------------------------------------------------
if [ -f "${ROOT}/${SUPERVISE}" ] && [ -x "${ROOT}/${SUPERVISE}" ]; then
    ok "/${SUPERVISE} (every service runs under it)"
else
    fail "/${SUPERVISE} is missing or not executable; no init script can start its service"
fi
while IFS='|' read -r name bin init; do
    have_bin=0; have_init=0
    [ -e "${ROOT}/${bin}" ] && have_bin=1
    [ -e "${ROOT}/${init}" ] && have_init=1
    if [ "${have_bin}" = 1 ] && [ "${have_init}" = 0 ]; then
        fail "${name}: /${bin} is installed and /${init} is not - nothing would ever start it"
        continue
    fi
    if [ "${have_bin}" = 0 ] && [ "${have_init}" = 1 ]; then
        fail "${name}: /${init} is installed and /${bin} is not - the service it starts is missing"
        continue
    fi
    if [ "${have_bin}" = 0 ]; then
        fail "${name}: neither /${bin} nor /${init} is installed"
        continue
    fi
    bad=0
    [ -x "${ROOT}/${bin}" ] || { fail "${name}: /${bin} is not executable"; bad=1; }
    [ -x "${ROOT}/${init}" ] || { fail "${name}: /${init} is not executable; rcS would skip it"; bad=1; }
    daemon=$(sed -n 's/^DAEMON=\(.*\)$/\1/p' "${ROOT}/${init}" | head -1)
    if [ "${daemon}" != "/${bin}" ]; then
        fail "${name}: /${init} starts '${daemon:-nothing}', not /${bin}"
        bad=1
    fi
    [ "${bad}" = 0 ] && ok "${name}: /${bin} and /${init}, both executable"
done <<< "${SERVICES}"

# ---- 2. one build -------------------------------------------------------------
REL="${ROOT}/etc/doors-release"
release_id=""
if [ -f "${REL}" ]; then
    release_id=$(sed -n 's/^BUILD_ID=//p' "${REL}" | head -1)
fi
if [ -z "${release_id}" ]; then
    fail "/etc/doors-release is missing or names no BUILD_ID"
else
    ok "/etc/doors-release: BUILD_ID=${release_id}"
    for b in ${STAMPED}; do
        if [ ! -f "${ROOT}/${b}" ]; then
            # A service binary's absence is section 1's to report; any other
            # stamped binary is reported here, or nobody would.
            case "${SERVICES}" in *"|${b}|"*) ;; *) fail "/${b} is missing" ;; esac
            continue
        fi
        stamps=$(grep -a -o 'DOORS_BUILD_ID=[A-Za-z0-9._+-]*' "${ROOT}/${b}" 2>/dev/null \
                 | sed 's/^DOORS_BUILD_ID=//' | sort -u)
        count=$(printf '%s' "${stamps}" | grep -c .)
        if [ "${count}" -eq 0 ]; then
            fail "/${b} carries no build stamp; it cannot say which build it is"
        elif [ "${count}" -gt 1 ]; then
            fail "/${b} carries more than one build stamp: $(echo ${stamps})"
        elif [ "${stamps}" != "${release_id}" ]; then
            fail "/${b} is build ${stamps}, and /etc/doors-release says ${release_id}"
        else
            ok "/${b} is build ${stamps}"
        fi
    done
fi

# ---- 3. no per-unit settings --------------------------------------------------
for f in ${PER_UNIT}; do
    if [ -e "${ROOT}/${f}" ]; then
        fail "/${f} is in the tree; it is per-unit and decided on the unit"
    else
        ok "/${f} absent (per-unit)"
    fi
done

if [ "${failed}" -ne 0 ]; then
    echo "INSTALLATION CHECK: FAIL - ${failed} problem(s) in ${ROOT}"
    exit 1
fi
echo "INSTALLATION CHECK: PASS"
exit 0
