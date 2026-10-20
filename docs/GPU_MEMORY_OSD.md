# GPU-memory text OSD

The OSD now displays the **actual GTA process's** GPU-memory usage on the
selected adapter. It does not read the ASC target as if it were real usage.

Example of display labels (numbers vary):
```
VRAM (GTA): 5234 / 10368 MiB (Windows budget)
Shared GPU (GTA): 120 / 6144 MiB (Windows budget)
GTA Stream [engine]: 1413 / 3100 MiB ...
```

Enable in `SanFrameOptimizer.ini`:
```ini
[Overlay]
Enabled=1
ShowGPUMemory=1
ShowSharedGPUMemory=1
```

`ShowGPUUsage` means GPU engine utilization %, not VRAM. `ShowStreamingMemory`
is GTA's CPU streaming heap, not VRAM. `ShowGPUMemory` and
`ShowSharedGPUMemory` are enabled by default in this distribution.

**Semantics:** WDDM/DXGI per-process CurrentUsage and Budget from
`IDXGIAdapter3::QueryVideoMemoryInfo`, on the adapter matched to GTA's
presentation device. Uses 64-bit counters (can report values exceeding 4 GiB).
Windows may adjust Budget dynamically. These are *process allocations*,
not all processes' combined graphics memory and not the 8192 MiB soft target.
They are sampled on a low-priority worker every ~1 second, never per frame.
If adapter matching or DXGI 1.4 fails, the OSD says `unavailable` rather than
inventing GPU numbers. Windows 10+ and a WDDM graphics driver are expected.
Unusual DXVK/ReFX wrappers or multi-GPU routing might require further adapter
matching work.

### Build
Run `build.bat Release` with CMake and the Visual Studio C++/Windows SDK tools
available. CMake fetches the dependencies listed in `deps.lock`; Plugin-SDK
and Premake are not required. See the [build instructions](../README.md#build).
