# ReFX DXVK factory / OSD integration (source patch)

## Incident evidence

From the supplied 2026-10-15 logs:

- ReFX: `Renderer config: mode=ASI ... Backend='DXVK'`.
- ReFX: `Executable IAT bridge: Create9=0 Create9Ex=0 GetProcAddress=1`.
- ReFX: `GTA D3D9 factory resolved through executable IAT`.
- ReFX: `DXVK Direct3DCreate9 ... mode=REFX_WRAPPED`.
- SanFrame: `Direct3DCreate9 hook installed`, but **no** `CreateDevice`,
  `Present`, `Overlay: ... initialized`, or `Overlay OSD font submitted` line.

ReFX's ASI exports `Direct3DCreate9` (confirmed in its
`res/exports_gtasa_asi.def`). Its GTA-only `GetProcAddress` import redirector
returns this entry, not the factory export from the system `d3d9.dll` or
the private `ReFXDXVK/d3d9.dll`.

Therefore SanFrame's previous hook missed the game's *factory call*
even though SanFrame's separate DXVK binary detector correctly found the
private DLL. **The skipped auto-tune is expected**: ReFX owns its DXVK tuning.

## Fix

- Install the D3D9 factory detour on already-loaded `ReFXDXVK.asi` when its
  `Direct3DCreate9` export is present. Otherwise retain the standard D3D9
  factory hook. Do not eagerly `LoadLibrary` the private DXVK backend.
- Hook the game's actual device via the returned factory's `CreateDevice`
  method; retain the existing `Present` and `EndScene` OSD renderer.
- Track factory provenance for the returned ReFX-wrapped game device.
  Label it `ReFX DXVK / Vulkan` only when its private DXVK DLL is also
  verified as loaded; do not infer native Vulkan from the mere ASI presence.
- Clean the default INI while retaining all settings/sections recognized by
  the original configuration. See changed defaults below.

## Changed default values (intentional)

| Section / key | Old | New | Reason |
|---|---|---|---|
| `Performance.FrameLimiter` | 1 | 0 | Avoid double-limiting ReFX/DXVK; explicitly opt in if SanFrame should own pacing |
| `FXP.InstancedSubmission` | 1 | 0 | ReFX's multithreaded/pure device skips this path; prevent repeated fallback overhead |
| `FXP.StatsIntervalSeconds` | 10 | 0 | No periodic activity logging during normal gameplay |
| `Debug.IniDiagnostics` | 1 | 0 | No startup inventory of every INI key |
| `Debug.RuntimeReportIntervalSeconds` | 30 | 0 | Avoid periodic runtime diagnostic snapshots |
| `Overlay.Scale` | 0.6 | 0.85 | Larger text on high-resolution displays (still auto-scaled and clamped) |

`DXVK.AutoTune=1` remains enabled *for stock DXVK*, but **does not alter
ReFX DXVK**; that existing ownership policy is preserved.

## How to validate on the GTA machine

1. Build the Win32/x86 Release ASI using the repository `build.bat`, on Windows
   with Visual Studio C++, the Windows SDK (`fxc.exe`), and pinned dependencies.
2. Back up the installed `SanFrameOptimizer.ini`. Replace the old ASI with the
   rebuilt ASI and optionally use the new `config/SanFrameOptimizer.ini`.
   Make sure there is only one SanFrame ASI in the GTA/Mod Loader paths.
3. Ensure `ReFXDXVK.asi` loads **before SanFrame** (as in the supplied log).
   Keep ReFX `Backend=DXVK`; SanFrame does not configure it.
4. Launch GTA, start actual gameplay, and inspect `SanFrameOptimizer.log` for:
   - `ReFX ASI factory detected: hooking ReFXDXVK.asi!Direct3DCreate9`
   - `D3D9 CreateDevice hook installed` and `D3D9 Present hook installed`
   - `ReFX D3D9 factory: GTA device captured`
   - `Renderer detected: ReFX DXVK / Vulkan`
   - `Overlay: Dear ImGui D3D9 renderer initialized`
   - `Overlay OSD font submitted`
5. Verify FPS, frame time, API and GPU lines appear visually, remain after
   changing resolution/Alt-Tab, and do not obscure GTA HUD or ReFX's own UI.
   Check for crashes, D3D device resets and mod compatibility. Comparing
   before/after FPS requires representative capture traces, not log counts.

## Boundaries

This archive is **source**, not a rebuilt `.asi`; there is no Windows x86
toolchain or running GTA instance in the current environment. Static/config
checks and a portable backend-selection test passed, but the hook chain and
real overlay visibility have **not** been tested in-game.

The fix targets the factory routing observed in the supplied logs. If SanFrame
loads before ReFX and ReFX installs the factory IAT bridge afterward, the
early hook may still miss the ASI factory. For this case, explicitly control
ASI loading order or add a safely deferred late-module registration path.
Native-ReFX Vulkan presentation is separate from DXVK and is not covered by
this device factory change. Final GPU compositing after ReFX effects must be
visually validated.
