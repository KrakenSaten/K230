#!/bin/bash
# The release gates must not report success when they did not run.
#
# tests/package_sync_test.sh and tests/build_outputs_test.sh are about the
# repository itself - what `git archive` exports into the image, and whether
# every build output is ignored - so neither can do anything without a git
# checkout it can read. Both used to print "0 failure(s)" and exit 0 when git
# could not answer, which is indistinguishable from passing.
#
# That is not hypothetical. Round 1 of this hardening work was verified from a
# Windows git worktree under WSL, where .git is a file holding a path WSL
# cannot resolve. package_sync_test.sh skipped in every one of those runs and
# reported success, and a change that had actually broken three of its checks
# went to master (5b6bd80 fixed it). The suite was green the whole time.
#
# So this suite checks the gates' own behaviour: given a git that cannot
# answer, they must not claim to have passed.
#
# The unavailable git is simulated with a git on PATH that always fails, which
# is exactly what the WSL case looks like from inside the script - the binary
# is there and returns non-zero. Nothing here needs a broken checkout.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

GATES="tests/package_sync_test.sh tests/build_outputs_test.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/bin"
printf '#!/bin/sh\nexit 1\n' > "$TMP/bin/git"
chmod 0755 "$TMP/bin/git"

# Prove the stand-in really does make git unusable, so a gate that passes
# below cannot have passed because git still worked.
PATH="$TMP/bin:$PATH" git rev-parse --is-inside-work-tree >/dev/null 2>&1
check "the stand-in git fails the way an unreadable checkout does" \
      "$([ $? -ne 0 ] && echo 1 || echo 0)"

for gate in $GATES; do
    name=$(basename "$gate")

    out=$(PATH="$TMP/bin:$PATH" bash "$gate" 2>&1); rc=$?

    check "$name does not exit 0 when it could not run" \
          "$([ "$rc" -ne 0 ] && echo 1 || echo 0)"
    check "$name says NOT RUN" \
          "$(printf '%s' "$out" | grep -q 'NOT RUN' && echo 1 || echo 0)"
    check "$name does not claim 0 failures" \
          "$(printf '%s' "$out" | grep -q '0 failure(s)' && echo 0 || echo 1)"
    check "$name says it is a release gate" \
          "$(printf '%s' "$out" | grep -q 'release gate' && echo 1 || echo 0)"

    # The override is allowed to continue, but it must still not pretend the
    # gate ran. An operator reading the log has to be able to tell.
    out=$(POCKETOS_ALLOW_SKIPPED_GATES=1 PATH="$TMP/bin:$PATH" bash "$gate" 2>&1); rc=$?
    check "$name with the override exits 0" \
          "$([ "$rc" -eq 0 ] && echo 1 || echo 0)"
    check "$name with the override still says NOT RUN" \
          "$(printf '%s' "$out" | grep -q 'NOT RUN' && echo 1 || echo 0)"
    check "$name with the override still does not claim 0 failures" \
          "$(printf '%s' "$out" | grep -q '0 failure(s)' && echo 0 || echo 1)"
done

# And the gates must actually run here, where git works: a suite that reported
# NOT RUN on every machine would satisfy everything above and prove nothing.
for gate in $GATES; do
    name=$(basename "$gate")
    out=$(bash "$gate" 2>&1); rc=$?
    check "$name really runs in this checkout" \
          "$([ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q '0 failure(s)' && echo 1 || echo 0)"
    check "$name is not NOT RUN here" \
          "$(printf '%s' "$out" | grep -q 'NOT RUN' && echo 0 || echo 1)"
done

echo "required_gates_test: $failed failure(s)"
exit $((failed > 0))
