# Live GTA SA streaming-memory limit (2026-10-15)

## Requirement

SanFrame's OSD must use the **effective, in-process GTA San Andreas
Classic 1.0 US `CStreaming` budget**, including limits modified during
gameplay. It must not derive capacity from Improved Streaming's
`DoubleStreamingMemoryLimit` INI setting or any other third-party INI.

## Implementation

The existing `gta::streamingMemorySnapshot()` safely reads the GTA 1.0 US
engine globals with `VirtualQuery` memory-page validation:

| Game member | Runtime address | Type |
| --- | --- | --- |
| `CStreaming::ms_memoryAvailable` | `0x008A5A80` | unsigned 32-bit bytes |
| `CStreaming::ms_memoryUsed` | `0x008E4CB4` | unsigned 32-bit bytes |

The address mapping follows DK22Pac/plugin-sdk's GTA SA `CStreaming.cpp`:
https://github.com/DK22Pac/plugin-sdk/blob/master/plugin_sa/game_sa/CStreaming.cpp

`StreamingMemoryTelemetry::sample()` re-reads these values at up to 4 Hz
whenever the OSD is drawing; it preserves them verbatim in 64-bit fields
for percentage and peak calculations. There is no `ImprovedStreaming.ini`
lookup, scale inference, 2x multiplier, or 50 MiB fallback. A zero engine
limit or unreadable memory displays `unavailable`.

For example, if a limit adjuster sets the GTA global to 512 MiB, the
next OSD sample reports 512 MiB. If another mod changes it later to
768 MiB, the next sample reports 768 MiB. The same behavior applies to
usage. The OSD label says `GTA Stream (live)`; MiB is explicitly a
power-of-two unit (1 MiB = 1,048,576 bytes).

## Diagnostic proof of the engine-reported limit

The first valid OSD sample and every subsequent change of the game's
`ms_memoryAvailable` value now produce a SanFrameOptimizer.log line:

```text
[GTA Stream] CStreaming::ms_memoryAvailable detected: 536870912 bytes (512 whole MiB); source=game memory 0x008A5A80; multiplier=none
```

Changes use `changed` instead of `detected`. The OSD labels the reading
`GTA Stream [engine]`. These events are based on readings from GTA's memory,
not on a third-party INI or a guess about whether a mod doubles a limit.
Only changes are logged, so normal per-frame use does not fill the log.

If another mod *really writes twice the capacity into GTA's global*,
SanFrame must display that doubled **actual game value**, without multiplying
it again. The purpose of this fix is to remove SanFrame-side doubling, not
to override GTA's budget. A mod that patches every comparison to use an
entirely separate capacity without updating the global may require its
own integration: the OSD cannot prove the private limit from this global.

SanFrame does not hook or reschedule GTA streaming; the OSD only reads engine counters.

## Scope and limitations

- Supported: mods such as Open Limit Adjuster and fastman92 that update
  GTA's `ms_memoryAvailable` variable.
- The observed runtime value is authoritative **as GTA's budget counter**.
  It is not GPU VRAM, physical RAM, process commit, or total renderer memory.
- A custom mod that completely redirects the GTA streaming algorithms to
  a separate capacity variable, or changes units internally, may require
  explicit integration with that mod. SanFrame cannot safely infer a
  hidden/moved limit by scanning INIs or applying a multiplier.
- Targets GTA SA classic 1.0 US at the stock image base. Nonmatching
  game versions are not claimed to be supported.
- No memory limits are written or changed by this patch.

## Validation

The `streaming_memory_accounting` unit test covers:
stock 50 MiB; runtime limit increases and decreases; arbitrary observed
limits; usage above the budget; unchanged peak; absence of 2x multiplication;
and full unsigned 32-bit counter range.

Build on Windows (Win32/x86) and execute:

```bat
cmake --build build --config Release --target SanFrameOptimizer streaming_memory_accounting_test
ctest --test-dir build -C Release -R "^streaming_memory_accounting$" --output-on-failure
```

In game, verify the OSD reflects the actual `ms_memoryAvailable`
value, especially after limit-adjuster changes, and does not change just
because `DoubleStreamingMemoryLimit` is toggled in a plugin INI.

**Validation status:** The pure accounting test has been compiled
and executed on Linux using g++ with strict warnings. Building the
Windows ASI and in-game behavior are still unverified.
