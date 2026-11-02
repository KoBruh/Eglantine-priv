# Windows MKW follow-up, 1 November 2026

Current package: run `eglantine.exe` directly. A native MKW session preset now
enforces the eight requested settings after per-game configuration is read.
The original implementation notes below describe the earlier defaults-only pass;
see `PERFORMANCE-INVESTIGATION.md` for the subsequent automatic preset and CPU work.

## Baseline inspected

Repository: https://github.com/snowstormxd/Eglantine

Branch: `mkw-windows-pgo`, commit `0c2ecb55b1fcba5503f702e4ae09565b8ba6b21f`.
Its Windows PGO run `41316284676` succeeded. The job spent 2,931 seconds
building the GUI and reported 1,515 compiler-cache misses, with no hits.

This baseline already integrates bounded readback staging, texture array-pitch
corrections, fiber-reference lifetime fixes, shader-module release, conservative
texture/buffer budgets, uniform-buffer reuse and selected GPU coherency fixes.
These are retained. This is a continuation of that branch, not a reset to upstream.

## Changes

1. **Discovery:** deconstructed-loader construction reads metadata without
   building a RomFS image. Discovery collects candidate files once, then fills
   the content provider before populating entries. Recognized ExeFS directories
   are terminal; their sibling RomFS trees are skipped. Folder-size enumeration
   is removed from discovery; loose dumps display **Not scanned** in that column.
   Both deep discovery and standard shallow base/update layouts are supported.
   Discovery skips directory links, reports traversal errors and checks cancellation.
2. **Launch:** loose RomFS image preparation runs on a worker with a busy dialog.
   A weak cache hands the prepared image to the actual loader without rebuilding
   it, and owns no game resources after the session releases them. Discovery is
   suspended before launch so it cannot clear the content provider underneath a
   game. Driver initialization and the remaining core load still run on the GUI
   thread; their duration is logged. The outdated loose-format warning is removed.
3. **Close/stop:** closing requests asynchronous teardown and closes the frontend
   after the emulation thread finishes. Restart/subprogram transitions pump Qt
   events while waiting, allowing queued callbacks to complete. The unsafe
   `QThread::terminate()` fallback is removed. Status sampling stops before core
   teardown, and discovery restarts only after teardown. Timer connections no
   longer accumulate on every stop. A driver or guest-service deadlock remains
   possible; shutdown phase logs identify where it stalls.
4. **Amiibo:** remove the keyboard/controller hotkey entry and its action binding.
   Existing stored F2/Home+A mappings are no longer loaded. The menu remains.
5. **Memory:** queued game-list rows own their items until the model accepts them;
   cancelled rows and unused flat-list directories are released. JIT invalidation
   now removes matching range metadata instead of retaining it indefinitely.
   These fix specific retention paths, not a proof of stable MKW memory usage.
6. **Diagnostics:** discovery counts and elapsed time, RomFS build time, core-load
   time, shutdown phases, and ten-second FPS/shader/process-memory samples are
   logged. Samples and shutdown phases flush buffered output. Concurrent log-byte
   accounting uses atomics. `collect-debug.ps1` gathers text logs, optional memory
   CSV, build/profile metadata and OS/CPU/GPU driver details. It does not copy
   game files, console keys, saves or account configuration. The main log retains
   the existing size limit; use the memory CSV for long sessions with noisy logs.
7. **Defaults:** Vulkan, 1x, Fast GPU mode, asynchronous shaders, BC3,
   conservative VRAM, reactive flushing off and FIFO Relaxed. Existing explicit
   settings and per-game overrides remain authoritative; use a fresh configuration
   to verify defaults. A driver may fall back from FIFO Relaxed when unsupported.
8. **Selective Zortos integration:** see below.
9. **CI:** keep one Windows x64 GUI output with PGO; disable OpenGL compilation
   and remove translation dependencies. Cache the checksum-pinned Eden profile,
   partition compiler caches by installed dependency/profile fingerprint, reuse
   dependency downloads without saving duplicate exact hits, and report cache
   statistics in the job summary. Use available runner CPUs and lighter artifact
   compression. Hosted speed improvements need a new workflow run to measure.

## Zortos source selection

Reviewed `zortos293/Eglantine` at
`62d8525087a3332e01cd218a752db8b14de6a2a9` and the rendering/cache work in
`11a110898fcf`, `60ef6d275016` and
`badc91ebce6b6cb44eabe67f7aa0dbbc6abd9e46` against this fork.

Selected from `badc91e`:

- `src/dynarmic/src/dynarmic/backend/block_range_information.{h,cpp}`:
  indexed ranges with removal on invalidation. This replaces the baseline's
  interval map that never removed invalidated records.
- `src/core/device_memory_manager.inc`: update the current ASID after a process
  transition, so subsequent page runs are attributed to the correct process.

The bitmap batching in that commit is already present, with extra overflow
guards, in this baseline. Earlier readback/lifetime changes are already integrated
or adapted. Native pipeline-worker translation, broader texture remap/coherency
changes and unrelated platform/updater work are excluded from this batch. The
reported 25 FPS in Zortos's build is a reason to benchmark changes independently;
README performance claims are not treated as measurements for this PC.

Source: https://github.com/zortos293/Eglantine/commit/badc91ebce6b6cb44eabe67f7aa0dbbc6abd9e46

## Validation and remaining gameplay checks

- `game_discovery_test`: base/update layouts, Unicode paths, terminal ExeFS,
  recognized payload pruning, unrelated RomFS folders, missing roots and cancellation.
- `jit_ranges_test`: 40,000 randomized operations against a reference model,
  repeated invalidation, duplicate starts, full address space and clear/reuse.
- Existing uniform-stream, GPU-written-range, usage-tracker and Ampere-lowering
  integration tests.
- GitHub workflow checked with actionlint.
- Native Windows x64 Release/PGO GUI compiled successfully with clang 23.1.4.
- Portable DLL/AMD64 loader smoke check passed with the native Windows Qt platform.
  Offscreen Qt is unsuitable for this application's Vulkan-surface probing.
- Native frontend reached the game list in an isolated profile; all eight saved
  defaults matched the requested values, and F2 opened no amiibo UI. The user
  stopped Computer Use before interactive X-close testing, so that check is unverified.
- Debug bundle fixture passed and excluded the fixture's keys directory.

Run `tools/windows/record-memory.ps1` during an actual MKW session, then use
`collect-debug.ps1 -MemoryCsv <csv>` from the extracted package. Include the route,
game version and whether the shader cache was cold or warm when reporting results.
The user confirmed that the game is on a separate EPYC/NVIDIA VM, so no local
MKW gameplay is possible. Long-session gameplay, VRAM stability and
close-during-game behavior require testing there. No game, firmware or keys are distributed.

Implementation and automated validation were assisted by Codex. Human gameplay
validation and review remain required under the repository's contribution policy.
