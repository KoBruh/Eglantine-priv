# PGO profile

`eglantine.profdata` at the repository root is the profile the release PGO builds
(`linux-x86_64-pgo`, `linux-aarch64-pgo` and `windows-x86_64-pgo` in
`.github/workflows/release-binaries.yml`) compile against with `-fprofile-use`. It is trained on
our own code playing Pokémon Pokopia, and `train-profile.ps1` rebuilds it from a fresh checkout.

## How it is made

1. **Instrumented build.** An MSYS2 UCRT64 clang (MinGW) build configured like the workflow's
   "Configure MinGW PGO" step (the `v3` preset and ThinLTO included), with `-fprofile-generate` in
   place of `-fprofile-use`, then bundled
   like "Package MinGW binary" (`windeployqt6` plus every non-system DLL) so it runs outside MSYS2.
2. **Training.** `tools/nx2/run_scenario.py --clean-exit` plays each scenario below, docked
   (`use_docked_mode=1`) and handheld (`use_docked_mode=0`). The window renders off-screen, never
   takes focus and is muted. `LLVM_PROFILE_FILE=<dir>/<run>-%p-%m.profraw` gives every process
   its own raw profile. `--clean-exit` matters: the profile runtime writes the file from an exit
   handler, so the runner asks eden-cli to quit (WM_CLOSE to its window, SIGTERM elsewhere)
   instead of killing it.
3. **Merge.** MSYS2's `llvm-profdata merge --sparse` writes `eglantine.profdata`.

| Scenario | What it covers |
| --- | --- |
| `tools/nx2/scenarios/pokopia_explore.txt` | boot, title, loading the save, the main menu |
| `tools/nx2/scenarios/pokopia_pokedex.txt` | the Pokédex list and a 3D model page |
| `tools/nx2/scenarios/pokopia_sky.txt` | the world, tilting the camera up to the sky |
| `tools/nx2/scenarios/pokopia_walk.txt` | a long walk and run with camera turns, so the world streams in new areas and the JIT keeps compiling new game code |

### Why these choices

- **MinGW, not clang-cl.** The profile names functions by their mangled names. MinGW clang uses
  Itanium mangling like the Linux builds; a clang-cl (MSVC ABI) profile would match neither the
  Linux nor the MinGW build.
- **`-mllvm -static-func-full-module-prefix=false`, in training and in every `-fprofile-use`
  build.** IR PGO names a `static` function, a function in an anonymous namespace or a lambda as
  `<source path>;<name>`, and CMake passes absolute source paths. Without the option, a profile
  trained in `C:/Users/.../Eglantine` would not match any of those functions on a CI runner
  (`D:/a/Eglantine/Eglantine`, `/home/runner/...`). With it the name is `<file name>;<name>`.
  Two same-named static functions in same-named files can share a record; their hashes usually
  differ, so the profile keeps both. Clang warns that the option is unused on link lines, which is
  harmless (no CMake check reads that warning).
- **`--sparse`.** Functions the training never ran are left out of the profile, so clang builds
  them as it would without PGO. With zero counts it would mark them cold and optimize them for
  size, which would slow down whatever other games use them.
- **The LLVM version.** A clang refuses a profile written by a newer LLVM major (the indexed
  format changes between majors). The profile is written by the `llvm-profdata` of the older of
  the two CI compilers: MSYS2's UCRT64 clang (Windows PGO job) and apt.llvm.org's
  `clang-$PGO_LLVM_VERSION` (Linux PGO jobs). Both are LLVM 23 today (MSYS2 23.1.4,
  apt.llvm.org 23.1.6), and the script uses MSYS2's `llvm-profdata`, which is the older one. The
  script stops before building if MSYS2's LLVM major is newer than `PGO_LLVM_VERSION`.

## Training a new profile

