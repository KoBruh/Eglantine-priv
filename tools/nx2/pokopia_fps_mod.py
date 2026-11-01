#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a frame-rate mod (exefs IPS patch) for Pokemon Pokopia (0400A3A01EE4C000).

Speed checks at 120 fps, each alternating 60 fps (the game's code) and 120 fps (the patches)
every 600 frames of one run in the same scene: scene animations (campfire, items) cycle at the same
rate (1.59 s, 2.00 s), walking scrolls the screen at the same speed (about 500 px/s) and the bag
opens in the same 0.2 s. Not measured: dialog text, wandering Pokemon. An earlier build without
the whole-frame patch was reported as running fast in all of these; if anything still does, the
cause is a per-frame step this tool doesn't patch yet.

    python tools/nx2/pokopia_fps_mod.py <...\\exefs\\main> [--fps 120] [--load-dir DIR]

How the game paces itself (KTGL engine, update 655360's main module): its frame clock thread
(main+0x89df20, "KIDSTaskScheduler2") waits until tick_frequency / (60 / X) ticks have passed,
and its delta-time code (main+0xbfc404) steps the world by delta * 60 "60 fps frames", snapping
to X when a frame is within 1.5 ms of X / 60 s. X is main+0x13fc00, a function that returns 1.0.
The step ("frame scale", a float at main+0x34c03f0) is what the game's systems advance by.
Patches:

- main+0x13fc00 returns 60 / fps instead of 1.0: the frame clock targets fps and the nominal step
  shrinks with it (0.5 frames of 1/60 s at 120 fps).
- main+0xbfc4f8 (fcsel s0, 1.0, s0, mi) is removed: it floored each step at one 60 fps frame, so
  above 60 fps the game would run fast whenever a frame missed the nominal length.
- Whole frames: most readers add the step to float timers, which works for any step, but 10
  counters round it to whole frames (round(0.5) = 1: twice as fast at 120 fps) or truncate it
  (trunc(0.5) = 0: stopped). The store of the step (main+0xbfc534) calls a few instructions put in
  the unused end of .text's last page (main+0x2749500): they keep a carry of the step and publish
  floor(carry) as a float in the module's unused memory between MOD0's bss end (0x3d14b00) and
  the end of the NSO's mapping (0x3d20000). Those 10 counters read that instead of the step, so
  they count the 60 fps frames that really passed (0 or 1 per update at 120 fps; at 60 fps always
  1, as before).

- main+0x8832a4: MainThread's main loop sleeps 1 ms between checks for a frame the frame clock
  has released, so each frame started up to 1 ms late. It polls every --poll-us microseconds
  instead (default 100; 1000 keeps the game's). Measured in the world at 120 Hz, alternating
  the two every 600 frames of one run: 8.76 ms -> 8.45 ms a frame.

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
POLL_SLEEP = 0x8832a4          # mov w0, #0x4240 ; movk w0, #0xf, lsl #16  (1 ms) ; bl SleepThread
NSO_HEADER = 0x100             # IPS offsets count from the NSO header

# Whole-frame steps (see the module docstring).
SCALE_STORE = 0xbfc534         # str s0, [x22]: the frame-scale global (main+0x34c03f0) is written
STR_S0_X22 = 0xBD0002C0
CAVE = 0x2749500               # unused tail of .text's last page (.text ends at 0x2749480)
STATE_PAGE = 0x3d1f000         # past the bss end in MOD0 (0x3d14b00), inside the mapping (0x3d20000)
CARRY = 0xff8                  # float: fraction of a 60 fps frame carried to the next update
WHOLE = 0xffc                  # float: whole 60 fps frames in this update
# (GOT load "ldr xR, [xR, #0xfa0]" of the frame scale's address, the load "ldr sF, [xR]" of it)
# where the game rounds or truncates the frame scale to whole frames.
WHOLE_FRAME_SITES = [
    (0x26615c, 0x266160), (0x47af94, 0x47afdc), (0x47c718, 0x47c71c), (0x8fbe18, 0x8fbe1c),
    (0x90c350, 0x90c358), (0xe51678, 0xe51680), (0xe516e0, 0xe516e8), (0xe5172c, 0xe51734),
    (0xe51778, 0xe51780), (0xe909a4, 0xe909b4),
]


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


def text_segment(nso):
    """The NSO's (decompressed) text segment, which starts at module offset 0."""
    flags = struct.unpack_from("<I", nso, 0xC)[0]
    file_off, mem_off, size = struct.unpack_from("<III", nso, 0x10)
    comp = struct.unpack_from("<I", nso, 0x60)[0]
    raw = nso[file_off:file_off + (comp if flags & 1 else size)]
    assert mem_off == 0
    return lz4_block(raw, size) if flags & 1 else raw


def adrp(pc, register, page):
    delta = (page - (pc & ~0xFFF)) >> 12
    return 0x90000000 | (delta & 3) << 29 | ((delta >> 2) & 0x7FFFF) << 5 | register


def ldr_s(rt, rn, offset):
    return 0xBD400000 | offset // 4 << 10 | rn << 5 | rt


def str_s(rt, rn, offset):
    return 0xBD000000 | offset // 4 << 10 | rn << 5 | rt


def whole_frame_patches(word):
    """Give the game's whole-frame counters an exact count of 60 fps frames.

    Called through `bl` in place of the frame-scale store: it stores the scale as before, adds it
    to a carry and publishes floor(carry) (removing it from the carry) as a float. The sites that
    turned the scale into whole frames read that number instead; their rounding leaves it as is.
    """
    cave = [
        STR_S0_X22,                       # str    s0, [x22]          (the store it replaces)
        adrp(CAVE + 4, 16, STATE_PAGE),   # adrp   x16, state
        ldr_s(1, 16, CARRY),              # ldr    s1, [x16, #CARRY]
        0x1E202821,                       # fadd   s1, s1, s0
        0x1E254022,                       # frintm s2, s1
        0x1E223821,                       # fsub   s1, s1, s2
        str_s(1, 16, CARRY),              # str    s1, [x16, #CARRY]
        str_s(2, 16, WHOLE),              # str    s2, [x16, #WHOLE]
        0xD65F03C0,                       # ret
    ]
    if word(SCALE_STORE) != STR_S0_X22 or any(word(CAVE + 4 * i) for i in range(len(cave))):
        return None
    patches = [(SCALE_STORE, 0x94000000 | ((CAVE - SCALE_STORE) >> 2) & 0x3FFFFFF)]
    patches += [(CAVE + 4 * i, w) for i, w in enumerate(cave)]
    for got_load, scale_load in WHOLE_FRAME_SITES:
        got, load = word(got_load), word(scale_load)
        register = got & 31
        if got != 0xF947D000 | register << 5 | register or load & 0xFFFFFFE0 != 0xBD400000 | register << 5:
            return None
        patches += [(got_load, adrp(got_load, register, STATE_PAGE)),   # the state page, not &scale
                    (scale_load, ldr_s(load & 31, register, WHOLE))]
    return patches


def fmov_s0(value):
    """FMOV S0, #value, if value is encodable as an 8-bit floating-point immediate."""
    for imm8 in range(256):
        sign = -1.0 if imm8 & 0x80 else 1.0
        exp = (((imm8 >> 4) & 7) ^ 4) - 3  # NOT(b):c:d - 3
        frac = 16 + (imm8 & 15)
        if abs(sign * frac / 16 * 2.0 ** exp - value) < 1e-9:
            return 0x1E201000 | imm8 << 13
    return None


def mov_w0(value):
    """MOVZ W0, #lo ; MOVK W0, #hi, LSL #16."""
    return 0x52800000 | (value & 0xFFFF) << 5, 0x72A00000 | (value >> 16) << 5


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("main", type=pathlib.Path, help="the game's exefs/main (update 655360)")
    parser.add_argument("--fps", type=int, default=120)
    parser.add_argument("--poll-us", type=int, default=100,
                        help="main loop poll interval in microseconds (default 100; the game's is 1000)")
    parser.add_argument("--load-dir", type=pathlib.Path,
                        default=pathlib.Path(os.environ.get("APPDATA", "~")) / "eden" / "load")
    args = parser.parse_args()

    nso = args.main.read_bytes()
    if nso[:4] != b"NSO0":
        sys.exit(f"{args.main}: not an NSO")
    build_id = nso[0x40:0x60].hex().upper().rstrip("0")
    if build_id != BUILD_ID.rstrip("0"):
        sys.exit(f"{args.main}: build {build_id}, the patches are for {BUILD_ID} (update 655360)")
    text = text_segment(nso)
    word = lambda offset: struct.unpack_from("<I", text, offset)[0] if offset + 4 <= len(text) else 0
    found = (word(NOMINAL_FRAME), word(STEP_FLOOR), word(POLL_SLEEP), word(POLL_SLEEP + 4))
    if found != (FMOV_S0_1_0, FCSEL_FLOOR, *mov_w0(1_000_000)):
        sys.exit("unexpected code at the patch sites: " + ", ".join(f"{w:#010x}" for w in found))
    whole_frames = whole_frame_patches(word)
    if whole_frames is None:
        sys.exit("unexpected code at the whole-frame sites")
    if not 1 <= args.poll_us <= 1000:
        sys.exit("--poll-us: 1 to 1000")
    nominal = fmov_s0(60 / args.fps)
    if nominal is None:
        sys.exit(f"{args.fps} fps: 60/{args.fps} is not encodable as an FMOV immediate "
                 "(120, 240 and 30 are)")

    out = bytearray(b"IPS32")
    patches = [(NOMINAL_FRAME, nominal), (STEP_FLOOR, NOP)] + whole_frames
    if args.poll_us != 1000:
        low, high = mov_w0(args.poll_us * 1000)
        patches += [(POLL_SLEEP, low), (POLL_SLEEP + 4, high)]
    for offset, word in patches:
        out += struct.pack(">IH", offset + NSO_HEADER, 4) + struct.pack("<I", word)
    out += b"EEOF"
    target = args.load_dir.expanduser() / TITLE_ID / f"{args.fps} FPS" / "exefs" / f"{BUILD_ID}.ips"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(out)
    print(f"wrote {target}")
    print(f"set 'Switch 2 Display Refresh Rate' (nx2_refresh_rate) to {args.fps} or more for this game")


if __name__ == "__main__":
    main()
