// Copyright (c) 2026 Hopinette. MIT license; see LICENSE.
#pragma once
#include <algorithm>
#include <cstdint>

namespace paping {
struct Statistics {
    std::uint64_t attempted = 0, connected = 0, failed = 0, interrupted = 0, close_incomplete = 0;
    double minimum = 0, maximum = 0, total = 0;

    void success(double milliseconds, bool closed) {
        ++attempted;
        ++connected;
        if (connected == 1) minimum = maximum = milliseconds;
        else {
            minimum = std::min(minimum, milliseconds);
            maximum = std::max(maximum, milliseconds);
        }
        total += milliseconds;
        if (!closed) ++close_incomplete;
    }
    void failure() { ++attempted; ++failed; }
    void cancel() { ++attempted; ++interrupted; }
    double average() const { return connected ? total / static_cast<double>(connected) : 0; }
};
} // namespace paping
