# Changelog

## Unreleased - 2026-10-20

- Fix submission reset recovery and gate indexed-draw profiling separately
  from periodic counter reporting. Avoid batch timing when adaptive batching
  is disabled. Correct hybrid-GPU memory adapter selection and WMI recovery.
- Clean the INI, disable periodic reports and make experimental paths opt-in.
- Remove orphaned streaming/native-resource policies, their unused tests,
  unused configuration fields and obsolete Plugin-SDK dependency entries.
- Remove generated output, local dependencies/tools, old archives,
  superseded build notes and raw disassembly from the source tree.
- Refresh build/compatibility/provenance documentation. Default builds skip
  tests; `--tests` opts in. Add source-only packaging. Preserve the local
  BankGothic font and its default embedding behavior.


## DistantStreaming compatibility patch (2026-10-19)

- Avoid a second `EndScene` inline detour when `DistantStreaming.asi` is loaded;
  OSD and both memory overlay settings remain enabled through `Present`.
- Add `[Compatibility] OverlayPresentOnly=1` manual override.
- Add isolated regression test for the hook-selection policy and an optional
  DistantStreaming safe INI profile (`compatibility/`).
- Root cause not verified without a GTA crash dump or in-game reproduction.

## Cache64 OSD integration (2026-10-18)

- Added independent Cache64 x64-helper process RAM line to the in-game OSD, enabled by `ShowCache64Memory=1`.
- Samples 64-bit private-memory and working-set byte counters via WMI on a background worker, matched to the helper spawned by the current GTA PID.
- No helper protocol changes or main-thread process scanning. See `docs/CACHE64_OSD.md` for semantics and validation.


## Source cleanup — 2026-10-19

- Removed unused GTA asset-loader coexistence detection and its orphaned test.
- Removed unused per-resource asset-conversion cost prediction and its stale unit-test cases.
- Removed obsolete scheduler descriptions from documentation and preserved all
  read-only overlay memory settings (`ShowStreamingMemory` and `ShowCache64Memory`).
- No Windows ASI build or in-game validation performed for this source change.


## Unreleased — 2026-10-15 (source-only, game test pending)

- Add first-observation and change-only diagnostics for live GTA streaming
  capacity (`ms_memoryAvailable`), logging exact bytes and source address.
  Label the OSD value as `[engine]`, remove obsolete vanilla-limit constant,
  and extend regression tests to reject any inferred/doubled capacity.

- Remove Improved Streaming INI-based `DoubleStreamingMemoryLimit` scaling
  from the GTA streaming OSD. Always sample the live GTA SA CStreaming
  available/used counters, including changes by limit adjusters at runtime.
- Add isolated streaming-memory accounting regression test and document the
  distinction between GTA's memory budget and SanFrame's timing budget.
- Correct stale renderer-detection test references to native Vulkan configuration fields removed in 1.4.1. Validate legacy INI ignored and optimization master gate instead; resolves six MSVC C2039 errors in the 2026-10-15 build log.
- Hook ReFXDXVK.asi!Direct3DCreate9 when the ReFX GTA IAT factory bridge
  is active, rather than mistakenly hooking system/private DXVK factories.
- Pass ReFX factory provenance to OSD detection; verify the loaded DXVK
  backend before declaring the game device `ReFX DXVK / Vulkan`.
- Remove eager loading of the ReFX private DXVK DLL from SanFrame's hook setup.
- Reorganize all 140 existing INI keys, with ReFX-aware, quieter defaults.
- Add integration diagnosis and in-game validation checklist.
- No Windows x86 build or GTA gameplay test completed for this source patch.

## 1.4.2 — 2026-10-13

- Bind the chosen OSD font and scaled pixel size explicitly for text, shadow
  and outline layers.
- Log the loaded font-resource module, byte count and actual submitted font;
  track `Overlay.EmbeddedFont` separately from general overlay rendering.
- The installed game ASI was an older build without font resource 101; replace
  it with the font-embedded release to restore BankGothic in game.
- Compiled the Release ASI without running tests or launching GTA.

## 1.4.1 — 2026-10-13

- Removed native Vulkan policy/statistics optimization and its unused API and
  configuration fields. Kept versioned backend detection and OSD callbacks.
- Removed native callback pacing and engine scheduling; ReFX owns presentation.
- Added periodic cumulative diagnostic coverage, including configuration-off,
  auto-disabled, adaptive, unexercised, fallback and failure evidence.
- Added Present outcomes, FX lifetime counters, alpha/shadow counters, pacing,
  worker-job and actual scratch-allocation activity. Final snapshots precede
  counter teardown, and repeated registration retains evidence.
- Removed the unsupported claim that execution proves full effectiveness/FPS.
- Compiled the Release ASI only; no tests or GTA session run for this change.

## 1.4.0 — 2026-10-13

- Implemented engine experiments for system-timed friction/wind reuse,
  ordered FX operator plans, indexed alpha insertion and indexed stock shadow grouping.
- Documented numerical, mutation and live-gameplay limits after mixed CPU results.
- Preserved stock alpha pool/tie order, shadow casting/water/drawing and custom FX traversal.
- Added signature/conflict guards, owner-thread fallbacks, project invalidation,
  hook-overwrite reporting for FX operators and execution counters.
- Avoided padding-dependent operator keys and per-particle plan-array copies.
- Separated friction/wind cache domains to prevent adjacent FX objects from
  repeatedly evicting each other; matched native shadow loop exit state.
- Expanded executable differential tests and added randomized list/group
  tests and optimizer-master-switch coverage. Embedded-font tests still pass.

## 1.3.1 — 2026-10-13

- Restored local BankGothic embedding after the 1.3.0 build disabled it.
- Resolve font resources by code address instead of the ASI filename.
- Honor the configured font size for both embedded and scalable fallback fonts,
  including explicit ImGui 1.92 font-size selection.
- Borrow pinned font resource data rather than retaining another 44 KB copy.
- Added resource/glyph/lifetime tests against a renamed ASI.
- Clear FX render resources on project unload even in instancing-only mode.

## 1.3.0 — 2026-10-13

- Integrated FXP particle simulation, compiled interpolation, matrix caching,
  triangle instancing and actual-execution diagnostics into one ASI and INI.
- Embedded build-time FX shaders; removed the runtime shader compiler dependency.
- Shared SanFrame submission/state boundaries and reset handling with FXP.
- Added conservative fixed-function, threading and native-backend fallbacks.
- Reused FX state-block objects; removed legacy particle LOD storage/scheduler,
  standalone loader, duplicate logs and unused SDK implementation objects.
- Added compiled-curve reciprocal widths/front-cache lookup and avoided zeroing
  particle packet lanes that are immediately overwritten.
- Fixed stale curve evaluation after failed compilation, null output handling,
  and cached-curve rebuilding at the cache capacity limit.
- Reduced disabled-report profiling overhead.
- Added Win32 integration, configuration and native-routine differential tests.
- Prepared Git ignores, CI, dependency pins, license notices and source/release
  packaging. Previous scratch notes/ZIPs were retained in `.local-archive/`.

In-game FPS improvement and whole-mod-stack compatibility remain unmeasured.
