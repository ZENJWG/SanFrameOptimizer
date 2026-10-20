#pragma once
#include <cmath>
#include <limits>
#include <optional>

namespace mws {
inline std::optional<int> submissionDepthBin(float viewZ) noexcept {
    const double bin = std::floor(static_cast<double>(viewZ) / 64.0);
    if (!std::isfinite(bin) ||
        bin < std::numeric_limits<int>::min() ||
        bin > std::numeric_limits<int>::max())
        return std::nullopt;
    return static_cast<int>(bin);
}
}
