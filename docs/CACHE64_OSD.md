# Cache64 x64 helper memory in SanFrameOptimizer OSD

The [Overlay] `ShowCache64Memory=1` switch enables one additional text
line in the in-game OSD:

```
Cache64 RAM: 12742 MiB private | 12210 MiB working set (PID 11800)
```

**What the numbers mean**

- **Private**: `Win32_Process.PrivatePageCount` (64-bit byte counter),
  approximate total private committed process memory. Includes cache
  allocations and the helper's own process overhead; NOT the helper's
  exact `AssetSummary.residentBytes`, and not the 32768 MiB configured
  *maximum cache budget*.
- **Working set**: `Win32_Process.WorkingSetSize` (64-bit byte counter),
  RAM pages resident in the helper process working set; this is not a
  dedicated measurement of the cache pages.
- **PID**: matching running `DistantStreaming_AssetCache64.exe` process,
  spawned by the *current* GTA process (exact `Name` and `ParentProcessId`).
  Multiple GTA instances therefore do not sum another game's helper usage.
- **GTA Stream [engine]** remains separate and unchanged. **VRAM (GTA)**
  is GPU residency, not helper CPU RAM.

The 32-bit GTA ASI does not call `GetProcessMemoryInfo` on the helper
because `SIZE_T`-based memory counters in a Win32 caller may lose the
high 32 bits for >4 GiB allocations. Instead, a low-priority worker
queries WMI `Win32_Process` 64-bit memory fields every 2 seconds.
The D3D9/ImGui frame only reads atomic snapshot values.
No helper modification, privileged access, named pipe access,
process injection, or third-party INI parsing is required.

If the helper has not launched or has exited, the OSD reads:
`Cache64 RAM: not running / waiting for helper`.
If WMI cannot initialize/connect, it reads:
`Cache64 RAM: unavailable (Windows WMI)`.
The OSD never shows stale usage as current if a subsequent sample finds
no helper.

Configuration in `config/SanFrameOptimizer.ini`:

```ini
[Overlay]
Enabled=1
ShowCache64Memory=1
```

Set `ShowCache64Memory=0` to disable the WMI worker and OSD line.
This telemetry only reports process RAM, not exact cached IMG archive
bytes. Measuring `AssetSummary.residentBytes` exactly would require
the DistantStreaming helper/ASI to publish an additional read-only
telemetry channel; the existing pipe is private to its parent game.

## Windows validation

1. Build the Win32 ASI, install it and enable `ShowCache64Memory=1`.
2. Run GTA before the helper launches: OSD says waiting.
3. Launch DistantStreaming with its helper: helper PID, private MiB,
   and working-set MiB appear; they should rise while the cache warms.
4. Compare PrivatePageCount and WorkingSetSize for that exact PID using
   Windows PowerShell `Get-CimInstance Win32_Process` or Task Manager
   (Task Manager column names may differ).
5. Repeat with >4096 MiB helper private memory to validate no wrapping
   to 32-bit values. Stop helper to check that stale values clear.
6. Verify `GTA Stream [engine]` and GPU counters have not changed.

This environment could not compile a Windows ASI or validate in game.
