#!/usr/bin/env python3
"""Compose unaltered simulator captures and check native composited states.

Usage: neon_revision_images.py docs/design/neon/revision
Needs Pillow on the host only. Native crops/pairs copy pixels exactly; only
compact-overview.png resizes. No colour, brightness or sharpening operation.
"""
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
from collections import Counter
import json
from pathlib import Path
import sys
from PIL import Image, ImageDraw

root = Path(sys.argv[1])
repo = Path(__file__).resolve().parents[2]
palette = json.loads((repo / 'docs/design/themes.json').read_text())['themes']['neon']['modes']


def pair(name, source, crop=None):
    images = []
    for side in ('before', 'after'):
        im = Image.open(root / side / source).convert('RGB')
        images.append(im.crop(crop) if crop else im)
    w, h = images[0].size
    assert images[1].size == (w, h)
    out = Image.new('RGB', (w * 2 + 24, h + 36), '#111722')
    draw = ImageDraw.Draw(out)
    for i, im in enumerate(images):
        x = i * (w + 24)
        draw.text((x + 8, 8), ('BEFORE: ddf9791' if i == 0 else 'REVISED') + ' | SDL simulator evidence', fill='white')
        out.paste(im, (x, 36))
    out.save(root / name)


for orientation in ('portrait', 'landscape'):
    for screen in ('launcher', 'display'):
        pair(f'{screen}-{orientation}-before-after.png', f'{orientation}-large-{screen}.png')
    pair(f'activity-{orientation}-before-after.png', f'activity-{orientation}-large.png')
    pair(f'states-{orientation}-before-after.png', f'states-{orientation}-large.png')
pair('hdmi-before-after.png', 'hdmi-large-appearance.png')
pair('native-controls.png', 'states-landscape-large.png', (24, 100, 604, 400))
pair('native-reading.png', 'activity-landscape-large.png', (626, 76, 1212, 337))
pair('native-launcher-focus.png', 'portrait-large-launcher-focus.png', (28, 142, 310, 340))

# A compact index complements full-resolution captures and native crops.
files = [('Launcher', 'portrait-large-launcher.png'), ('Settings controls', 'portrait-large-display.png'),
         ('Synthetic Activity', 'activity-landscape-large.png'), ('States', 'states-landscape-large.png')]
out = Image.new('RGB', (1024, 1112), '#111722')
draw = ImageDraw.Draw(out)
for row, (label, file) in enumerate(files):
    for col, side in enumerate(('before', 'after')):
        im = Image.open(root / side / file).convert('RGB')
        im.thumbnail((490, 240))
        x, y = col * 512 + 8, row * 278 + 8
        draw.text((x, y), f'{side.upper()} - {label} - SDL simulator', fill='white')
        out.paste(im, (x, y + 24))
out.save(root / 'compact-overview.png')


def rgb(value):
    return tuple(bytes.fromhex(value.lstrip('#')))


def contrast(a, b):
    def lum(c):
        c = [v / 255 for v in c]
        c = [v / 12.92 if v <= .03928 else ((v + .055) / 1.055) ** 2.4 for v in c]
        return sum(v * w for v, w in zip(c, (.2126, .7152, .0722)))
    lo, hi = sorted((lum(a), lum(b)))
    return (hi + .05) / (lo + .05)


report = []
for mode in ('normal', 'outdoor', 'night'):
    tokens = {k: rgb(v) for k, v in palette[mode].items() if isinstance(v, str)}
    for orientation in ('portrait', 'landscape'):
        for size in ('small', 'medium', 'large'):
            prefix = '' if mode == 'normal' else mode + '-'
            file = f'states-{prefix}{orientation}-{size}.png'
            im = Image.open(root / 'after' / file).convert('RGB')
            counts = Counter(im.get_flattened_data() if hasattr(im, "get_flattened_data") else im.getdata())
            # These pixels are inside the rendered card and the three actual
            # button surfaces, clear of glyphs, corners and outlines.
            surfaces = {name: im.getpixel(point) for name, point in {
                'panel': (30, 60), 'primary': (60, 150),
                'secondary': (60, 270), 'disabled': (60, 350)}.items()}
            checks = [('body', 'text_primary', 'panel'), ('secondary text', 'text_secondary', 'panel'),
                      ('primary label', 'text_on_accent', 'primary'),
                      ('secondary label', 'text_primary', 'secondary'),
                      ('warning', 'status_warn', 'panel'), ('error', 'status_error', 'panel'),
                      ('disabled', 'disabled_fg', 'disabled')]
            ratios = {}
            for label, token, surface in checks:
                assert counts[tokens[token]] > 20, (file, label, 'no opaque ink sample')
                ratio = contrast(tokens[token], surfaces[surface])
                floor = (1.5 if mode == 'night' else 3) if label == 'disabled' else (2.5 if mode == 'night' else 4.5)
                assert ratio >= floor, (file, label, ratio, surfaces[surface])
                ratios[label] = round(ratio, 2)
            # Three native cyan edge pixels and four light focus pixels.
            assert all(im.getpixel((x, 150)) == tokens['accent_primary'] for x in range(24, 27)), file
            focus = [im.getpixel((x, 150)) == tokens['text_primary'] for x in range(32, 49)]
            assert '1111' in ''.join('1' if x else '0' for x in focus), file
            report.append(dict(capture=file, contrast=ratios, structural_px=3, focus_px=4))
(root / 'composited-state-checks.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'{len(report)} native state captures: composited contrast and border/focus pixels pass')
