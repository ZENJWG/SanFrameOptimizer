#pragma once
#include <d3d9.h>
#include <array>
#include <algorithm>
#include <cstdint>

namespace fxp::gpu {
struct Stats {
    uint64_t samples{}, baselineSamples{}, explosionSamples{}, dropped{}, pending{};
    double milliseconds{}, maximum{}, baselineMs{}, explosionMs{}, explosionMaximum{};
};
// Asynchronous timestamp ring: no FLUSH, waits, spin loops or query reuse while
// a result is pending. Each scope retains its explosion tag until retirement.
class Timer {
    struct Slot {
        IDirect3DQuery9* begin{}; IDirect3DQuery9* end{};
        IDirect3DQuery9* frequency{}; IDirect3DQuery9* disjoint{};
        bool pending{}, explosion{};
    };
    std::array<Slot, 8> slots_{};
    IDirect3DDevice9* device_{};
    int active_{-1}; bool initialized_{}, supported_{};
    Stats stats_{};
    template<class T> static void release(T*& value) { if (value) { value->Release(); value = nullptr; } }
public:
    ~Timer() { reset(); }
    Timer() = default;
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    void reset() {
        for (auto& slot : slots_) { release(slot.begin); release(slot.end); release(slot.frequency); release(slot.disjoint); slot = {}; }
        device_ = nullptr; active_ = -1; initialized_ = supported_ = false;
        stats_ = {};
    }
    bool initialize(IDirect3DDevice9* device) {
        if (device_ == device && initialized_) return supported_;
        reset(); device_ = device; initialized_ = true;
        if (!device) return false;
        for (auto& slot : slots_) {
            if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &slot.begin)) ||
                FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &slot.end)) ||
                FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &slot.frequency)) ||
                FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &slot.disjoint))) {
                reset(); device_ = device; initialized_ = true; return false;
            }
        }
        supported_ = true; return true;
    }
    void poll() {
        for (auto& slot : slots_) {
            if (!slot.pending) continue;
            UINT64 first{}, last{}, frequency{}; BOOL disjoint{};
            // The end timestamp follows the begin timestamp in the command
            // stream. If it is still pending, skip three driver calls whose
            // results cannot complete this sample yet. Never force a flush.
            const HRESULT endResult = slot.end->GetData(&last, sizeof(last), 0);
            if (endResult == S_FALSE) continue;
            if (FAILED(endResult)) { ++stats_.dropped; slot.pending = false; continue; }
            const HRESULT results[] = {
                slot.begin->GetData(&first, sizeof(first), 0),
                slot.frequency->GetData(&frequency, sizeof(frequency), 0),
                slot.disjoint->GetData(&disjoint, sizeof(disjoint), 0)
            };
            bool ready = true, error = false;
            for (HRESULT result : results) { ready = ready && result == S_OK; error = error || FAILED(result); }
            if (error) { ++stats_.dropped; slot.pending = false; continue; }
            if (!ready) continue;
            slot.pending = false;
            if (disjoint || !frequency || last < first) { ++stats_.dropped; continue; }
            const double ms = double(last - first) * 1000 / double(frequency);
            ++stats_.samples; stats_.milliseconds += ms; stats_.maximum = std::max(stats_.maximum, ms);
            if (slot.explosion) { ++stats_.explosionSamples; stats_.explosionMs += ms; stats_.explosionMaximum = std::max(stats_.explosionMaximum, ms); }
            else { ++stats_.baselineSamples; stats_.baselineMs += ms; }
        }
    }
    bool begin(bool explosion) {
        if (!supported_ || active_ >= 0) return false;
        poll();
        for (size_t i = 0; i < slots_.size(); ++i) {
            auto& slot = slots_[i]; if (slot.pending) continue;
            if (FAILED(slot.disjoint->Issue(D3DISSUE_BEGIN))) { ++stats_.dropped; return false; }
            if (FAILED(slot.begin->Issue(D3DISSUE_END))) { slot.disjoint->Issue(D3DISSUE_END); ++stats_.dropped; return false; }
            slot.explosion = explosion; active_ = int(i); return true;
        }
        ++stats_.dropped; return false;
    }
    void end() {
        if (active_ < 0) return;
        auto& slot = slots_[size_t(active_)]; active_ = -1;
        const HRESULT last = slot.end->Issue(D3DISSUE_END);
        const HRESULT frequency = slot.frequency->Issue(D3DISSUE_END);
        const HRESULT disjoint = slot.disjoint->Issue(D3DISSUE_END);
        if (FAILED(last) || FAILED(frequency) || FAILED(disjoint)) ++stats_.dropped;
        else slot.pending = true;
    }
    Stats take() {
        poll(); auto result = stats_; stats_ = {};
        for (const auto& slot : slots_) result.pending += slot.pending;
        return result;
    }
};
}
