# Performance Optimizer

CPU-side optimizations for Storm.exe v1.0.147 that leave the rendered image unchanged. The game runs on the
native Direct3D 9 runtime; every code patch is located by a byte signature and installed only after the
protected game code has been decrypted.

- **GPU selection.** Adds `GpuPreference=2` (high performance) to the Windows per-app graphics settings of
  `Storm.exe`, the same entry *Settings > Display > Graphics* writes. A GPU choice already made there is kept,
  and so are the other settings Windows stores in that value. Like a choice made in Settings, the entry stays
  after the mod is disabled.
- **Particles.** The per-particle oscillation used two `fsin` and two `fcos`; it now uses sum-to-product
  identities with one SSE2 sine/cosine per particle. The velocity damping evaluated `exp()` with
  `f2xm1`/`fscale` per particle; the factor is now computed once per emitter and frame.
- **Sorting.** The Shell sort used for render and particle depth ordering keeps its exact algorithm, because
  equal keys are common and their order decides the blending order. It now runs on contiguous key/index pairs
  without the x87 `fnstsw` stall per comparison; the result is identical to the game's.
- **Math.** 4x4 matrix multiply and transpose run on SSE, and the renderer's vertex-constant filter compares
  its shadow copy with SSE2 instead of a byte-wise `repe cmpsb`.
- **Physics grid.** `CPhysSpace::FillIndexes` had a fixed 1024-cell buffer: larger queries silently dropped the
  far rows and wrote one line to `error.log` per call. Oversized queries are now trimmed symmetrically around
  their centre and nothing is logged. This only happens with `VisibleRadius` above the in-game maximum (320).
  Cell bounds are computed in the game's single-precision order, so normal queries return the same cells.
- **Audio.** Engine sounds set their pitch every frame, usually to the current value; the call is skipped when
  `GetFrequency` already matches. The 3D listener commit runs on a fixed 60 Hz schedule (deferred values
  persist until the next commit). DirectSound reopened the same registry keys on every buffer call; keys it
  opens from predefined roots are cached for the session, and missing keys are rechecked once a second.

Matrix and particle math run in SSE instead of the game's single-precision x87 mode, so a few results can
differ in the last bit; the rendered image does not change visibly.

## Configuration

`PerformanceOptimizer.ini` switches each group on (`1`) or off (`0`); a missing key counts as on:

```ini
[Optimizer]
HighPerformanceGpu=1
FastParticles=1
PhysicsGridFix=1
FastMath=1
FastAudio=1
```

## Benchmarking

Measurements use the separate development mod `Mods\StormBench` (disabled in `Mods.ini`), driven by
`tools\bench.ps1`:

```powershell
.\tools\bench.ps1 -Label after -Track 0 -Duration 15 -DisplayMode 1
```

The script enables StormBench for one run, turns VSync and the FPS limit off, and restores both ini files
afterwards. `STORM_OPTIMIZER` (`-Optimizer`) disables single groups for comparisons: `nogpu`, `noparticles`,
`nogrid`, `nomath`, `noaudio`.
