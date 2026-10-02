#!/usr/bin/env bash
# Export the public-source candidate: the tracked tree of a commit without the
# paths docs/licensing/public-source-exclude.txt keeps private.
#
#   public_source_tree.sh <out.tar> [commit]     write the tree as a tar
#   public_source_tree.sh --list [commit]        print the paths it would hold
#
# A tree, not a repository: this repository's history still contains every
# excluded file, so publishing the history would publish them too
# (docs/licensing/APACHE_2_READINESS.md §14). Nothing is deleted from the
# private repository. Publishing remains the owner's decision.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXCLUDE="${REPO}/docs/licensing/public-source-exclude.txt"
LIST=0
if [ "${1:-}" = "--list" ]; then LIST=1; shift; OUT=""; else OUT="${1:?usage: public_source_tree.sh <out.tar> [commit] | --list [commit]}"; shift; fi
COMMIT="${1:-HEAD}"

# The manifest at that commit, so the export is a function of the commit.
mapfile -t ENTRIES < <(git -C "${REPO}" show "${COMMIT}:docs/licensing/public-source-exclude.txt" |
    tr -d '\r' | grep -v -E '^[[:space:]]*(#|$)' | sed -E 's/[[:space:]]+(B[0-9A-Z]+)[[:space:]].*$//')
SPEC=(.)
for e in "${ENTRIES[@]}"; do SPEC+=(":(exclude,literal)${e%/}"); done

if [ "${LIST}" = 1 ]; then
    # ls-tree takes no exclude pathspecs, so the entries are applied here: a
    # trailing / excludes everything under it, otherwise the exact path.
    git -C "${REPO}" -c core.quotepath=off ls-tree -r --name-only "${COMMIT}" |
        awk 'NR == FNR { e[++n] = $0; next }
             { for (i = 1; i <= n; i++) {
                   p = e[i]
                   if (substr(p, length(p)) == "/" ? index($0, p) == 1 : $0 == p) next
               }
               print }' <(printf '%s\n' "${ENTRIES[@]}") -
    exit 0
fi
git -C "${REPO}" archive --format=tar -o "${OUT}" "${COMMIT}" -- "${SPEC[@]}"
echo "wrote ${OUT}: $(tar -tf "${OUT}" | grep -c -v '/$') files from $(git -C "${REPO}" rev-parse --short "${COMMIT}"), ${#ENTRIES[@]} exclusions"
