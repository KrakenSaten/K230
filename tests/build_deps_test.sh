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
echo "build_deps_test: $failed failure(s)"
exit $((failed > 0))
