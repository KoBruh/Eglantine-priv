<!--
SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Mods

Ready-made mods for games that run on Eglantine. Each game has a folder named after it and its title ID, and each mod is a folder inside it with an `exefs` (code patches) or `romfs` (game files) folder.

| Game | Title ID | Game version | Mods |
| --- | --- | --- | --- |
| Pokémon Pokopia | `0400A3A01EE4C000` | 2.0.1 | Frame rate: `120 FPS`, `180 FPS`, `240 FPS`, `360 FPS` |

A mod only works with the game version listed for it. On any other version it does nothing.

## Installing a mod

1. Download the mod's folder. The easiest way is **Code → Download ZIP** on this repository's GitHub page, then take the folder you need out of `mods`. For a single small mod you can also recreate its folders by hand and download each file with GitHub's **Download raw file** button.
2. In Eglantine, right-click the game and choose **Open Mod Data Location**.
3. Copy the mod's folder into it, for example `240 FPS` with its `exefs` folder inside.
4. Right-click the game, open **Properties → Add-Ons**, and check the mod is enabled. Enable only one mod of a kind at a time, such as one frame-rate mod.

See [Installing Mods](../docs/user/Mods.md) in the User Handbook for more.

## Frame-rate mods

A frame-rate mod lets a game run above its usual frame rate, but the game still can't show more frames per second than Eglantine's **FPS Limit**, which is 60 by default. In the game's **Properties → Graphics**, set **FPS Limit (FPS mods)** to the mod's frame rate, for example 240. Setting it there applies to that game only, so other games stay at 60.

Pick a mod your monitor can show and your PC can keep up with. With VSync on, you never see more frames than your monitor's refresh rate.

## Adding a mod

Mods here should be small, made for a specific game version, and explain what they change. The Pokopia frame-rate mods are made with [`tools/nx2/pokopia_fps_mod.py`](../tools/nx2/pokopia_fps_mod.py).
