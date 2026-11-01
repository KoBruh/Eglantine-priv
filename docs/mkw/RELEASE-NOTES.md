Experimental MKW 1.0 / Fast-mode integration, rebuilt from upstream adae7635708e155d967e84d7ad84bf63c5203409.

Extract the entire ZIP and run **eglantine.exe**. Includes Qt and MinGW runtime DLLs; no separate Visual C++ runtime is required. See **TESTING.md** inside the ZIP for the 60-minute comparison and fallback switches.

Selected changes address deferred tree instance state, GPU-generated command coherence in Fast mode, submitted CPU command snapshots, unaligned GPU-written uniforms, ASTC alpha and array pitches, repeated shader translation work, readback waits, and retained shader/texture/buffer/JIT/driver-command memory. Read traps remain enabled.

Native **Windows x64 PGO only**, using upstream's MSYS2 UCRT64 clang/Qt setup and Eden PGO profile. No cross compilation or CPU-specific instruction requirement was added. Small regression tests and a packaged startup smoke check run in CI. Hosted CI cannot validate MKW gameplay, visual correctness or a 40-minute memory crash; those require target-PC testing.

Source selection and attribution: docs/mkw in the source branch. This integration was assisted by Codex. No upstream PR has been opened; human review and gameplay testing are required before proposing one under upstream's contribution policy.
