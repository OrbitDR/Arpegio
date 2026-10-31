"""Draws the application icon: four notes rising on a dark tile, an arpeggio.

Writes src/app/arpegio.ico with PNG images inside, using the standard library alone.
"""
import math
import struct
import sys
import zlib
from pathlib import Path

SIZES = [16, 24, 32, 48, 64, 128, 256]
TILE = (18, 19, 22)
BRASS = (227, 177, 92)
DOTS = [(0.26, 0.74, 0.085), (0.42, 0.60, 0.095), (0.58, 0.45, 0.105), (0.76, 0.27, 0.125)]
RADIUS = 0.2  # of the tile's corners
OVERSAMPLE = 4


def inside_tile(x, y):
    dx = max(RADIUS - x, x - (1 - RADIUS), 0)
    dy = max(RADIUS - y, y - (1 - RADIUS), 0)
    return dx * dx + dy * dy <= RADIUS * RADIUS


def color_at(x, y):
    if not inside_tile(x, y):
        return None
    if any(math.hypot(x - cx, y - cy) <= r for cx, cy, r in DOTS):
        return BRASS
    # A thin line ties the notes together.
    for (ax, ay, _), (bx, by, _) in zip(DOTS, DOTS[1:]):
        t = max(0.0, min(1.0, ((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / ((bx - ax) ** 2 + (by - ay) ** 2)))
        if math.hypot(x - (ax + t * (bx - ax)), y - (ay + t * (by - ay))) <= 0.018:
            return BRASS
    return TILE


def render(size):
    rows = bytearray()
    for row in range(size):
        rows.append(0)  # no PNG filter
        for column in range(size):
            red = green = blue = alpha = 0
            for sub_y in range(OVERSAMPLE):
                for sub_x in range(OVERSAMPLE):
                    x = (column + (sub_x + 0.5) / OVERSAMPLE) / size
                    y = (row + (sub_y + 0.5) / OVERSAMPLE) / size
                    color = color_at(x, y)
                    if color:
                        red += color[0]
                        green += color[1]
                        blue += color[2]
                        alpha += 1
            if alpha:
                rows += bytes((red // alpha, green // alpha, blue // alpha, alpha * 255 // OVERSAMPLE**2))
            else:
                rows += bytes(4)
    return bytes(rows)


def png(size):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(render(size), 9)) + chunk(b"IEND", b"")


def main():
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent.parent / "src" / "app" / "arpegio.ico"
    images = [png(size) for size in SIZES]
    offset = 6 + 16 * len(images)
    out = struct.pack("<HHH", 0, 1, len(images))
    for size, image in zip(SIZES, images):
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    target.write_bytes(out + b"".join(images))
    print(f"{target}: {len(out) + sum(map(len, images))} bytes")


if __name__ == "__main__":
    main()
