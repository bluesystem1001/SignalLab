#pragma once

#include <span>
#include <string_view>

#include "signallab/algorithm/spectrum.hpp"
#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::algorithm {

struct BasicFeatures {
    double mean;
    double variance;
    double rms;
    double peakToPeak;
};

inline constexpr std::string_view kFeaturesVersion = "features/1.0.0";

// Time complexity: O(N). Extra space: O(1), where N is samples.size().
[[nodiscard]] core::Result<BasicFeatures> calculateBasicFeatures(
    std::span<const double> samples);

// Time complexity: O(N). Extra space: O(1), where N is signal.sampleCount().
[[nodiscard]] core::Result<BasicFeatures> calculateBasicFeatures(
    const core::Signal& signal);

// Sums the one-sided spectrum powers inside the inclusive frequency band.
// Time complexity: O(B). Extra space: O(1), where B is the spectrum bin count.
[[nodiscard]] core::Result<double> calculateBandEnergy(
    const Spectrum& spectrum,
    double lowHz,
    double highHz);

}  // namespace signallab::algorithm
