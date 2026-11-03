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

Every change to `main` is built and published on the [Releases](https://github.com/notsapinho/Eglantine/releases) page. Day-to-day work happens on `dev`, which has no automatic builds.

| System | File | Notes |
| --- | --- | --- |
| Windows x64 | `eglantine-windows-x86_64-pgo.zip` (recommended) or `eglantine-windows-x86_64.zip` | Extract and run `eglantine.exe`. Try the plain (MSVC) zip if you see graphical issues. |
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

The game runs at 60 FPS by default. The bundled mods for **Pokémon Pokopia 2.0.1** raise that to 120, 180, 240 or 360 FPS. They are in the `Pokopia FPS mods` folder of the Windows zips and the Linux tar.gz, and on their own as `pokopia-fps-mods.zip` on each release.

1. In Eglantine, right-click Pokémon Pokopia and choose **Open Mod Data Location**.
   - On Windows that is `%APPDATA%\eden\load\0400A3A01EE4C000`; on Linux, `~/.local/share/eden/load/0400A3A01EE4C000`.
2. Copy **one** folder (for example `240 FPS`) from `Pokopia FPS mods` into it.
3. Right-click the game, open **Properties → Add-Ons**, and check that only one FPS mod is enabled.
4. In **Emulation → Configure → Graphics**, set **Switch 2 Display Refresh Rate (Hz)** to the mod's rate (for example 240). Without this step the emulated screen still refreshes at 60 Hz and the game stays at 60 FPS.

Pick the mod closest to your monitor's refresh rate that your PC can keep up with. The mods only apply to version 2.0.1; on other versions they do nothing. They are made with [`tools/nx2/pokopia_fps_mod.py`](./tools/nx2/pokopia_fps_mod.py).

## Building

See the [General Build Guide](./docs/Build.md). On Windows, [`tools/windows/build.ps1`](./tools/windows/build.ps1) builds with MSVC and needs no administrator rights. The other development tools are described in the [tools directory](./tools), and developer notes are in [docs/dev](./docs/dev).

## Contributing

Pull requests are welcome; open them against `dev`. Please keep changes focused and documented, and describe how you tested them. For development discussions, join the [Discord](https://discord.gg/jCCZ58VExk).

## Credits

Eglantine was started by Camille LaVey ([Djihads80/Eglantine](https://github.com/Djihads80/Eglantine)) and builds on the work of the [Eden](https://git.eden-emu.dev/eden-emu/eden), yuzu and Sudachi projects and their contributors.

Eglantine is not affiliated with or endorsed by Nintendo. Use it only with games, keys and firmware you own and dumped yourself.

## License

Eglantine is licensed under the GPLv3 (or any later version). See [LICENSE.txt](./LICENSE.txt).
