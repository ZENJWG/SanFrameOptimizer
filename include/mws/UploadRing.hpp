#pragma once
#include <cstdint>

namespace mws {
// No allocation crosses the ring end or overwrites a live NOOVERWRITE span.
// DISCARD tells D3D9 to rename the backing storage before reusing offset 0.
struct UploadSpan {
    std::uint32_t offset{}, next{};
    bool discard{};
};
constexpr UploadSpan reserveUploadSpan(std::uint32_t cursor,
    std::uint32_t count, std::uint32_t capacity) noexcept {
    if (!count || count > capacity) return {};
    const bool wrap = cursor == 0 || cursor > capacity || count > capacity - cursor;
    const std::uint32_t start = wrap ? 0 : cursor;
    return {start, start + count, wrap};
}
}
