#!/bin/bash
# Running the tests must not dirty the checkout they ran in.
#
# apply_to_sdk.sh treats a non-empty `git status --porcelain` as a dirty tree,
# and an untracked file counts. Since the dirty-worktree guard it no longer
# merely stamps "<commit>-dirty" into BUILD_ID and carries on: it refuses the
# build unless POCKETOS_ALLOW_DIRTY_BUILD=1 says otherwise. Every binary
# `make all` and `make test` produce therefore has to be git-ignored, or a
# release checkout that ran its tests before building now fails to build at
# all - where before it shipped an image claiming uncommitted changes it did
# not contain (P1-6 of the v0.0.8 review: six PocketNotes and PocketClock test
# binaries were not ignored).
#
# That makes this test more load-bearing than it was, not less: it is what
# keeps the default path working for anyone who has not read the guard. The
# guard's own behaviour is covered by tests/package_sync_test.sh.
#
# The list comes from the Makefile itself (print-build-outputs), so a binary
# added there without a .gitignore entry fails here.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "skip: not a git checkout, or its .git is not readable from here"
    echo "build_outputs_test: 0 failure(s)"
    exit 0
fi

# A sub-make of its own: the parent's MAKEFLAGS would bring -j and a
# jobserver this one cannot use.
outputs=$(MAKEFLAGS= MAKELEVEL= make -s --no-print-directory print-build-outputs 2>/dev/null)
count=$(printf '%s\n' "$outputs" | grep -c .)
check "the Makefile names its build outputs ($count)" "$([ "$count" -ge 40 ] && echo 1 || echo 0)"

missing=""
for f in $outputs; do
    git check-ignore -q -- "$f" || missing="$missing $f"
done
check "every one of them is git-ignored" "$([ -z "$missing" ] && echo 1 || echo 0)"
[ -n "$missing" ] && echo "  not ignored:$missing"

echo "build_outputs_test: $failed failure(s)"
exit $((failed > 0))
