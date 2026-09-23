#!/bin/bash
# The apply manifest's source_tree_state is the Doors tree's own state.
#
# apply_to_sdk.sh decides whether the Doors checkout is dirty, then checks the
# MeshCore and Crypto checkouts against their pins. The dependency check used
# to report through the same global, TREE_STATE, so by the time the manifest
# was written source_tree_state held Crypto's answer: a dirty Doors tree built
# with POCKETOS_ALLOW_DIRTY_BUILD=1 was recorded as clean, and BUILD_INFO lost
# its "(applied from a dirty tree)" note.
#
# Nothing is restated here. The functions (pin_check, doors_tree_state,
# meshcore_tree_check, write_manifest) and the lines that call them, in the
# order apply_to_sdk.sh calls them, are lifted out of the script itself and
# run against scratch git checkouts: a Doors tree and a vendor/RIFT and
# vendor/Crypto inside it, each made clean or dirty on purpose so the three
# answers cannot be mistaken for one another.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
APPLY=${APPLY:-platforms/k230/scripts/apply_to_sdk.sh}
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- the code under test, out of the script --------------------------------
fn() { sed -n "/^$1() {/,/^}/p" "$APPLY"; }
for f in pin_check doors_tree_state meshcore_tree_check write_manifest; do
    fn "$f" > "$TMP/$f.sh"
    check "apply_to_sdk.sh defines $f" "$([ -s "$TMP/$f.sh" ] && echo 1 || echo 0)"
done
grep -E '^doors_tree_state "\$\{REPO_DIR\}"$|^readonly SOURCE_TREE_STATE$' "$APPLY" > "$TMP/doors.sh"
check "the Doors state is decided once and made read-only" \
    "$([ "$(wc -l < "$TMP/doors.sh")" = 2 ] && [ "$(sed -n 2p "$TMP/doors.sh")" = "readonly SOURCE_TREE_STATE" ] && echo 1 || echo 0)"
sed -n '/^RIFT_DIR_SRC=/,/^echo "Crypto  :/p' "$APPLY" > "$TMP/deps.sh"
check "the dependency checks are found, both of them" \
    "$([ "$(grep -c '^meshcore_tree_check ' "$TMP/deps.sh")" = 2 ] && echo 1 || echo 0)"
check "and the manifest reports the Doors state from its own variable" \
    "$(grep -q '^source_tree_state=${SOURCE_TREE_STATE}$' "$APPLY" && echo 1 || echo 0)"
check "as the provenance summary does" \
    "$(grep -q 'Source worktree : ${SOURCE_TREE_STATE}' "$APPLY" && echo 1 || echo 0)"
check "and nothing in the script still writes the shared TREE_STATE" \
    "$(grep -qE '(^|[^_A-Z])TREE_(STATE|COMMIT)=' "$APPLY" && echo 0 || echo 1)"
{
    echo 'set -euo pipefail'
    cat "$TMP/pin_check.sh" "$TMP/doors_tree_state.sh" "$TMP/meshcore_tree_check.sh" "$TMP/write_manifest.sh"
    # What the manifest also names, which is not under test here.
    echo 'SNAPSHOT_COMMIT=snap REPO_COMMIT=snap BUILD_ID=snap DIRTY_OVERRIDE=no BSP_COMMIT=bsp'
    echo 'SDK_COMMIT=sdk RADIOLIB_COMMIT=rl RADIOLIB_STATE=clean GGWAVE_COMMIT=gg GGWAVE_STATE=clean CONF=conf'
    cat "$TMP/doors.sh" "$TMP/deps.sh"
    echo 'write_manifest "$OUT"'
} > "$TMP/run.sh"

# ---- scratch checkouts -----------------------------------------------------
G() { git -c user.email=t@t -c user.name=t -c init.defaultBranch=main "$@"; }
D=$TMP/doors
mkrepo() { # <dir>: a checkout with one tracked file, committed
    mkdir -p "$1" && G -C "$1" init -q && echo base > "$1/file" &&
        G -C "$1" add file && G -C "$1" commit -q -m init
}
mkrepo "$D/vendor/RIFT"
mkrepo "$D/vendor/Crypto"
mkdir -p "$D/protocols/meshcore"
G -C "$D" init -q
echo 0.0.0 > "$D/VERSION"
printf 'vendor/\n' > "$D/.gitignore"
G -C "$D/vendor/RIFT" rev-parse HEAD > "$D/protocols/meshcore/vendor_rift_commit.txt"
G -C "$D/vendor/Crypto" rev-parse HEAD > "$D/protocols/meshcore/vendor_crypto_commit.txt"
G -C "$D" add -A && G -C "$D" commit -q -m init
check "the scratch Doors tree starts clean" "$([ -z "$(git -C "$D" status --porcelain)" ] && echo 1 || echo 0)"

