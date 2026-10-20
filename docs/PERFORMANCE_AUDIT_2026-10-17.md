# Performance and stability review — 2026-10-17

Scope: SanFrameOptimizer in this directory only. Source inspection focused on
presentation, frame pacing, state suppression, deferred submission, telemetry,
worker preparation and GPU profiling. This is not an exhaustive correctness
audit of the GTA engine or external renderers.

## Implemented fixes

| Area | Finding | Change |
| --- | --- | --- |
| Limiter hitch recovery | A frame already more than `MaxCatchUpFrames` late was made to wait an additional complete interval. Sustained slow rendering periodically paid this extra delay. | Present the overdue frame without another interval of waiting and schedule the following deadline from the current time. The configured small catch-up window remains supported. |
| Limiter precision | Every interval discarded fractional QPC ticks; target changes sharing the same integer interval were not recognized. | Carry fractional ticks between deadlines and track the actual target FPS. |
| Reset recovery | Deadlines and frame statistics could span device resets. Monitor refresh cache could survive a display mode change. | Clear pacing/statistics timing before reset and when adopting a created/reset device; invalidate the refresh query cache. |
| Frame statistics | Intervals of one second or longer were silently excluded, hiding severe stalls. | Include all finite, positive intervals in the existing bounded sample window. Device reset intervals are excluded explicitly through timing reset. |
| Diagnostics | String-literal feature names and descriptions were converted to owning strings before even checking whether a state had already been reported. | Accept `string_view` and use heterogeneous map lookup. Store owning strings only when registration or a retained state/detail needs them. Mutex protection and execution counters are preserved. |
| GPU profiling | Each pending sample polled four D3D queries even when its end timestamp was unavailable. | Check the end timestamp first; skip the other three calls while it remains pending. Retain nonblocking polling with flags zero. Clear accumulated statistics on timer reset and reject null-device initialization. |
| ReFX OSD | The overlay-only Present branch omitted draw telemetry advancement and periodic logger flushing. | Advance draw telemetry and flush logs when due on that branch too. ReFX retains ownership of rendering and pacing. |

The shipped limiter remains disabled. Its timing improvements apply when
`FrameLimiter=1`; they do not automatically improve uncapped/ReFX-owned pacing.
The diagnostics and enabled profiler changes reduce overhead independently.
No default queue-depth, worker-count, shader-compatibility or rendering-quality
settings were increased.

## Special K assessment

