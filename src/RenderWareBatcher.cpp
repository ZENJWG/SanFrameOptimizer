#include "mws/ProperShadersCompatibility.hpp"
#include "mws/OptimizationReport.hpp"
#include "mws/RenderWareBatcher.hpp"
#include "mws/Diagnostics.hpp"
#include "mws/GtaSa10US.hpp"
#include "mws/ExecutableInfo.hpp"
#include "mws/Logger.hpp"
#include "mws/StockHook.hpp"
#include "mws/RenderSubmission.hpp"
#include "mws/PassProfiler.hpp"
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>

namespace {
// GTA SA 1.0 US CCustomBuildingPipeline::CustomPipeRenderCB.
// Batching policy is documented in docs/PERFORMANCE_IMPLEMENTATION.md.
constexpr std::size_t kMaxMeshes = 64;

struct ResourceEntry {
    void* next;
    void* previous;
    std::int32_t size;
    void* owner;
    void* ownerRef;
    void* destroyNotify;
};
struct VertexStream {
    IDirect3DVertexBuffer9* buffer;
    std::uint32_t offset, stride;
    std::uint16_t geometryFlags;
    std::uint8_t managed, dynamicLock;
};
struct Header {
    std::uint32_t serialNumber, numMeshes;
    IDirect3DIndexBuffer9* indexBuffer;
    D3DPRIMITIVETYPE primitiveType;
    VertexStream streams[2];
    std::int32_t useOffsets;
    IDirect3DVertexDeclaration9* declaration;
    std::uint32_t totalIndices, totalVertices;
};
struct Mesh {
    std::uint32_t numIndices, minVertex;
    void* material;
    std::int32_t vertexAlpha;
    IDirect3DVertexShader9* vertexShader;
    std::uint32_t baseIndex, numVertices, startIndex, numPrimitives;
};
static_assert(sizeof(ResourceEntry) == 24);
static_assert(sizeof(VertexStream) == 16);
static_assert(sizeof(Header) == 64);
static_assert(sizeof(Mesh) == 36);
struct Batch {
    ResourceEntry entry;
    Header header;
    std::array<Mesh, kMaxMeshes> meshes;
};
static_assert(offsetof(Batch, header) == sizeof(ResourceEntry));
static_assert(offsetof(Batch, meshes) == sizeof(ResourceEntry) + sizeof(Header));

using Callback = void(__cdecl*)(ResourceEntry*, void*, std::uint8_t, std::uint32_t);
struct Pipeline {
    std::uintptr_t address;
    const char* name;
    Callback original{};
    std::uint64_t callbacks{}, saved{};
    bool merge{};
};
Pipeline g_pc{0x5D77D0, "PC"};
Pipeline g_dn{0x5D6480, "day/night"};
unsigned g_logInterval{}, g_presents{};
std::atomic<std::uint32_t> g_maxPrimitives{0};
std::atomic<std::uint64_t> g_savedDraws{0};
bool g_skygfxLoaded{};
bool g_zeroCallbackWarningLogged{};
std::uint64_t g_rwNoEntry{}, g_rwHeaderRejected{}, g_rwSingleMesh{};
std::uint64_t g_rwNoIndexBuffer{}, g_rwNonTriangleList{}, g_rwTooManyMeshes{}, g_rwBadEntrySize{};
std::uint64_t g_rwInvalidPair{}, g_rwIncompatiblePair{}, g_rwValidPairs{};
std::uint64_t g_rwPairMaterial{}, g_rwPairAlpha{}, g_rwPairShader{};
std::uint64_t g_rwPairVertexRange{}, g_rwPairNonAdjacent{}, g_rwPairPrimitiveLimit{};
// Diagnostic-only counters. Material-pointer identity is NOT proof of equivalent
// material state; these counters never relax the stock rendering barriers.
std::array<std::uint64_t, 7> g_rwRejectedTopology{};
std::uint64_t g_rwMaterialOnlyMismatch{}, g_rwMaterialWithOtherMismatch{};
std::uint64_t g_rwMaterialOtherAlpha{}, g_rwMaterialOtherShader{};
std::uint64_t g_rwMaterialOtherVertexRange{}, g_rwMaterialOtherIndexOrder{};
std::uint64_t g_rwMaterialOtherPrimitiveLimit{};
// A pointer or serial match alone is NEVER enough: recheck resource metadata
// and every source descriptor, including for cached no-merge decisions.
struct PlanSnapshot {
    ResourceEntry* resource{};
    ResourceEntry identity{};
    Header header{};
    std::array<Mesh, kMaxMeshes> source{};
    std::uint32_t primitiveLimit{};
    bool valid{};
};
struct MergePlan : PlanSnapshot {
    std::array<Mesh, kMaxMeshes> merged{};
    std::uint32_t outputCount{};
};
std::array<MergePlan, 32> g_plans{};
// Separate storage keeps frequently rejected groups from evicting useful
// positive plans. No merged descriptor payload is needed for these entries.
std::array<PlanSnapshot, 32> g_noMergePlans{};
std::uint64_t g_planHits{}, g_planMisses{}, g_planRebuilds{};
std::uint64_t g_noMergeHits{};
bool g_planEnabled{true};
std::size_t planSlot(ResourceEntry* entry) {
    return (reinterpret_cast<std::uintptr_t>(entry) >> 5) % g_plans.size();
}
bool matchesSnapshot(const PlanSnapshot& plan, ResourceEntry* entry, const Header& header,
    const Mesh* meshes, std::uint32_t maxPrimitives) {
    return plan.valid && plan.resource == entry && plan.primitiveLimit == maxPrimitives &&
        plan.identity.size == entry->size && plan.identity.owner == entry->owner &&
        plan.identity.ownerRef == entry->ownerRef &&
        std::memcmp(&plan.header, &header, sizeof(Header)) == 0 &&
        std::memcmp(plan.source.data(), meshes, header.numMeshes * sizeof(Mesh)) == 0;
}
const MergePlan* cachedPlan(ResourceEntry* entry, const Header& header,
    const Mesh* meshes, std::uint32_t maxPrimitives) {
    const auto& plan = g_plans[planSlot(entry)];
    if (!matchesSnapshot(plan, entry, header, meshes, maxPrimitives)) {
        ++g_planMisses;
        return nullptr;
    }
    ++g_planHits;
    return &plan;
}
bool cachedNoMerge(ResourceEntry* entry, const Header& header,
    const Mesh* meshes, std::uint32_t maxPrimitives) {
    const auto& plan = g_noMergePlans[planSlot(entry)];
    if (!matchesSnapshot(plan, entry, header, meshes, maxPrimitives))
        return false;
    ++g_noMergeHits;
    return true;
}
void saveNoMerge(ResourceEntry* entry, const Header& header, const Mesh* source,
    std::uint32_t primitiveLimit) {
    if (!g_planEnabled || header.streams[0].dynamicLock || header.streams[1].dynamicLock) return;
    auto& plan = g_noMergePlans[planSlot(entry)];
    plan.valid = false;
    plan.resource = entry;
    plan.identity = *entry;
    plan.header = header;
    std::memcpy(plan.source.data(), source, header.numMeshes * sizeof(Mesh));
    plan.primitiveLimit = primitiveLimit;
    plan.valid = true;
}
void savePlan(ResourceEntry* entry, const Header& header, const Mesh* source,
    const Batch& batch, std::uint32_t primitiveLimit) {
    // RW dynamic-lock streams can change underneath stable pointers/headers.
    if (!g_planEnabled || header.streams[0].dynamicLock || header.streams[1].dynamicLock) return;
    auto& plan = g_plans[planSlot(entry)];
    plan.valid = false;
    plan.resource = entry;
    plan.identity = *entry;
    plan.header = header;
    std::memcpy(plan.source.data(), source, header.numMeshes * sizeof(Mesh));
    std::memcpy(plan.merged.data(), batch.meshes.data(), batch.header.numMeshes * sizeof(Mesh));
    plan.outputCount = batch.header.numMeshes;
    plan.primitiveLimit = primitiveLimit;
    plan.valid = true;
    ++g_planRebuilds;
}
bool validMesh(const Mesh& mesh, const Header& header, std::uint32_t maxPrimitives) {
    // Fixed-function, non-empty triangle lists only. Widen before arithmetic.
    return mesh.material && !mesh.vertexShader && mesh.numVertices &&
        mesh.numPrimitives && mesh.numPrimitives <= maxPrimitives &&
        std::uint64_t{mesh.numPrimitives} * 3 == mesh.numIndices &&
        std::uint64_t{mesh.startIndex} + mesh.numIndices <= header.totalIndices;
}

bool compatible(const Mesh& a, const Mesh& b, std::uint32_t maxPrimitives) {
    return a.material == b.material && a.vertexAlpha == b.vertexAlpha &&
        a.vertexShader == b.vertexShader && a.baseIndex == b.baseIndex &&
        a.minVertex == b.minVertex && a.numVertices == b.numVertices &&
        std::uint64_t{a.startIndex} + a.numIndices == b.startIndex &&
        std::uint64_t{a.numPrimitives} + b.numPrimitives <= maxPrimitives;
}

// Keep the bounded stack copy out of the overwhelmingly common no-merge path.
__declspec(noinline) void renderBatch(ResourceEntry* entry, const Header& header,
    const Mesh* meshes, void* object, std::uint8_t type, std::uint32_t flags,
    std::uint32_t maxPrimitives, Pipeline& pipeline) {
    Batch batch;
    batch.entry = *entry;
    batch.header = header;
    batch.header.numMeshes = 0;
    for (std::uint32_t i = 0; i < header.numMeshes; ++i) {
        const auto& next = meshes[i];
        if (batch.header.numMeshes) {
            auto& tail = batch.meshes[batch.header.numMeshes - 1];
            if (validMesh(tail, header, maxPrimitives) &&
                validMesh(next, header, maxPrimitives) &&
                compatible(tail, next, maxPrimitives)) {
                tail.numIndices += next.numIndices;
                tail.numPrimitives += next.numPrimitives;
                continue;
            }
        }
        batch.meshes[batch.header.numMeshes++] = next;
    }
    // The stock callback reads entry/header/mesh metadata synchronously. Keep
    // the real RW resource and its lifetime/ownership links entirely unchanged.
    savePlan(entry, header, meshes, batch, maxPrimitives);
    const auto savedNow = header.numMeshes - batch.header.numMeshes;
    pipeline.original(&batch.entry, object, type, flags);
    const bool firstPipelineSave = pipeline.saved == 0 && savedNow != 0;
    pipeline.saved += savedNow;
    if (g_savedDraws.fetch_add(savedNow, std::memory_order_relaxed) == 0 && savedNow != 0) {
        mws::Logger::instance().info("RW batching: first compatible mesh merge submitted to the stock callback.");
    }
    if (firstPipelineSave) {
        mws::Diagnostics::instance().effective(
            std::string("Optimization.") + (&pipeline == &g_dn ? "RenderWareDayNightBatching" : "RenderWareBatching"),
            "adjacent compatible meshes merged; at least one RenderWare draw was removed");
    }
}

__declspec(noinline) void renderCachedPlan(ResourceEntry* entry, const Header& header,
    const MergePlan& plan, void* object, std::uint8_t type, std::uint32_t flags,
    Pipeline& pipeline) {
    Batch batch;
    batch.entry = *entry;
    batch.header = header;
    batch.header.numMeshes = plan.outputCount;
    std::memcpy(batch.meshes.data(), plan.merged.data(), plan.outputCount * sizeof(Mesh));
    // Stock callbacks may reenter rendering and replace a cache slot. Retain
    // everything needed after the callback before handing it control.
    const auto savedNow = header.numMeshes - plan.outputCount;
    pipeline.original(&batch.entry, object, type, flags);
    pipeline.saved += savedNow;
    g_savedDraws.fetch_add(savedNow, std::memory_order_relaxed);
}

void renderHook(Pipeline& pipeline, ResourceEntry* entry, void* object,
    std::uint8_t type, std::uint32_t flags) {
    const bool firstCallback = pipeline.callbacks == 0;
    ++pipeline.callbacks;
    mws::PassProfiler::Scope passScope(
        &pipeline == &g_dn ? mws::PassProfiler::Pass::BuildingsDayNight :
                             mws::PassProfiler::Pass::BuildingsPC);
    if (firstCallback) {
        mws::Diagnostics::instance().active(
            std::string("Optimization.") + (&pipeline == &g_dn ? "RenderWareDayNightBatching" : "RenderWareBatching"),
            "live RenderWare callback observed");
    }
    bool immutable = false;
    if (&pipeline == &g_pc && entry && entry->size >= static_cast<std::int32_t>(sizeof(Header))) {
        const auto& h = *reinterpret_cast<const Header*>(entry + 1);
        immutable = h.streams[0].buffer && !h.streams[0].dynamicLock && !h.streams[1].buffer;
    }
    mws::RenderSubmission::enterStaticBuilding(immutable);
    struct Scope { ~Scope() { mws::RenderSubmission::leaveStaticBuilding(); } } scope;
    const auto limit = g_maxPrimitives.load(std::memory_order_relaxed);
    if (pipeline.merge && entry && limit) {
        if (entry->size < static_cast<std::int32_t>(sizeof(Header))) {
            ++g_rwHeaderRejected;
            ++g_rwBadEntrySize;
            pipeline.original(entry, object, type, flags);
            return;
        }
        // This is an internal RW callback: the entry is already owned and
        // validated by RW. No VirtualQuery, allocation or D3D query per draw.
        const auto& header = *reinterpret_cast<const Header*>(entry + 1);
        if (header.indexBuffer && header.primitiveType == D3DPT_TRIANGLELIST &&
            header.numMeshes > 1 && header.numMeshes <= kMaxMeshes &&
            entry->size >= static_cast<std::int32_t>(
                sizeof(Header) + header.numMeshes * sizeof(Mesh))) {
            const auto* meshes = reinterpret_cast<const Mesh*>(&header + 1);
            if (const auto* plan = g_planEnabled ? cachedPlan(entry, header, meshes, limit) : nullptr) {
                renderCachedPlan(entry, header, *plan, object, type, flags, pipeline);
                return;
            }
            if (g_planEnabled && cachedNoMerge(entry, header, meshes, limit)) {
                pipeline.original(entry, object, type, flags);
                return;
            }
            for (std::uint32_t i = 1; i < header.numMeshes; ++i) {
                if (!validMesh(meshes[i - 1], header, limit) ||
                    !validMesh(meshes[i], header, limit)) {
                    ++g_rwInvalidPair;
                    continue;
                }
                if (!compatible(meshes[i - 1], meshes[i], limit)) {
                    ++g_rwIncompatiblePair;
                    // These categories are disjoint: report the first failing
                    // compatibility condition, matching compatible() order.
                    const auto& left = meshes[i - 1];
                    const auto& right = meshes[i];
                    if (left.material != right.material) {
                        ++g_rwPairMaterial;
                        // A separate material pointer blocks a merge regardless
                        // of geometry. Record whether it is the *only* mismatch
                        // to guide future verified state-equivalence research.
                        const bool alphaDiffers = left.vertexAlpha != right.vertexAlpha;
                        const bool shaderDiffers = left.vertexShader != right.vertexShader;
                        const bool vertexDiffers = left.baseIndex != right.baseIndex ||
                            left.minVertex != right.minVertex ||
                            left.numVertices != right.numVertices;
                        const bool indexDiffers =
                            std::uint64_t{left.startIndex} + left.numIndices != right.startIndex;
                        const bool primitiveDiffers =
                            std::uint64_t{left.numPrimitives} + right.numPrimitives > limit;
                        if (!(alphaDiffers || shaderDiffers || vertexDiffers ||
                              indexDiffers || primitiveDiffers)) {
                            ++g_rwMaterialOnlyMismatch;
                        } else {
                            ++g_rwMaterialWithOtherMismatch;
                            g_rwMaterialOtherAlpha += alphaDiffers;
                            g_rwMaterialOtherShader += shaderDiffers;
                            g_rwMaterialOtherVertexRange += vertexDiffers;
                            g_rwMaterialOtherIndexOrder += indexDiffers;
                            g_rwMaterialOtherPrimitiveLimit += primitiveDiffers;
                        }
                    }
                    else if (left.vertexAlpha != right.vertexAlpha) ++g_rwPairAlpha;
                    else if (left.vertexShader != right.vertexShader) ++g_rwPairShader;
                    else if (left.baseIndex != right.baseIndex ||
                             left.minVertex != right.minVertex ||
                             left.numVertices != right.numVertices) ++g_rwPairVertexRange;
                    else if (std::uint64_t{left.startIndex} + left.numIndices != right.startIndex)
                        ++g_rwPairNonAdjacent;
                    else ++g_rwPairPrimitiveLimit;
                    continue;
                }
                ++g_rwValidPairs;
                {
                    renderBatch(entry, header, meshes, object, type, flags, limit, pipeline);
                    return;
                }
            }
            saveNoMerge(entry, header, meshes, limit);
        } else if (header.numMeshes <= 1) {
            ++g_rwSingleMesh;
        } else {
            ++g_rwHeaderRejected;
            // A header may fail multiple constraints. Count the first cause in
            // the same order as the eligibility check.
            if (!header.indexBuffer) ++g_rwNoIndexBuffer;
            else if (header.primitiveType != D3DPT_TRIANGLELIST) {
                ++g_rwNonTriangleList;
                const auto kind = static_cast<std::uint32_t>(header.primitiveType);
                if (kind < g_rwRejectedTopology.size()) ++g_rwRejectedTopology[kind];
                else ++g_rwRejectedTopology[0]; // unknown/out-of-range topology
            }
            else if (header.numMeshes > kMaxMeshes) ++g_rwTooManyMeshes;
            else ++g_rwBadEntrySize;
        }
    } else if (pipeline.merge && !entry) {
        ++g_rwNoEntry;
    }
    pipeline.original(entry, object, type, flags);
}

void __cdecl pcHook(ResourceEntry* e, void* o, std::uint8_t t, std::uint32_t f) { renderHook(g_pc, e, o, t, f); }
void __cdecl dnHook(ResourceEntry* e, void* o, std::uint8_t t, std::uint32_t f) { renderHook(g_dn, e, o, t, f); }
} // namespace

