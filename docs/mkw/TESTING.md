# Mario Kart World Windows validation

Experimental Windows integration focused on MKW, **Fast GPU accuracy** and Vulkan.
The target system is the user's EPYC VM with 64 GiB RAM and an NVIDIA GPU reported
as a 5080 with 45 GiB VRAM. Use the driver's reported memory budget in the log and
the dedicated/shared memory CSV counters when evaluating this virtualized system.
It is built natively on Windows with upstream's UCRT64 clang PGO toolchain. The
profile is Eden's published `v020525` profile, not a newly trained MKW profile.

Launch **eglantine.exe** directly. MKW automatically receives Vulkan, 1x, Fast,
asynchronous shaders on, BC3, conservative VRAM, reactive flushing off and FIFO
Relaxed after per-game settings load. The preset overrides those saved values for
the session without rewriting saved preferences. Other title IDs are unchanged.

The supplied session log used a saved **2x** resolution override. The native
preset now enforces **1x for MKW**, with no launcher or environment variable needed.
Use the same effective 1x setting in the normal Eglantine comparison.
`MKW-Diagnostics.cmd` optionally enables the detailed GPU profiler; use it for
debugging, then return to the executable for timing. See
`PERFORMANCE-INVESTIGATION.md` for findings and the local recording benchmark.

The performance launcher now clears an inherited detailed-profiler environment
setting. The Vulkan startup log states whether the detailed GPU profiler is on.
For the 40–55 FPS follow-up, compare the same warm-cache route using the executable.
`record-memory.ps1` also records process CPU, busiest-thread CPU and the busiest
reported GPU engine. Blank GPU fields mean counters were unavailable; the first
row has no CPU delta. The GPU number is one engine's peak, not total GPU usage.

This follow-up starts at your existing Windows branch commit `0c2ecb55b1fc`.
See `WINDOWS-IMPROVEMENTS.md` for the inspected baseline and selected Zortos patches.

Check the new frontend behavior first: start with a deeply nested extracted-game
library, confirm the entry appears without enumerating RomFS assets, launch it,
and test close/stop/relaunch both while paused and while running. The size column
intentionally says **Not scanned** for loose dumps. F2 and Home+A must not open amiibo.
Keep `eden_log.txt` and `eden_log.txt.old.txt`, especially after a failed close.
Use `collect-debug.ps1 -MemoryCsv mkw-memory.csv` to gather a report after testing.

Extract the **whole** ZIP into a new directory and run `eglantine.exe`. Use a copy
of your existing configuration and saves. Do not change the game update, driver,
resolution or settings between the upstream and integration runs. Start with your
reported Fast configuration; leave GPU read tracking enabled. Frame generation
should be off while comparing real frame pacing. BC3 recompression is a useful
second comparison if you currently use uncompressed ASTC; record that setting.
The automatic BC3 threshold is raised to a driver-reported 20 GiB budget;
`EDEN_NX2_KEEP_ASTC=1` keeps uncompressed ASTC for a controlled comparison.

## Compare against the pinned upstream build

Base: `adae7635708e155d967e84d7ad84bf63c5203409`.

1. Use the same MKW version and run the same route/courses on each build for **at least 60
   minutes**. Include the course transitions and foliage where you saw corruption.
2. Record a first pass with an empty *copy* of the shader cache, then a warm-cache
   pass. Keep the original cache. Separating these distinguishes compilation
   hitches from continuing readback / texture-cache stalls.
3. At 0, 10, 20, 40 and 60 minutes record process private bytes, working set,
   dedicated/shared GPU memory, and frame-time percentiles if your existing
   capture tool provides them. Track the trend through repeated courses, not
   only peak FPS or the first race. `record-memory.ps1` automates memory sampling.
4. Check trees/foliage, drifting effects, collision, lighting, frame order and
   course transitions. Higher FPS with missing rendering is a failed result.
5. Keep the log, the CSV, the source commit in the ZIP, the game version and your
   actual driver-reported VRAM total. Do one Accurate-mode comparison on the same
   route if Fast still has missing objects. This distinguishes the Fast command
   path from shader/texture bugs that affect both modes.

## Controlled fallback comparisons

Change **one** variable at a time, then restart the emulator:

```powershell
$env:EDEN_NX2_COMMAND_POOL_RELEASE = '0' # original driver command-memory reuse
.\eglantine.exe
```

The new default releases recording memory every eight uses. For the previous
fork's every-reuse behavior, use `EDEN_NX2_COMMAND_POOL_RELEASE_INTERVAL=1`.
Remove these variables after the comparison.

`EDEN_NX2_UBOREUSE=0` disables small uniform allocation reuse.
`EDEN_NX2_PB_SNAPSHOT=0` disables submitted CPU command snapshots; GPU-written
command reads remain synchronized. Remove these variables for the default build.
Do not disable read traps as a performance test unless also checking correctness:
upstream issue #180 reports lost collision with that workaround.

## What the automated checks establish

- Standalone tests use the actual uniform reuse, GPU-written bitmap/range and
  buffer usage tracker headers, plus actual Ampere lowering with an FSEL/SEL reference interpreter. They cover mutation, scheduler/session reset,
  remapping, partial pages, integer overflow and 100,000 interval-oracle queries.
- The native Windows build compiles and links the GUI, packages Qt/plugins/DLLs,
  checks the AMD64 PE header and starts the packaged GUI without MSYS2 on PATH.
- A hosted Windows runner does **not** provide this NVIDIA GPU or the game.
  Its startup check is a loader/liveness check, not a successful game boot.

No 60-minute gameplay result or measured FPS improvement is claimed until tested
on the target machine. The integration does not implement full Ampere mesh/task
support. Upstream hardware emulation gaps may still cause missing graphics.
