"""Minimal PNG read/write on zlib alone, for the PocketTimber art tools.

The build host is a bare python3 (no numpy, no pypng), so the converter
and the procedural assets depend on nothing but the standard library.
Handles 8-bit RGB and RGBA, non-interlaced, which is all the pipeline
produces. Pixels are lists of rows, each row a list of (r, g, b, a).
"""
import struct
import zlib


def _chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


def write_png(path, rows, alpha=True):
    """rows: list of rows of (r, g, b, a) tuples (a ignored when not alpha)."""
    h = len(rows)
    w = len(rows[0]) if h else 0
    channels = 4 if alpha else 3
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for px in row:
            raw.extend(px[:channels])
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6 if alpha else 2, 0, 0, 0)
    data = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + \
        _chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def read_png(path):
    """Returns (width, height, rows) with every pixel as (r, g, b, a)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos = 8
    idat = bytearray()
    w = h = 0
    color_type = depth = interlace = 0
    palette = None
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            w, h, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"IDAT":
            idat.extend(body)
        elif kind == b"IEND":
            break
    if depth != 8 or interlace != 0:
        raise ValueError("%s: only 8-bit non-interlaced PNGs are handled" % path)
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    stride = w * channels
    raw = zlib.decompress(bytes(idat))
    rows = []
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        filt = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        bpp = channels
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if filt == 1:
                line[i] = (line[i] + a) & 0xFF
            elif filt == 2:
                line[i] = (line[i] + b) & 0xFF
            elif filt == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif filt == 4:
                line[i] = (line[i] + _paeth(a, b, c)) & 0xFF
        prev = line
        row = []
        for x in range(w):
            i = x * channels
            if color_type == 6:
                row.append((line[i], line[i + 1], line[i + 2], line[i + 3]))
            elif color_type == 2:
                row.append((line[i], line[i + 1], line[i + 2], 255))
            elif color_type == 0:
                row.append((line[i], line[i], line[i], 255))
            elif color_type == 4:
                row.append((line[i], line[i], line[i], line[i + 1]))
            else:
                r, g, b = palette[line[i]]
                row.append((r, g, b, 255))
        rows.append(row)
    return w, h, rows
