// Include the implementation to exercise the private vtable adapter without
// requiring a GPU or installing the entire resource-barrier mock surface.
#include "../src/RenderSubmission.cpp"
#include <iostream>
#include <stdexcept>

namespace {
struct MockDevice {
    void** table{};
    unsigned queries{}, indexedCalls{}, nativeIndexedCalls{}, scenes{};
    bool reenter{};
};
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
HRESULT STDMETHODCALLTYPE creation(MockDevice*, D3DDEVICE_CREATION_PARAMETERS* out) {
    *out = {};
    out->BehaviorFlags = D3DCREATE_MULTITHREADED | D3DCREATE_HARDWARE_VERTEXPROCESSING;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE caps(MockDevice*, D3DCAPS9* out) {
    *out = {};
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE vertexShader(MockDevice* self, IDirect3DVertexShader9** out) {
    ++self->queries;
    *out = nullptr;
    return D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE indexedUP(MockDevice* self, D3DPRIMITIVETYPE, UINT, UINT,
    UINT, const void* indices, D3DFORMAT, const void*, UINT) {
    ++self->indexedCalls;
    if (self->reenter)
        return reinterpret_cast<IDirect3DDevice9*>(self)->DrawIndexedPrimitive(
            D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
    return indices ? D3D_OK : D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE indexed(MockDevice* self, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) {
    ++self->nativeIndexedCalls;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE beginScene(MockDevice* self) {
    ++self->scenes;
    return D3D_OK;
}
}
int main() {
    // Keep COM fixtures alive through the exception cleanup path.
    std::array<void*, 119> table{}, copiedTable{};
    MockDevice mock{table.data()};
    auto* device = reinterpret_cast<IDirect3DDevice9*>(&mock);
    try {
        table[9] = reinterpret_cast<void*>(&creation);
        table[7] = reinterpret_cast<void*>(&caps);
        table[93] = reinterpret_cast<void*>(&vertexShader);
        table[108] = reinterpret_cast<void*>(&vertexShader);
        table[84] = reinterpret_cast<void*>(&indexedUP);
        table[82] = reinterpret_cast<void*>(&indexed);
        table[41] = reinterpret_cast<void*>(&beginScene);
        const auto originalTable = table;
        mws::Config config;
        check(!mws::RenderSubmission::install(device, config), "multithreaded batching installed");
        check(table == originalTable && !g_device, "rejected device acquired hooks or ownership");

        // A focused draw adapter fixture: null indices must reach the original
        // method before batching probes or modifies any live device state.
        g_device = device;
        g_thread = GetCurrentThreadId();
        g_immediate = true;
        g_maxPrimitives = 65535;
        g_submissionRouteLogged = true;
        check(H(IDirect3DDevice9, 84, DrawIndexedPrimitiveUP)::install(device), "draw adapter failed");
        float vertices[9]{};
        check(FAILED(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1,
            nullptr, D3DFMT_INDEX16, vertices, 12)), "null index failure hidden");
        check(mock.indexedCalls == 1 && mock.queries == 0 && g_bytes == 0,
            "invalid indexed input entered the batching path");
        g_logInterval = 0;
        g_drawIndexedPrimitiveCalls = 256;
        profileIndexedDraw({D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1});
        check(mock.queries == 0 && g_dipProfileSamples == 0,
            "disabled reports still queried draw state");
        // Model a native runtime forwarding an UP draw through its indexed
        // vtable while holding a runtime lock. The nested draw must reach the
        // original directly, without state queries or deferred submission.
        check(H(IDirect3DDevice9, 82, DrawIndexedPrimitive)::install(device), "indexed adapter failed");
        mock.reenter = true;
        g_immediate = false;
        g_instancing = g_genericIndexedInstancing = true;
        g_drawIndexedPrimitiveCalls = 0;
        const WORD indices[]{0, 1, 2};
        check(SUCCEEDED(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1,
            indices, D3DFMT_INDEX16, vertices, 12)), "native forwarding failed");
        check(mock.nativeIndexedCalls == 1 && mock.queries == 0 &&
            g_drawIndexedPrimitiveCalls == 0 && g_count == 0,
            "runtime-internal draw reentered batching/state queries");
        check(g_forwardDepth == 0, "forwarding guard leaked after call");
        // A later top-level game call must still be intercepted.
        g_instancing = false;
        check(SUCCEEDED(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1)) &&
            g_drawIndexedPrimitiveCalls == 1 && mock.nativeIndexedCalls == 2,
            "forwarding guard suppressed a later game draw");
        // Reproduce the live freeze: another renderer copies the already
        // hooked vtable onto the same device. BeginScene must preserve its
        // original chain, and uninstall must restore the copied table too.
        check(H(IDirect3DDevice9, 41, BeginScene)::install(device), "scene adapter failed");
        copiedTable = table;
        mock.table = copiedTable.data();
        check(SUCCEEDED(device->BeginScene()) && mock.scenes == 1,
            "copied device vtable made BeginScene fail");
        check(SUCCEEDED(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1)) &&
            g_drawIndexedPrimitiveCalls == 2 && mock.nativeIndexedCalls == 3,
            "copied device vtable lost draw interception");
        mws::RenderSubmission::uninstall();
        check(table == originalTable, "draw adapter not restored");
        check(copiedTable == originalTable, "copied device vtable not restored");
        check(!config.workerPreparation, "unused worker pool enabled by default");
        std::cout << "Submission safety, copied vtable, native reentry and disabled-profile checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        mws::RenderSubmission::uninstall();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
