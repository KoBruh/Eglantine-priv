# MKW performance investigation, 1 November 2026

This continues `snowstormxd/Eglantine` at `0c2ecb55b1fc`, including the previous
Windows fixes. It does not replace the fork with Zortos's branch. The supplied
session log was inspected locally; the private log is not distributed.

## What the log establishes

| Observation | Implication |
| --- | --- |
| Discovery finished in 29 ms | The previous discovery fix worked in this session. |
| Saved `Renderer.resolution_setup: Res2X` | The session used 2x despite the new 1x default. Doubling both dimensions can quadruple pixel work for scaled targets. |
| After 180 s, 28 samples with no active shader builds average 46.99 FPS, range 29.01–60.05 | Continuing slowdown remains after initial compilation. These are periodic snapshots, not whole-session averages or frame-time percentiles. |
| Runtime shader compilation usually contributes tens of milliseconds per ten-second reporting window | Initial shader hitches exist, but compilation alone does not explain the continuing slowdown. |
| Fifteen shader workers on sixteen host threads; separate GPU front/back threads already active | Simply adding threads is not an established fix. |
| Buffer cache repeatedly near 507–510 MiB, with frequent texture deletion | The fixed cache threshold and resource churn deserve attention even with substantial free VRAM. |
| Driver budget about 47 GiB, while the old startup label said 8 GiB | The old label described an initial cache allowance, not physical VRAM. Driver budgets must be logged separately. |
| Private memory rises to roughly 20.7 GiB and later levels off/falls | This recording does not establish an unbounded leak. Peak memory is still significant. |
| Teardown completes in about two seconds | This session does not reproduce the previous stuck-close problem. |

Mostly negligible presentation-fence waits suggest that presentation is not the
dominant recorded wait. They do not prove that all GPU work is cheap. Likewise,
the logged `speed=1.000` reflects the multicore guest clock and does not establish
60 rendered frames per second. A locked 60 FPS requires completing each actual
frame within approximately 16.67 ms, including translation and synchronization.

## Why the console and a large desktop GPU differ

