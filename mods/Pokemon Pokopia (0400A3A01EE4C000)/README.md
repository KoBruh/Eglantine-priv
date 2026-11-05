<!--
SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Pokémon Pokopia: frame-rate mods

Pokémon Pokopia runs at 60 FPS on the console. These mods let it run at 120, 180, 240 or 360 FPS, with the game still running at its normal speed.

- **Game version:** 2.0.1 only (main module build ID `76A489F3BD3642FA5E18BD83D32C98BACC5F7D0E`). On any other version they do nothing.
- **Title ID:** `0400A3A01EE4C000`

## Choosing one

Pick the highest mod your monitor can show and your PC can keep up with. With VSync on, you never see more frames than your monitor's refresh rate.

| Your monitor | Mod | FPS Limit |
| --- | --- | --- |
| 120–165 Hz | `120 FPS` | 120 |
| 180–200 Hz | `180 FPS` | 180 |
| 240–300 Hz | `240 FPS` | 240 |
| 360 Hz and up | `360 FPS` | 360 |

## Installing

1. In Eglantine, right-click Pokémon Pokopia and choose **Open Mod Data Location**.
   - On Windows that is `%APPDATA%\eglantine\load\0400A3A01EE4C000`; on Linux, `~/.local/share/eglantine/load/0400A3A01EE4C000`.
2. Copy **one** of the four folders into it, for example `240 FPS`.
3. Right-click the game, open **Properties → Add-Ons**, and check that exactly one FPS mod is enabled.
4. In the same **Properties** window, open **Graphics** and set **FPS Limit (FPS mods)** to the mod's number, for example 240. It sits just under VSync Mode.
   - The FPS Limit is 60 by default, and the game can't go past it even with a mod. Setting it here applies to Pokopia only; the same setting in **Emulation → Configure → Graphics** applies to every game, and other games should stay at 60.
5. Start the game. The FPS counter in the status bar should now go past 60.

## If it doesn't work

- **Still 60 FPS:** the FPS Limit is still 60, the mod isn't enabled in Add-Ons, or the game isn't on version 2.0.1.
- **Lower or uneven frame rate:** your PC can't keep up with that mod. Use the next one down, and set the FPS Limit to match.
- **Two FPS mods enabled:** keep only one.

## How they're made

Each mod is one IPS patch for the game's main module, written by [`tools/nx2/pokopia_fps_mod.py`](../../tools/nx2/pokopia_fps_mod.py). The patch sets the game's frame clock to the mod's rate and makes the per-frame step match it, so animations, walking and menus keep their normal speed. The script's header explains each patch.
