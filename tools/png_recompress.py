#!/usr/bin/env python3
"""Losslessly shrink the PNGs the render-snapshot test writes.

test/test_render_snapshot/png_writer.h deliberately emits RFC 1951 "stored"
(uncompressed) deflate blocks so the host test needs no zlib dependency --
see that header's own comment. That's the right call for gitignored test
output, but it makes every 800x480 frame ~384 KB, which is far too much to
commit into docs/screenshots/ on every layout change.

This re-encodes each file's pixel data with real deflate plus per-row PNG
filtering. It is exactly lossless: the decoded pixels are compared against
the original before anything is written, and a file that doesn't match is
left alone rather than replaced.

Usage: tools/png_recompress.py docs/screenshots/*.png
"""

import struct
import sys
import zlib

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def read_chunks(blob):
    assert blob[:8] == PNG_MAGIC, "not a PNG"
    pos = 8
    while pos < len(blob):
        (length,) = struct.unpack(">I", blob[pos : pos + 4])
        ctype = blob[pos + 4 : pos + 8]
        data = blob[pos + 8 : pos + 8 + length]
        yield ctype, data
        pos += 12 + length


def write_chunk(out, ctype, data):
    out += struct.pack(">I", len(data))
    body = ctype + data
    out += body
    out += struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    return out


def unfilter(raw, width, height, bpp):
    """Undo PNG row filters, returning flat pixel rows."""
    stride = width * bpp
    out = bytearray()
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        ftype = raw[pos]
        pos += 1
        line = bytearray(raw[pos : pos + stride])
        pos += stride
        for i in range(stride):
            left = line[i - bpp] if i >= bpp else 0
            up = prev[i]
            upleft = prev[i - bpp] if i >= bpp else 0
            if ftype == 0:
                pass
            elif ftype == 1:
                line[i] = (line[i] + left) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + up) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif ftype == 4:
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                pred = left if (pa <= pb and pa <= pc) else (up if pb <= pc else upleft)
                line[i] = (line[i] + pred) & 0xFF
            else:
                raise ValueError(f"unknown filter type {ftype}")
        out += line
        prev = line
    return bytes(out)


def refilter(pixels, width, height, bpp):
    """Re-apply filters, picking the one with the smallest absolute sum per
    row -- the standard heuristic, and what makes the deflate below actually
    pay off on flat e-ink-style artwork."""
    stride = width * bpp
    out = bytearray()
    prev = bytearray(stride)
    for y in range(height):
        line = pixels[y * stride : (y + 1) * stride]
        best = None
        for ftype in range(5):
            cand = bytearray(stride)
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                up = prev[i]
                upleft = prev[i - bpp] if i >= bpp else 0
                if ftype == 0:
                    cand[i] = line[i]
                elif ftype == 1:
                    cand[i] = (line[i] - left) & 0xFF
                elif ftype == 2:
                    cand[i] = (line[i] - up) & 0xFF
                elif ftype == 3:
                    cand[i] = (line[i] - ((left + up) >> 1)) & 0xFF
                else:
                    p = left + up - upleft
                    pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                    pred = left if (pa <= pb and pa <= pc) else (up if pb <= pc else upleft)
                    cand[i] = (line[i] - pred) & 0xFF
            score = sum(b if b < 128 else 256 - b for b in cand)
            if best is None or score < best[0]:
                best = (score, ftype, cand)
        out.append(best[1])
        out += best[2]
        prev = bytearray(line)
    return bytes(out)


def recompress(path):
    with open(path, "rb") as handle:
        blob = handle.read()

    header = None
    idat = b""
    others = []
    for ctype, data in read_chunks(blob):
        if ctype == b"IHDR":
            header = data
        elif ctype == b"IDAT":
            idat += data
        elif ctype == b"IEND":
            pass
        else:
            others.append((ctype, data))

    width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", header)
    if depth != 8 or interlace != 0 or color not in (0, 2, 6):
        print(f"{path}: unsupported format (depth={depth} color={color}), left alone")
        return
    bpp = {0: 1, 2: 3, 6: 4}[color]

    raw = zlib.decompress(idat)
    pixels = unfilter(raw, width, height, bpp)
    refiltered = refilter(pixels, width, height, bpp)

    # Prove it round-trips before overwriting anything.
    if unfilter(refiltered, width, height, bpp) != pixels:
        print(f"{path}: re-filter did not round-trip, left alone")
        return

    out = bytearray(PNG_MAGIC)
    out = write_chunk(out, b"IHDR", header)
    for ctype, data in others:
        out = write_chunk(out, ctype, data)
    out = write_chunk(out, b"IDAT", zlib.compress(refiltered, 9))
    out = write_chunk(out, b"IEND", b"")

    before, after = len(blob), len(out)
    if after >= before:
        print(f"{path}: already smaller than a recompress ({before} B), left alone")
        return
    with open(path, "wb") as handle:
        handle.write(bytes(out))
    print(f"{path}: {before} -> {after} B ({100 * after // before}%)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for arg in sys.argv[1:]:
        recompress(arg)
