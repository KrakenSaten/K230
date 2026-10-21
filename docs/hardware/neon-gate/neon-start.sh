#!/bin/sh
# PR #98 Neon hardware session, unit B: hand the panel from the installed,
# supervised doors-shell to the staged test shell in this directory.
#
# Changes nothing under /usr, /etc/init.d or /boot. It does: back up
# /etc/pocketos/settings.conf (hash-checked), stop S90doors-shell through its
# own init script, start ./doors-shell with ./lib first on its library path.
# The test shell will write settings.conf as you change theme or rotation;
# neon-restore.sh puts the backup back and restarts the installed service.
set -u
K=$(cd "$(dirname "$0")" && pwd)
. "$K/kit.env"
cd "$K" || exit 1
fail() { echo "START: $*; nothing further changed" >&2; exit 1; }

echo "== kit"
sha256sum -c SHA256SUMS || fail "kit files do not match SHA256SUMS"
[ -e "$K/test-shell.pid" ] && kill -0 "$(cat "$K/test-shell.pid")" 2>/dev/null && fail "a test shell is already running"

echo "== installed state (must be what the kit was prepared against)"
echo "$ORIG_SHELL_SHA256  /usr/bin/doors-shell" | sha256sum -c - || fail "installed doors-shell is not $ORIG_SHELL_SHA256"
echo "$ORIG_LVGL_SHA256  /usr/lib/liblvgl.so.9.5.0" | sha256sum -c - || fail "installed liblvgl is not $ORIG_LVGL_SHA256"
echo "$ORIG_SETTINGS_SHA256  /etc/pocketos/settings.conf" | sha256sum -c - || fail "settings.conf changed since the kit was prepared"

echo "== settings backup"
mkdir -p backup log
if [ -s backup/settings.conf ]; then
	echo "$ORIG_SETTINGS_SHA256  backup/settings.conf" | sha256sum -c - || fail "an existing backup/settings.conf does not match; inspect it"
else
	cp -p /etc/pocketos/settings.conf backup/settings.conf
	sync
	echo "$ORIG_SETTINGS_SHA256  backup/settings.conf" | sha256sum -c - || fail "backup copy does not match"
fi
cat backup/settings.conf

echo "== stop the installed shell"
/etc/init.d/S90doors-shell stop || fail "S90doors-shell stop failed"
sleep 1
[ -z "$(pidof doors-shell pocketos-shell)" ] || fail "a shell is still running: $(pidof doors-shell pocketos-shell)"
ps | grep '[p]os-supervise doors-shell' && fail "the doors-shell supervisor is still running"
"$K/neon-status.sh" panel-owners | grep -q . && fail "something still holds /dev/dri/card0"

echo "== start the test shell"
# The installed service's environment (S90doors-shell, no /etc/default file on
# this unit), with this kit's LVGL first and the logs kept apart.
LD_LIBRARY_PATH="$K/lib" K230_LVGL_DRM_STAGING=1 POCKETOS_LOG_DIR="$K/log" POCKETOS_LOG_STDERR=0 \
	setsid "$K/doors-shell" > "$K/log/stdio.log" 2>&1 < /dev/null &
sleep 3
# setsid may fork, so find the shell by its executable, not by $!.
P=""
for p in $(pidof doors-shell); do [ "$(readlink /proc/$p/exe)" = "$K/doors-shell" ] && P=$p; done
[ -n "$P" ] || { echo "START: the test shell exited; log follows"; tail -30 "$K/log/stdio.log"; echo "Run ./neon-restore.sh"; exit 1; }
echo "$P" > "$K/test-shell.pid"
[ "$("$K/neon-status.sh" panel-owners)" = "$P" ] || echo "WARNING: panel owners are not exactly the test shell: $("$K/neon-status.sh" panel-owners)"
"$K/neon-status.sh"
echo "START: test shell $P is on the panel. Restore with ./neon-restore.sh"
