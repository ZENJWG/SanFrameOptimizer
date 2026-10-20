#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <cstring>

namespace fxp {
template<class Matrix> struct MatrixMemo {
    bool valid{};
    Matrix local{}, parent{}, composite{};
    const Matrix* parentPointer{};
    std::uint64_t epoch{};
    bool matches(std::uint64_t update, const Matrix& input, const Matrix* parentInput) const {
        return valid && epoch == update && parentInput && parentPointer == parentInput &&
            !std::memcmp(&local, &input, sizeof(Matrix)) &&
            !std::memcmp(&parent, parentInput, sizeof(Matrix));
    }
    void inputs(std::uint64_t update, const Matrix& input, const Matrix* parentInput) {
        local = input; parent = *parentInput; parentPointer = parentInput; epoch = update;
    }
};
// Four-way associative storage. Saturation returns nullptr: callers fall back to
// vanilla rather than evicting live entries and losing deferred simulation time.
template<class T, std::size_t Buckets> class FixedTable {
    static_assert((Buckets & (Buckets - 1)) == 0);
    struct Entry { std::uintptr_t key{}; T value{}; };
    std::array<std::array<Entry, 4>, Buckets> buckets_{};
    auto& bucket(std::uintptr_t key) {
        return buckets_[((key >> 4) * std::uintptr_t{2654435761u}) & (Buckets - 1)];
    }
public:
    T* find(std::uintptr_t key) {
        if (!key) return nullptr;
        for (auto& e : bucket(key)) if (e.key == key) return &e.value;
        return nullptr;
    }
    T* get(std::uintptr_t key) {
        if (!key) return nullptr;
        if (auto* v = find(key)) return v;
        for (auto& e : bucket(key)) if (!e.key) {
            e.value = {}; e.key = key; return &e.value;
        }
        return nullptr;
    }
    void erase(std::uintptr_t key) {
        if (!key) return;
        for (auto& e : bucket(key)) if (e.key == key) { e = {}; return; }
    }
    void clear() { for (auto& b : buckets_) for (auto& e : b) e = {}; }
};

} // namespace fxp
