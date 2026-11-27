# Copyright (c) 2026 Doors authors.
# SPDX-License-Identifier: Apache-2.0
"""Regenerate DeskBuddy's two-ink illustration. Host tools: Inkscape and Pillow."""
from pathlib import Path
import subprocess
import tempfile
from PIL import Image

source = Path(__file__).resolve().with_name("perch.svg")
target = source.parents[3] / "apps/deskbuddy/perch.bin"
with tempfile.TemporaryDirectory() as tmp:
    png = Path(tmp) / "perch.png"
    subprocess.run(["inkscape", str(source), "-w", "512", "-h", "512", "-o", str(png)], check=True)
    image = Image.open(png).convert("RGBA").resize((256, 256), Image.Resampling.LANCZOS)
    # Lower half of the palette is landscape, upper half plumage.
    target.write_bytes(bytes((128 if r > b else 0) + max(r, b) * a // 510
                             for r, _, b, a in image.getdata()))
