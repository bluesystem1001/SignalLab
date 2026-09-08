#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "signallab/algorithm/spectrum.hpp"
#include "signallab/core/result.hpp"

namespace signallab::algorithm {

struct PeakOptions {
    double minimumAmplitude = 0.0;
    double minimumDistanceHz = 0.0;
    std::size_t maximumPeakCount = 10;
};

struct SpectrumPeak {
    std::size_t binIndex;
    double frequencyHz;
    double amplitude;
};

inline constexpr std::string_view kPeakDetectorVersion =
    "peak-detector-log-parabola/1.0.0";

// Finds local maxima, suppresses nearby weaker peaks and refines each selected
// peak with a three-point parabola in the log-amplitude domain.
// Time complexity: O(B log B + B * P). Extra space: O(B), where B is the
// spectrum bin count and P is maximumPeakCount.
[[nodiscard]] core::Result<std::vector<SpectrumPeak>> detectPeaks(
    const Spectrum& spectrum,
    const PeakOptions& options = {});

}  // namespace signallab::algorithm
