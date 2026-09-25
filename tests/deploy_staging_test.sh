#!/bin/bash
# What deploy.sh sends to a unit, and what it refuses to send.
#
# Unit A was found (2026-09-22) with /usr/sbin/meshcored and no
# /etc/init.d/S65meshcored, and with a release file naming a different build
# from the binaries next to it. This exercises the build-host half of
# deploy.sh against a fake SDK target tree, with ssh replaced by a stand-in
# that keeps the archive instead of sending it. Nothing leaves this machine:
# what is checked is the archive deploy.sh built and the refusals it made
# before contacting anything. The unit-side half is tests/initscript_test.sh.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)
. tests/mkbootimg.sh
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ssh: record the command line, keep stdin (the archive), succeed. Never a
# network connection.
mkdir -p "$TMP/bin"
cat > "$TMP/bin/ssh" <<EOF
#!/bin/sh
echo "\$*" > "$TMP/ssh.args"
cat > "$TMP/archive.tar"
exit 0
EOF
chmod 0755 "$TMP/bin/ssh"

# A Buildroot target tree holding one complete installation of build abc1234,
# plus everything else deploy.sh carries.
make_tree() { # <vendor dir>
    local t="$1/k230_linux_sdk/output/k230_pocketos_defconfig/target"
    mkdir -p "$t/usr/share/doors" "$t/usr/share/pocketos" "$t/etc"
    mkbootimg_rootfs_doors "$t" abc1234
    rm -rf "$t/etc/default"
    for f in usr/bin/pos-hwcheck usr/bin/pos-spixfer usr/bin/pos-wave usr/bin/pos-camera; do
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
deploy() { # <vendor dir>: runs deploy.sh, prints its exit code
    rm -f "$TMP/archive.tar" "$TMP/ssh.args"
    PATH="$TMP/bin:$PATH" bash platforms/k230/scripts/deploy.sh 192.0.2.1 "$1" \
        > "$TMP/out.txt" 2>&1
    echo $?
}

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

# ---- unit A's state, in the tree: refused before the unit is touched -----------
refused() { # <label> <vendor dir> <message>
    rc=$(deploy "$2")
    check "REFUSED: $1" "$([ "$rc" != 0 ] && echo 1 || echo 0)"
    check "REFUSED: $1 - before anything was sent" "$([ ! -e "$TMP/ssh.args" ] && echo 1 || echo 0)"
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
on_unit=$(sed -n '/^for f in \/usr\/sbin\/sysd/,/; do$/p' platforms/k230/scripts/deploy.sh \
          | grep -oE "$SVC_RE" | sort -u)
check "the unit's own completeness check names the same services" \
    "$([ -n "$on_unit" ] && [ "$on_unit" = "$known" ] && echo 1 || echo 0)"
[ "$on_unit" = "$known" ] || diff <(echo "$on_unit") <(echo "$known") | head

echo "deploy_staging_test: $failed failure(s)"
exit $((failed > 0))