apply() { # [VAR=value ...]: run the lifted code; the manifest lands in $TMP/manifest
    rm -f "$TMP/manifest"
    env "$@" REPO_DIR="$D" OUT="$TMP/manifest" bash "$TMP/run.sh" > "$TMP/run.log" 2>&1
}
mf() { sed -n "s/^$1=//p" "$TMP/manifest" 2>/dev/null; }
dirty() { echo change >> "$1/file"; }
tidy() { git -C "$1" checkout -q -- file; }

# ---- 1. Doors dirty, dependencies clean: the case that was reported wrong ---
echo change >> "$D/VERSION"
apply; rc=$?
check "a dirty Doors tree with clean dependencies applies" "$([ $rc = 0 ] && echo 1 || echo 0)"
check "and the manifest says the Doors tree was dirty" "$([ "$(mf source_tree_state)" = dirty ] && echo 1 || echo 0)"
check "while MeshCore and Crypto are clean" \
    "$([ "$(mf meshcore_state)" = clean ] && [ "$(mf crypto_state)" = clean ] && echo 1 || echo 0)"
git -C "$D" checkout -q -- VERSION

# ---- 2. the other way round -------------------------------------------------
dirty "$D/vendor/Crypto"
apply POCKETOS_ALLOW_PIN_DRIFT=1; rc=$?
check "a dirty Crypto checkout applies only with the drift override" "$([ $rc = 0 ] && echo 1 || echo 0)"
check "and the Doors tree is reported clean, not Crypto's dirty" \
    "$([ "$(mf source_tree_state)" = clean ] && [ "$(mf crypto_state)" = dirty ] && echo 1 || echo 0)"
apply; rc=$?
check "without the override a dirty Crypto checkout is still refused" \
    "$([ $rc != 0 ] && [ ! -e "$TMP/manifest" ] && grep -q 'Crypto (vendor/Crypto) checkout' "$TMP/run.log" && echo 1 || echo 0)"
tidy "$D/vendor/Crypto"

# ---- 3. the last check run is not the one reported ----------------------------
# Crypto is checked last; whatever it says must not become the Doors answer.
echo change >> "$D/VERSION"
dirty "$D/vendor/RIFT"
apply POCKETOS_ALLOW_PIN_DRIFT=1; rc=$?
check "Doors dirty, MeshCore dirty, Crypto clean: applied under the override" \
    "$([ $rc = 0 ] && echo 1 || echo 0)"
check "and each is reported as itself" \
    "$([ "$(mf source_tree_state)" = dirty ] && [ "$(mf meshcore_state)" = dirty ] &&
       [ "$(mf crypto_state)" = clean ] && echo 1 || echo 0)"
tidy "$D/vendor/RIFT"
git -C "$D" checkout -q -- VERSION

# ---- 4. dirty detection is what it was --------------------------------------
apply; rc=$?
check "all clean: clean" "$([ $rc = 0 ] && [ "$(mf source_tree_state)" = clean ] && echo 1 || echo 0)"
touch "$D/untracked"
apply
check "an untracked file still makes the Doors tree dirty" "$([ "$(mf source_tree_state)" = dirty ] && echo 1 || echo 0)"
rm -f "$D/untracked"
G -C "$D" rm -q --cached VERSION
apply
check "and so does a staged change" "$([ "$(mf source_tree_state)" = dirty ] && echo 1 || echo 0)"
G -C "$D" add VERSION

# ---- 5. read-only means read-only --------------------------------------------
{
    cat "$TMP/run.sh" | sed '$d'
    echo 'SOURCE_TREE_STATE=clean'
    echo 'write_manifest "$OUT"'
} > "$TMP/overwrite.sh"
echo change >> "$D/VERSION"
rm -f "$TMP/manifest"
REPO_DIR="$D" OUT="$TMP/manifest" bash "$TMP/overwrite.sh" > "$TMP/run.log" 2>&1; rc=$?
check "a later write to the Doors state stops the apply instead of changing the record" \
    "$([ $rc != 0 ] && [ ! -e "$TMP/manifest" ] && grep -q 'readonly' "$TMP/run.log" && echo 1 || echo 0)"
git -C "$D" checkout -q -- VERSION

echo "provenance_state_test: $failed failure(s)"
exit $((failed > 0))
