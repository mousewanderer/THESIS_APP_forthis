#!/usr/bin/env python3
"""Draws the home-screen icons www/icon-192.png and www/icon-512.png.

A white seedling on AgriDose green. Plain Python, no extra packages:
each pixel is tested against a few simple shapes (ellipses for the
leaves, rectangles for the stem and the ground), 3x3 samples per pixel
for smooth edges, and the result is written as a PNG with zlib.

Run:  python AgriDose_ESP32_Link/tools/make_icons.py
"""
import math
import pathlib
import struct
import zlib

WWW = pathlib.Path(__file__).resolve().parent.parent / "www"
GREEN = (0x14, 0x53, 0x2D)
WHITE = (0xFF, 0xFF, 0xFF)


def in_ellipse(x, y, cx, cy, a, b, angle_deg):
    t = math.radians(angle_deg)
    dx, dy = x - cx, y - cy
    u = dx * math.cos(t) + dy * math.sin(t)
    v = -dx * math.sin(t) + dy * math.cos(t)
    return (u / a) ** 2 + (v / b) ** 2 <= 1.0


def in_capsule(x, y, x0, x1, yc, r):
    """Horizontal bar with round ends."""
    if x0 <= x <= x1:
        return abs(y - yc) <= r
    end = x0 if x < x0 else x1
    return (x - end) ** 2 + (y - yc) ** 2 <= r * r


def is_white(x, y):
    """x, y from 0 to 1, y going down."""
    stem = 0.47 <= x <= 0.53 and 0.40 <= y <= 0.74
    left_leaf = in_ellipse(x, y, 0.37, 0.345, 0.15, 0.075, 30)
    right_leaf = in_ellipse(x, y, 0.64, 0.32, 0.17, 0.085, -35)
    ground = in_capsule(x, y, 0.30, 0.70, 0.75, 0.03)
    return stem or left_leaf or right_leaf or ground


def draw(size, samples=3):
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            hits = 0
            for sy in range(samples):
                for sx in range(samples):
                    x = (px + (sx + 0.5) / samples) / size
                    y = (py + (sy + 0.5) / samples) / size
                    hits += is_white(x, y)
            k = hits / (samples * samples)
            row += bytes(round(g + (w - g) * k) for g, w in zip(GREEN, WHITE))
        rows.append(bytes(row))
    return rows


def write_png(path, size, rows):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + r for r in rows)          # filter type 0 on every row
    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)   # 8-bit RGB
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)
    print(f"{path.name}: {len(png)} bytes")


if __name__ == "__main__":
    for size in (192, 512):
        write_png(WWW / f"icon-{size}.png", size, draw(size))
