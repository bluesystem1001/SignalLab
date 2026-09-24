#pragma once

#include <cstddef>
#include <vector>

#include "signallab/algorithm/spectrum.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::app {

struct DisplayPoint {
    double timeSeconds;
    double value;
};

[[nodiscard]] std::vector<DisplayPoint> makeDisplayPoints(
    const core::Signal& signal,
    std::size_t pixelWidth);

[[nodiscard]] std::vector<DisplayPoint> makeDisplayPoints(
    const core::Signal& signal,
    std::size_t pixelWidth,
    std::size_t firstIndex,
    std::size_t pastLastIndex);

struct SpectrumDisplayPoint {
    double frequencyHz;
    double amplitude;
};

[[nodiscard]] std::vector<SpectrumDisplayPoint> makeSpectrumDisplayPoints(
    const algorithm::Spectrum& spectrum,
    std::size_t pixelWidth);

[[nodiscard]] std::vector<SpectrumDisplayPoint> makeSpectrumDisplayPoints(
    const algorithm::Spectrum& spectrum,
    std::size_t pixelWidth,
    std::size_t firstIndex,
    std::size_t pastLastIndex);

}  // namespace signallab::app
