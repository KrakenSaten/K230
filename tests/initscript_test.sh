#!/bin/bash
# PocketOS init-script tests (M2).
#
# The boot path had no automated test: S60radiod and S90pocketos-shell were
# only ever checked with `sh -n`. This exercises their control flow against a
# fake root, plus the file modes that decide whether they run at all on the
# device.
#
# How the scripts are exercised: each one is copied and its absolute paths are
# rewritten to point into a temporary root. The rewrite is mechanical and is
# itself asserted (no un-rewritten /etc, /var, /usr, /run or /dev/dri path may
# survive), so what runs is the real control flow with different path
# constants. The supervisor is the real tools/supervise/pos-supervise; only
# the daemons are stubs, and they record their environment so the tests can
# check what the init script exported.
#
# File modes: Buildroot copies platforms/k230/rootfs_overlay with `rsync -a`,
# so the mode recorded in git is the mode in the image. The other scripts are
# installed with an explicit `install -m 0755` by the Makefile, so their modes
# are hygiene rather than correctness; both are checked, and the distinction
# is stated in the output. The check reads `git ls-files --stage`, not the
# filesystem: on a DrvFs (WSL /mnt/c) checkout every file reports as
# executable, so `test -x` there would pass no matter what git records.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)
OVERLAY=platforms/k230/rootfs_overlay
S60_SRC=$OVERLAY/etc/init.d/S60radiod
S90_SRC=$OVERLAY/etc/init.d/S90pocketos-shell
failed=0

check() { # <label> <0|1>
    if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi
}
contains() { # <haystack> <needle>
    case "$1" in *"$2"*) echo 1 ;; *) echo 0 ;; esac
}
# A check for behaviour this release fixes later. It reports, and says what
# will fix it, but does not fail the suite until the fix lands and the call
# becomes a plain check().
xfail() { # <label> <0|1> <what will fix it>
    if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "XFAIL $1 -- $3"; fi
}

# ---- file modes ---------------------------------------------------------

# Prefer what git records, which is what a fresh clone and `git archive` will
# produce. Where git cannot answer (the synced Buildroot package tree has no
# .git), fall back to the filesystem, but only after proving the filesystem
# keeps modes at all: DrvFs reports every file as executable.
MODE_SOURCE=none
if git ls-files --stage -- "$S60_SRC" 2>/dev/null | grep -q .; then
    MODE_SOURCE=git
else
    # The probe must live on the filesystem holding the repository, not in
    # $TMPDIR: a WSL checkout under /mnt/c has its sources on DrvFs (every
    # file reports as executable) while /tmp is ext4, so probing /tmp would
    # answer for the wrong filesystem.
    probe=./.mode_probe.$$
    if : > "$probe" 2>/dev/null; then
        chmod 0644 "$probe"
        if [ ! -x "$probe" ]; then
            chmod 0755 "$probe"
            [ -x "$probe" ] && MODE_SOURCE=fs
        fi
        rm -f "$probe"
    fi
fi

mode_of() {
    case $MODE_SOURCE in
    git) git ls-files --stage -- "$1" 2>/dev/null | awk '{print $1}' ;;
    fs)  [ -x "$1" ] && echo 100755 || echo 100644 ;;
    esac
}

if [ "$MODE_SOURCE" = none ]; then
    echo "note: no git index and the filesystem does not keep modes; mode checks skipped"
