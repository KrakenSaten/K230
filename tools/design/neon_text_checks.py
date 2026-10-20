#!/usr/bin/env python3
"""Check matched before/after captures of the Neon text-colour refinement.

Usage: neon_text_checks.py BEFORE_DIR AFTER_DIR [PAIRS_DIR]

Both directories come from tools/design/neon_text_captures.py. Checks:
  1. every non-Neon frame is pixel-identical before and after;
  2. every frame's layout audit (clipped, truncated, overlap, zero) is
     identical before and after, at every rotation, text size and mode;
  3. in Neon, a pixel that changed was neutral before (white or grey text,
     grey framing, or their anti-aliased mix with a dark background): no
     cyan, magenta, lime or status-coloured pixel, fill or photograph moved
     or changed.
Prints one line per check and exits non-zero on any failure. With PAIRS_DIR,
also writes side-by-side before/after pairs of the reported screens, pixels
copied unaltered (no scaling, colour or sharpening operation).
"""
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import sys
from PIL import Image, ImageChops, ImageDraw

before, after = Path(sys.argv[1]), Path(sys.argv[2])
frames = sorted(p.name for p in before.glob('*.png'))
assert frames and frames == sorted(p.name for p in after.glob('*.png')), 'frame sets differ'
failures = 0


def check(label, ok, detail=''):
    global failures
    print(f'{"ok  " if ok else "FAIL"} {label}{" - " + detail if detail and not ok else ""}')
    failures += 0 if ok else 1


def chroma(rgb):
    return max(rgb) - min(rgb)


other = [f for f in frames if not f.startswith('neon-')]
changed = [f for f in other if ImageChops.difference(Image.open(before / f).convert('RGB'),
                                                     Image.open(after / f).convert('RGB')).getbbox()]
check(f'{len(other)} non-Neon frames pixel-identical', not changed, ' '.join(changed[:5]))

audits = sorted(p.name for p in before.glob('*.audit.json'))
moved = [a for a in audits if (before / a).read_text() != (after / a).read_text()]
check(f'{len(audits)} layout audits identical before and after', not moved, ' '.join(moved[:5]))

neon = [f for f in frames if f.startswith('neon-')]
bad, total = [], 0
for f in neon:
    b = Image.open(before / f).convert('RGB')
    a = Image.open(after / f).convert('RGB')
    box = ImageChops.difference(b, a).getbbox()
    if not box:
        continue
    bp, ap = b.load(), a.load()
    n = 0
    for y in range(box[1], box[3]):
        for x in range(box[0], box[2]):
            if bp[x, y] != ap[x, y]:
                total += 1
                # Neutral before: white/grey text or framing (chroma under 40),
                # or its anti-aliased edge on the dark navy background.
                if chroma(bp[x, y]) >= 40 and max(bp[x, y]) > 90:
                    n += 1
    if n:
        bad.append(f'{f}:{n}')
check(f'{len(neon)} Neon frames: {total} changed pixels, all neutral before', not bad, ' '.join(bad[:8]))
PAIRS = [(f'{screen}-{rotation}-{mode}', f'neon-{rotation}-large-{mode}-{screen}.png')
         for screen, modes in (('launcher', ('normal', 'outdoor', 'night')), ('folder', ('normal', 'outdoor')),
                               ('favorites', ('normal',)), ('settings', ('normal', 'outdoor', 'night')),
                               ('system', ('normal', 'outdoor')), ('controls', ('normal', 'outdoor')),
                               ('lock', ('normal',)))
         for mode in modes for rotation in ('portrait', 'landscape')]
if len(sys.argv) > 3:
    pairs = Path(sys.argv[3])
    pairs.mkdir(parents=True, exist_ok=True)
    for name, frame in PAIRS:
        b, a = Image.open(before / frame).convert('RGB'), Image.open(after / frame).convert('RGB')
        w, h = b.size
        out = Image.new('RGB', (w * 2 + 24, h + 36), '#111722')
        draw = ImageDraw.Draw(out)
        draw.text((8, 8), 'BEFORE: 56aefe5 | SDL simulator', fill='white')
        draw.text((w + 32, 8), 'AFTER: Neon text roles | SDL simulator', fill='white')
        out.paste(b, (0, 36))
        out.paste(a, (w + 24, 36))
        out.save(pairs / f'{name}.png', optimize=True)
    print(f'{len(PAIRS)} pairs written to {pairs}')
print(f'neon_text_checks: {failures} failure(s)')
sys.exit(1 if failures else 0)
