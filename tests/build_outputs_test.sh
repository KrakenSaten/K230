#!/bin/bash
# Running the tests must not dirty the checkout they ran in.
#
# apply_to_sdk.sh stamps "<commit>-dirty" into an image's BUILD_ID whenever
# `git status --porcelain` is not empty, and an untracked file counts. Every
# binary `make all` and `make test` produce therefore has to be git-ignored,
# or a release checkout that ran its tests before building ships an image
# claiming uncommitted changes it does not contain (P1-6 of the v0.0.8
# review: six PocketNotes and PocketClock test binaries were not).
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
