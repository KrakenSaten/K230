"""Strict PNG reader for the Doors brand tools in tools/design.

Standard library only, like the rest of tools/design, so the build host needs
nothing beyond python3. It reads what the graphics package delivers - 8-bit
RGB or RGBA, not interlaced - and refuses everything else by name. A palette,
a 16-bit channel or an interlaced file is a different export of the artwork,
not something to convert quietly. Chunk CRCs are checked, so a truncated or
damaged file fails here rather than producing plausible pixels.

    read_png(path) -> Png
        .width, .height, .colour_type (2 = RGB, 6 = RGBA)
        .chunks: chunk type names in file order, e.g. ["IHDR", "IDAT", "IEND"]
        .rgba:   bytes, four per pixel, rows top to bottom, pixels left to right
                 (an RGB file reads as alpha 255)
"""
import collections
import struct
import zlib

SIGNATURE = b"\x89PNG\r\n\x1a\n"

Png = collections.namedtuple("Png", "width height colour_type chunks rgba")


class PngError(ValueError):
    pass


def _unfilter(raw, width, height, bpp):
    stride = width * bpp
    if len(raw) != height * (stride + 1):
        raise PngError("image data is %d bytes, expected %d" % (len(raw), height * (stride + 1)))
    out = bytearray(height * stride)
    prev = bytearray(stride)
    for y in range(height):
        base = y * (stride + 1)
        kind = raw[base]
        line = bytearray(raw[base + 1:base + 1 + stride])
        if kind == 0:
            pass
        elif kind == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif kind == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                if pa <= pb and pa <= pc:
                    pred = a
                elif pb <= pc:
                    pred = b
                else:
                    pred = c
                line[i] = (line[i] + pred) & 0xFF
        else:
            raise PngError("row %d has unknown filter type %d" % (y, kind))
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != SIGNATURE:
        raise PngError("%s: not a PNG file" % path)
    pos = 8
    chunks = []
    idat = bytearray()
    header = None
    while True:
        if pos + 12 > len(data):
            raise PngError("%s: truncated (no IEND chunk)" % path)
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        crc = data[pos + 8 + length:pos + 12 + length]
        if len(body) != length or len(crc) != 4:
            raise PngError("%s: truncated inside a %s chunk" % (path, kind.decode("latin-1")))
        if struct.unpack(">I", crc)[0] != zlib.crc32(kind + body) & 0xFFFFFFFF:
            raise PngError("%s: CRC mismatch in a %s chunk" % (path, kind.decode("latin-1")))
        name = kind.decode("latin-1")
        chunks.append(name)
        pos += 12 + length
        if name == "IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif name == "IDAT":
            idat.extend(body)
        elif name == "IEND":
            break
        elif name[0].isupper() and name not in ("PLTE",):
            raise PngError("%s: unknown critical chunk %s" % (path, name))
    if header is None or chunks[0] != "IHDR":
        raise PngError("%s: IHDR is not the first chunk" % path)
    width, height, depth, colour_type, compression, filtering, interlace = header
    if depth != 8:
        raise PngError("%s: %d-bit channels; only 8-bit is accepted" % (path, depth))
    if colour_type not in (2, 6):
        names = {0: "greyscale", 3: "palette", 4: "greyscale+alpha"}
        raise PngError("%s: %s colour type; only RGB or RGBA is accepted"
                       % (path, names.get(colour_type, "colour type %d" % colour_type)))
    if compression != 0 or filtering != 0:
        raise PngError("%s: non-standard compression or filter method" % path)
    if interlace != 0:
        raise PngError("%s: interlaced; only non-interlaced is accepted" % path)
    bpp = 3 if colour_type == 2 else 4
    pixels = _unfilter(zlib.decompress(bytes(idat)), width, height, bpp)
    if bpp == 4:
        rgba = bytes(pixels)
    else:
        out = bytearray(width * height * 4)
        out[0::4] = pixels[0::3]
        out[1::4] = pixels[1::3]
        out[2::4] = pixels[2::3]
        out[3::4] = b"\xff" * (width * height)
        rgba = bytes(out)
    return Png(width, height, colour_type, chunks, rgba)