else
    echo "note: file modes read from $MODE_SOURCE"
    # Image-critical: these modes are copied into the rootfs by Buildroot and
    # BusyBox rcS runs `$i start`, which needs the executable bit.
    for f in "$S60_SRC" "$S90_SRC"; do
        check "image-critical: $(basename "$f") recorded 100755" \
              $([ "$(mode_of "$f")" = "100755" ] && echo 1 || echo 0)
    done
    # Overlay data files must NOT be executable.
    for f in $OVERLAY/etc/default/telnet $OVERLAY/etc/pocketos/settings.conf; do
        check "image-critical: $(basename "$f") recorded 100644" \
              $([ "$(mode_of "$f")" = "100644" ] && echo 1 || echo 0)
    done
    # Hygiene: scripts meant to be run directly by a developer or operator.
    for f in tools/supervise/pos-supervise tools/hwcheck/hwcheck.sh \
             tools/design/gen_fonts.sh platforms/k230/scripts/apply_to_sdk.sh \
             platforms/k230/scripts/build_image.sh platforms/k230/scripts/deploy.sh; do
        check "hygiene: $f recorded 100755" \
              $([ "$(mode_of "$f")" = "100755" ] && echo 1 || echo 0)
    done
    nonexec=""
    for f in tests/*.sh tests/hw/*.sh; do
        [ "$(mode_of "$f")" = "100755" ] || nonexec="$nonexec $f"
    done
    check "hygiene: every tests/*.sh recorded 100755" \
          $([ -z "$nonexec" ] && echo 1 || echo 0)
    [ -n "$nonexec" ] && echo "     not executable:$nonexec"
fi

# ---- fake root ----------------------------------------------------------

ROOT=$(mktemp -d)
# Kill anything this test started before removing its root, so a failure part
# way through cannot leave a supervisor and a stub daemon behind.
cleanup() {
    pkill -f "$ROOT/usr/bin/pos-supervise" 2>/dev/null
    sleep 0.3
    pkill -9 -f "$ROOT/usr/" 2>/dev/null
    rm -rf "$ROOT"
}
trap cleanup EXIT
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/sbin" "$ROOT/var/run" "$ROOT/var/lib/pocketos/log" \
         "$ROOT/run/pocketos" "$ROOT/etc/default" "$ROOT/etc/init.d" "$ROOT/dev/dri" "$ROOT/root"
: > "$ROOT/dev/dri/card0"
export POCKETOS_RUNTIME_DIR="$ROOT/run/pocketos"

cp "$REPO/tools/supervise/pos-supervise" "$ROOT/usr/bin/pos-supervise"
chmod 0755 "$ROOT/usr/bin/pos-supervise"

# A well-behaved daemon: records the environment its init script gave it, then
# waits for SIGTERM. It deliberately does not exec: a process that execs sleep
# loses the argv the tests identify it by, and would then be counted as gone
# while it is still running.
make_daemon() { # <path> <envfile>
    cat > "$1" <<EOD
#!/bin/sh
env > "$2"
trap 'exit 0' TERM INT
while :; do sleep 0.2; done
EOD
    chmod 0755 "$1"
}
make_daemon "$ROOT/usr/sbin/radiod" "$ROOT/radiod.env"
make_daemon "$ROOT/usr/bin/pocketos-shell" "$ROOT/shell.env"

# Rewrite the absolute paths of an init script into the fake root. /dev/null is
# deliberately left alone.
# The runtime directory is rewritten first and only where a separator precedes
# it: "/var/run/pocketos-shell-supervise.pid" contains the literal string
# "/run/pocketos", so an unanchored rule would rewrite the middle of the shell
# service's pid file path and the script would then write it nowhere.
rewrite() { # <source> <destination>
    sed -e "s#\([[:space:]]\)/run/pocketos#\1$ROOT/run/pocketos#g" \
        -e "s#/usr/bin/#$ROOT/usr/bin/#g" \
        -e "s#/usr/sbin/#$ROOT/usr/sbin/#g" \
        -e "s#/var/run#$ROOT/var/run#g" \
        -e "s#/var/lib/pocketos#$ROOT/var/lib/pocketos#g" \
        -e "s#/etc/default#$ROOT/etc/default#g" \
        -e "s#/dev/dri/card0#$ROOT/dev/dri/card0#g" \
        -e "s#HOME=/root #HOME=$ROOT/root #g" \
        "$1" > "$2"
    chmod 0755 "$2"
}
rewrite "$REPO/$S60_SRC" "$ROOT/etc/init.d/S60radiod"
rewrite "$REPO/$S90_SRC" "$ROOT/etc/init.d/S90pocketos-shell"

# The rewrite must be complete: any surviving system path would make the test
# lie about what it exercised (or touch the host).
leaked=$(grep -nE '(^|[^A-Za-z0-9_/])/(etc|var|usr|run)/' "$ROOT/etc/init.d/S60radiod" \
                  "$ROOT/etc/init.d/S90pocketos-shell" | grep -v "$ROOT" | grep -v '^\s*#')
check "path rewrite left no system path behind" $([ -z "$leaked" ] && echo 1 || echo 0)
[ -n "$leaked" ] && echo "$leaked" | head -5

# A rule that matches inside an already-rewritten path produces "$ROOT/var$ROOT/run/..."
doubled=$(grep -n "$ROOT[^ ]*$ROOT" "$ROOT/etc/init.d/S60radiod" "$ROOT/etc/init.d/S90pocketos-shell")
check "path rewrite did not nest one prefix inside another" $([ -z "$doubled" ] && echo 1 || echo 0)
[ -n "$doubled" ] && echo "$doubled" | head -5

S60="$ROOT/etc/init.d/S60radiod"
S90="$ROOT/etc/init.d/S90pocketos-shell"

alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }
pidof_file() { [ -s "$1" ] && cat "$1" || echo ""; }
wait_for() { # <file> — up to 5 s
    n=0
    while [ ! -e "$1" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
    [ -e "$1" ]
}
# The supervisor carries the daemon's path in its own argv, so a plain
# pgrep -f for the daemon matches both. Count the ones that are not the
# supervisor.
count_daemons() { pgrep -af "$ROOT/usr/sbin/radiod" 2>/dev/null | grep -vc 'pos-supervise'; }
count_supervisors() { pgrep -af "$ROOT/usr/bin/pos-supervise" 2>/dev/null | wc -l; }
wait_gone() { # <pid> — up to 5 s
    n=0
    while [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
    kill -0 "${1:-}" 2>/dev/null && return 1 || return 0
}
wait_daemons() { # <expected count> — up to 5 s
    n=0
    while [ "$(count_daemons)" -ne "$1" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
    [ "$(count_daemons)" -eq "$1" ]
}

# ---- S60radiod ----------------------------------------------------------

out=$("$S60" start 2>&1)
check "S60 start reports OK" $(contains "$out" "OK")
check "S60 start names backend, region and power" $(contains "$out" "(mock, EU868, 2 dBm)")
SUPPID=$(pidof_file "$ROOT/var/run/radiod-supervise.pid")
check "S60 start writes the supervise pid file" $([ -n "$SUPPID" ] && echo 1 || echo 0)
check "S60 start leaves the supervisor running" $(alive "$SUPPID" && echo 1 || echo 0)
check "S60 start starts the daemon" $(wait_for "$ROOT/radiod.env" && echo 1 || echo 0)
check "S60 start records the daemon pid in the runtime dir" \
      $([ -s "$ROOT/run/pocketos/radiod.pid" ] && echo 1 || echo 0)
check "S60 exports the persistent log directory" \
      $(grep -q "^POCKETOS_LOG_DIR=$ROOT/var/lib/pocketos/log$" "$ROOT/radiod.env" && echo 1 || echo 0)
check "S60 exports POCKETOS_LOG_STDERR=0" \
      $(grep -q '^POCKETOS_LOG_STDERR=0$' "$ROOT/radiod.env" && echo 1 || echo 0)
check "S60 creates the stdio log" $([ -f "$ROOT/var/lib/pocketos/log/radiod.stdio.log" ] && echo 1 || echo 0)

out=$("$S60" start 2>&1)
check "S60 start is idempotent" $(contains "$out" "already running")
check "S60 does not start a second daemon" $(wait_daemons 1 && echo 1 || echo 0)

out=$("$S60" stop 2>&1)
check "S60 stop reports OK" $(contains "$out" "OK")
check "S60 stop removes the supervise pid file" \
      $([ ! -f "$ROOT/var/run/radiod-supervise.pid" ] && echo 1 || echo 0)
check "S60 stop ends the supervisor" $(wait_gone "$SUPPID" && echo 1 || echo 0)
check "S60 stop ends the daemon" $(wait_daemons 0 && echo 1 || echo 0)

out=$("$S60" stop 2>&1)
check "S60 stop when not running says so" $(contains "$out" "not running")

# The stdio log of the previous run is kept as .1 when the next one starts.
rm -f "$ROOT/radiod.env"
printf 'previous boot\n' > "$ROOT/var/lib/pocketos/log/radiod.stdio.log"
printf 'RADIOD_BACKEND=sx1262\nRADIOD_TX_POWER_DBM=7\n' > "$ROOT/etc/default/radiod"
out=$("$S60" start 2>&1)
check "S60 honours /etc/default/radiod" $(contains "$out" "(sx1262, EU868, 7 dBm)")
check "S60 rotates the previous stdio log to .1" \
      $(grep -q 'previous boot' "$ROOT/var/lib/pocketos/log/radiod.stdio.log.1" 2>/dev/null && echo 1 || echo 0)
"$S60" stop >/dev/null 2>&1
wait_daemons 0
rm -f "$ROOT/etc/default/radiod" "$ROOT/radiod.env"

out=$("$S60" restart 2>&1)
check "S60 restart starts the service" $(wait_for "$ROOT/radiod.env" && echo 1 || echo 0)
RACE="restart does not wait for the outgoing supervisor (M2 bounded stop)"
xfail "S60 restart leaves exactly one daemon" $(wait_daemons 1 && echo 1 || echo 0) "$RACE"
xfail "S60 restart leaves exactly one supervisor" \
      $([ "$(count_supervisors)" -eq 1 ] && echo 1 || echo 0) "$RACE"
"$S60" stop >/dev/null 2>&1
wait_daemons 0

# ---- S90pocketos-shell --------------------------------------------------

out=$("$S90" start 2>&1)
check "S90 is disabled by default" $(contains "$out" "disabled")
check "S90 disabled starts nothing" $([ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)

printf 'ENABLE=1\n' > "$ROOT/etc/default/pocketos-shell"
printf 'ENABLE=1\n' > "$ROOT/etc/default/k230_phone_ui"
out=$("$S90" start 2>&1)
check "S90 refuses while the vendor launcher is enabled" $(contains "$out" "owns the panel")
check "S90 refusal names the launcher switch" $(contains "$out" "/etc/default/k230_phone_ui")
check "S90 refusal starts nothing" $([ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)

printf 'ENABLE=0\n' > "$ROOT/etc/default/k230_phone_ui"
sleep 600 &
LAUNCHER=$!
echo "$LAUNCHER" > "$ROOT/var/run/k230_phone_ui.pid"
out=$("$S90" start 2>&1)
check "S90 refuses while the vendor launcher is running" $(contains "$out" "is running")
check "S90 running-launcher refusal starts nothing" $([ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)
kill "$LAUNCHER" 2>/dev/null
rm -f "$ROOT/var/run/k230_phone_ui.pid"

printf 'ENABLE=1\nPOCKETOS_DRM_ROTATION=180\n' > "$ROOT/etc/default/pocketos-shell"
out=$("$S90" start 2>&1)
check "S90 starts when it owns the panel" $(contains "$out" "OK")
check "S90 starts the shell" $(wait_for "$ROOT/shell.env" && echo 1 || echo 0)
SHPID=$(pidof_file "$ROOT/var/run/pocketos-shell-supervise.pid")
check "S90 start writes the supervise pid file" $([ -n "$SHPID" ] && echo 1 || echo 0)
check "S90 start leaves the supervisor running" $(alive "$SHPID" && echo 1 || echo 0)
check "S90 start records the daemon pid in the runtime dir" \
      $([ -s "$ROOT/run/pocketos/pocketos-shell.pid" ] && echo 1 || echo 0)
check "S90 exports the persistent log directory" \
      $(grep -q "^POCKETOS_LOG_DIR=$ROOT/var/lib/pocketos/log$" "$ROOT/shell.env" && echo 1 || echo 0)
check "S90 exports the vendor staging default" \
      $(grep -q '^K230_LVGL_DRM_STAGING=1$' "$ROOT/shell.env" && echo 1 || echo 0)
check "S90 exports a bench override that was set" \
      $(grep -q '^POCKETOS_DRM_ROTATION=180$' "$ROOT/shell.env" && echo 1 || echo 0)
check "S90 does not export a bench override that was not set" \
      $(grep -q '^POCKETOS_TOUCH_CALIB=' "$ROOT/shell.env" && echo 0 || echo 1)

out=$("$S90" stop 2>&1)
check "S90 stop reports OK" $(contains "$out" "OK")
check "S90 stop removes the supervise pid file" \
      $([ ! -f "$ROOT/var/run/pocketos-shell-supervise.pid" ] && echo 1 || echo 0)
check "S90 stop ends the supervisor" $(wait_gone "$SHPID" && echo 1 || echo 0)

echo "initscript_test: $failed failure(s)"
exit $((failed > 0))