Needs Windows, [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`, in `C:\msys64`),
Python 3 on `PATH`, a Pokopia dump and the emulator's user folder (`%APPDATA%\eden`) with keys,
firmware and a Pokopia save past the intro. The scenarios load that save; the same runs are what
`tools/nx2` scenarios always do, but a game can autosave while it runs.

Update MSYS2 first (`pacman -Syu` in an MSYS2 shell) so its clang is the one the CI gets, then:

```powershell
powershell -ExecutionPolicy Bypass -File tools\pgo\train-profile.ps1 -Game E:\Pokopia\exefs\main -Jobs 8
```

It installs the MSYS2 packages it needs (`--needed`), builds into `build\pgo-generate`, runs the
eight trainings (about 20 minutes), keeps their reports in `build\pgo-training\runs` and writes
`eglantine.profdata`. To keep the training out of your own user folder, put a portable one next to
the instrumented program (`<BuildDir>\bin\user`, with a Pokopia save) and pass its config with
`-Config <BuildDir>\bin\user\config\sdl2-config.ini`. Check that config's `[Data Storage]`
first: a `nand_directory`, `load_directory` or `sdmc_directory` that is set (`\default=false`)
points every run back at that folder, saves and mods included. `-SkipBuild` reuses the instrumented build; `-SkipTraining` only merges the
`.profraw` files already in `build\pgo-training\raw`. Each clang process can take 2 GB or more, so
pick `-Jobs` for the RAM you have.

Check the result with both CI compilers' `llvm-profdata show eglantine.profdata`, then commit it.

## When to refresh it

The profile is keyed by function name and a hash of each function's control flow. A function that
changed since training gets no profile (clang warns "function control flow change detected"
under `-Wbackend-plugin`, which the CI silences) and is built as without PGO. A stale profile
never breaks the build, it just stops helping. Retrain:

- after big changes to hot code: the CPU JIT and its backend (`src/dynarmic`, `src/core/arm`),
  the kernel and service code the game calls every frame, the GPU command processing, the Vulkan
  renderer, texture cache and shader recompiler;
- after a large merge from upstream;
- when the CI's compilers move to a new LLVM major (MSYS2's clang or `PGO_LLVM_VERSION`): write
  the profile with the older one's `llvm-profdata`, and do not raise `PGO_LLVM_VERSION` past
  MSYS2's clang unless the profile is rewritten for it;
- before a release, if the last training is old.

## Checking a profile

**Staleness.** Build with the profile and count the mismatches: configure like "Configure MinGW
PGO", but replace `-Wno-backend-plugin` with `-Wno-error=backend-plugin` (sirit builds with
`-Werror`) and add `-mllvm -pgo-warn-missing-function` and
`-mllvm -no-pgo-warn-mismatch-comdat-weak=false` (without it, inline and template functions that
changed are not reported). Then count `hash mismatch` (functions changed since training) and `no profile data available` (functions training never ran, or new
ones) in the build log. Build it from a checkout in another directory to also check that the
profile matches a checkout elsewhere, as on the CI.

**Speed.** Build the same commit without the profile and run `pokopia_walk.txt` with `--uncapped`
on both builds, alternating them, a few times each. Compare the boot loading time ("Boot loading
screen ends" in `report.md`) and the CPU time per frame once the world is up (`threads.csv`, less
the host timer thread, which spins). With the `120 FPS` mod Pokopia paces itself to 120 fps
even uncapped, so its mean frame rate barely moves; the CPU each frame costs, and the frames that
miss the 120 fps slot (over 9 ms in `EDEN_NX2_FRAME_LOG`), are what PGO lowers.

## The profile in the repository

- Trained on `dev` after the third upstream nx2 merge (the code of 351ef46ae8) with MSYS2's clang
  and `llvm-profdata` 23.1.4, configured as the release builds are (`v3` preset, ThinLTO). The
  previous profile was trained at ea3ae74a75, before that merge.
- 8 runs (four scenarios, docked and handheld), 1,244 s of play at the game's own 60 FPS (no FPS
  mod), from a portable user folder. 3.7 MB; 14,003 of the 53,863 instrumented functions ran.
- A MinGW `-fprofile-use` build in another directory, configured as the release (`v3`, ThinLTO):
  2 functions with a hash mismatch, the same cold library templates as before (an
  `nlohmann::json` constructor, reported in five files, and a `boost::algorithm` string search);
  44,636 functions have no profile and are built as without PGO.
- Pokopia in the world, release builds, `240 FPS` mod with FPS Limit 240 and VSync off, a fixed
  walk (95-190 s): the merged code with the previous profile 169.8 fps (3 runs) against `dev`'s
  181.4 (4); with this profile 181.7 (3), against 185.1 for `dev` in the runs alternating with
  them (1.8% under). The old profile had gone stale in code the merge changed, the draw path
  among it (60% more time per draw in the profiler).
