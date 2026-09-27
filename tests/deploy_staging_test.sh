#!/bin/bash
# What deploy.sh sends to a unit, and what it refuses to send.
#
# Unit A was found (2026-09-22) with /usr/sbin/meshcored and no
# /etc/init.d/S65meshcored, and with a release file naming a different build
# from the binaries next to it. This exercises the build-host half of
# deploy.sh against a fake SDK target tree, with ssh replaced by a stand-in
# unit that is this machine. Nothing leaves this machine: what is checked is
# the archive deploy.sh built, the refusals it made before contacting
# anything, and how it hands the work to the unit and waits for it. The
# unit-side half itself (deploy_unit.sh) is tests/initscript_test.sh.
#
# The hand-over is the second part. deploy.sh used to pipe the archive into
# the unpack over the same ssh session that stopped netd; over Wi-Fi that cut
# its own connection, and the unit waited forever with every service stopped
# (unit A, 192.168.10.171, 2026-09-25). What is checked below is the property
# that replaced it: the unit's deploy runs to its end without the connection
# that started it - through polls that fail, and with the host gone entirely.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)
. tests/mkbootimg.sh
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

TMP=$(mktemp -d)
FAKE="$TMP"
export FAKE
STAGE_AT="$TMP/unit/stage"
# The detached deploy is a session of its own (setsid), so its pid is its
# process group: ending the group ends the fake unit's sleep too.
stop_unit() {
    local p
    p=$(cat "$STAGE_AT/pid" 2>/dev/null) || return 0
    [ -n "$p" ] && kill -- "-$p" 2>/dev/null
    return 0
}
trap 'stop_unit; rm -rf "$TMP"' EXIT

# ssh: the unit is this machine. The remote script runs here, with the
# staging directory moved under $FAKE/unit; never a network connection. At
# the launch the staged archive is kept for inspection and deploy_unit.sh is
# swapped for fake_unit.sh (below) after it has been checked, so nothing here
# stops or unpacks anything on this machine. Knobs, all files under $FAKE:
#   drop     this many polls fail as an unreachable host would (exit 255)
#   corrupt  the staged archive is altered after it arrives
mkdir -p "$TMP/bin"
cat > "$TMP/bin/ssh" <<'EOF'
#!/bin/bash
cmd=${!#}
printf '%s\n' "$cmd" >> "$FAKE/ssh.log"
case "$cmd" in
*'echo notstarted'*)
    left=$(cat "$FAKE/drop" 2>/dev/null || echo 0)
    if [ "$left" -gt 0 ]; then
        echo $((left - 1)) > "$FAKE/drop"
        echo "ssh: connect to host 192.0.2.1 port 22: Connection timed out" >&2
        exit 255
    fi ;;
esac
cmd=$(printf '%s\n' "$cmd" | sed "s#^STAGE=/tmp/doors-deploy\$#STAGE=$FAKE/unit/stage#")
if [ -e "$FAKE/corrupt" ]; then
    cmd=$(printf '%s\n' "$cmd" | sed 's#^tar -xf -$#tar -xf -; echo x >> payload.tar#')
fi
case "$cmd" in
*'setsid nohup'*)
    cp "$FAKE/unit/stage/payload.tar" "$FAKE/archive.tar"
    cp "$FAKE/unit/stage/deploy_unit.sh" "$FAKE/staged_unit.sh"
    cp "$FAKE/fake_unit.sh" "$FAKE/unit/stage/deploy_unit.sh"
    sh -c "$cmd"; rc=$?
    [ -e "$FAKE/unit/stage/status" ] || echo returned-before-the-unit-finished >> "$FAKE/launch.log"
    exit $rc ;;
