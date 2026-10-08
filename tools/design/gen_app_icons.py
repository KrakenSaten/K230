#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Generate ui/pocketui/pos_app_icons.c, the Doors app icons as A8 masks.

    gen_app_icons.py [-o pos_app_icons.c] [SOURCE ...]

A SOURCE is a PNG or a directory of PNGs. Without sources: the launcher's
fifteen icons, LAUNCHER_ICONS below, into ui/pocketui/pos_app_icons.c. The
generated file is committed, like the fonts and the brand mark, so a build
needs neither this script nor the PNGs; tests/app_icons_test.sh fails if it no
longer matches its sources.

Every PNG becomes one `const lv_image_dsc_t pos_app_icon_<name>`, where
<name> is the file name, which is the app's id (or, for a launcher icon a
package named otherwise, APP_IDS below). The sources are the packages'
tintable icons: white on transparent. Only the alpha channel is kept,
antialiasing included, so an icon has no colour of its own and is drawn in
the image_recolor of its style (POS_STYLE_APP_ICON, accent_primary), which the
theme engine rewrites on every theme and mode change (DS §8). A source with
any other colour under a visible pixel is refused: that colour would be thrown
away without anyone deciding to.

The whole canvas is kept, not trimmed: the packages place each icon inside
the same square cell, and trimming would move every icon by a different
amount. Nothing is scaled. All icons must have the same size, and no app id
may come twice. Only the launcher's apps are listed: the extension package's
icons for apps that do not exist are not compiled in. Standard library only.
"""
import hashlib
import os
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True          # a build must leave no __pycache__ in the tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngstrict import PngError, read_png  # noqa: E402

PREFIX = "pos_app_icon_"
THRESHOLD = "docs/design/brand/doors-threshold/icons/png-32/"
EXTENSION = "docs/design/brand/doors-icon-extension/png-32/"
# First-party app icons in the extension's line language, for apps no package
# has one for (docs/design/doors-app-icons/README.md).
FIRST_PARTY = "docs/design/doors-app-icons/png-32/"
# One icon per launcher app (ui/shell/shell.c apps[]), from the package that
# supplied it (docs/design/brand/README.md), or first-party where none did.
LAUNCHER_ICONS = [THRESHOLD + n + ".png" for n in
                  ("radio", "system", "fleet", "radar", "timber", "notes",
                   "clock", "calendar", "calculator", "settings")] + [EXTENSION + "wave.png",
                                                                    EXTENSION + "files.png",
                                                                    EXTENSION + "camera.png",
                                                                    EXTENSION + "recorder.png",
                                                                    FIRST_PARTY + "zabbix.png",
                                                                    FIRST_PARTY + "browser.png",
                                                                    FIRST_PARTY + "vision.png",
                                                                    FIRST_PARTY + "mp3.png",
                                                                    FIRST_PARTY + "video.png",
                                                                    FIRST_PARTY + "solitaire.png",
                                                                    FIRST_PARTY + "blackjack.png",
                                                                    FIRST_PARTY + "2048.png",
                                                                    FIRST_PARTY + "poker.png",
                                                                    FIRST_PARTY + "deskbuddy.png",
                                                                    FIRST_PARTY + "terminal.png",
                                                                    EXTENSION + "gallery.png"]
# An icon is named by its file name, which is the app's id - except where a
# package drew an icon for an app under another name. The extension's Gallery
# is the Photo app's (docs/apps/PHOTO.md, DS §45).
APP_IDS = {EXTENSION + "gallery.png": "photo"}


def mask(png, src):
    if png.colour_type != 6:
        raise PngError("%s: no alpha channel; an icon must be white on transparent" % src)
    w = png.width
    alpha = png.rgba[3::4]
    visible = [i for i, a in enumerate(alpha) if a]
    if not visible:
        raise PngError("%s: every pixel is transparent" % src)
    for i in visible:
        if png.rgba[i * 4:i * 4 + 3] != b"\xff\xff\xff":
            raise PngError("%s: pixel (%d, %d) is #%s, not white; a tint mask carries no colour"
                           % (src, i % w, i // w, png.rgba[i * 4:i * 4 + 3].hex()))
    return [bytes(alpha[y * w:(y + 1) * w]) for y in range(png.height)]


def main():
    root = Path(__file__).resolve().parents[2]
    args = sys.argv[1:]
    dst = root / "ui/pocketui/pos_app_icons.c"
    if args[:1] == ["-o"] and len(args) >= 2:
        dst, args = Path(args[1]), args[2:]
    sources = [Path(a) for a in args] or [root / p for p in LAUNCHER_ICONS]

    def shown(p):
        try:
            return p.resolve().relative_to(root).as_posix()
        except ValueError:
            return p.name

    icons = []
    try:
        files = []
        for s in sources:
            if s.is_dir():
                found = sorted(p for p in s.iterdir() if p.suffix == ".png")
                if not found:
                    raise PngError("%s: no PNG files" % s)
                files += found
            else:
                files.append(s)
        for p in files:
            app_id = APP_IDS.get(shown(p), p.stem)
            # Always behind PREFIX in C, so an id may start with a digit
            # (PG 2048's is "2048").
            if not re.fullmatch(r"[a-z0-9][a-z0-9_]*", app_id):
                raise PngError("%s: the file name is not an app id usable in a C name" % p)
            if any(app_id == i[0] for i in icons):
                raise PngError("%s: a second icon for app id %s" % (p, app_id))
            png = read_png(str(p))
            rows = mask(png, p)
            want = (icons[0][2], icons[0][3]) if icons else (png.width, png.width)
            if (png.width, png.height) != want:
                raise PngError("%s: %d x %d; every icon must be the same square size"
                               % (p, png.width, png.height))
            icons.append((app_id, rows, png.width, png.height,
                          hashlib.sha256(p.read_bytes()).hexdigest(), shown(p)))
    except (PngError, OSError) as e:
        print("gen_app_icons: %s" % e, file=sys.stderr)
        return 1
    icons.sort(key=lambda i: i[0])
    w, h = icons[0][2], icons[0][3]
    lines = [
        "/* Generated by tools/design/gen_app_icons.py. Do not edit; regenerate.",
        " *",
        " * The Doors app icons as LVGL A8 alpha masks, %d x %d each, the whole" % (w, h),
        " * canvas, alpha kept exactly. They have no colour; the style's",
        " * image_recolor draws them (POS_STYLE_APP_ICON). Sources:",
        " *",
    ]
    for name, _, _, _, digest, path in icons:
        lines += [" *   %s" % path, " *     sha256 %s" % digest]
    lines += [
        " */",
        "#ifdef LV_LVGL_H_INCLUDE_SIMPLE",
        '#include "lvgl.h"',
        "#else",
        '#include "lvgl/lvgl.h"',
        "#endif",
    ]
    for name, rows, _, _, _, _ in icons:
        sym = PREFIX + name
        lines += ["", "static const uint8_t %s_map[] LV_ATTRIBUTE_MEM_ALIGN = {" % sym]
        for r in rows:
            lines.append("    " + ",".join("0x%02x" % v for v in r) + ",")
        lines += [
            "};",
            "",
            "const lv_image_dsc_t %s = {" % sym,
            "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
            "    .header.cf = LV_COLOR_FORMAT_A8,",
            "    .header.flags = 0,",
            "    .header.w = %d," % w,
            "    .header.h = %d," % h,
            "    .header.stride = %d," % w,
            "    .data_size = sizeof(%s_map)," % sym,
            "    .data = %s_map," % sym,
            "};",
        ]
    lines.append("")
    dst.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print("gen_app_icons: %d icons -> %s, %d x %d A8: %s"
          % (len(icons), shown(dst), w, h, " ".join(i[0] for i in icons)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