Reviewed the [Special K repository](https://github.com/SpecialKO/SpecialK),
[framerate implementation](https://github.com/SpecialKO/SpecialK/blob/main/src/framerate.cpp)
and [D3D9 integration](https://github.com/SpecialKO/SpecialK/blob/main/src/render/d3d9/d3d9.cpp).
These are upstream main-branch references as inspected on the review date.

Its limiter preserves fractional QPC time, combines sleeping with a final busy
wait, and has explicit missed-frame handling. The fractional-time principle
was implemented independently here; SanFrame already had hybrid waiting.
Special K also has scanline/Latent Sync and swap-chain wait integration. Those
need renderer and presentation-mode coordination beyond this pacing fix;
they were not transplanted. The upstream repository declares GPL-3.0; no
Special K source was copied into this project.

SanFrame already supplies the essential local mechanisms: high-resolution
waitable timers, optional MMCSS, D3D9Ex maximum frame latency, below-refresh
capping and stock-DXVK limiter coordination. Duplicating them with another
injected limiter would require ownership coordination. ReFX-specific tuning
stays with ReFX as the existing integration requires.

## Remaining performance considerations

- State-cache invalidation around overlay state blocks, Reset and UP draw
  side effects protects correctness. Removing these barriers just to improve
  skip counts can produce stale state and incorrect rendering.
- Submission eligibility queries account for foreign state blocks/hooks.
  Replacing every getter with a local mirror needs proof that all external
  state mutations are observed.
- Worker preparation has a bounded queue and synchronous overflow fallback.
  More workers or moving GTA/D3D work there is not an established FPS gain.
- GPU pass timing remains sampled and optional. Query creation still happens
  lazily and can contribute a startup cost; the patch reduces recurring polling.

## Validation

Release compilation and linking succeeded using:

```powershell
cmake --build build --config Release --target SanFrameOptimizer --parallel 4
```

Output: `build/Release/SanFrameOptimizer.asi`, with the matching INI copied by
the existing build rule. No tests or game benchmarks were run, as requested.
FPS and percentile improvements have not been measured. Compilation does not
establish runtime compatibility with every installed mod or driver.

## Follow-up review

Additional changes from the second source review:

- Cache unchanged mesh groups that have no valid adjacent merge, avoiding
  repeated pair classification. A separate 32-entry cache retains the full
  header/resource/mesh snapshot (about 76 KiB on x86) and does not evict positive
  merge plans. Dynamic-lock streams are excluded from storage. Reset/device
  installation/uninstall invalidates both caches. `noMergeHits` is reported;
  pair-rejection counters now count evaluated pairs, excluding these hits.
- Move cached merged-descriptor copying into a noinline helper. The common
  building callback no longer needs its own 64-mesh temporary buffer. Capture
  saved-draw counts before calling stock callbacks, which may reenter and
  replace a cache slot. Share snapshot validation between both plan caches.
- Check resource entry size before optimizer-side header reads. Unsupported
  short entries stay on the original callback path with instancing ineligible.
- Cache completed texture mutation-guard installation in 64 bounded weak
  entries. Reusing a texture avoids repeated descriptor and mip-surface COM
  calls. Texture Release hooks advance the existing resource generation on
  destruction; generation mismatches and reset invalidate cached guards.
  No texture reference is retained. Existing locking/mipmap barriers remain.
- Cache dynamic-buffer rejection in the existing descriptor cache too, instead
  of repeating `GetDesc` for known dynamic buffers. Buffer destruction retains
  the existing generation invalidation. Submission memory accounting now
  includes both descriptor and texture-guard storage.
- Publish submission hook originals before replacing their vtable entries;
  publish/read the patch count atomically. A concurrent resource Release can
  therefore find its original function as soon as it reaches the replacement.
  Hook installation still belongs to the existing single render-thread path.
- Include the multiplier in the optional FX friction/wind cache key and reject
  nonfinite multipliers from caching. Calls with different multipliers can no
  longer share a cached result. This opt-in feature remains disabled by default.
- Skip DPI queries in Manual/ResolutionOnly overlay modes and client-size
  queries in Manual/DpiOnly modes, where those inputs cannot affect scaling.

The follow-up Release target also compiles and links successfully. No tests
or in-game benchmarks were run. RenderWare gains require the stock building
callback route; the existing SkyGFX exclusion remains in place.

## Third review

- Non-indexed triangle-list UP submissions now append the contiguous vertex
  range with one `memcpy` instead of a per-triangle/per-vertex copy loop.
  Existing capacity checks, indexed-range validation, strip winding, fan
  expansion and immediate UP binding side effects remain. Peak upload-byte
  accounting runs once per appended draw instead of once per vertex.
- Periodic renderer detection reuses an uninitialized path buffer populated by
  `GetModuleFileNameW`, explicitly rejecting failed/truncated paths. It parses
  filenames/extensions as views with case-insensitive comparisons. Only D3D9
  candidates require an owning filesystem path; ordinary loaded modules no
  longer allocate lowercased path/name/extension strings or clear 64 KiB each.
  Module enumeration, backend telemetry and refresh frequency are preserved.
- Shadow profiling without shadow grouping no longer constructs the grouping
  plan's 128 buckets on the stack. A noinline helper creates that buffer only
  when grouping is enabled. CPU/GPU profiling still wraps the original call.
- Optional alpha indexing reads its owner thread first and attempts an atomic
  compare-exchange only for initial ownership, removing a locked instruction
  from subsequent insertions. Foreign threads still use original insertion.
- Draw telemetry now publishes its patch count with release/acquire atomics,
  complementing its existing original-before-replacement ordering. Callback
  lookup reads one published count snapshot. This addresses concurrent reads
  during the existing single-writer installation path; it does not add support
  for unloading hooks while other threads are executing them.

The Release ASI was rebuilt successfully after these changes. No tests or
benchmarks were run. These are source-level reductions in recurring work;
their effect on average FPS and low-percentile frame times is unmeasured.
