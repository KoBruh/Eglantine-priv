#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check how a dumped 3D texture (EDEN_NX2_DUMP_3D) should be un-tiled.

    python tools/nx2/volume_check.py <3d-...bin> [--png out.png --block-depth N --native 0|1]

Decodes the raw guest bytes with the emulator's block-linear swizzle (decoders.cpp) under each
candidate layout and scores how smooth the result is along x, y and z: a colour-grading LUT is a
smooth cube, so the right layout has small neighbour differences on every axis while a wrong one
jumps. The layout the emulator used is in the file name and marked in the table. --png writes the
slices side by side (z left to right) for the chosen layout.
"""
import argparse
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from frames import write_png  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]
SWIZZLE_X_BITS, SWIZZLE_Y_BITS = 0b100101111, 0b011010000


def pdep(value, mask):
    result, bit = 0, 1
    while mask:
        low = mask & -mask
        if value & bit:
            result |= low
        mask &= mask - 1
        bit <<= 1
    return result


def native_gob_offset(offset, native):
    if not native:
        return offset
    difference = ((offset >> 5) ^ (offset >> 6)) & 1
    return offset ^ (difference << 5) ^ (difference << 6)


def unswizzle(data, width, height, depth, bpp, block_height, block_depth, native):
    """Mirror of SwizzleImpl<TO_LINEAR=false> in src/video_core/textures/decoders.cpp."""
    pitch = width * bpp
    stride = (pitch + 63) & ~63
    gobs_in_x = (stride + 63) >> 6
    block_size = gobs_in_x << (9 + block_height + block_depth)
    slice_size = -(-height // (8 << block_height)) * block_size
    x_shift = 9 + block_height + block_depth
    out = bytearray(pitch * height * depth)
    for z in range(depth):
        offset_z = (z >> block_depth) * slice_size + ((z & ((1 << block_depth) - 1)) << (9 + block_height))
        for y in range(height):
            block_y = y >> 3
            offset_y = (block_y >> block_height) * block_size + ((block_y & ((1 << block_height) - 1)) << 9)
            swizzled_y = pdep(y, SWIZZLE_Y_BITS)
            for column in range(width):
                x = column * bpp
                offset = native_gob_offset(offset_z + offset_y + ((x >> 6) << x_shift) +
                                           (pdep(x, SWIZZLE_X_BITS) | swizzled_y), native)
                linear = (z * height + y) * pitch + column * bpp
                out[linear:linear + bpp] = data[offset:offset + bpp] if offset + bpp <= len(data) else bytes(bpp)
    return bytes(out)


def pixel_formats():
    """PixelFormat enum index -> (name, bits per block), from surface.h."""
    text = (REPO / "src" / "video_core" / "surface.h").read_text(encoding="utf-8")
    return [(name, int(bits)) for name, bits in re.findall(r"PIXEL_FORMAT_ELEM\((\w+),\s*\d+,\s*\d+,\s*(\d+)\)", text)]


def channels(texel, name):
    """A texel as floats for the formats LUTs use; raw bytes otherwise."""
    if name.endswith("FLOAT") and len(texel) == 8:
        return struct.unpack("<4e", texel)
    if name.startswith("A2B10G10R10") and len(texel) == 4:
        v = struct.unpack("<I", texel)[0]
        return (v & 1023) / 1023, (v >> 10 & 1023) / 1023, (v >> 20 & 1023) / 1023
    return tuple(b / 255 for b in texel)


def smoothness(linear, width, height, depth, bpp, name):
    """Mean absolute neighbour difference along x, y and z (lower is smoother)."""
    def texel(x, y, z):
        start = ((z * height + y) * width + x) * bpp
        return channels(linear[start:start + bpp], name)

    totals, counts = [0.0, 0.0, 0.0], [0, 0, 0]
    step = max(1, width // 16)
    for z in range(0, depth, step):
        for y in range(0, height, step):
            for x in range(0, width, step):
                here = texel(x, y, z)
                for axis, (nx, ny, nz) in enumerate(((x + 1, y, z), (x, y + 1, z), (x, y, z + 1))):
                    if nx < width and ny < height and nz < depth:
                        there = texel(nx, ny, nz)
                        totals[axis] += sum(abs(a - b) for a, b in zip(here, there)) / len(here)
                        counts[axis] += 1
    return [t / max(c, 1) for t, c in zip(totals, counts)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("--png", type=pathlib.Path, help="write the slices for --block-depth/--native")
    parser.add_argument("--block-depth", type=int)
    parser.add_argument("--native", type=int)
    args = parser.parse_args()

    match = re.search(r"-(\d+)x(\d+)x(\d+)-format(\d+)-block(\d+)x(\d+)x(\d+)-native(\d)", args.dump.name)
    if not match:
        sys.exit("the file name does not carry the EDEN_NX2_DUMP_3D layout")
    width, height, depth, fmt, _, block_height, block_depth, native = map(int, match.groups())
    formats = pixel_formats()
    name, bits = formats[fmt] if fmt < len(formats) else (f"format{fmt}", 32)
    bpp = bits // 8
    data = args.dump.read_bytes()
    print(f"{args.dump.name}: {width}x{height}x{depth} {name} ({bpp} B/texel), {len(data)} bytes; "
          f"emulator layout: block height {block_height}, block depth {block_depth}, native tiling {native}")
    if not any(data):
        print("all bytes are zero: the guest memory holds no data (written by the GPU, or not loaded yet)")
        return

    print(f"\n{'native':>6} {'blk_d':>5} {'x':>7} {'y':>7} {'z':>7}  (mean neighbour difference, lower = smoother)")
    results = []
    for candidate_native in (0, 1):
        for candidate_depth in range(0, 6):
            linear = unswizzle(data, width, height, depth, bpp, block_height, candidate_depth, candidate_native)
            score = smoothness(linear, width, height, depth, bpp, name)
            results.append((sum(score), candidate_native, candidate_depth))
            mark = "  <- emulator" if (candidate_native, candidate_depth) == (native, block_depth) else ""
            print(f"{candidate_native:>6} {candidate_depth:>5} {score[0]:7.4f} {score[1]:7.4f} {score[2]:7.4f}{mark}")
    best = min(results)
    print(f"\nsmoothest: native {best[1]}, block depth {best[2]}")

    if args.png:
        use_native = native if args.native is None else args.native
        use_depth = block_depth if args.block_depth is None else args.block_depth
        linear = unswizzle(data, width, height, depth, bpp, block_height, use_depth, use_native)
        scale = 4
        out_w, out_h = width * depth * scale, height * scale
        image = bytearray(out_w * out_h * 4)
        for z in range(depth):
            for y in range(height):
                for x in range(width):
                    c = channels(linear[((z * height + y) * width + x) * bpp:][:bpp], name)
                    rgb = bytes(max(0, min(255, int(v * 255))) for v in (list(c) + [0, 0, 0])[:3])
                    for sy in range(scale):
                        row = (y * scale + sy) * out_w
                        for sx in range(scale):
                            p = (row + (z * width + x) * scale + sx) * 4
                            image[p:p + 4] = rgb + b"\xff"
        write_png(args.png, out_w, out_h, bytes(image))
        print(f"slices (z left to right, native {use_native}, block depth {use_depth}): {args.png}")


if __name__ == "__main__":
    main()
