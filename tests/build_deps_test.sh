#!/bin/bash
# Header dependency tracking (PocketFleet finding 1): after a header changes,
# every object that includes it must be rebuilt by plain `make`.
set -u
cd "$(dirname "$0")/.." || exit 1
CC=${CC:-gcc}
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

make CC="$CC" tools/pos/pos_radio.o core/pocketipc/pocketipc.o >/dev/null 2>&1
check "dependency file generated for pos_radio.o" $([ -f tools/pos/pos_radio.d ] && echo 1 || echo 0)
check "pos_radio.d names pocketipc.h" $(grep -q 'core/pocketipc/pocketipc.h' tools/pos/pos_radio.d && echo 1 || echo 0)

sleep 1  # make compares mtimes at one-second granularity on some filesystems
touch core/pocketipc/pocketipc.h
out=$(make -n CC="$CC" tools/pos/pos_radio.o core/pocketipc/pocketipc.o 2>&1)
check "header touch rebuilds pos_radio.o" $(printf '%s' "$out" | grep -q 'tools/pos/pos_radio.c' && echo 1 || echo 0)
check "header touch rebuilds pocketipc.o" $(printf '%s' "$out" | grep -q 'core/pocketipc/pocketipc.c' && echo 1 || echo 0)

make CC="$CC" tools/pos/pos_radio.o core/pocketipc/pocketipc.o >/dev/null 2>&1
out=$(make -n CC="$CC" tools/pos/pos_radio.o 2>&1)
check "no rebuild when nothing changed" $(printf '%s' "$out" | grep -q 'tools/pos/pos_radio.c' && echo 0 || echo 1)

# The build identity (cold review F12). It reaches the compiler as a -D, which
# make cannot see, so an object built at one commit used to keep that commit's
# identity until its source changed: `make` after a commit produced binaries
# reporting the build before it, and tests/radiod_mock_test.sh failed comparing
# radiod's reported build to the checkout's. What is checked here is the
# behaviour - change the identity, rebuild nothing else, and the value inside
# the object must follow.
ID_SAVED=""
[ -f BUILD_ID ] && ID_SAVED=$(cat BUILD_ID)
embedded() { grep -a -q "$1" core/pocketlog/pocketlog.o 2>/dev/null && echo 1 || echo 0; }

echo "revision-A" > BUILD_ID
make CC="$CC" core/pocketlog/pocketlog.o core/pocketpaths.o >/dev/null 2>&1
check "the identity is compiled in" "$(embedded revision-A)"

sleep 1
echo "revision-B" > BUILD_ID
make CC="$CC" core/pocketlog/pocketlog.o >/dev/null 2>&1
check "a changed identity rebuilds the object that embeds it" "$(embedded revision-B)"
check "and the old identity is gone from it" \
    $([ "$(embedded revision-A)" = "0" ] && echo 1 || echo 0)

# Only those objects: the stamp must not turn every build into a full one.
out=$(make -n CC="$CC" core/pocketpaths.o 2>&1)
check "an object without the identity is left alone" \
    $(printf '%s' "$out" | grep -q 'core/pocketpaths.c' && echo 0 || echo 1)

# Every source that names the macros has to be in POCKETOS_ID_OBJS, or the
# next one added silently goes stale the way pocketlog.o did.
sleep 1
echo "revision-C" > BUILD_ID
out=$(make -n CC="$CC" all tests/pocketlog_test 2>&1)
missing=""
for src in $(grep -rl 'POCKETOS_BUILD_ID\|POCKETOS_VERSION' --include='*.c' \
             apps core services tools tests 2>/dev/null); do
    printf '%s' "$out" | grep -q -- "$src" || missing="$missing $src"
done
check "every source that embeds the identity is rebuilt with it" \
    $([ -z "$missing" ] && echo 1 || echo 0)
[ -n "$missing" ] && echo "  not rebuilt:$missing"

# Leave the checkout building the identity it really has.
if [ -n "$ID_SAVED" ]; then printf '%s\n' "$ID_SAVED" > BUILD_ID; else rm -f BUILD_ID; fi
make CC="$CC" core/pocketlog/pocketlog.o >/dev/null 2>&1
check "restoring the identity rebuilds it once more" \
    $([ "$(embedded revision-C)" = "0" ] && echo 1 || echo 0)

echo "build_deps_test: $failed failure(s)"
exit $((failed > 0))
