# Validation — 2026-10-13

These are historical results for the revisions named below, not a report of
checks performed during the current cleanup. The current CMake configuration
registers the tests declared in `CMakeLists.txt`; retained `tests/fxp` harness
sources are not currently registered. Periodic diagnostics now default off.

## 1.4.1 native optimization removal and runtime diagnostics

This revision was compiled as a Win32 Release ASI. Tests and GTA were not run,
as requested. Earlier test and benchmark results below describe earlier builds.
Native Vulkan policy/statistics exports are no longer called. Backend detection
and OSD compatibility callbacks remain, without native pacing or engine
scheduling. Runtime coverage defaults to 30-second snapshots and a final
session report. Counters prove observed activity/work reuse only, with explicit
notes where outcome counts are not measured; they do not prove FPS gains.

## 1.4.0 engine experiments

Win32 Release passed all 12 CTest checks locally with no skips, with both local
font embedding enabled and disabled. The expanded
native test uses the user's local US 1.0 executable in an isolated test process;
game startup and graphics/resource loading are not executed. GTA Reversed was
consulted for layouts and call relationships at commit
[`04c5b6643a9059e879f6015ec02e618f141fb27a`](https://github.com/gta-reversed/gta-reversed/tree/04c5b6643a9059e879f6015ec02e618f141fb27a/source/game_sa).
Implementations retain native routines where practical and gate hooks using
bytes inspected in the local executable. Research downloads and executable
bytes are excluded from source/release packages.

The native harness checks:

- 512 friction/wind cases, including particle-time bypass. Largest observed
  velocity error was `1.90734863e-6`; non-velocity bytes stayed unchanged.
  Wind changes, project invalidation and foreign-thread fallback are checked.
- 512 movement/render plans, matching stock output byte for byte, including
  untouched padding/default fields. Operator type mutation and custom callback
  fallback are checked.
- All intermediate list orders and pool-return identities for 200 native alpha
  insertions, duplicate distances and full-pool failure.
- All 48 native shadow group traces plus 100 randomized scenes with variable
  counts. Only the draw body is replaced by a trace stub; stock comparison,
  loop advancement, register/stack handling and the installed mid-hook run.
  Completed-loop registers and stack locals are asserted before the stock
  flush/water boundary, including the shortcut past trailing unrelated rows.

The standalone algorithm test covers 8,000 alpha insertions, clear/removal
reconstruction, ties/full pools/NaN fallback, 1,000 randomized shadow groups
and capacity rejection. The embedded-font resource/glyph/lifetime test
continues to pass.

Local warmed fixture timings varied across runs:

| Workload | Stock | New path |
| --- | ---: | ---: |
| 100,000 system-timed friction calls | 4.6–7.0 ms | 2.4–2.9 ms |
| 100,000 movement manager calls | 13.1–13.7 ms | 15.7–16.0 ms |
| 1,000 lists of 200 alpha insertions | 10.0–11.0 ms | 10.1–10.7 ms |
| 1,000 scenes of 48 shadow group lookups | 1.1–2.3 ms | 2.6–3.0 ms |

The friction harness substitutes the same CRT `pow` adapter on both sides;
it does not benchmark the original game's math-library helper. Shadow timings
exclude real drawing and include trace recording. These are fixture timings,
not gameplay FPS. No timing thresholds determine pass/fail. Alpha results are
near break-even; the plan and shadow hooks added overhead in these workloads.
These results are from older test fixtures; consult the shipped INI for
current defaults. There is no asset-conversion prediction path.

Live testing remains necessary for particle appearance, mirrors, shadow/water
rendering, fast travel, mission loads, save/load, Alt+Tab and mod
stacks. Immutable keys within a pass/update are a requirement for reuse/indexing;
custom FX dispatch and unsupported layouts fall back to the game.

## 1.3.1 font and lifecycle fixes

The Win32 Release build with local font embedding enabled passed all eleven
CTest checks. The new font test loads the actual ASI as data under a different
filename, checks the embedded bytes against the local BankGothic TTF, bakes an
`A` glyph, verifies borrowed resource lifetime during ImGui context teardown,
and checks the scalable fallback's configured 28-pixel size. A separate build
with embedding disabled also passed this test, verifying resource absence and
the fallback path. No game hooks or
GTA session run in this test. Local embedding is restored by default; source
bundles still omit the TTF. FX instancing-only mode now installs the project
unload hook so cached render resources are cleared on unload.

The earlier 1.3.0 measurements below remain historical results for that build.

## 1.3.0 build and automated checks


Clean Win32 Release configure/build succeeded with Visual Studio 18 2026,
CMake 4.4.4 and the Windows SDK shader compiler. Dependencies were freshly
downloaded from the pinned sources, without references to the sibling FXP
project. Default public builds omit the optional local BankGothic font.

Ten CTest checks passed locally, with no skips:

1. FX curve math, endpoints/loop/extrapolation, SIMD tails and particle lanes.
2. x86 thiscall/fastcall SafetyHook trampoline and restoration.
3. Hardware D3D9 stock-versus-FX triangle instancing.
4. Hardware D3D9 FX/SanFrame state-cache/submission-hook coexistence.
5. FX master switches, interval bounds and unsupported-host rejection.
6. Optional stock-executable particle/curve differential test.
7. Renderer detection.
8. DXVK configuration ownership.
9. SkyGFX constant/binding/profile hooks and compatibility transitions.
10. Submission multithreading/null-index/disabled-profile safety.

The frame-statistics Python tool's two existing checks passed as well.

Both D3D9 graphics tests compare 21 texture/color/blend/transform/fog cases,
including linear, EXP/EXP2, vertex/range/table fog, affine and perspective
projections. Observed maximum RGB difference was 1/255. They verify shader and
stream-frequency restoration, Reset/recreation, and fallback for flat shading
and texture transforms. The integration fixture uses SanFrame's actual state
and submission implementations; it omits GTA-only RenderWare state commits.

The local EXE test compared 48,000 channel values and 4,096 scalar particles,
plus emitter list/pause/kill/SIMD behavior. Largest observed curve error was
`3.81469727e-6`; particle error was `9.53674316e-7`. It also checked foreign
thread fallback, metadata changes, project/front-cache invalidation and hook
overwrite detection. A simulated overwrite notice is expected in this test.
The executable stays local and is never packaged or launched as a game.

## Performance evidence

An informational warmed native-routine microbenchmark, run inside the same
differential harness, measured:

| Workload | Stock | Integrated replacement |
| --- | ---: | ---: |
| 300,000 five-channel curve calls | 10.646 ms | 8.139 ms |
| 50,000 eight-particle emitter calls | 9.945 ms | 9.913 ms |

These are a local single-run fixture result, not whole-game FPS or a universal
speedup. Curve time decreased about 24%; emitter time was essentially unchanged.
Common fixture-reset overhead is included. Timings are informational and do not
make the test flaky by imposing speed thresholds. Earlier measurements before
the reciprocal/front-cache/packet changes showed little CPU benefit.

FX curves reuse precomputed segment reciprocal widths and a bounded lookup
front cache. SSE2 packets zero only unused lanes. The renderer uses 72-byte
triangle instances and a reusable state-block object, and its shaders are
compiled at build time. Legacy LOD scheduling, large particle-clock storage,
duplicate DLL startup, runtime shader compilation and unused SDK implementation
objects are absent from the merged runtime.

Successful live particle/curve/draw execution promotes individual FX features
from ACTIVE to EFFECTIVE. Periodic activity counters and fallback/overwrite
messages distinguish useful work from an initialized but unused path. These
counters demonstrate execution, not net CPU/GPU time saved.

## Remaining scope

No GTA session, saved-scene FPS comparison, full Proper Shaders setup or ReFX
native gameplay was launched. Native CPU policies and D3D9-only renderer gating
were reviewed; full mod-stack visual correctness, explosions, save/load,
mirrors and Alt+Tab still require live testing. Transparent overdraw and other
mods' explosion lighting are unchanged and may dominate frame time.

Compiled keys are treated as immutable until project unload. In-place edits
without metadata/pointer changes require `CompiledCurves=0`. A project can have
up to 8,192 cached curve objects; unusual curves or capacity saturation fall
back to stock evaluators. Conflicting entry signatures reject replacement hooks.

The current source package excludes the local font, old archives, dependency
checkouts, game files and private logs. There is no GitHub workflow in this
source tree. See `THIRD_PARTY_NOTICES.md` for licensing provenance.
