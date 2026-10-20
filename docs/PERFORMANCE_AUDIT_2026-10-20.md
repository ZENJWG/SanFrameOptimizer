# Source audit, 2026-10-20

Reviewed the D3D9 submission/state-cache paths, RenderWare batching, frame
pacing, overlay, telemetry workers, FXP submission, configuration and existing
performance notes. No tests or game benchmarks were run. Source-level work
reductions do not establish an FPS improvement on a particular mod stack.

## Changes

| Area | Finding | Change |
| --- | --- | --- |
| Reset recovery | Reset releases instancing shaders, but the next frame could enqueue instances before shader recreation. Replay then discards queued geometry and permanently faults the submission optimizer. | Use normal draws until the shaders exist again. Skip shader recreation while the device is lost. Discard pending work at reset when the device cannot render, instead of replaying into a lost device. |
| Indexed-draw diagnostics | A positive report interval implicitly enabled eight state getters plus temporary COM references every 256 indexed draws. | Gate this inspection with the explicit `PassTiming` option. Counter reporting alone no longer requests these getters. |
| Batch diagnostics | Flushes sampled QPC even when adaptive batching was disabled. | Sample flush cost only when the corresponding adaptive policy is configured. Its normal adaptive behavior is preserved. |
| GPU memory display | The monitor-owning GPU took priority over the D3D9 rendering adapter, including on hybrid systems. | Rank PCI identity above monitor ownership; monitor ownership breaks ties. Existing single-adapter fallback remains. PCI IDs cannot uniquely distinguish identical GPUs without a matching monitor. |
| Cache64 telemetry | WMI failures kept retrying the same connection indefinitely, and enumeration timeout was reported as an absent helper. | Reconnect after query errors and classify timeout as unavailable. Sampling remains on the worker. |
| Default overhead | The supplied INI enabled experimental alpha/shadow/operator paths despite the documented opt-in policy, and periodic reports formatted large diagnostic blocks. | Disable those experimental paths and periodic reports by default. Keep particle instancing opt-in, matching the README's compatibility guidance. |

## INI cleanup

The shipped file goes from 241 to 205 lines and from 49 to 17 comment lines.
Retained comments explain units, enum values, compatibility or meaningful
tradeoffs. Removed the ignored `ProperShadersVisibilityOptIn` option, the
redundant `D3D9ExMaxFrameLatency` fallback, and the DXVK `Profile` entry that
does not change the current tuner's queue-depth calculation. The legacy latency
fallback remains accepted; ignored settings no longer have runtime fields.
`MaxFrameLatency=-1` uses
`Performance.MaxRenderAheadFrames`.

The header and INI-loader fallbacks now also default periodic reports,
particle instancing and interval FXP statistics off. Existing explicit INI
values still take priority. Core D3D9 state deduplication, RenderWare batching,
immediate batching, static-prop instancing and visibility caching remain
enabled in the supplied profile. Overlay choices and the existing limiter,
VSync, queue-depth and DXVK tuning values are preserved.

INI comment removal improves readability; it is not a per-frame speedup.

## Remaining performance considerations

- Static instancing still performs substantial state inspection to reject
  incompatible draws. Replacing these getters with an incomplete local state
  mirror can corrupt rendering when another mod changes state. Retain the
  safety checks; assess `StaticPropInstancing=0` against representative scenes
  before attempting a larger redesign.
- Visibility caching compares object identity, collision bounds and pose on
  every eligible call. Its gain depends on reuse and scene composition.
- The shipped limiter remains off so an existing renderer/limiter retains
  pacing ownership. Use a single pacing owner and a sustainable cap when
  tuning frame-time stability. No new FPS cap is imposed by this audit.
- Worker preparation and transient scratch have no current gameplay callers;
  enabling them or increasing worker count cannot accelerate those paths.
- Experimental switches remain available for deliberate comparisons. The
  prior `docs/VALIDATION.md` measurements do not justify enabling every switch.

Original edited files and local source archives were retained outside the
repository during source cleanup.

## Compilation

Release compilation and linking succeeded with the installed Visual Studio
CMake and MSVC tools. The existing build cache referenced a removed standalone
CMake installation; its tool paths were updated to the installed copy and
the project was configured with `BUILD_TESTING=OFF` and
`FETCHCONTENT_UPDATES_DISCONNECTED=ON`.

Built only the `SanFrameOptimizer` target. Outputs are
`build/Release/SanFrameOptimizer.asi` and the matching
`build/Release/SanFrameOptimizer.ini`. No test executables, CTest run or in-game
benchmark was requested or performed.
