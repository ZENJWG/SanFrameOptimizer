#include "mws/UploadRing.hpp"
#include <cassert>
#include <cstdint>
#include <vector>
#include <utility>

int main() {
    using mws::reserveUploadSpan;
    static_assert(reserveUploadSpan(0, 128, 2048).discard);
    static_assert(reserveUploadSpan(128, 128, 2048).offset == 128);
    static_assert(reserveUploadSpan(2010, 128, 2048).discard);
    static_assert(reserveUploadSpan(2048, 128, 2048).discard);
    static_assert(reserveUploadSpan(0, 0, 2048).next == 0);
    std::uint32_t cursor = 0;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> inFlight;
    for (unsigned i=0; i<100000; ++i) {
        const std::uint32_t count = 1 + ((i * 97u) % 128u);
        const auto allocation = reserveUploadSpan(cursor, count, 2048);
        if (allocation.discard) inFlight.clear();
        assert(allocation.next <= 2048);
        for (const auto& span : inFlight)
            assert(allocation.next <= span.first || allocation.offset >= span.second);
        inFlight.emplace_back(allocation.offset, allocation.next);
        cursor = allocation.next;
    }
}
