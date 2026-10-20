#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>
#include <xmmintrin.h>

namespace fxp {
// Transpose FXP's channel-pointer arrays into contiguous, four-channel key rows.
// No LUT approximation: the original piecewise-linear keys are retained.
class Curve {
    std::vector<float> times_, rows_, inverseWidths_;
    unsigned channels_{}, stride_{};
    bool loop_{};
public:
    bool compile(std::span<const uint16_t> times, std::span<const float> channelMajor,
                 unsigned channels, bool loop) {
        // A failed rebuild must never leave the previous curve executable.
        times_.clear(); rows_.clear(); inverseWidths_.clear(); channels_ = stride_ = 0; loop_ = false;
        if (times.empty() || times.size() > 127 || !channels || channels > 64 ||
            channelMajor.size() != times.size() * channels) return false;
        for (size_t i = 1; i < times.size(); ++i)
            if (times[i] <= times[i - 1]) return false;
        if (loop && times.size() > 1 && !times.back()) return false;
        for (float v : channelMajor) if (!std::isfinite(v)) return false;
        channels_ = channels; stride_ = (channels + 3) & ~3u; loop_ = loop;
        times_.resize(times.size()); rows_.assign(times.size() * stride_, 0);
        for (size_t k = 0; k < times.size(); ++k) {
            times_[k] = times[k] * (1.0f / 256);
            for (unsigned c = 0; c < channels; ++c)
                rows_[k * stride_ + c] = channelMajor[c * times.size() + k];
        }
        inverseWidths_.resize(times.size());
        for (size_t k = 1; k < times.size(); ++k)
            inverseWidths_[k] = 1.0f / (times_[k] - times_[k - 1]);
        return true;
    }
    bool evaluate(float t, float* out) const {
        if (times_.empty() || !out || !std::isfinite(t)) return false;
        if (loop_ && times_.size() > 1) t -= std::trunc(t / times_.back()) * times_.back();
        size_t k = 1;
        // Strict upper bound preserves endpoint behavior and pre-first-key extrapolation.
        if (times_.size() > 1)
            k = size_t(std::upper_bound(times_.begin() + 1, times_.end(), t) - times_.begin());
        if (times_.size() == 1 || k == times_.size()) {
            std::copy_n(rows_.data() + (times_.size() - 1) * stride_, channels_, out);
            return true;
        }
        const float f = (t - times_[k - 1]) * inverseWidths_[k];
        const float* a = rows_.data() + (k - 1) * stride_;
        const float* b = rows_.data() + k * stride_;
        const __m128 factor = _mm_set1_ps(f);
        unsigned c{};
        for (; c + 4 <= channels_; c += 4) {
            const auto x = _mm_loadu_ps(a + c);
            _mm_storeu_ps(out + c, _mm_add_ps(x, _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(b + c), x), factor)));
        }
        for (; c < channels_; ++c) out[c] = a[c] + (b[c] - a[c]) * f;
        return true;
    }
};
}