Switchbrew's hardware research describes eight ARM Cortex-A78C cores, 12 GB of
LPDDR5X and an Ampere GPU in the T239. The emulator must translate ARM execution
and guest GPU commands for a different CPU/driver/memory system. Sparse mappings,
texture swizzling, shader translation and CPU/GPU ordering all consume host time.
A larger GPU cannot remove an ordered CPU dependency or an unnecessary upload.
The console's shared memory does not map directly onto separate host RAM/VRAM.
[Hardware research](https://switchbrew.org/wiki/Switch_2:_Tegra_T239).

NVIDIA documents dedicated Tensor/RT hardware and DLSS on Switch 2. That does
not make desktop Tensor/RT acceleration an automatic replacement for this
emulator's shader and command translation. This update does not add DLSS or frame
generation, and makes no claim that generated frames improve emulation speed.
[NVIDIA architecture overview](https://blogs.nvidia.com/blog/nintendo-switch-2-leveled-up-with-nvidia-ai-powered-dlss-and-4k-gaming/).

## Comparing normal Eglantine with this fork

The normal `nx2` branch was inspected through `6bde8c2`, beyond this fork's pinned
`adae763` base. Seven relevant upstream sparse-memory changes are integrated as
individual attributed commits: `4d6a13e`, `139b7e5`, `a855e83`, `1c794af`,
`130c255`, `3d61564`, and `6bde8c2` in `Djihads80/Eglantine`.

They implement the game's nx2:gpu 216 tile mappings, use the backing supplied by
the guest, track mapped tiles, and upload mapped areas of large sparse 2D images.
AMD backing/zero-block fallbacks are retained. The merge also preserves this
fork's existing layered-texture fallback and coherency fixes. A regression test
demonstrates that a mapped 64 KiB portion of a 256 MiB terrain mask prepares less
than 1 MiB of upload data; this is a layout test, not a game benchmark.

New tests found two upstream boundary defects: an empty range could select a
tile, and an overflowing range end could miss the image. Both are corrected with
empty-range rejection and saturating endpoints. Four hundred randomized mip
layouts verify coverage, bounds and non-overlap across several texel sizes.

The supplied log does not include a matching normal-Eglantine run. It therefore
cannot isolate the reported full-speed difference. Saved 2x resolution, caches,
game version, VM scheduling and code differences are confounding factors. This
fork also pays synchronization costs to preserve GPU-written commands and avoid
missing objects; those protections remain. Reintroducing known rendering or
collision failures would not establish a successful optimization.

## Additional host optimizations

**Vulkan recording memory:** the previous Windows path released command-buffer
recording resources on every reuse. The new path reuses them and releases every
eight uses, with aggressive release when available system commit falls below
4 GiB. Reset still occurs only after the resource pool confirms GPU completion.
It does not reset an entire pool containing other pending buffers. Khronos
documents the CPU cost of repeated allocation and the benefit of recycling.
[Command buffer recycling](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html).

The reproducible `tools/integration/vulkan_command_reuse_bench.cpp` measured
512 recordings of 2,048 fill commands on the local RTX 4070:

| Release interval | Total CPU recording time | Microseconds/buffer |
| --- | ---: | ---: |
| Every reuse | 63.819 ms | 124.646 |
| Every eight uses | 31.393 ms | 61.315 |
| Every sixteen uses | 31.444 ms | 61.413 |
| Never release explicitly | 31.980 ms | 62.462 |

No commands are submitted in this microbenchmark. These numbers establish a
local driver recording cost; they do not predict an equivalent gameplay gain
or measure the remote 5080 VM. Periodic reclamation retains a memory safeguard.

**Buffer working set:** native-title buffers can now use a 1 GiB collection
threshold when the driver budget is at least 12 GiB, budget headroom is at least
2 GiB and free system commit is at least 8 GiB. Otherwise the previous 512 MiB
threshold applies. Aggressive collection begins at twice the selected threshold.
These are collection thresholds, not hard allocation limits. This avoids evicting
useful geometry solely because the VM has crossed a fixed small-card threshold.
The policy uses the existing driver-budget accounting, including allocator
overhead; it does not treat reported shared GPU memory as free dedicated VRAM.

**Worker lifetime:** broader testing exposed a detached touch-calibration worker
accessing its owner's event after destruction. The owner now joins it during
teardown. Thirty-two cancellation iterations verify worker captures are released.
This is a specific lifetime fix, not evidence that all memory leaks are gone.

## Running this package

Extract the whole ZIP and launch **eglantine.exe**. When MKW program
`0400C3F00006E000` launches, the executable applies Vulkan, 1x, Fast, asynchronous
shaders, BC3, conservative VRAM, reactive flushing off and FIFO Relaxed after
saved per-game settings load. Those session values win over saved overrides.
Saved preferences are not rewritten by the preset, global settings are restored
after stop or failed startup, and other titles are unaffected. No CMD is needed.

Use **MKW-Diagnostics.cmd** only for a diagnostic run: it also enables the existing
fine-grained GPU profiler, whose timing overhead may affect results. Collect the
result using `collect-debug.ps1`, optionally with the memory CSV. Regular session
samples remain enabled in the executable. See `TESTING.md` for the
controlled route and warm/cold shader-cache comparison.

For a single-variable comparison, set
`EDEN_NX2_COMMAND_POOL_RELEASE_INTERVAL=1` before running the performance launcher
or the executable to restore the previous release cadence. `EDEN_NX2_COMMAND_POOL_RELEASE=0`
disables explicit releases. Remove the variables to restore the default cadence.

## Validation and limits

Native Windows x64 PGO GUI and the 80-case C++ suite compile and pass locally.
Standalone cache, command-range, discovery and policy checks pass. The portable
AMD64/Qt loader is checked separately with MSYS2 removed from PATH. The profile
remains Eden's checksum-pinned `v020525` profile, not an MKW training run.

The game and target VM are unavailable here. Sustained 60 FPS, image correctness,
long-session memory stability and close-during-game remain unverified. No raw
private log, game, firmware, keys, saves or personal settings are included.
Implementation, integration and automated validation were assisted by Codex.

## Conservative cache pass after the 40–55 FPS report

The follow-up report says rendering is correct but remains around 40–55 FPS.
There is no new session log or access to the VM, so the present limiting stage
is still unmeasured. This pass targets repeated CPU bookkeeping and retains the
existing rendering, readback and synchronization rules.

| Candidate | Decision |
| --- | --- |
| Repeated texture descriptor invalidation during image deletion | Integrate upstream #209; invalidate immediately, clear the bitmap once before its next read. Keep debug poisoning in debug builds. |
| Repeated shader-cache locking and removal for a list of memory writes | Batch the existing invalidations under one lock, removing pending shaders once at the end. Every supplied range is still processed. |
| Detailed profiler accidentally active during normal play | The performance launcher clears an inherited `EDEN_NX2_GPU_PROFILE`; the diagnostics launcher enables it explicitly. The log reports the actual profiler state. |
| CPU translation / VM scheduling / GPU execution bottleneck | Add process CPU, busiest-thread CPU and peak GPU-engine counters to the optional recorder before choosing more invasive changes. |
| More aggressive DMA method batching | Defer. The reviewed proposal excludes active split-thread mode, which this fork uses; changing engine dispatch without game validation has wider correctness implications. |
| MKW-specific PGO training | Needs an actual game workload. The existing checksum-pinned Eden profile remains in use. |
| More overlapping readbacks, different barriers or larger unbounded caches | Needs GPU timing and sustained visual/memory testing. No such behavior change is included. |

The texture change is attributed to Takoroka's
[upstream #209](https://github.com/Djihads80/Eglantine/pull/209).
The shader batching is adapted from ymatrix1's
[#237](https://github.com/Djihads80/Eglantine/pull/237), selecting the batching
only. Its unrelated CPU-invalidation bypass is not included. Upstream benchmark
claims are not treated as measurements for this VM.

The descriptor ID lookup always reads the descriptor table first. After deletion
or scale invalidation that read must report a fresh descriptor, so the ID is
rebuilt before it can be used. Release builds omit only the expensive diagnostic
walk that poisons every cached ID; the table invalidation and actual image-view
reference removal still run. Debug builds retain that diagnostic walk.

New regression tests compare 40,000 descriptor operations against eager refresh,
including descriptor mutations, address/limit changes and repeated invalidations.
A five-channel test repeatedly invalidates all tables before checking that every
cached view receives the current generation. Shader tests compare the actual
batched and sequential caches through 600 randomized rounds with overlapping,
duplicate and cross-page writes, including pending removals and empty batches.
The complete native suite passes 84 cases; two explicit CPU benchmarks also pass.

Local PGO-build CPU measurements, not gameplay frame times:

| Synthetic workload | Previous bookkeeping | New bookkeeping |
| --- | ---: | ---: |
| 2,000 batches, 50 invalidations per batch, five descriptor tables | 467.006 ms | 9.899 ms |
| 20 batches of 768 shader writes with 1,024 cached shaders | 7.615 ms | 3.997 ms |

The descriptor benchmark covers bitmap invalidation only, not the removed
diagnostic map walk or GPU rendering. It uses an indirect call to prevent the
compiler from combining repeated invalidations itself. These timings cannot
establish a gameplay improvement or locked 60 FPS.

`record-memory.ps1` now records CPU core-equivalent utilization (100% means one
fully occupied logical CPU), utilization normalized across logical processors,
the busiest sampled thread, and the busiest reported GPU engine with its name.
The GPU value is a single engine's peak, not total GPU utilization. Unsupported
GPU counters remain blank. The first row has no CPU delta. Use this alongside the
log's FPS and detailed-profiler samples to distinguish hypotheses; low GPU-engine
utilization alone does not establish a CPU bottleneck. The recorder is compatible
with Windows PowerShell 5.1 and PowerShell 7.

The fork also receives the directory-type snapshot fix from upstream PR #253,
preventing the scanner from reading a directory row after a flat view discards it.
This is a lifetime correction, not an FPS optimization.

## Automatic preset and bounded CPU optimizations

The profile now lives in `Settings::ApplyMarioKartWorldPreset` and is called
unconditionally after reading per-game configuration, before creating the render
target and loading the core. The old `EGLANTINE_MKW_1X` environment condition is
removed. Tests apply conflicting global/per-game preferences, verify all eight
effective settings, restore the original globals, reapply, and check unrelated
program IDs. Failed or cancelled startup also restores global setting selection.

Two further CPU bookkeeping reductions preserve the same cache decisions:

- Descriptor tables clear the bitmap words exposed by their current limit,
  rather than spare preallocation. A later limit increase clears the newly
  exposed words before they can be read. Tests cover 64-bit word boundaries,
  preallocation boundaries, shrinking to zero and regrowing beyond one million
  entries, plus the existing 40,000-operation comparison.
- Shader-removal batches larger than sixteen sort the removed pointer list and
  use binary search while pruning storage. Small batches retain the linear scan.
  This reduces large-batch membership work from O(N*M) to O(M log M + N log M).
  The randomized real-cache comparison still matches sequential small removals.

The native PGO GUI and all **87 regular cases** pass, along with the two explicit
CPU benchmark cases. In the latest synthetic run, eager versus deferred active
descriptor invalidation measured 457.809 versus 0.523 ms; sequential versus
batched shader invalidation measured 7.370 versus 1.934 ms. These are the same
synthetic workload shapes described above, not game frame times. They do not
establish a gameplay FPS improvement. Rendering, synchronization, readback and
cache allocation policies retain their existing behavior.