esac
exec sh -c "$cmd"
EOF
chmod 0755 "$TMP/bin/ssh"
# What runs where deploy_unit.sh would: says what it was given, takes
# $FAKE/unit-sleep seconds, exits $FAKE/unit-rc.
cat > "$TMP/fake_unit.sh" <<'EOF'
#!/bin/sh
echo "fake unit: stopping, then unpacking $1"
sleep "$(cat "$FAKE/unit-sleep" 2>/dev/null || echo 0)"
echo "fake unit: every service started"
exit "$(cat "$FAKE/unit-rc" 2>/dev/null || echo 0)"
EOF
# BusyBox has setsid (docs/BUILD_ENVIRONMENT.md); a build host without one
# gets a stand-in that only runs the command, which the kill in stop_unit then
# misses - so say so rather than pretend.
if ! command -v setsid >/dev/null 2>&1; then
    echo "note: no setsid on this host; the detached start is run without a new session"
    printf '#!/bin/sh\nexec "$@"\n' > "$TMP/bin/setsid"; chmod 0755 "$TMP/bin/setsid"
fi

# A Buildroot target tree holding one complete installation of build abc1234,
# plus everything else deploy.sh carries.
make_tree() { # <vendor dir>
    local t="$1/k230_linux_sdk/output/k230_pocketos_defconfig/target"
    mkdir -p "$t/usr/share/doors" "$t/usr/share/pocketos" "$t/etc"
    mkbootimg_rootfs_doors "$t" abc1234
    rm -rf "$t/etc/default"
    for f in usr/bin/pos-hwcheck usr/bin/pos-spixfer usr/bin/pos-wave usr/bin/pos-camera usr/bin/pos-zabbix \
             usr/bin/pos-browser usr/bin/pos-record usr/bin/pos-drmtest usr/bin/pos-display-boot; do
        printf '#!/bin/sh\n' > "$t/$f"; chmod 0755 "$t/$f"
    done
    ln -sfn doors "$t/usr/bin/pos"
    ln -sfn doors-release "$t/etc/pocketos-release"
    printf 'notices\n' > "$t/usr/share/doors/THIRD_PARTY_NOTICES.txt"
    ln -sfn ../doors/THIRD_PARTY_NOTICES.txt "$t/usr/share/pocketos/THIRD_PARTY_NOTICES.txt"
    # The shell's art directory (DS §31.6), sent whole.
    mkdir -p "$t/usr/share/doors/ui"
    printf 'art\n' > "$t/usr/share/doors/ui/bg-home-portrait.bin"
    # The overlay apply_to_sdk.sh applied, which a finalised tree matches.
    local o="$1/k230_linux_sdk/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d"
    mkdir -p "$o"
    cp -p "$t"/etc/init.d/S* "$o/"
}
fresh_unit() { # a unit that has never been deployed to, and no knobs set
    stop_unit
    rm -rf "$TMP/unit" "$TMP/archive.tar" "$TMP/staged_unit.sh" "$TMP/ssh.log" "$TMP/launch.log" \
           "$TMP/drop" "$TMP/corrupt" "$TMP/unit-sleep" "$TMP/unit-rc"
    mkdir -p "$TMP/unit"
}
deploy() { # <vendor dir>: runs deploy.sh against the unit as it stands, prints its exit code
    rm -f "$TMP/archive.tar" "$TMP/ssh.log"
    PATH="$TMP/bin:$PATH" DEPLOY_POLL_INTERVAL="${POLL_INTERVAL:-0.2}" \
        bash platforms/k230/scripts/deploy.sh 192.0.2.1 "$1" > "$TMP/out.txt" 2>&1
    echo $?
}
fresh_unit

