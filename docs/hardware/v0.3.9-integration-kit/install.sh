#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# Install the integration/v0.3.9 doors-shell and pos-vision (a protocol-3
# pair) on this unit, with a rollback: ./RESTORE.sh in this directory.
# Only the shell service is restarted; radiod, meshcored, netd and sysd keep
# running. No library, model, setting or radio state is written.
set -u
K=$(cd "$(dirname "$0")" && pwd)
cd "$K" || exit 1
fail() { echo "INSTALL: $*; nothing installed" >&2; exit 1; }
FILES="/usr/bin/doors-shell /usr/bin/pos-vision"

echo "== kit"
sha256sum -c SHA256SUMS || fail "kit files do not match SHA256SUMS"
[ -e backup/installed.sha256 ] && fail "this kit was already used (backup/ exists); run ./RESTORE.sh first"

echo "== another session"
[ -z "$(pidof pos-vision pos-camera pos-record)" ] || fail "a camera/audio helper is running: $(pidof pos-vision pos-camera pos-record)"
for p in $(pidof doors-shell pocketos-shell); do
	[ "$(readlink /proc/$p/exe)" = /usr/bin/doors-shell ] || fail "a shell not from /usr/bin runs: $p $(readlink /proc/$p/exe)"
done
ls -d /root/*/test-shell.pid 2>/dev/null && fail "a test-kit shell pid file exists"

echo "== storage"
need=0
for f in $FILES; do need=$((need + $(wc -c < "$f") / 1024 + 1)); done
free=$(df -k /usr/bin | awk 'NR==2 {print $4}')
echo "backup needs ${need} KiB plus new files; free ${free} KiB on /usr/bin's filesystem"
[ "$free" -gt $((need * 3 + 4096)) ] || fail "not enough free space"

echo "== baseline"
mkdir -p backup log
for f in $FILES /usr/lib/liblvgl.so.9.5.0; do [ -e "$f" ] || echo "$f" >> backup/absent.txt; done
touch backup/absent.txt
sha256sum $FILES /usr/lib/liblvgl.so.9.5.0 /usr/share/doors/vision/*.kmodel > backup/installed.sha256 2>/dev/null
for f in /etc/pocketos/settings.conf /var/lib/pocketos/vision/settings.v1 /var/lib/pocketos/vision/owner.v1 \
         /var/lib/pocketos/deskbuddy/prefs.v1 /var/lib/pocketos/deskbuddy/guard.v1; do
	if [ -e "$f" ]; then echo "present $(sha256sum "$f")"; else echo "absent $f"; fi
done > backup/preserved.txt
find /etc/pocketos /etc/default /var/lib/pocketos -type f | sort | xargs sha256sum > backup/state-before.txt 2>/dev/null
echo "$(pidof meshcored) $(pidof radiod) $(pidof netd) $(pidof sysd)" > backup/services-before.txt
cat /etc/doors-release > backup/release.txt
for f in $FILES; do [ -e "$f" ] && cp -a "$f" "backup/$(basename "$f")"; done
(cd backup && for f in $FILES; do [ -e "$(basename $f)" ] && sha256sum "$(basename $f)"; done) > backup/copies.sha256
sync
grep -q doors-shell backup/copies.sha256 || fail "the installed shell could not be backed up"
cat backup/installed.sha256 backup/preserved.txt backup/services-before.txt

echo "== stop the shell"
/etc/init.d/S90doors-shell stop || fail "S90doors-shell stop failed"
sleep 1
[ -z "$(pidof doors-shell pocketos-shell)" ] || { /etc/init.d/S90doors-shell start; fail "a shell is still running"; }
[ -z "$(pidof pos-vision)" ] || { /etc/init.d/S90doors-shell start; fail "pos-vision still running"; }

echo "== install"
for b in doors-shell pos-vision; do
	cp "$K/$b" "/usr/bin/.$b.new" && chmod 0755 "/usr/bin/.$b.new" && mv "/usr/bin/.$b.new" "/usr/bin/$b" ||
		{ echo "INSTALL: copying $b failed; restoring" >&2; "$K/RESTORE.sh"; exit 1; }
done
sync
(cd "$K" && sha256sum doors-shell pos-vision | sed 's#  #  /usr/bin/#') | sha256sum -c - || { "$K/RESTORE.sh"; exit 1; }

echo "== start the shell"
/etc/init.d/S90doors-shell start
sleep 5
n=0; for p in $(pidof doors-shell); do n=$((n+1)); done
ps | grep -q '[p]os-supervise doors-shell' && sup=1 || sup=0
[ "$n" -eq 1 ] && [ "$sup" -eq 1 ] || { echo "INSTALL: shell did not come up supervised (n=$n sup=$sup); restoring" >&2; "$K/RESTORE.sh"; exit 1; }
[ "$(pidof meshcored) $(pidof radiod) $(pidof netd) $(pidof sysd)" = "$(cat backup/services-before.txt)" ] &&
	echo "other services: the same processes" || echo "INSTALL: note - another service's pid changed"
echo "INSTALL: OK - doors-shell $(pidof doors-shell) supervised; rollback: $K/RESTORE.sh"
