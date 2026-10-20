# Third-party notices and provenance

Dependency archive identities are recorded in `deps.lock`. Retain the license
texts in `licenses/` when distributing derived binaries.

| Component | License text |
| --- | --- |
| MinHook, Tsuda Kageyu and contributors | [MinHook](licenses/MinHook.txt) |
| Dear ImGui, Omar Cornut and contributors | [ImGui](licenses/ImGui.txt) |
| FXP Optimized implementation, 2026 contributors | [MIT](licenses/FXP-Optimized-MIT.txt) |
| Plugin-SDK layout references, DK22Pac and contributors | [Plugin-SDK](licenses/Plugin-SDK.txt) |
| SafetyHook, cursey and contributors | [Boost](licenses/SafetyHook.txt) |
| Zydis and Zycore | [Zydis](licenses/Zydis.txt), [Zycore](licenses/Zycore.txt) |

FXP-derived files under `src/fxp`, `resources/FxpTriangles.hlsl` and `tests/fxp`
originate from FXP Optimized 0.2.2. Their MIT notice is preserved. Integration
changes are described in the changelog and validation notes.

The checked game layouts in `src/fxp/GtaFxAbi.h` refer to Plugin-SDK revision
`15f15b60bbf74c106e1b496ff92c98764abf4605`. Plugin-SDK is not downloaded,
compiled or linked by this project; the local ABI views avoid its bundled
SafetyHook headers. Its attribution is retained for provenance.

The inherited SanFrame/SAVO implementation did not include a project-wide
license. This cleanup does not relicense it or assign one on its authors'
behalf. Dependency and FXP licenses apply to their respective components.

No game executable, game assets, disassembly dumps, private logs or local
dependency checkout is included in source packages. Optional reference
harnesses require independently supplied game files.

No redistribution license accompanied the locally supplied BankGothic font.
The local font is ignored by Git and excluded from source archives. Local
builds embed it by default when present; builds without it use Dear ImGui's
scalable fallback. Embedding can be disabled explicitly. A resulting ASI that
embeds the font is subject to its redistribution terms.
