<!--
SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Mods

Ready-made mods for games that run on Eglantine. Each game has a folder named after it and its title ID, with a README that says what its mods do and what they need. Each mod is a folder inside it, holding an `exefs` (code patches) or `romfs` (game files) folder.

| Game | Mods |
| --- | --- |
| [Pokémon Pokopia](./Pokemon%20Pokopia%20%280400A3A01EE4C000%29) | Frame rate: 120, 180, 240 and 360 FPS |

A mod only works with the game version its README names. On any other version it does nothing.

## Installing a mod

1. Read the game folder's README first: some mods need a setting changed as well.
2. Download the mod's folder. The easiest way is **Code → Download ZIP** on this repository's GitHub page, then take the folder you need out of `mods`. For a small mod you can also recreate its folders by hand and download each file with GitHub's **Download raw file** button.
3. In Eglantine, right-click the game and choose **Open Mod Data Location**.
4. Copy the mod's folder into it, for example `240 FPS` with its `exefs` folder inside.
5. Right-click the game, open **Properties → Add-Ons**, and check the mod is enabled. Enable only one mod of a kind at a time.

See [Installing Mods](../docs/user/Mods.md) in the User Handbook for more.

## Adding a mod

Put it in its game's folder, or a new `<Game name> (<title ID>)` folder, and give that folder a README: what the mod changes, the game version it's for, any setting it needs, and how it was made.
