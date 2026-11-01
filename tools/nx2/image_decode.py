#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode a texture dumped by EDEN_NX2_TRACE_DUMP (mip level 0) to a PNG.

    python tools/nx2/image_decode.py <tex-...bin> [out.png] [--max 512]

Un-tiles the raw guest bytes with the emulator's block-linear swizzle (as volume_check.py) and
decodes BC1, BC4, BC5 and 8-bit RGBA formats; this shows what the game uploaded, before any of
the emulator's own decoding. BC4 shows as grey, BC5 as red/green (a normal map's x/y).
"""
import argparse
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from frames import write_png  # noqa: E402
from volume_check import unswizzle  # noqa: E402

# Maxwell/Ampere TIC texture formats (src/video_core/textures/texture.h) -> (name, bits per
# element: a texel, or a 4x4 block for BC formats).
TIC_FORMATS = {0x08: ("A8B8G8R8", 32), 0x24: ("BC1 (DXT1)", 64), 0x27: ("BC4 (DXN1)", 64),
               0x28: ("BC5 (DXN2)", 128)}


def rgb565(value):
    r, g, b = value >> 11 & 31, value >> 5 & 63, value & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def bc1_block(block):
    c0, c1, bits = struct.unpack("<HHI", block)
    a, b = rgb565(c0), rgb565(c1)
    if c0 > c1:
        palette = [a + (255,), b + (255,), tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,),
                   tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,)]
    else:
        palette = [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]
    return [palette[bits >> (2 * i) & 3] for i in range(16)]


def bc4_values(block):
    r0, r1 = block[0], block[1]
    bits = int.from_bytes(block[2:8], "little")
    if r0 > r1:
        palette = [r0, r1] + [((6 - i) * r0 + (1 + i) * r1) // 7 for i in range(6)]
    else:
        palette = [r0, r1] + [((4 - i) * r0 + (1 + i) * r1) // 5 for i in range(4)] + [0, 255]
    return [palette[bits >> (3 * i) & 7] for i in range(16)]


def decode_block(name, block):
    if name.startswith("BC1"):
        return bc1_block(block)
    if name.startswith("BC4"):
        return [(v, v, v, 255) for v in bc4_values(block)]
    if name.startswith("BC5"):
        return [(r, g, 0, 255) for r, g in zip(bc4_values(block[:8]), bc4_values(block[8:]))]
    raise ValueError(f"{name}: not a supported block format")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("png", type=pathlib.Path, nargs="?")
    parser.add_argument("--max", type=int, default=512, help="downscale so the longest side is at most this")
    args = parser.parse_args()

    # tex-<address>-<w>x<h>-tic<format>-bh<block height>-native<0|1>.bin (TIC format, hex)
    match = re.search(r"-(\d+)x(\d+)-tic([0-9a-f]+)-bh(\d+)-native(\d)", args.dump.name)
    if not match:
        sys.exit("the file name does not carry the EDEN_NX2_TRACE_DUMP layout")
    width, height, block_height, native = (int(match.group(i)) for i in (1, 2, 4, 5))
    tic_format, block_depth = int(match.group(3), 16), 0
    name, bits = TIC_FORMATS.get(tic_format, (f"TIC format {tic_format:#x}", 0))
    if not bits:
        sys.exit(f"{name}: no decoder for this format")
    data = args.dump.read_bytes()
    print(f"{args.dump.name}: {width}x{height} {name}, {len(data)} bytes, block height {block_height}, native {native}")
    if not any(data):
        sys.exit("all bytes are zero: the guest memory holds no data for this image")

    compressed = name.startswith(("BC1", "BC4", "BC5"))
    element = 4 if compressed else 1
    bpp = bits // 8
    ew, eh = -(-width // element), -(-height // element)
    linear = unswizzle(data, ew, eh, 1, bpp, block_height, block_depth, native)
    rgba = bytearray(width * height * 4)
    for by in range(eh):
        for bx in range(ew):
            raw = linear[(by * ew + bx) * bpp:(by * ew + bx + 1) * bpp]
            texels = decode_block(name, raw) if compressed else [tuple(raw[:4]) if bpp >= 4 else (raw[0],) * 3 + (255,)]
            for i, texel in enumerate(texels):
                x, y = bx * element + i % element, by * element + i // element
                if x < width and y < height:
                    rgba[(y * width + x) * 4:(y * width + x) * 4 + 4] = bytes(texel)
    step = max(1, max(width, height) // args.max)
    out_w, out_h = width // step, height // step
    small = bytearray(out_w * out_h * 4)
    for y in range(out_h):
        for x in range(out_w):
            src = ((y * step) * width + x * step) * 4
            small[(y * out_w + x) * 4:(y * out_w + x) * 4 + 4] = rgba[src:src + 4]
    for i in range(3, len(small), 4):
        small[i] = 255  # show colour regardless of alpha
    out = args.png or args.dump.with_suffix(".png")
    write_png(out, out_w, out_h, bytes(small))
    print(f"-> {out} ({out_w}x{out_h})")


if __name__ == "__main__":
    main()
