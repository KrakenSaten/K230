#!/bin/bash
# Compare two renderer/art revisions on identical deterministic game states.
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
reference=${1:?reference revision required}
candidate=${2:?candidate revision required}
mkdir -p work
scratch=$(mktemp -d "$PWD/work/relay-compare.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
for variant in reference candidate; do
    ref=$reference; [ "$variant" != candidate ] || ref=$candidate
    mkdir "$scratch/$variant"
    for source in relay.c relay_art.c relay_render.c relay.h; do
        git show "$ref:apps/neon-outpost/$source" > "$scratch/$variant/$source"
    done
done
flags=(-std=c11 -O2 -Wall -Wextra -Werror -I"$scratch/reference")
rename=(-Drn_render=rn_render_reference -Drn_title=rn_title_reference
        -Drn_texture=rn_texture_reference -Drn_sprite=rn_sprite_reference -Drn_weapon=rn_weapon_reference)
for source in relay_art relay_render; do
    "${CC:-gcc}" "${flags[@]}" "${rename[@]}" -c "$scratch/reference/$source.c" -o "$scratch/$source.o"
done
"${CC:-gcc}" -std=c11 -O2 -Wall -Wextra -Werror -DRN_REFERENCE -I"$scratch/candidate" -Itests \
    tests/relay_render_replay.c "$scratch/candidate/relay.c" "$scratch/candidate/relay_art.c" \
    "$scratch/candidate/relay_render.c" "$scratch/relay_art.o" "$scratch/relay_render.o" -lm -o "$scratch/compare"
"$scratch/compare"
