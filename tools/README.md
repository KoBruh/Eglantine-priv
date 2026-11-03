# Tools

Tools for Eglantine and its subprojects. When adding new scripts please use `#!/bin/sh -e` or `#!/usr/bin/env <interpreter>` (for `.py`, `.rb`, or `.perl`). Keep scripts POSIX compliant (i.e not require hard `bash` to run, just plain old `sh`).

## Switch 2

- [`nx2`](./nx2): Python tools for the Switch 2 work (standard library only):
  - `run_scenario.py`: Runs `eden-cli` through a scripted scenario (see [`nx2/scenarios`](./nx2/scenarios)) and reports what the emulator did wrong.
  - `thread_cpu.py`: Per-thread CPU time of a running emulator, by thread name (Windows).
  - `pokopia_fps_mod.py`: Writes the frame-rate mods (exefs IPS patches) for Pokémon Pokopia in [`dist/mods`](../dist/mods).
  - `frames.py`: Turns `EDEN_NX2_FRAME_CAPTURE` output into PNGs and a contact sheet.
  - `image_decode.py`: Decodes a texture dumped by `EDEN_NX2_TRACE_DUMP` to a PNG.
  - `volume_check.py`: Checks how a dumped 3D texture (`EDEN_NX2_DUMP_3D`) should be un-tiled.
  - `nso_syms.py`, `nso_dis.py`, `npdm_info.py`, `a64_coverage.py`: Symbol lookup, disassembly, `main.npdm` dump, and the AArch64 encodings dynarmic doesn't decode, for a title's NSOs.
  - `gdbrsp.py`: Minimal GDB remote-serial-protocol client for the gdbstub.
- [`integration`](./integration): Standalone regression programs for a few video_core, dynarmic and Ampere-lowering headers. CMake doesn't build them.

## Windows

- `windows/build.ps1`: Configures and builds Eglantine with MSVC and Ninja, without administrator rights. See `Get-Help .\tools\windows\build.ps1`.
- `windows/load-msvc-env.ps1`, `windows/load-msvc-env.sh`: Load the Visual C++ environment into PowerShell, or into MSYS2/Git Bash (through `vcvarsall.sh`).
- `windows/install-vulkan-sdk.ps1`, `windows/install-vulkan-sdk.sh`: Install the Vulkan SDK.

## macOS

- `macos/package.sh`: Packages a local build into a self-contained `eglantine.app` zip and checks it (see [Build.md](../docs/Build.md#macos-packages)).

## CI

Used by the [`release-binaries`](../.github/workflows/release-binaries.yml) workflow.

- `ci/linux-deps.sh`: Installs what the Linux release builds need (the apt packages, clang and lld from apt.llvm.org, CMake 3.31 or newer), skipping what is already there.
- `ci/boat-snapshot.sh`: Makes the boat.dev snapshot the Linux x86_64 release job starts from (`linux-deps.sh` already run, Qt in `/opt/Qt`). Run it again after changing `linux-deps.sh`, the Qt version or `PGO_LLVM_VERSION`.

## Third-Party

- [CPMUtil Scripts](./cpm), run through `cpmutil.sh`.

## Binaries

Built when `ENABLE_DEBUG_TOOLS` is on.

- `maxwell-spirv`: Converts Maxwell shaders (dumped from `.ash` files) into SPIR-V code (emitted into STDOUT).
- `maxwell-disas`: Dumb raw Maxwell disassembler.
- `maxwell-ir`: Dump generated IR of Maxwell shaders.

## Scripts

- `generate_converters.py`: Generates converters for given formats of textures (C++ helper).
- `svc_generator.py`: Generates the files `src/core/hle/kernel/svc.cpp` and `src/core/hle/kernel/svc.h` based off prototypes.
- `gendynarm.cpp`: Generates dynarmic's decoder tables (`arm.inc`, `asimd.inc`, `a64.inc`): `gendynarm -arm|-asimd|-a64`.
- `shellcheck.sh`: Ensure POSIX compliance (and syntax sanity) for all tools in this directory and subdirectories.
- `llvmpipe-run.sh`: Sets environment variables needed to run any command (or Eglantine) with llvmpipe.
- `optimize-assets.sh`: Optimizes PNG assets with OptiPng.
- `update-icons.sh`: Rebuild all icons (macOS, Windows, bitmaps, Android) based on the master SVG file (`dist/icon_variations/base.svg`, copied to `dist/dev.eden_emu.eden.svg`)
    * Requires: ImageMagick, `optipng`, `png2icns` (libicns)
- `dtrace-tool.pl`: Runs dtrace sampling over the program and turns it into a flame graph (requires FlameGraph).
- `clang-format.sh`: Runs `clang-format` on the Qt frontend (`src/qt_common`, `src/yuzu`).
    * Requires: clang
- `cpp-lint.sh`: Homemade dumb C++ linter.
- `fuzzsettings.cpp`, `fuzzsettings.sh`: Fuzz settings files.

## Android

It's recommended to run these scripts after almost any Android change, as they are relatively fast and keep the APK from bloating.

- `unused-strings.sh`: Finds unused strings in `strings.xml` files.
- `stale-translations.sh`: Finds translated strings that aren't present in the source `strings.xml` file.

## Translations

- [Translation Scripts](./translations)
