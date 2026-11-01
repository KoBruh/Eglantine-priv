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

Extract the whole ZIP and launch **MKW-Performance.cmd**. It applies a session-only
1x override to MKW program `0400C3F00006E000`, after saved per-game settings load.
It does not rewrite your saved resolution. Other requested defaults remain
Vulkan, Fast, asynchronous shaders, BC3, conservative VRAM, reactive flushing off
and FIFO Relaxed; check explicit overrides in an existing profile.

Use **MKW-Diagnostics.cmd** only for a diagnostic run: it also enables the existing
fine-grained GPU profiler, whose timing overhead may affect results. Collect the
result using `collect-debug.ps1`, optionally with the memory CSV. Regular session
samples remain enabled in the performance launcher. See `TESTING.md` for the
controlled route and warm/cold shader-cache comparison.

For a single-variable comparison, set
`EDEN_NX2_COMMAND_POOL_RELEASE_INTERVAL=1` before running the performance launcher
to restore the previous release cadence. `EDEN_NX2_COMMAND_POOL_RELEASE=0`
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
