# SPEC-1-SanFrameOpt measurement-first optimization pass

## Background

SanFrameOpt already has D3D9 state deduplication, RenderWare building callbacks, static-prop instancing, immediate batching, optimized FX, and visibility-bounds caching. The associated `docs/VALIDATION.md` documents several experiments that were slower or break-even. This patch prioritizes measured overhead reduction without relaxing rendering-order and compatibility protections.

This is a **source-level implementation**, not a measured FPS claim. The Linux portable tests run, but the Windows x86/Direct3D9 game build and GTA SA + ReFX DXVK runtime validation have **not** been performed here.

## Requirements

**Must**
- Sample CPU and asynchronous GPU work separately without `D3DGETDATA_FLUSH` or waits; report scope, coverage, missed/pending query results, and limitations.
- Cache immutable D3D9 vertex/index-buffer descriptions only while resource lifetimes are provable. Preserve getter-based validation and existing fallbacks if state tracking is incomplete.
- Bypass demonstrably low-yield batching conservatively and reopen probes.
- Avoid `DISCARD` per instanced draw: append to larger dynamic vertex-buffer rings with `NOOVERWRITE`; `DISCARD` on wrap.
- Cache stock RenderWare adjacent-mesh merge plans only when original resource/header/mesh content still matches.
- Optionally sample optimized vs. passthrough setter CPU costs and decide independently for the current renderer.
- Preserve pure/multithreaded-device restrictions, render order, compatibility backoffs, reset behavior, and stock-pass fallback.

**Should**
- Include a compact startup/OSD eligibility summary and explanations of skipped paths in verbose logs.
- Report visibility bounds cache's *total hook cost* and comparison against stock path.
- Keep known-regressive shadow-group indexing and operator-plan experiments disabled by default.

**Could**
- Add verified game-logic and reflection-specific pass boundaries if precise stock/ReFX callback identification becomes possible.
- Optimize opaque-world grouping only once profiling establishes it is worthwhile.

**Won't in this pass**
- Change alpha ordering, assume shader/material equivalence, enable known-regressive experimental optimizations, or claim benchmark results without GTA runtime measurements.

## Method

### Instrumentation

`src/PassProfiler.cpp` samples frame-to-frame CPU interval, stock PC and day/night RenderWare callbacks, and the stock shadow routine (when its signature matches). QPC measures CPU time; `src/fxp/renderer/GpuTimer.h`'s asynchronous disjoint/timestamp ring measures GPU time. Each timer polls `GetData(..., 0)`; it never forces flushes or waits. FX manager GPU scope instrumentation is activated through its existing profiler when `PassTiming=1`.

**Limitations:** Frame interval includes game logic, synchronization, presenting and other work, so it is *not* game-logic time. The PC/DN timer covers callbacks only when stock hooks are installed; ReFX/SkyGFX replacement pipelines can bypass them. When ReFX uses the native Vulkan overlay-only route, the D3D9 drawing optimizers and new pass timer do not run; startup availability marks them auto-disabled rather than pretending an enabled INI switch is effective. GPU timer results may remain pending or be dropped. There is no independently proven reflection or game-logic GPU-pass boundary in this patch. `SkygfxPassProfiling` remains a separate existing observation path.

### Resource validation and batching

- `RenderSubmission.cpp`: 64 weak-resource descriptor entries cache **non-dynamic** VB/IB `GetDesc` results. Hooked COM `Release()` events bump an epoch at actual destruction; the next lookup clears stale descriptors. A failed lifetime hook falls back to the original ineligible path. No descriptor lookup relies merely on an address that might have been recycled. The already-acquired vertex declaration is passed into the bounded instanced-layout cache, removing a redundant device getter.
- The retained per-draw D3D9 binding reads (`GetTexture`, `GetStreamSource`, `GetIndices`) protect against state blocks, third-party hooks and incomplete binding interception. They have **not** been removed.
- Separate immediate-UP/instance adaptive windows measure submitted draws, avoided draws, singleton batches, flush reasons, and sampled CPU flush time. Bypass requires sufficient observations, fewer than 5% avoided draws, at least 75% singleton flushes, and sampled average flush work above 3 µs. The path is reopened for periodic sampling; ordering and compatibility gates are unchanged. This is a conservative heuristic, not a randomized end-to-end frame-time A/B.
- Static-instance matrix uploads use a `128 * 16`-matrix ring. FX instance uploads use a `8192 * 4`-instance ring. Both append using `NOOVERWRITE`, use a new stream offset, and `DISCARD` at initial use/wrap. Reset clears cursors and resources.

### RenderWare plans

