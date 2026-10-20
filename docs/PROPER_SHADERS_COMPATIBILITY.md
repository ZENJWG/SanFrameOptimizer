# Proper Shaders automatic compatibility

Static analysis of the supplied 1,907,328-byte x86 ASI:

- SHA-256: `fd0075e80373d02b9548e6ced5059580a46749b05c3b14dee497eb24ab0db390`
- Preferred image base: `0x10000000`. Addresses below are preferred virtual addresses, not runtime addresses under relocation.
- The ASI was read and disassembled with pefile/Capstone; it was not executed or modified.

## Observations and policy

| Observation in this binary | Automatic policy |
| --- | --- |
| Device proxy vtable at `0x10188BB0`. SetTexture (`0x1001DA20`), SetVertexShader (`0x1001DF40`), SetPixelShader (`0x1001E120`), and float constant setters increment the same counter at `0x101C1080` before forwarding. Its embedded batching diagnostic says the proxy notices external shader changes. | Disable the entire external D3D9 state cache, including shader/constant and SkyGFX suppression. A redundant setter can still have proxy-visible side effects; the former guarded mode did not cover this. |
| SetRenderState at `0x1001D8F0` conditionally rewrites state `0x17` (ZFUNC), including comparison values 4 to 7 and 2 to 5. The binary contains reversed-depth, deferred rendering, depth-prepass and shadow-pass features. | Preserve original state calls and device flags. Disable stock draw merging, static instancing and RenderWare plan caching/batching. |
| Particle render wrapper at `0x10096F00` sets internal flags around a call to GTA `0x4A92A0`, the same FxManager render entry used by the integrated particle renderer. The binary includes soft-particle shading. | Disable particle instanced submission, leaving original render calls and shaders in control. Preserve simulation, curve, matrix and operator optimizers subject to existing signature checks. |
| Calls to GTA visibility helpers `0x534290` and `0x534540`, plus custom shadow, reflection, fading and transparent-render passes. | Conservatively disable visibility caching and alpha/shadow indexing. These are inferred compatibility risks, not demonstrated visual failures; the legacy visibility opt-in cannot bypass this policy. |

Raw third-party disassembly is excluded from the source distribution. The
addresses and observations above summarize the analysis used for this policy.

## Integration

Detection uses loaded module handles for `ProperShaders.asi`, `ProperShaders.dll`, `proper_shaders.asi` and `proper_shaders.dll`; files merely present on disk do not trigger it. Gates run at subsystem installation and device creation. Particle render and engine-index callbacks also fall back if their hooks were armed earlier in ASI startup order. Compatibility overrides are runtime-only; experimental paths remain opt-in in the shipped INI.

Diagnostics identify disabled features and reasons. State/draw/visibility ranking rows show `COMPAT-OFF(ProperShaders)`. The stock DXVK reset workaround, overlay, worker arena and frame-pacing options remain independently controlled. Automatic disabling is conservative for the loaded module, even if individual shader features are switched off in its configuration.

No tests or in-game visual validation were run. Static analysis establishes overlap and side effects, not a guarantee of compatibility across every setting or different ASI version. Injecting or unloading renderer modules after device creation is outside this startup compatibility policy.
