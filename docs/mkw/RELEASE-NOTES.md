MKW Windows PGO conservative cache pass, continuing your existing Windows branch. Experimental prerelease; gameplay on the target VM remains unverified here.

Extract the **whole ZIP** and run **MKW-Performance.cmd**. This uses the session-only MKW 1x override and clears an inherited detailed GPU-profiler setting. Regular session diagnostics remain enabled. Use MKW-Diagnostics.cmd when detailed profiling is wanted.

This pass reduces repeated CPU bookkeeping:

- Integrates Takoroka's upstream #209: coalesce texture descriptor bitmap invalidations and keep the full diagnostic ID-poisoning walk in debug builds. Image/view removal and descriptor refresh still run.
- Adapts shader invalidation batching from ymatrix1's #237: process every memory-write range under one lock and remove pending shaders once per batch. The CPU-invalidation bypass from that proposal is excluded.
- Adds actual-cache differential tests covering descriptor mutations, table switches, repeated image deletion, overlapping shader writes, duplicate ranges and pending removals.
- Adds CPU, busiest-thread and peak GPU-engine utilization to the optional memory recorder, tested with Windows PowerShell 5.1 and PowerShell 7.
- Includes the scanner directory-type lifetime fix submitted in PR #253.

Local native PGO build and **84 regression cases** pass, as do the two explicit CPU bookkeeping benchmarks. The bitmap workload measured 467.006 to 9.899 ms and the shader workload 7.615 to 3.997 ms. These are synthetic CPU workloads, **not gameplay FPS measurements**.

This update preserves the existing rendering, GPU synchronization, readback, graphics settings and bounded cache policies. Locked 60 FPS and absence of gameplay regressions cannot be established without running the game on your VM.

See PERFORMANCE-INVESTIGATION.md for the ranked optimization ideas, source attribution and measurement limits. Run record-memory.ps1 during a comparison and collect-debug.ps1 afterward to gather the log and optional CSV. The source commit and pinned PGO profile are recorded in the package. No game, keys, firmware, saves or personal settings are included.

Implementation, selective upstream integration, commit/release text and automated validation were assisted by Codex.
