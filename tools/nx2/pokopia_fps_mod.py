#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a frame-rate mod (exefs IPS patch) for Pokemon Pokopia (0400A3A01EE4C000).

    python tools/nx2/pokopia_fps_mod.py <...\\exefs\\main> [--fps 120] [--load-dir DIR]

How the game paces itself (KTGL engine, update 655360's main module): its frame clock thread
(main+0x89df20, "KIDSTaskScheduler2") waits until tick_frequency / (60 / X) ticks have passed,
and its delta-time code (main+0xbfc404) steps the world by delta * 60 "60 fps frames", snapping
to X when a frame is within 1.5 ms of X / 60 s. X is main+0x13fc00, a function that returns 1.0.
Two patches:

- main+0x13fc00 returns 60 / fps instead of 1.0: the frame clock targets fps and the nominal step
  shrinks with it (0.5 frames of 1/60 s at 120 fps).
- main+0xbfc4f8 (fcsel s0, 1.0, s0, mi) is removed: it floored each step at one 60 fps frame, so
  above 60 fps the game would run fast whenever a frame missed the nominal length.

The emulator presents at most once per refresh of the emulated display when the speed limit is
on: set "Switch 2 Display Refresh Rate" (nx2_refresh_rate) to at least the target.

Writes <load dir>/0400A3A01EE4C000/<fps> FPS/exefs/<build id>.ips; the default load dir is the
emulator's (%APPDATA%\\eden\\load). Remove that folder, or disable the mod in the game's add-ons,
to go back.
"""
import argparse
import os
import pathlib
import struct
import sys

TITLE_ID = "0400A3A01EE4C000"
BUILD_ID = "76A489F3BD3642FA5E18BD83D32C98BACC5F7D0E"  # update 655360 (v2.0.1)
NOMINAL_FRAME = 0x13fc00       # fmov s0, #1.0 ; ret
STEP_FLOOR = 0xbfc4f8          # fcsel s0, s1, s0, mi   (s1 = 1.0)
FMOV_S0_1_0 = 0x1E2E1000
FCSEL_FLOOR = 0x1E204C20
NOP = 0xD503201F
NSO_HEADER = 0x100             # IPS offsets count from the NSO header


def lz4_block(src, size):
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        literal = token >> 4
        if literal == 15:
            while True:
                literal += src[i]
                i += 1
                if src[i - 1] != 255:
                    break
        out += src[i:i + literal]
        i += literal
        if i >= n:
            break
        offset = src[i] | src[i + 1] << 8
        i += 2
        match = token & 15
        if match == 15:
            while True:
                match += src[i]
                i += 1
                if src[i - 1] != 255:
                    break
        match += 4
        start = len(out) - offset
        if offset >= match:
            out += out[start:start + match]
        else:
            for k in range(match):
                out.append(out[start + k])
    if len(out) != size:
        raise ValueError("bad LZ4 data")
    return out


def text_word(nso, offset):
    """The u32 at a module offset of the NSO's (decompressed) text segment."""
    flags = struct.unpack_from("<I", nso, 0xC)[0]
    file_off, mem_off, size = struct.unpack_from("<III", nso, 0x10)
    comp = struct.unpack_from("<I", nso, 0x60)[0]
    raw = nso[file_off:file_off + (comp if flags & 1 else size)]
    text = lz4_block(raw, size) if flags & 1 else raw
    return struct.unpack_from("<I", text, offset - mem_off)[0]


def fmov_s0(value):
    """FMOV S0, #value, if value is encodable as an 8-bit floating-point immediate."""
    for imm8 in range(256):
        sign = -1.0 if imm8 & 0x80 else 1.0
        exp = (((imm8 >> 4) & 7) ^ 4) - 3  # NOT(b):c:d - 3
        frac = 16 + (imm8 & 15)
        if abs(sign * frac / 16 * 2.0 ** exp - value) < 1e-9:
            return 0x1E201000 | imm8 << 13
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("main", type=pathlib.Path, help="the game's exefs/main (update 655360)")
    parser.add_argument("--fps", type=int, default=120)
    parser.add_argument("--load-dir", type=pathlib.Path,
                        default=pathlib.Path(os.environ.get("APPDATA", "~")) / "eden" / "load")
    args = parser.parse_args()

    nso = args.main.read_bytes()
    if nso[:4] != b"NSO0":
        sys.exit(f"{args.main}: not an NSO")
    build_id = nso[0x40:0x60].hex().upper().rstrip("0")
    if build_id != BUILD_ID.rstrip("0"):
        sys.exit(f"{args.main}: build {build_id}, the patches are for {BUILD_ID} (update 655360)")
    found = text_word(nso, NOMINAL_FRAME), text_word(nso, STEP_FLOOR)
    if found != (FMOV_S0_1_0, FCSEL_FLOOR):
        sys.exit(f"unexpected code at the patch sites: {found[0]:#010x}, {found[1]:#010x}")
    nominal = fmov_s0(60 / args.fps)
    if nominal is None:
        sys.exit(f"{args.fps} fps: 60/{args.fps} is not encodable as an FMOV immediate "
                 "(120, 240 and 30 are)")

    out = bytearray(b"IPS32")
    for offset, word in ((NOMINAL_FRAME, nominal), (STEP_FLOOR, NOP)):
        out += struct.pack(">IH", offset + NSO_HEADER, 4) + struct.pack("<I", word)
    out += b"EEOF"
    target = args.load_dir.expanduser() / TITLE_ID / f"{args.fps} FPS" / "exefs" / f"{BUILD_ID}.ips"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(out)
    print(f"wrote {target}")
    print(f"set 'Switch 2 Display Refresh Rate' (nx2_refresh_rate) to {args.fps} or more for this game")


if __name__ == "__main__":
    main()
