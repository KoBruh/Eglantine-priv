MKW Windows PGO: automatic game settings and further bounded CPU optimizations.

Extract the **whole ZIP** and run **eglantine.exe**. No CMD launcher is required.

When MKW launches, the executable automatically applies **Vulkan, 1x resolution, Fast GPU mode, asynchronous shaders on, ASTC BC3, conservative VRAM, reactive flushing off and FIFO Relaxed** after saved per-game settings load. These session values override conflicting saved settings. The preset does not rewrite saved preferences, restores global setting selection after stop/failed startup, and leaves other title IDs alone. The log records the automatic preset and effective settings.

Further CPU work:

- Descriptor invalidation clears active bitmap words instead of unused preallocation. Growing the table limit clears newly exposed slots before use. Added shrink/regrowth and allocation-boundary regressions.
- Large shader-removal batches use sorted membership searches; small removals retain the linear path. Existing randomized real-cache comparisons verify the same survivors as sequential removal.

The native Windows PGO build, **87 regular regression cases**, two explicit CPU benchmark cases, and isolated portable startup check pass locally. Synthetic CPU workloads improved; **gameplay FPS on the target VM is unmeasured**. This pass retains the existing rendering, synchronization, readback and allocation policies.

CMD files remain optional helpers: MKW-Performance.cmd clears inherited detailed profiling; MKW-Diagnostics.cmd enables it. Neither is required for the game preset. The optional record-memory.ps1 and collect-debug.ps1 remain included.

See PERFORMANCE-INVESTIGATION.md and TESTING.md for source history, benchmark limits and comparisons. Matching source and checksums accompany this release. No game, firmware, keys, saves or personal settings are included.

Implementation, commit/release text and automated validation were assisted by Codex.
