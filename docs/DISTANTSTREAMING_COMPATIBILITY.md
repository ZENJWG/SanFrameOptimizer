# DistantStreaming compatibility

When `DistantStreaming.asi` is loaded at game-device creation, SanFrame skips
its optional inline D3D9 EndScene detour and draws the overlay through Present.
This avoids additional interception of EndScene while preserving both
`ShowStreamingMemory` and `ShowCache64Memory`.

For renamed ASIs or another mod with conflicting EndScene interception, set:

```ini
[Compatibility]
OverlayPresentOnly=1
```

SanFrame does not change DistantStreaming's configuration, streaming requests
or memory budget. The GTA streaming counter and Cache64 helper counter remain
independent, read-only telemetry sources.

This mitigation was motivated by overlapping device interception observed in
the supplied October 19 binary/logs. It is not a confirmed crash diagnosis or
a guarantee of compatibility with every DistantStreaming version. No external
mod binary or user-specific DistantStreaming configuration is distributed.