# ---- a complete tree is sent whole -------------------------------------------
V="$TMP/complete"; make_tree "$V"
rc=$(deploy "$V")
check "a complete tree is deployed" "$([ "$rc" = 0 ] && [ -s "$TMP/archive.tar" ] && echo 1 || echo 0)"
check "after the installation check passed" "$(grep -q 'INSTALLATION CHECK: PASS' "$TMP/out.txt" && echo 1 || echo 0)"
tar -tvf "$TMP/archive.tar" > "$TMP/list.txt" 2>/dev/null
for f in usr/sbin/meshcored etc/init.d/S65meshcored usr/sbin/radiod etc/init.d/S60radiod \
         usr/bin/doors-shell etc/init.d/S90doors-shell usr/bin/pos-supervise etc/doors-release \
         usr/share/doors/ui/bg-home-portrait.bin; do
    check "the archive carries $f" "$(grep -q " $f\$" "$TMP/list.txt" && echo 1 || echo 0)"
done
check "meshcored travels executable" \
    "$(grep " usr/sbin/meshcored\$" "$TMP/list.txt" | grep -q '^-rwxr-xr-x' && echo 1 || echo 0)"
check "and so does its init script" \
    "$(grep " etc/init.d/S65meshcored\$" "$TMP/list.txt" | grep -q '^-rwxr-xr-x' && echo 1 || echo 0)"
check "every file travels owned by root" \
    "$(awk '{print $2}' "$TMP/list.txt" | grep -qv '^0/0$' && echo 0 || echo 1)"
check "no per-unit settings file travels" \
    "$(grep -q ' etc/default/' "$TMP/list.txt" && echo 0 || echo 1)"

# ---- the hand-over: the unit's deploy does not need this connection ----------
check "the unit side is deploy_unit.sh as committed, checked on the unit" \
    "$(cmp -s "$TMP/staged_unit.sh" platforms/k230/scripts/deploy_unit.sh &&
       grep -q 'sha256sum -c SHA256SUMS' "$TMP/ssh.log" && echo 1 || echo 0)"
check "it is started detached from the ssh session" \
    "$(grep -q '^setsid nohup sh ./run.sh < /dev/null > /dev/null 2>&1 &' "$TMP/ssh.log" && echo 1 || echo 0)"
check "the unit's output reaches the host" \
    "$(grep -q 'fake unit: every service started' "$TMP/out.txt" && grep -q '^Done\.$' "$TMP/out.txt" && echo 1 || echo 0)"
check "the unit keeps the result and log, and drops the archive" \
    "$(grep -qx 0 "$STAGE_AT/status" && [ -s "$STAGE_AT/log" ] && [ ! -e "$STAGE_AT/payload.tar" ] && echo 1 || echo 0)"

# The start returns while the unit is still working: the session that started
# it is not what keeps it running.
fresh_unit; echo 1 > "$TMP/unit-sleep"
rc=$(deploy "$V")
check "the start command returns before the unit has finished" \
    "$(grep -q returned-before-the-unit-finished "$TMP/launch.log" 2>/dev/null && echo 1 || echo 0)"
check "and the host waits for the unit's result" "$([ "$rc" = 0 ] && echo 1 || echo 0)"

# 2026-09-25, as it would be now: the unit stops answering once netd is down
# (polls fail like an unreachable host) and answers again after netd started.
fresh_unit; echo 1 > "$TMP/unit-sleep"; echo 4 > "$TMP/drop"
rc=$(deploy "$V")
check "WI-FI: polls that cannot connect do not end the deploy" "$([ "$rc" = 0 ] && echo 1 || echo 0)"
check "WI-FI: the host says the unit went quiet, and why that is expected" \
    "$(grep -q 'does not answer' "$TMP/out.txt" && grep -q 'Expected while netd restarts' "$TMP/out.txt" && echo 1 || echo 0)"
check "WI-FI: and that it came back, with the unit's whole log" \
    "$(grep -q 'answers again' "$TMP/out.txt" && grep -q 'fake unit: stopping' "$TMP/out.txt" &&
       grep -q 'fake unit: every service started' "$TMP/out.txt" && echo 1 || echo 0)"

