# SanFrame Optimizer

GTA San Andreas Classic PC US 1.0 rendering and frame-pacing optimizer for
Windows, Win32/x86. Created by Zeneric. The integrated FXP runtime is included
in the ASI; do not also load a standalone FXP Optimized/FxPerfSA optimizer.

## Features

- Adaptive D3D9 state deduplication with per-category controls.
- RenderWare building/day-night mesh batching, immediate draw batching,
  restricted static-prop instancing and visibility-bounds caching.
- FX particle simulation, compiled curves and matrix reuse. Experimental
  operator caches/plans and particle instancing are opt-in.
- Optional foreground/background frame caps, high-resolution timing, MMCSS,
  refresh-aware caps, render-ahead control and D3D9Ex.
- Stock DXVK tuning, ReFX backend detection and automatic compatibility gates.
- Text overlay with FPS, frame time, renderer, GPU/streaming/helper memory,
  clock and optional usage/draw counters.

SanFrame does not change asset-streaming budgets, schedule asset loading, or
provide window, resolution, aspect-ratio, FOV or HUD fixes. Memory displays
are read-only. Particle counts, update rate and authored effect quality are
preserved.

## Build

Requirements: Windows, Visual Studio C++ tools with C++23 support, CMake 4.2+
and a Windows SDK with `fxc.exe`. The current source was compiled with Visual
Studio 2026. Use a Developer Command Prompt with CMake on `PATH`.

```bat
build.bat Release
```

The default build compiles only the ASI and its dependencies. CMake downloads
pinned MinHook, Dear ImGui and SafetyHook sources; SafetyHook brings Zydis and
Zycore. See `deps.lock`. Plugin-SDK, Premake, a bundled MSBuild executable and
other mod source trees are not build dependencies.

Equivalent commands:

```bat
cmake -S . -B build -A Win32 -DBUILD_TESTING=OFF
cmake --build build --config Release --target SanFrameOptimizer --parallel
```

Outputs: `build/Release/SanFrameOptimizer.asi` and `SanFrameOptimizer.ini`.
Shaders compile during the build; no runtime shader compiler is required.
`build.bat Debug` builds the Debug configuration.

Local font embedding defaults on. Builds embed
`resources/BankGothic-Bold.ttf` when present, preserving the BankGothic overlay.
Source archives omit the local TTF; builds without it use Dear ImGui's scalable
fallback. Set `-DSANFRAME_EMBED_LOCAL_FONT=OFF` to disable embedding explicitly.

## Install and configure

Copy the ASI and INI beside `gta_sa.exe` or into an ASI-enabled Mod Loader
folder. Keep one copy of the optimizer and restart after INI changes.
Existing game-directory INIs are not automatically rewritten.

[config/SanFrameOptimizer.ini](config/SanFrameOptimizer.ini) lists the supported
settings and shipped values.

- `[Main] Enabled=0` disables the plugin.
- `[Optimization] Enabled=0` disables optimization, tuning and pacing while
  retaining the overlay. `[FXP] Enabled=0` disables only FXP.
- The frame limiter defaults off. Enable it only when SanFrame should own
  pacing; `TargetFPS` and `BackgroundFPS` select caps.
- Experimental alpha/shadow indexing, operator caches/plans and particle
  instancing default off. Enable individually after checking your renderer.
- Worker preparation and transient scratch default off; current gameplay
  paths do not submit jobs or allocations to those APIs.
- Periodic diagnostics default off. `OptimizationLogInterval` counts presented
  frames; `StatsIntervalSeconds` and `RuntimeReportIntervalSeconds` use seconds.
  Zero disables each report. `PassTiming`, `D3DStateTimeSampling` and
  `FrameStats` enable optional measurements.

Installed hooks and activity counters do not establish an FPS improvement.
Compare the same scenes and settings when tuning. Optional FrameStats logs
can be compared using `python tools/compare_framestats.py baseline.log candidate.log`.

## Compatibility

ReFX owns its private backend configuration; SanFrame tunes positively
identified stock DXVK only. For ReFX ASI factory interception, load ReFX
before SanFrame. See [ReFX/OSD integration](docs/REFX_FACTORY_OSD.md).

Proper Shaders automatically disables overlapping state/draw/visibility
optimizers and particle instancing. See
[Proper Shaders compatibility](docs/PROPER_SHADERS_COMPATIBILITY.md).

DistantStreaming selects a Present-only overlay to avoid an additional
EndScene detour. `Compatibility.OverlayPresentOnly=1` forces that behavior for
renamed or other conflicting ASIs. See
[DistantStreaming compatibility](docs/DISTANTSTREAMING_COMPATIBILITY.md).

Pure/multithreaded D3D9 devices and unsupported rendering states retain the
original draw path. Compatibility gates do not establish whole-mod-stack
visual equivalence; in-game validation remains necessary.

## Optional tests

```bat
build.bat Release --tests
python tools/test_compare_framestats.py
```

`--tests` enables the CMake-registered checks and runs CTest. Graphics checks
can skip when hardware is unavailable. Additional FXP reference/graphics
harness sources are retained under `tests/fxp`; they are not currently
registered in CMake. Historical measurements and their scope are recorded
in [validation notes](docs/VALIDATION.md).

## Source layout and packaging

`src/` and `include/` contain the implementation and public SAVO compatibility
header. `resources/` contains shader source, `cmake/` the FXP build setup,
`tests/` regression/harness sources, and `tools/` packaging and log utilities.
`docs/` contains compatibility, telemetry and performance notes.

```bat
python tools/package.py --source-only
python tools/package.py
```

The first command requires no build. The second packages source plus the
built ASI/INI. Both write archives and SHA-256 checksums to ignored `dist/`.
Packaging uses a source allowlist, excluding local fonts, caches, binaries,
logs and dependency checkouts. No game executable or assets are included.

See [third-party notices and provenance](THIRD_PARTY_NOTICES.md) and
[CHANGELOG.md](CHANGELOG.md). Dependency licenses are retained in `licenses/`;
they do not grant a project-wide license to the inherited implementation.
