<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->

<h1 align="center">
  <br>
  <img src="https://github.com/user-attachments/assets/35031845-4bfa-4916-8bf5-bc3bce67d5e2" alt="Eglantine" width="200">
  <br>
  <b>Eglantine</b>
  <br>
</h1>

<h4 align="center">An experimental, free and open-source Nintendo Switch 2 emulator written in C++, derived from Eden, yuzu and Sudachi.</h4>

<p align="center">
  <a href="https://github.com/notsapinho/Eglantine/releases"><img src="https://img.shields.io/github/v/release/notsapinho/Eglantine?include_prereleases&label=latest%20build" alt="Latest build"></a>
  <a href="./LICENSE.txt"><img src="https://img.shields.io/badge/license-GPLv3-blue" alt="License: GPLv3"></a>
  <a href="https://discord.gg/jCCZ58VExk"><img src="https://img.shields.io/discord/1367654015269339267?color=5865F2&label=Eglantine&logo=discord&logoColor=white" alt="Discord"></a>
</p>

<p align="center">
  <a href="#compatibility">Compatibility</a> |
  <a href="#download">Download</a> |
  <a href="#getting-started">Getting started</a> |
  <a href="#frame-rate-mods">Frame-rate mods</a> |
  <a href="#building">Building</a> |
  <a href="#contributing">Contributing</a> |
  <a href="#license">License</a>
</p>

> [!WARNING]
> Eglantine is very experimental. Expect crashes, graphical glitches and missing features, and keep backups of your saves.

## Compatibility

| Game | Status |
| --- | --- |
| Pokémon Pokopia | Playable. [Frame-rate mods](#frame-rate-mods) take it to 120, 180, 240 or 360 FPS. |
| Mario Kart World | Runs at decent to full speed, depending on your hardware and settings. |
| Splatoon Raiders | Runs, with variable performance. |

Other games are being brought up; their notes are in [docs/dev/bringup](./docs/dev/bringup). A full compatibility list will follow.

## Download

Every change to `main` is built and published on the [Releases](https://github.com/notsapinho/Eglantine/releases) page. Day-to-day work happens on `dev`, which has no automatic builds. Every package is built with clang and profile-guided optimization (PGO).

| System | File | Notes |
| --- | --- | --- |
| Windows x64 | `eglantine-windows-x86_64.zip` | Extract and run `eglantine.exe`. |
| Linux x86_64 / arm64 | `eglantine-linux-*.AppImage` | Self-contained; needs glibc 2.39 or newer (Ubuntu 24.04, Fedora 40, Debian 13, Arch). `chmod +x` it, then run it. |
| Linux x86_64 / arm64 | `eglantine-linux-*.tar.gz` | Uses your system's Qt 6 and libraries. |

`eden-cli` (the command-line frontend) ships next to `eglantine` in every package.

## Getting started

You need:

- A GPU with Vulkan 1.3 support and up-to-date drivers.
- Your own console's keys and firmware, dumped from your own hardware. Eglantine does not include them, and they can't be shared.
- Games you own, dumped from your own cartridges or eShop purchases.

The [User Handbook](./docs/user/README.md) covers setup, with a [Quick Start](./docs/user/QuickStart.md), controllers, mods, saves and troubleshooting.

## Frame-rate mods

**Pokémon Pokopia** runs at 60 FPS on the console. The bundled mods raise that to 120, 180, 240 or 360 FPS. They need **version 2.0.1** of the game (on any other version they do nothing), and they ship in the `Pokopia FPS mods` folder of the Windows zip and the Linux tar.gz, and on their own as `pokopia-fps-mods.zip` on each release.

A mod needs two things: the mod itself, and the game's **FPS Limit** setting raised to the same number. The FPS Limit is 60 unless you change it, and the game can't go past it even with a mod. It has nothing to do with your monitor's refresh rate.

### Choosing a mod

Pick the highest mod your monitor can show and your PC can keep up with. With VSync on, you never see more frames than your monitor's refresh rate.

| Your monitor | Mod | FPS Limit |
| --- | --- | --- |
| 120–165 Hz | `120 FPS` | 120 |
| 180–200 Hz | `180 FPS` | 180 |
| 240–300 Hz | `240 FPS` | 240 |
| 360 Hz and up | `360 FPS` | 360 |

### Installing

1. In Eglantine, right-click Pokémon Pokopia and choose **Open Mod Data Location**.
   - On Windows that is `%APPDATA%\eden\load\0400A3A01EE4C000`; on Linux, `~/.local/share/eden/load/0400A3A01EE4C000`.
2. Copy **one** folder from `Pokopia FPS mods` into it, for example `240 FPS`.
3. Right-click the game, open **Properties → Add-Ons**, and check that exactly one FPS mod is enabled.
4. In the same **Properties** window, open **Graphics** and set **FPS Limit (FPS mods)** to the mod's number, for example 240. It sits just under VSync Mode.
   - Setting it here applies to Pokopia only. The same setting in **Emulation → Configure → Graphics** applies to every game, and other games should stay at 60.
5. Start the game. The FPS counter in the status bar should now go past 60.

### If it doesn't work

- **Still 60 FPS:** the FPS Limit is still 60, the mod isn't enabled in Add-Ons, or the game isn't on version 2.0.1.
- **Lower or uneven frame rate:** your PC can't keep up with that mod. Use the next one down, and set the FPS Limit to match.
- **Two FPS mods enabled:** keep only one.

The mods are made with [`tools/nx2/pokopia_fps_mod.py`](./tools/nx2/pokopia_fps_mod.py).

## Building

See the [General Build Guide](./docs/Build.md). On Windows, [`tools/windows/build.ps1`](./tools/windows/build.ps1) builds with MSVC and needs no administrator rights. The other development tools are described in the [tools directory](./tools), and developer notes are in [docs/dev](./docs/dev).

## Contributing

Pull requests are welcome; open them against `dev`. Please keep changes focused and documented, and describe how you tested them. For development discussions, join the [Discord](https://discord.gg/jCCZ58VExk).

## Credits

Eglantine was started by Camille LaVey ([Djihads80/Eglantine](https://github.com/Djihads80/Eglantine)) and builds on the work of the [Eden](https://git.eden-emu.dev/eden-emu/eden), yuzu and Sudachi projects and their contributors.

Eglantine is not affiliated with or endorsed by Nintendo. Use it only with games, keys and firmware you own and dumped yourself.

## License

Eglantine is licensed under the GPLv3 (or any later version). See [LICENSE.txt](./LICENSE.txt).
