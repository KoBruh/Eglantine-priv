MKW Windows PGO performance follow-up, continuing snowstormxd/Eglantine's existing Windows fork. Experimental prerelease; target-VM gameplay remains unverified.

Extract the **whole ZIP** and run **MKW-Performance.cmd**. The supplied log showed a saved 2x resolution override. This launcher applies 1x to the current MKW session without rewriting that saved preference. Qt and native runtime DLLs are included.

Changes are published in individual commits:

- Retains the earlier fast loose-game discovery, responsive preparation/teardown, amiibo shortcut removal, diagnostics and specific memory-lifetime fixes.
- Integrates seven newer normal-Eglantine sparse-memory fixes, including guest tile mapping and partial uploads of large terrain textures. Adds empty/overflow range corrections and layout regressions.
- Reuses Vulkan command-recording memory between periodic releases, with reclamation under system commit pressure. A local RTX 4070 CPU recording microbenchmark improved from 63.819 to 31.393 ms; this is **not a gameplay FPS measurement**.
- Allows a larger native buffer working set when both driver budget and system commit have room, retaining the smaller threshold under pressure.
- Reports actual driver heap budgets separately from the initial cache allowance.
- Joins the touch-calibration worker before its owner is destroyed.
- Keeps one Windows x64 Vulkan GUI PGO output, dependency/compiler caches, regression checks and an isolated portable startup check.

Requested fresh defaults remain Vulkan, 1x, Fast, async shaders, BC3, conservative VRAM, reactive flushing off and FIFO Relaxed. Existing explicit settings other than the performance launcher's session-only resolution override still apply. The pinned PGO profile is Eden's published v020525 profile, not an MKW-trained profile.

The native 80-case C++ suite and standalone regression checks pass locally. No game is available on this PC: **locked 60 FPS, visual correctness and long-session memory stability are not established**. The update preserves synchronization fixes rather than accepting missing graphics for higher FPS.

Read **PERFORMANCE-INVESTIGATION.md** and **TESTING.md**. Use **MKW-Diagnostics.cmd** for a profiling run, then **collect-debug.ps1** with an optional memory CSV. Detailed profiling can affect timing. `source-commit.txt` identifies the corresponding source; no game, keys, firmware, saves or personal settings are distributed.

Implementation, selective upstream integration and automated validation were assisted by Codex. Human review and gameplay validation remain required under the repository's contribution policy.
