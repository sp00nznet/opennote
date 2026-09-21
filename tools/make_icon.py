"""Generate res/icon.ico.

The icon is drawn here rather than painted once and committed as a mystery,
so it can be changed by editing numbers and re-running:

    py tools/make_icon.py res/icon.ico

Why a script and not a generated image: an application icon is mostly seen at
16 and 32 pixels -- in the title bar, the taskbar, the Alt+Tab list -- and at
that size only geometry survives. Anything painted and then scaled down turns
to mush, which is what the previous icon (a leather notebook and a pen) did:
at 16 pixels it read as a brown blob.

So the mark is a page with a folded corner and three lines of text, the first
of them a heading. Flat colour, hard edges, one accent. Below 20 pixels the
heading and one body line are drawn and the rest dropped, because three bars in
sixteen pixels is a smudge.

Pure standard library: the rasteriser and the PNG and ICO writers are all here.

The generalised version of this, with the toolbar's glyphs alongside it and an
accent colour per product, lives in asset-forge as pipelines/ui_icons.py. This
file stays because a build should not need another repository checked out.
"""

import os
import struct
import sys
import zlib

# --- the mark ---------------------------------------------------------------

OUTLINE = (0x1E, 0x24, 0x30, 255)     # dark slate, the page's edge
PAGE    = (0xFF, 0xFF, 0xFF, 255)
FOLD    = (0xCF, 0xD6, 0xE2, 255)     # the turned-down corner, shaded
HEADING = (0x2F, 0x6F, 0xED, 255)     # the accent: one blue bar
BODY    = (0x51, 0x5C, 0x6B, 255)

SIZES = [16, 20, 24, 32, 48, 64, 128, 256]


def rounded_rect(x0, y0, x1, y1, r):
    """A shape is a function (x, y) -> inside, in canvas units."""
    def f(x, y):
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        if x0 + r <= x <= x1 - r or y0 + r <= y <= y1 - r:
            return x0 <= x <= x1 and y0 <= y <= y1
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return f


def triangle(ax, ay, bx, by, cx, cy):
    def side(px, py, x0, y0, x1, y1):
        return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0)

    def f(x, y):
        d1 = side(x, y, ax, ay, bx, by)
        d2 = side(x, y, bx, by, cx, cy)
        d3 = side(x, y, cx, cy, ax, ay)
        neg = d1 < 0 or d2 < 0 or d3 < 0
        pos = d1 > 0 or d2 > 0 or d3 > 0
        return not (neg and pos)
    return f


def bar(x0, y0, x1, y1):
    r = (y1 - y0) / 2.0
    return rounded_rect(x0, y0, x1, y1, r)


def mark(size):
    """The shapes to draw, in order, in 0..1 canvas units."""
    small = size < 20

    stroke = 0.055 if small else 0.038          # the page outline's thickness
    left, right = 0.14, 0.86
    top, bottom = 0.08, 0.92
    fold = 0.30 if small else 0.26              # how much corner is turned down
    radius = 0.05

    shapes = [
        # The page: outline first, then the fill inset by the stroke, which is
        # cheaper than stroking a path and looks the same at these sizes.
        (rounded_rect(left, top, right, bottom, radius), OUTLINE),
        (rounded_rect(left + stroke, top + stroke,
                      right - stroke, bottom - stroke, radius * 0.7), PAGE),

        # The folded corner, cut out of the top right and shaded.
        (triangle(right - fold, top, right, top, right, top + fold), OUTLINE),
        (triangle(right - fold + stroke * 1.4, top + stroke,
                  right - stroke, top + stroke,
                  right - stroke, top + fold - stroke * 1.4), FOLD),
    ]

    # The text. A heading and two body lines, or a heading and one line when
    # there is not room for three.
    inset = left + stroke * 2.6
    width = (right - stroke * 2.6) - inset

    if small:
        height = 0.11
        shapes.append((bar(inset, 0.38, inset + width * 0.92, 0.38 + height), HEADING))
        shapes.append((bar(inset, 0.60, inset + width * 0.72, 0.60 + height), BODY))
    else:
        height = 0.075
        shapes.append((bar(inset, 0.34, inset + width * 0.86, 0.34 + height), HEADING))
        shapes.append((bar(inset, 0.52, inset + width * 1.00, 0.52 + height * 0.8), BODY))
        shapes.append((bar(inset, 0.65, inset + width * 0.62, 0.65 + height * 0.8), BODY))

    return shapes


# --- rasteriser -------------------------------------------------------------

SAMPLES = 4     # per axis, so sixteen samples a pixel


def render(size):
    """Paint the mark at `size`, returning RGBA bytes, top row first."""
    shapes = mark(size)
    step = 1.0 / (size * SAMPLES)

    pixels = bytearray(size * size * 4)

    for py in range(size):
        for px in range(size):
            # Accumulate premultiplied colour over the samples in this pixel.
            r = g = b = a = 0.0

            for sy in range(SAMPLES):
                y = (py * SAMPLES + sy + 0.5) * step
                for sx in range(SAMPLES):
                    x = (px * SAMPLES + sx + 0.5) * step

                    # The last shape covering the sample wins: they are painted
                    # in order, and every one of them is opaque.
                    hit = None
                    for shape, colour in shapes:
                        if shape(x, y):
                            hit = colour
                    if not hit:
                        continue

                    r += hit[0]
                    g += hit[1]
                    b += hit[2]
                    a += 1.0

            total = SAMPLES * SAMPLES
            if a == 0:
                continue

            i = (py * size + px) * 4
            pixels[i + 0] = int(r / a + 0.5)
            pixels[i + 1] = int(g / a + 0.5)
            pixels[i + 2] = int(b / a + 0.5)
            pixels[i + 3] = int(255 * a / total + 0.5)

    return bytes(pixels)


# --- containers -------------------------------------------------------------

def png(size, rgba):
    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


def dib(size, rgba):
    """A BITMAPINFOHEADER image: BGRA bottom-up, then the AND mask.

    Windows reads PNG inside an .ico from Vista on, but only for the large
    entries; the small ones are safest as DIBs, and every reader handles them.
    """
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                         size * size * 4, 0, 0, 0, 0)

    rows = []
    for y in range(size - 1, -1, -1):
        row = bytearray()
        for x in range(size):
            i = (y * size + x) * 4
            row += bytes((rgba[i + 2], rgba[i + 1], rgba[i + 0], rgba[i + 3]))
        rows.append(bytes(row))

    # The mask is ignored for 32-bit entries but has to be there and padded to
    # four bytes a row.
    maskRow = (size + 31) // 32 * 4
    mask = bytes(maskRow * size)

    return header + b"".join(rows) + mask


def ico(images):
    """images: [(size, payload)] -- payload already a PNG or a DIB."""
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)

    for size, payload in images:
        out += struct.pack("<BBBBHHII",
                           0 if size >= 256 else size,
                           0 if size >= 256 else size,
                           0, 0, 1, 32, len(payload), offset)
        offset += len(payload)

    return out + b"".join(payload for _size, payload in images)


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "res/icon.ico"

    images = []
    for size in SIZES:
        rgba = render(size)
        # PNG for the big one, which keeps the file small; DIB for the rest.
        images.append((size, png(size, rgba) if size >= 256 else dib(size, rgba)))
        print("  %3d x %-3d  %6d bytes" % (size, size, len(images[-1][1])))

    blob = ico(images)
    os.makedirs(os.path.dirname(target) or ".", exist_ok=True)
    with open(target, "wb") as f:
        f.write(blob)

    print("%s  %d bytes, %d sizes" % (target, len(blob), len(images)))


if __name__ == "__main__":
    main()
