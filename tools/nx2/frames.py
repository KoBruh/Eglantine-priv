#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn EDEN_NX2_FRAME_CAPTURE output (nx2-frame-<n>.tga) into PNGs and a contact sheet.

    python tools/nx2/frames.py <capture dir> [--every N] [--scale S] [--columns C]

Writes <dir>/png/nx2-frame-<n>.png for every frame and <dir>/sheet.png: a grid of thumbnails in
frame order, left to right, top to bottom (the legend is printed). Standard library only.
"""
import argparse
import pathlib
import struct
import sys
import zlib


def read_tga(path):
    """Uncompressed 32-bit TGA as (width, height, top-down RGBA bytes)."""
    data = path.read_bytes()
    id_length, _, image_type = data[0], data[1], data[2]
    width, height = struct.unpack_from("<HH", data, 12)
    bpp, descriptor = data[16], data[17]
    if image_type != 2 or bpp != 32:
        raise ValueError(f"{path.name}: only uncompressed 32-bit TGA is supported")
    pixels = memoryview(data)[18 + id_length:18 + id_length + width * height * 4]
    rgba = bytearray(len(pixels))
    rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4] = pixels[2::4], pixels[1::4], pixels[0::4], pixels[3::4]
    if not descriptor & 0x20:  # bottom-up rows
        stride = width * 4
        rgba = b"".join(rgba[y * stride:(y + 1) * stride] for y in reversed(range(height)))
    return width, height, bytes(rgba)


def write_png(path, width, height, rgba):
    stride = width * 4
    raw = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def downscale(width, height, rgba, factor):
    """Nearest-neighbour downscale by an integer factor."""
    stride, out_w, out_h = width * 4, width // factor, height // factor
    rows = []
    for y in range(out_h):
        row = rgba[y * factor * stride:y * factor * stride + stride]
        out = bytearray(out_w * 4)
        for c in range(4):
            out[c::4] = row[c::4 * factor][:out_w]
        rows.append(bytes(out))
    return out_w, out_h, b"".join(rows)


def convert(directory, every=1, scale=4, columns=6, write_pngs=True):
    """Write the PNGs and sheet.png for a capture directory; returns the sheet's frame numbers."""
    frames = sorted(directory.glob("nx2-frame-*.tga"))
    if not frames:
        raise FileNotFoundError(f"no nx2-frame-*.tga in {directory}")
    png_dir = directory / "png"
    png_dir.mkdir(exist_ok=True)
    thumbs, legend = [], []
    for index, frame in enumerate(frames):
        width, height, rgba = read_tga(frame)
        if write_pngs:
            write_png(png_dir / (frame.stem + ".png"), width, height, rgba)
        if index % every == 0:
            thumbs.append(downscale(width, height, rgba, scale))
            legend.append(frame.stem.removeprefix("nx2-frame-"))

    tw, th = thumbs[0][0], thumbs[0][1]
    columns, gap = min(columns, len(thumbs)), 4
    rows = (len(thumbs) + columns - 1) // columns
    sheet_w, sheet_h = columns * (tw + gap) - gap, rows * (th + gap) - gap
    sheet = bytearray(b"\x20\x20\x20\xff" * sheet_w * sheet_h)
    for i, (w, h, rgba) in enumerate(thumbs):
        x0, y0 = (i % columns) * (tw + gap), (i // columns) * (th + gap)
        for y in range(min(h, th)):
            start = ((y0 + y) * sheet_w + x0) * 4
            sheet[start:start + w * 4] = rgba[y * w * 4:(y + 1) * w * 4]
    write_png(directory / "sheet.png", sheet_w, sheet_h, bytes(sheet))
    return [legend[r * columns:(r + 1) * columns] for r in range(rows)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", type=pathlib.Path)
    parser.add_argument("--every", type=int, default=1, help="put every Nth frame on the sheet")
    parser.add_argument("--scale", type=int, default=4, help="thumbnail downscale factor")
    parser.add_argument("--columns", type=int, default=6)
    parser.add_argument("--no-png", action="store_true", help="only write the sheet")
    args = parser.parse_args()
    try:
        rows = convert(args.directory, args.every, args.scale, args.columns, not args.no_png)
    except FileNotFoundError as error:
        sys.exit(str(error))
    print(f"sheet: {args.directory / 'sheet.png'} (frame numbers by row):")
    for row in rows:
        print("  " + "  ".join(row))


if __name__ == "__main__":
    main()
