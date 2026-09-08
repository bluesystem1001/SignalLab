#pragma once

#include <cstddef>
#include <string_view>

#include "signallab/algorithm/spectrum.hpp"
#include "signallab/core/result.hpp"

namespace signallab::algorithm {

inline constexpr std::string_view kQualityMetricsVersion =
    "quality-metrics-power-bins/1.0.0";

// SNR is 10*log10(fundamental-neighborhood power / remaining non-DC power).
// Harmonic power is part of the noise term. Time complexity: O(B), extra
// space: O(1), where B is the spectrum bin count.
[[nodiscard]] core::Result<double> calculateSnrDb(
    const Spectrum& spectrum,
    double fundamentalHz,
    std::size_t guardBins = 1);

// THD is sqrt(power around harmonics 2..K / fundamental-neighborhood power)
// multiplied by 100. Time complexity: O(B + K), extra space: O(1).
[[nodiscard]] core::Result<double> calculateThdPercent(
    const Spectrum& spectrum,
    double fundamentalHz,
    std::size_t harmonicCount = 5,
    std::size_t guardBins = 1);

}  // namespace signallab::algorithm
