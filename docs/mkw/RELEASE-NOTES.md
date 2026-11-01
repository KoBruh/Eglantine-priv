Experimental MKW Windows follow-up, based on snowstormxd/Eglantine mkw-windows-pgo at 0c2ecb55b1fcba5503f702e4ae09565b8ba6b21f.

Extract the entire ZIP and run **eglantine.exe**. Qt and MinGW runtime DLLs are included. No separate Visual C++ runtime is required.

Loose-game discovery no longer constructs RomFS images or sums every asset's size. Launch prepares the image on a worker with a busy dialog. Closing keeps Qt responsive through emulation teardown. Amiibo keyboard/controller shortcuts are removed. Specific game-list and JIT-metadata retention paths are fixed; long-session memory stability remains unverified.

Fresh defaults: Vulkan, 1x, Fast GPU mode, asynchronous shaders, BC3, conservative VRAM, reactive flushing off, FIFO Relaxed. Existing explicit global/per-game settings take precedence. OpenGL is omitted from this Windows-only build.

Selected Zortos JIT range cleanup and ASID fixes are included alongside the Windows branch's existing readback, fiber and GPU coherency changes. The full Zortos rendering branch is not merged. See **WINDOWS-IMPROVEMENTS.md** for the source review and attribution.

Native **Windows x64 PGO GUI**, using MSYS2 UCRT64 clang/Qt and a checksum-pinned Eden profile. This is not an MKW-trained PGO profile. No CPU-specific instruction requirement was added. Regression tests and packaged startup checks do not establish gameplay performance or visual correctness.

See **TESTING.md** for the target-VM comparison. Run **record-memory.ps1** during gameplay, then **collect-debug.ps1** to gather logs, optional memory CSV and system/build details. No game, keys, firmware, saves or personal settings are included.

Implementation and automated validation were assisted by Codex. Human review and gameplay validation remain required under the repository's contribution policy.