namespace mws::RenderWareBatcher {
bool install(IDirect3DDevice9* device, const Config& config) {
    if (ProperShadersCompatibility::loaded()) {
        ProperShadersCompatibility::report(config);
        return false;
    }
    g_maxPrimitives.store(0, std::memory_order_relaxed);
    if (!device) return false;
    D3DCAPS9 caps{};
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (FAILED(device->GetDeviceCaps(&caps)) || !caps.MaxPrimitiveCount ||
        FAILED(device->GetCreationParameters(&creation)) || (creation.BehaviorFlags & D3DCREATE_MULTITHREADED)) return false;
    g_logInterval = config.optimizationLogInterval;
    g_planEnabled = config.renderWarePlanCache;
    for (auto& plan : g_plans) plan.valid = false;
    for (auto& plan : g_noMergePlans) plan.valid = false;
    g_skygfxLoaded = GetModuleHandleA("skygfx.asi") != nullptr ||
                     GetModuleHandleA("SkyGFX.asi") != nullptr;
    if (g_skygfxLoaded) {
        Logger::instance().info(
            "RW compatibility: SkyGFX owns the building render callback; SanFrame stock RenderWare batching is auto-disabled and indexed submission fallback remains available.");
        if (config.renderWareBatching)
            Diagnostics::instance().autoDisable(
                "Optimization.RenderWareBatching",
                "SkyGFX replacement building pipeline bypasses the stock callback; using D3D indexed submission path instead");
        if (config.renderWareDayNightBatching)
            Diagnostics::instance().autoDisable(
                "Optimization.RenderWareDayNightBatching",
                "SkyGFX replacement building pipeline bypasses the stock day/night callback");
        g_maxPrimitives.store(std::min(caps.MaxPrimitiveCount, DWORD{65535}), std::memory_order_relaxed);
        return false;
    }
    auto installPipeline = [&](Pipeline& p, Callback hook, bool requested, bool merge) {
        p.merge = merge;
        if (!requested) return;
        // Both callbacks must retain stock draw helpers. The DN window ends at
        // CreateCustomObjPipe (0x5D6750), before any unrelated function.
        if (!p.original && (!stock::matches(p.address, 0x2D0, {0x5DA640, 0x5DA6A0}) ||
                            !stock::hook(p.address, hook, p.original))) {
            Logger::instance().info(std::string("RW ") + p.name + " batching skipped: stock callback fingerprint/hook unavailable.");
            Diagnostics::instance().skipped(
                std::string("Optimization.") + (std::string(p.name) == "day/night" ? "RenderWareDayNightBatching" : "RenderWareBatching"),
                "stock callback fingerprint/hook unavailable");
            return;
        }
        Logger::instance().info(std::string("RW ") + p.name + " callback ACTIVE: adjacent meshes, original order, bounded to 64 descriptors.");
        Diagnostics::instance().active(
            std::string("Optimization.") + (std::string(p.name) == "day/night" ? "RenderWareDayNightBatching" : "RenderWareBatching"),
            "callback hook installed; awaiting a live callback/merge");
    };
    installPipeline(g_pc, &pcHook, config.renderWareBatching || config.staticPropInstancing, config.renderWareBatching);
    installPipeline(g_dn, &dnHook, config.renderWareDayNightBatching, config.renderWareDayNightBatching);
    g_maxPrimitives.store(std::min(caps.MaxPrimitiveCount, DWORD{65535}), std::memory_order_relaxed);
    return g_pc.original || g_dn.original;
}

void appendOptimizerReport(OptimizationReport::Rows& rows, const Config& c) {
  rows.push_back({"draw-submissions", "RenderWareBatching", "RenderWareBatching", "callbacks", g_pc.callbacks, g_pc.saved, c.renderWareBatching, g_pc.original && g_pc.merge});
  rows.push_back({"draw-submissions", "RenderWareDayNightBatching", "RenderWareDayNightBatching", "callbacks", g_dn.callbacks, g_dn.saved, c.renderWareDayNightBatching, g_dn.original && g_dn.merge});
}
void onPresent() {
    if (!g_logInterval || ++g_presents < g_logInterval) return;
    g_presents = 0;
    std::ostringstream s;
    s << "RW totals: PC callbacks=" << g_pc.callbacks << " meshes saved=" << g_pc.saved
      << "; day/night callbacks=" << g_dn.callbacks << " meshes saved=" << g_dn.saved;
    Logger::instance().info(s.str());
    Logger::instance().info("RW merge plan cache: hits=" + std::to_string(g_planHits) +
        " misses=" + std::to_string(g_planMisses) +
        " builds=" + std::to_string(g_planRebuilds) +
        " noMergeHits=" + std::to_string(g_noMergeHits) +
        " (content and resource-generation validated)");
    // Emit diagnostic counters at the normal optimization-log interval only,
    // not inside the high-frequency building-render callback.
    std::ostringstream eligibility;
    eligibility << "RW eligibility cumulative: missingEntry=" << g_rwNoEntry
                << " singleMesh=" << g_rwSingleMesh
                << " rejectedHeaders=" << g_rwHeaderRejected
                << " headerCauses[noIB,topology,tooMany,size]="
                << g_rwNoIndexBuffer << "," << g_rwNonTriangleList << ","
                << g_rwTooManyMeshes << "," << g_rwBadEntrySize
                << " pairs[invalid,incompatible,compatible]="
                << g_rwInvalidPair << "," << g_rwIncompatiblePair << "," << g_rwValidPairs
                << " incompatibleCauses[material,alpha,shader,vertexRange,nonAdjacent,primitiveLimit]="
                << g_rwPairMaterial << "," << g_rwPairAlpha << "," << g_rwPairShader
                << "," << g_rwPairVertexRange << "," << g_rwPairNonAdjacent
                << "," << g_rwPairPrimitiveLimit;
    Logger::instance().info(eligibility.str());
    std::ostringstream topology;
    topology << "RW rejected topology: unknown=" << g_rwRejectedTopology[0]
             << " pointList=" << g_rwRejectedTopology[1]
             << " lineList=" << g_rwRejectedTopology[2]
             << " lineStrip=" << g_rwRejectedTopology[3]
             << " triangleList=" << g_rwRejectedTopology[4]
             << " triangleStrip=" << g_rwRejectedTopology[5]
             << " triangleFan=" << g_rwRejectedTopology[6];
    Logger::instance().info(topology.str());
    std::ostringstream material;
    material << "RW material-pointer mismatch detail: onlyMaterial="
             << g_rwMaterialOnlyMismatch
             << " plusOther=" << g_rwMaterialWithOtherMismatch
             << " overlappingOther[alpha,shader,vertexRange,indexOrder,primitiveLimit]="
             << g_rwMaterialOtherAlpha << "," << g_rwMaterialOtherShader << ","
             << g_rwMaterialOtherVertexRange << "," << g_rwMaterialOtherIndexOrder
             << "," << g_rwMaterialOtherPrimitiveLimit
             << "; observation only (no state equivalence assumed)";
    Logger::instance().info(material.str());
    if (!g_zeroCallbackWarningLogged && g_skygfxLoaded &&
        g_pc.callbacks == 0 && g_dn.callbacks == 0) {
        g_zeroCallbackWarningLogged = true;
        Logger::instance().warn(
            "RW route probe: SkyGFX is active and neither stock building callback has executed; live building rendering is using a replacement RenderWare callback. SanFrame will rely on D3D submission telemetry before selecting a compatible batching hook.");
    }
}
void onReset() {
    for (auto& plan : g_plans) plan.valid = false;
    for (auto& plan : g_noMergePlans) plan.valid = false;
}
void uninstall() {
    RenderSubmission::flush();
    for (auto& plan : g_plans) plan.valid = false;
    for (auto& plan : g_noMergePlans) plan.valid = false;
    g_maxPrimitives.store(0, std::memory_order_relaxed);
    stock::unhook(g_pc.address, g_pc.original);
    stock::unhook(g_dn.address, g_dn.original);
    std::ostringstream message;
    message << "RW batching shutdown: mesh submissions saved=" << g_savedDraws.load();
    Logger::instance().info(message.str());
    g_zeroCallbackWarningLogged = false;
}
}
