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
  <a href="#mods">Mods</a> |
  <a href="#building">Building</a> |
  <a href="#contributing">Contributing</a> |
  <a href="#license">License</a>
</p>

> [!WARNING]
> Eglantine is very experimental. Expect crashes, graphical glitches and missing features, and keep backups of your saves.

## Compatibility

| Game | Status |
| --- | --- |
| Pokémon Pokopia | Playable. |
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

## Mods

The [`mods`](./mods) folder has ready-made mods for some games. Its README lists them and shows how to install one.

## Building

See the [General Build Guide](./docs/Build.md). On Windows, [`tools/windows/build.ps1`](./tools/windows/build.ps1) builds with MSVC and needs no administrator rights. The other development tools are described in the [tools directory](./tools), and developer notes are in [docs/dev](./docs/dev).

## Contributing

Pull requests are welcome; open them against `dev`. Please keep changes focused and documented, and describe how you tested them. For development discussions, join the [Discord](https://discord.gg/jCCZ58VExk).

## Credits

Eglantine was started by Camille LaVey ([Djihads80/Eglantine](https://github.com/Djihads80/Eglantine)) and builds on the work of the [Eden](https://git.eden-emu.dev/eden-emu/eden), yuzu and Sudachi projects and their contributors.

Eglantine is not affiliated with or endorsed by Nintendo. Use it only with games, keys and firmware you own and dumped yourself.

## License

Eglantine is licensed under the GPLv3 (or any later version). See [LICENSE.txt](./LICENSE.txt).
