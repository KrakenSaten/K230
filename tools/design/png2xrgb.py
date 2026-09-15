#!/usr/bin/env python3
"""Convert the Doors boot artwork into the U-Boot splash, or check a splash.

    png2xrgb.py SOURCE.png OUTPUT.xrgb
    png2xrgb.py --check FILE.xrgb

What U-Boot wants (DOCUMENTED, vendor U-Boot overlay
board/canaan/common/logo/k230_logo.c): the RM69A10 build loads /logo.xrgb from
the boot partition and shows it full screen on OSD layer 4 before Linux
starts. There is no header. A file that is not exactly 568 x 1232 x 4 =
2,799,104 bytes is skipped with "logo.xrgb size mismatch" and nothing is shown.

Byte order: DRM XRGB8888. Each pixel is one little-endian 32-bit word
0xXXRRGGBB, so the file holds  B, G, R, X  for every pixel, rows from the top,
pixels from the left. The evidence: U-Boot programs OSD4 with format 0x03, DMA
map 0x40 and address mode 0x1100 (display_logo.c, vo_osd4_logo_test), which is
what the kernel's canaan_vo.c writes for DRM_FORMAT_XRGB8888 and for no other
format. X is written 0xFF, as in the vendor's own logo.xrgb, where every
fourth byte is 0xFF.

The conversion is a function of the pixels only: no scaling, cropping,
rotation, dithering or colour management, so one PNG always gives the same
bytes. The source must already be 568 x 1232 portrait, 8-bit RGB or RGBA,
fully opaque, and free of ICC-profile, gamma and chromaticity chunks; anything
else is refused with the reason rather than adjusted. The output is written to
a temporary file and renamed into place, and is never the source file.
"""
import hashlib
import os
import sys

sys.dont_write_bytecode = True          # a build must leave no __pycache__ in the tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngstrict import PngError, read_png  # noqa: E402

WIDTH = 568
HEIGHT = 1232
SIZE = WIDTH * HEIGHT * 4               # 2,799,104
PAD = 0xFF
COLOUR_MANAGED = ("iCCP", "gAMA", "cHRM")


def convert(src, dst):
    if os.path.splitext(dst)[1] != ".xrgb":
        raise PngError("%s: the output must be a .xrgb file" % dst)
    if os.path.exists(dst) and os.path.samefile(src, dst):
        raise PngError("%s: refusing to write the output over the source" % dst)
    png = read_png(src)
    if (png.width, png.height) != (WIDTH, HEIGHT):
        raise PngError("%s: %d x %d; the splash must be exactly %d x %d portrait "
                       "(nothing is scaled, cropped or rotated)"
                       % (src, png.width, png.height, WIDTH, HEIGHT))
    managed = [c for c in png.chunks if c in COLOUR_MANAGED]
    if managed:
        raise PngError("%s: carries %s; export it without colour management, the panel "
                       "takes the stored values as they are" % (src, ", ".join(managed)))
    alpha = png.rgba[3::4]
    if alpha.count(b"\xff") != WIDTH * HEIGHT:
        first = next(i for i, a in enumerate(alpha) if a != 0xFF)
        raise PngError("%s: pixel (%d, %d) is not opaque (alpha %d); the splash has no "
                       "alpha, so the source must not either"
                       % (src, first % WIDTH, first // WIDTH, alpha[first]))
    out = bytearray(SIZE)
    out[0::4] = png.rgba[2::4]          # B
    out[1::4] = png.rgba[1::4]          # G
    out[2::4] = png.rgba[0::4]          # R
    out[3::4] = bytes([PAD]) * (WIDTH * HEIGHT)
    tmp = dst + ".tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, dst)
    return bytes(out)


def check(path):
    """Returns a list of problems; empty when U-Boot would accept the file."""
    size = os.path.getsize(path)
    if size != SIZE:
        return ["%s is %d bytes; U-Boot accepts exactly %d (568 x 1232 x 4)" % (path, size, SIZE)]
    with open(path, "rb") as f:
        data = f.read()
    pad = data[3::4]
    if pad.count(bytes([PAD])) != WIDTH * HEIGHT:
        first = next(i for i, v in enumerate(pad) if v != PAD)
        return ["%s: the X byte of pixel (%d, %d) is 0x%02x, expected 0x%02x"
                % (path, first % WIDTH, first // WIDTH, pad[first], PAD)]
    return []


def main(argv):
    if len(argv) == 3 and argv[1] == "--check":
        try:
            problems = check(argv[2])
        except OSError as e:
            problems = [str(e)]
        for p in problems:
            print("FAIL " + p)
        if problems:
            return 1
        with open(argv[2], "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        print("ok   %s: %d x %d XRGB8888 (B,G,R,X), %d bytes, sha256 %s"
              % (argv[2], WIDTH, HEIGHT, SIZE, digest))
        return 0
    if len(argv) == 3 and not argv[1].startswith("-"):
        try:
            data = convert(argv[1], argv[2])
        except (PngError, OSError) as e:
            print("png2xrgb: %s" % e, file=sys.stderr)
            return 1
        print("png2xrgb: %s -> %s, %d bytes, sha256 %s"
              % (argv[1], argv[2], len(data), hashlib.sha256(data).hexdigest()))
        return 0
    print(__doc__.split("\n\n")[1], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