A bounded 32-slot content-validated positive cache stores only eligible merged runs. Each lookup checks the original resource-entry ownership/size, full resource header, full mesh sequence and primitive limit. It rejects resource entries with dynamically locked streams. Cache entries clear on installation, device reset and uninstall. Both stock pipeline callbacks remain authoritative for submission.

### State and visibility comparison

`D3DStateTimeSampling=1` enables optional sampled QPC comparisons over separate optimized and direct-pass-through windows, per current D3D device/backend, for render-state, texture, sampler and texture-stage categories. An observed setter advantage above 5% is needed to keep the category enabled after a complete paired sample. Count-based adaptive control and the cached-state invalidation barriers remain in effect. No GPU or end-to-end FPS gain is implied by setter microbenchmarks.

Visibility cache now logs sampled **complete callback** CPU time for optimized and original-stock calls, with distinct sample counts and sampled hit count; results are only heuristics because scenes differ over time.

### Availability reporting

A single startup summary uses registered optimization state, including `OFF`, `ACTIVE`, `EFFECTIVE`, `SKIPPED`, and `AUTO-OFF`. Tracking is on independently from verbose `IniDiagnostics` so the startup summary works with default settings. Set `[Overlay] ShowOptimizationStatus=1` for a compact status line; use `IniDiagnostics=1` for additional details/reasons.

## Implementation

Files changed in this patch:
- `src/PassProfiler.cpp`, `include/mws/PassProfiler.hpp`
- `src/D3D9Hook.cpp`, `src/EnginePass.cpp`, `src/RenderWareBatcher.cpp`, `include/mws/RenderWareBatcher.hpp`
- `src/RenderSubmission.cpp`, `include/mws/UploadRing.hpp`, `src/fxp/renderer/RenderStream.cpp`
- `src/D3D9StateCache.cpp`, `src/EngineOptimizations.cpp`
- `src/Diagnostics.cpp`, `include/mws/Diagnostics.hpp`, `src/Overlay.cpp`, `src/Plugin.cpp`, `src/FxpOptimizer.cpp`
- `src/Config.cpp`, `include/mws/Config.hpp`, `config/SanFrameOptimizer.ini`
- `CMakeLists.txt`, `tests/UploadRing.cpp`.

New INI controls under `[Optimization]`:

```ini
; Optional timing diagnostics; normally off.
PassTiming=0
RenderWarePlanCache=1
AdaptiveBatching=1
AdaptiveBatchWindow=600
AdaptiveBatchProbe=240
D3DStateTimeSampling=0
D3DStateTimeWindow=300
```

Under `[Overlay]`:

```ini
ShowOptimizationStatus=0
```

Existing `ShadowGroupIndex=0`, `AlphaListIndex=0` and higher-risk experimental operator settings remain off.

## Milestones

1. **Done — source patch:** all six optimization priorities plus startup status and visibility-timing telemetry have source-level implementations.
2. **Done — portable check:** `UploadRing` correctness is exercised by a 100,000-reservation wrap/overlap test (see `tests/UploadRing.cpp`).
3. **Required on Windows — compile:** use the project's documented x86 Visual Studio 2026 setup:

   ```bat
   cmake -S . -B build -A Win32 -DBUILD_TESTING=ON
   cmake --build build --config Release --parallel
   ctest --test-dir build -C Release --output-on-failure
   ```

4. **Required in game — correctness:** GTA San Andreas saved scenes across stock D3D9, DXVK/ReFX, and SkyGFX/ProperShaders combinations, including reset, alt-tab and device loss, weather/fog, water/reflections, transparent objects, shadows and particle storms.
5. **Required in game — performance:** compare per-feature controlled A/B with identical saved states and camera path; perform warm-up and at least 5 repeated runs per setup; record average and P99 frame time, CPU cost, GPU timings including pending/dropped counts, and visual reference screenshots.

## Gathering Results

Use the existing validation fixture workflow (`docs/VALIDATION.md`). Record GPU/backend, exact build/settings, pass eligibility summary, feature toggles, before/after frame-time distributions, draw counts and fallback events. Make claims only when the gameplay frame-time measurements are statistically repeatable and image correctness holds. Skip or disable any backend with unstable state, crashes, incorrect rendering or significant frame-time regression.

### Profiling documentation

- [Accurately profiling Direct3D API calls](https://learn.microsoft.com/en-us/windows/win32/direct3d9/accurately-profiling-direct3d-api-calls)
- [Direct3D 9 performance optimizations](https://learn.microsoft.com/en-us/windows/win32/direct3d9/performance-optimizations)