# The host gone altogether - killed after the start, as a laptop that loses the
# network or is closed would be. The unit finishes by itself.
fresh_unit; echo 2 > "$TMP/unit-sleep"
PATH="$TMP/bin:$PATH" DEPLOY_POLL_INTERVAL=0.2 \
    bash platforms/k230/scripts/deploy.sh 192.0.2.1 "$V" > "$TMP/out.txt" 2>&1 &
host=$!
n=0; while ! grep -q 'setsid nohup' "$TMP/ssh.log" 2>/dev/null && [ $n -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
kill -9 "$host" 2>/dev/null; wait "$host" 2>/dev/null
check "HOST GONE: the host was killed while the unit was still working" \
    "$([ ! -e "$STAGE_AT/status" ] && echo 1 || echo 0)"
n=0; while [ ! -e "$STAGE_AT/status" ] && [ $n -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
check "HOST GONE: the unit still finishes, and records success" \
    "$(grep -qx 0 "$STAGE_AT/status" 2>/dev/null && grep -q 'every service started' "$STAGE_AT/log" && echo 1 || echo 0)"

# A unit that fails says so, with its log, and the host fails.
fresh_unit; echo 1 > "$TMP/unit-rc"
rc=$(deploy "$V")
check "a failure on the unit fails the deploy" \
    "$([ "$rc" = 1 ] && grep -q 'the deploy on the unit failed (exit 1' "$TMP/out.txt" &&
       grep -q 'fake unit: every service started' "$TMP/out.txt" && echo 1 || echo 0)"

# An archive that does not arrive intact is never started.
fresh_unit; : > "$TMP/corrupt"
rc=$(deploy "$V")
check "CORRUPT: a staged archive that does not match is refused" \
    "$([ "$rc" = 1 ] && grep -q 'nothing has been stopped' "$TMP/out.txt" && echo 1 || echo 0)"
check "CORRUPT: and the unit side is never started" \
    "$(grep -q 'setsid' "$TMP/ssh.log" && echo 0 || echo 1)"

# No result in time: the host stops waiting, says the outcome is unknown and
# where to look - and a second deploy meanwhile is refused, not stacked.
fresh_unit; echo 30 > "$TMP/unit-sleep"
rc=$(DEPLOY_TIMEOUT=1 deploy "$V")
check "TIMEOUT: the host gives up with exit 2" "$([ "$rc" = 2 ] && echo 1 || echo 0)"
check "TIMEOUT: and says the unit carries on, and where its log is" \
    "$(grep -q 'is not stopped by this' "$TMP/out.txt" && grep -q "/tmp/doors-deploy/log" "$TMP/out.txt" && echo 1 || echo 0)"
rc=$(deploy "$V")
check "BUSY: a deploy while one is still running on the unit is refused" \
    "$([ "$rc" = 1 ] && grep -q 'another deploy is still running' "$TMP/out.txt" && echo 1 || echo 0)"
check "BUSY: and leaves the running one alone" \
    "$([ -e "$STAGE_AT/pid" ] && kill -0 "$(cat "$STAGE_AT/pid")" 2>/dev/null && echo 1 || echo 0)"
fresh_unit

# ---- unit A's state, in the tree: refused before the unit is touched -----------
refused() { # <label> <vendor dir> <message>
    rc=$(deploy "$2")
    check "REFUSED: $1" "$([ "$rc" != 0 ] && echo 1 || echo 0)"
    check "REFUSED: $1 - before anything was sent" "$([ ! -e "$TMP/ssh.log" ] && echo 1 || echo 0)"
    check "REFUSED: $1 - and it says why" "$(grep -qF -- "$3" "$TMP/out.txt" && echo 1 || echo 0)"
}
# These three are refused by deploy.sh's own list of what it carries, before
# the installation check runs; they say so, rather than claim the checker.
T_OF() { echo "$1/k230_linux_sdk/output/k230_pocketos_defconfig/target"; }
V="$TMP/noinit"; make_tree "$V"; rm -f "$(T_OF "$V")/etc/init.d/S65meshcored"
refused "a tree with meshcored and no S65meshcored (deploy.sh's file list)" "$V" \
    "missing $(T_OF "$V")/etc/init.d/S65meshcored"
V="$TMP/nobin"; make_tree "$V"; rm -f "$(T_OF "$V")/usr/sbin/meshcored"
refused "a tree with S65meshcored and no meshcored (deploy.sh's file list)" "$V" \
    "missing $(T_OF "$V")/usr/sbin/meshcored"
V="$TMP/stale"; make_tree "$V"
mkbootimg_stamped "$V/k230_linux_sdk/output/k230_pocketos_defconfig/target/usr/sbin/meshcored" 3e89c9c
refused "a tree holding a meshcored from another build" "$V" \
    "/usr/sbin/meshcored is build 3e89c9c, and /etc/doors-release says abc1234"
V="$TMP/mode"; make_tree "$V"; chmod 0644 "$V/k230_linux_sdk/output/k230_pocketos_defconfig/target/etc/init.d/S65meshcored"
refused "a tree whose S65meshcored is not executable" "$V" "/etc/init.d/S65meshcored is not executable"
V="$TMP/nosup"; make_tree "$V"; rm -f "$(T_OF "$V")/usr/bin/pos-supervise"
refused "a tree with no pos-supervise (deploy.sh's file list)" "$V" \
    "missing $(T_OF "$V")/usr/bin/pos-supervise"
# A package-only rebuild does not refresh the target tree's init scripts, which
# carry no build stamp: compared with the applied overlay instead.
V="$TMP/stale-init"; make_tree "$V"
printf '# the version apply_to_sdk.sh applied since\n' \
    >> "$V/k230_linux_sdk/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S65meshcored"
refused "a tree whose S65meshcored is older than the applied overlay" "$V" \
    "has not been finalised since apply_to_sdk.sh"
V="$TMP/default"; make_tree "$V"; mkdir -p "$V/k230_linux_sdk/output/k230_pocketos_defconfig/target/etc/default"
printf 'MESHCORED_ENABLE=1\n' > "$V/k230_linux_sdk/output/k230_pocketos_defconfig/target/etc/default/meshcored"
refused "a tree carrying a per-unit meshcored switch" "$V" "/etc/default/meshcored is in the tree"

# ---- the manifest cannot drift from the check ----------------------------------
# Every service file deploy.sh requires is one the installation check knows,
# and the other way round, so neither can gain a service the other forgets.
SVC_RE='usr/s?bin/(sysd|netd|radiod|meshcored|doors-shell|pos-supervise)|etc/init\.d/S[0-9]+[a-z-]+'
required=$(sed -n '/^for f in usr\/bin\/doors/,/; do$/p' platforms/k230/scripts/deploy.sh \
           | grep -oE "$SVC_RE" | sort -u)
known=$(sed -n '/^SERVICES="/,/"$/p; /^SUPERVISE=/p' tools/release/check_rootfs.sh \
        | grep -oE "$SVC_RE" | sort -u)
check "deploy.sh requires exactly the services check_rootfs.sh checks" \
    "$([ -n "$required" ] && [ "$required" = "$known" ] && echo 1 || echo 0)"
[ "$required" = "$known" ] || diff <(echo "$required") <(echo "$known") | head
# And the list the unit checks after unpacking, which cannot run the checker.
on_unit=$(sed -n '/^for f in \/usr\/sbin\/sysd/,/; do$/p' platforms/k230/scripts/deploy_unit.sh \
          | grep -oE "$SVC_RE" | sort -u)
check "the unit's own completeness check names the same services" \
    "$([ -n "$on_unit" ] && [ "$on_unit" = "$known" ] && echo 1 || echo 0)"
[ "$on_unit" = "$known" ] || diff <(echo "$on_unit") <(echo "$known") | head

echo "deploy_staging_test: $failed failure(s)"
exit $((failed > 0))
