#pragma once

#include <cstddef>
#include <vector>

#include "signallab/core/signal.hpp"

namespace signallab::app {

struct DisplayPoint {
    double timeSeconds;
    double value;
};

[[nodiscard]] std::vector<DisplayPoint> makeDisplayPoints(
    const core::Signal& signal,
    std::size_t pixelWidth);

}  // namespace signallab::app
